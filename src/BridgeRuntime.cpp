#include "BridgeRuntime.hpp"
#include "LiberaPaths.hpp"

#include "libera/System.hpp"
#include "libera/etherdream/EtherDreamManager.hpp"
#include "libera/helios/HeliosControllerInfo.hpp"
#include "libera/helios/HeliosManager.hpp"
#include "libera/lasercubenet/LaserCubeNetManager.hpp"
#include "libera/lasercubeusb/LaserCubeUsbManager.hpp"

#include "output/V1LaproGraphOut.hpp"
#include "server/IDNLaproService.hpp"
#include "shared/DACHWInterface.hpp"
#include "stage/SockIDNServer.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <exception>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace idn_bridge {
namespace {

using libera::core::LaserPoint;
using libera::core::PointFillRequest;
using namespace std::chrono_literals;

class RuntimeLogger {
public:
    void setEcho(bool enabled) {
        echo_.store(enabled, std::memory_order_relaxed);
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        lines_.clear();
        lastError_.clear();
    }

    void info(const std::string& line) {
        append(false, line);
    }

    void error(const std::string& line) {
        append(true, line);
    }

    std::vector<std::string> lines() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return std::vector<std::string>(lines_.begin(), lines_.end());
    }

    std::string lastError() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return lastError_;
    }

private:
    void append(bool isError, const std::string& line) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            lines_.push_back(line);
            if (lines_.size() > maxLines_) {
                lines_.pop_front();
            }
            if (isError) {
                lastError_ = line;
            }
        }

        if (!echo_.load(std::memory_order_relaxed)) {
            return;
        }

        if (isError) {
            std::cerr << line << std::endl;
        } else {
            std::cout << line << std::endl;
        }
    }

    static constexpr std::size_t maxLines_ = 250;

    mutable std::mutex mutex_;
    std::deque<std::string> lines_;
    std::string lastError_;
    std::atomic<bool> echo_{false};
};

using LogSink = std::shared_ptr<RuntimeLogger>;

std::string describeController(const libera::core::ControllerInfo& info) {
    std::ostringstream oss;
    oss << info.labelValue() << " [" << info.type() << ":" << info.idValue() << "]";
    return oss.str();
}

void printUsageImpl(const char* exe, std::ostream& out) {
    out << "Usage: " << exe << " [options]\n"
        << "  --discovery-timeout-ms <ms>    Libera discovery wait time (default 5000)\n"
        << "  --max-dacs <count>             Limit number of bridged controllers (default all)\n"
        << "  --slice-us <us>                Driver slice duration in microseconds (default 15000)\n"
        << "  --max-queue-points <count>     Max queued translated points per controller (default 300000)\n"
        << "  --latency-ms <ms>              Target buffered latency in milliseconds (default 50)\n"
        << "  --max-latency-ms <ms>          Max auto latency in milliseconds (default 1500)\n"
        << "  --no-auto-latency              Disable automatic latency increase on underrun\n"
        << "  --help                         Show this message\n";
}

bool parseUnsigned(const std::string& text, std::uint64_t& value) {
    try {
        std::size_t parsed = 0;
        const unsigned long long raw = std::stoull(text, &parsed, 10);
        if (parsed != text.size()) {
            return false;
        }
        value = static_cast<std::uint64_t>(raw);
        return true;
    } catch (...) {
        return false;
    }
}

class LiberaDacAdapter final : public DACHWInterface {
public:
    struct StatsSnapshot {
        std::uint64_t receivedSlices = 0;
        std::uint64_t receivedPoints = 0;
        std::uint64_t receivedLitPoints = 0;
        std::uint64_t callbackCalls = 0;
        std::uint64_t callbackUnderrunEvents = 0;
        std::uint64_t callbackUnderrunPoints = 0;
        std::uint64_t emittedPoints = 0;
        std::uint64_t blankFillPoints = 0;
        std::uint64_t droppedPoints = 0;
        std::size_t queuedPoints = 0;
        std::uint32_t outputPointRate = 0;
        std::uint32_t observedInputPointRate = 0;
        std::uint32_t latencyMs = 0;
        std::size_t targetBufferedPoints = 0;
        bool buffering = false;
    };

    LiberaDacAdapter(std::shared_ptr<libera::core::LaserController> controller,
                     std::string displayName,
                     unsigned maxPointRateValue,
                     std::size_t maxQueuedPoints,
                     std::uint32_t latencyMs,
                     std::uint32_t maxLatencyMs,
                     bool autoLatency,
                     LogSink logger)
        : controller_(std::move(controller))
        , displayName_(std::move(displayName))
        , maxPointRateValue_(maxPointRateValue > 0 ? maxPointRateValue : 100000u)
        , maxQueuedPoints_(std::max<std::size_t>(maxQueuedPoints, 1000))
        , latencyMs_(latencyMs)
        , maxLatencyMs_(std::max(maxLatencyMs, latencyMs))
        , autoLatency_(autoLatency)
        , logger_(std::move(logger)) {
        const auto initialRate = std::min<std::uint32_t>(30000u, maxPointRateValue_);
        currentPointRate_ = std::max<std::uint32_t>(initialRate, 1000u);
        commandedInputPps_.store(currentPointRate_, std::memory_order_relaxed);
        buffering_.store(true, std::memory_order_relaxed);

        controller_->setPointRate(currentPointRate_);
        controller_->setArmed(true);
        controller_->setRequestPointsCallback(
            [this](const PointFillRequest& req, std::vector<LaserPoint>& out) {
                fillFromQueue(req, out);
            });
    }

    ~LiberaDacAdapter() override {
        if (controller_) {
            controller_->setRequestPointsCallback({});
        }
    }

    int writeFrame(const TimeSlice& slice, double durationUs) override {
        const auto bpp = bytesPerPoint();
        if (bpp == 0 || slice.dataChunk.size() < bpp) {
            return 0;
        }

        const std::size_t pointCount = slice.dataChunk.size() / bpp;
        const unsigned commandedPointRate = inferCommandedPointRate(slice, pointCount, durationUs);
        receivedSlices_.fetch_add(1, std::memory_order_relaxed);
        receivedPoints_.fetch_add(pointCount, std::memory_order_relaxed);
        const auto* points = reinterpret_cast<const ISPDB25Point*>(slice.dataChunk.data());

        std::vector<LaserPoint> translated;
        translated.reserve(pointCount);
        for (std::size_t i = 0; i < pointCount; ++i) {
            translated.push_back(toLaserPoint(points[i]));
        }
        const std::size_t litCount = std::count_if(
            translated.begin(), translated.end(),
            [](const LaserPoint& p) {
                return p.i > 0.0f && (p.r > 0.0f || p.g > 0.0f || p.b > 0.0f);
            });
        receivedLitPoints_.fetch_add(litCount, std::memory_order_relaxed);

        {
            std::lock_guard<std::mutex> lock(queueMutex_);
            hasSeenInput_ = true;
            lastInputAt_ = std::chrono::steady_clock::now();
            const std::size_t projected = pendingPoints_.size() + translated.size();
            if (projected > maxQueuedPoints_) {
                const std::size_t overflow = projected - maxQueuedPoints_;
                const std::size_t dropCount = std::min<std::size_t>(overflow, pendingPoints_.size());
                for (std::size_t i = 0; i < dropCount; ++i) {
                    pendingPoints_.pop_front();
                }
                droppedPoints_.fetch_add(dropCount, std::memory_order_relaxed);
            }
            for (auto& p : translated) {
                pendingPoints_.push_back(p);
            }
        }

        maybeUpdatePointRate(commandedPointRate);
        return 0;
    }

    SliceType convertPoints(const std::vector<ISPDB25Point>& points) override {
        SliceType bytes(points.size() * sizeof(ISPDB25Point));
        if (!bytes.empty()) {
            std::memcpy(bytes.data(), points.data(), bytes.size());
        }
        return bytes;
    }

    unsigned bytesPerPoint() override {
        return static_cast<unsigned>(sizeof(ISPDB25Point));
    }

    unsigned maxBytesPerTransmission() override {
        return static_cast<unsigned>(4096 * sizeof(ISPDB25Point));
    }

    unsigned maxPointrate() override {
        return maxPointRateValue_;
    }

    void setMaxPointrate(unsigned rate) override {
        maxPointRateValue_ = std::max<unsigned>(rate, 1000);
        if (currentPointRate_ > maxPointRateValue_) {
            currentPointRate_ = maxPointRateValue_;
            controller_->setPointRate(currentPointRate_);
        }
    }

    void getName(char* nameBufferPtr, unsigned nameBufferSize) override {
        if (nameBufferPtr == nullptr || nameBufferSize == 0) {
            return;
        }
        std::snprintf(nameBufferPtr, nameBufferSize, "%s", displayName_.c_str());
    }

    StatsSnapshot getStatsSnapshot() const {
        StatsSnapshot snapshot;
        snapshot.receivedSlices = receivedSlices_.load(std::memory_order_relaxed);
        snapshot.receivedPoints = receivedPoints_.load(std::memory_order_relaxed);
        snapshot.receivedLitPoints = receivedLitPoints_.load(std::memory_order_relaxed);
        snapshot.callbackCalls = callbackCalls_.load(std::memory_order_relaxed);
        snapshot.callbackUnderrunEvents = callbackUnderrunEvents_.load(std::memory_order_relaxed);
        snapshot.callbackUnderrunPoints = callbackUnderrunPoints_.load(std::memory_order_relaxed);
        snapshot.emittedPoints = emittedPoints_.load(std::memory_order_relaxed);
        snapshot.blankFillPoints = blankFillPoints_.load(std::memory_order_relaxed);
        snapshot.droppedPoints = droppedPoints_.load(std::memory_order_relaxed);
        snapshot.outputPointRate = currentPointRate_;
        snapshot.observedInputPointRate = commandedInputPps_.load(std::memory_order_relaxed);
        snapshot.latencyMs = latencyMs_.load(std::memory_order_relaxed);
        snapshot.targetBufferedPoints = targetBufferedPoints();
        snapshot.buffering = buffering_.load(std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lock(queueMutex_);
            snapshot.queuedPoints = pendingPoints_.size();
        }
        return snapshot;
    }

    bool isAwaitingInput(std::chrono::milliseconds idleThreshold = 2000ms) const {
        std::lock_guard<std::mutex> lock(queueMutex_);
        return isAwaitingInputLocked(std::chrono::steady_clock::now(), idleThreshold);
    }

private:
    static unsigned inferCommandedPointRate(const TimeSlice& slice,
                                            std::size_t pointCount,
                                            double durationUs) {
        if (pointCount == 0) {
            return 0;
        }

        double effectiveDurationUs = durationUs;
        if (effectiveDurationUs <= 0.0) {
            effectiveDurationUs = static_cast<double>(slice.durationUs);
        }
        if (effectiveDurationUs <= 0.0) {
            return 0;
        }

        const double inferredRate =
            (1000000.0 * static_cast<double>(pointCount)) / effectiveDurationUs;
        if (!std::isfinite(inferredRate) || inferredRate <= 0.0) {
            return 0;
        }
        return static_cast<unsigned>(std::llround(inferredRate));
    }

    std::size_t targetBufferedPoints() const {
        const double latencyMs = static_cast<double>(latencyMs_.load(std::memory_order_relaxed));
        const double rawPoints = (static_cast<double>(currentPointRate_) * latencyMs) / 1000.0;
        const std::size_t minPoints = 1000;
        const std::size_t maxPoints = std::max<std::size_t>(minPoints, maxQueuedPoints_ - 1);
        return std::clamp<std::size_t>(
            static_cast<std::size_t>(std::llround(rawPoints)),
            minPoints,
            maxPoints);
    }

    static float unitFromU16(std::uint16_t value) {
        return static_cast<float>(value) / 65535.0f;
    }

    static float signedUnitFromU16(std::uint16_t value) {
        return unitFromU16(value) * 2.0f - 1.0f;
    }

    static LaserPoint toLaserPoint(const ISPDB25Point& in) {
        LaserPoint out{};
        out.x = std::clamp(signedUnitFromU16(in.x), -1.0f, 1.0f);
        out.y = std::clamp(signedUnitFromU16(in.y), -1.0f, 1.0f);

        out.r = std::clamp(unitFromU16(in.r), 0.0f, 1.0f);
        out.g = std::clamp(unitFromU16(in.g), 0.0f, 1.0f);
        out.b = std::clamp(unitFromU16(in.b), 0.0f, 1.0f);
        out.i = std::clamp(unitFromU16(in.intensity), 0.0f, 1.0f);

        const float rgbMax = std::max(out.r, std::max(out.g, out.b));
        if (out.i <= 0.0f && rgbMax > 0.0f) {
            out.i = rgbMax;
        }

        if (out.i <= 0.0f) {
            out.r = 0.0f;
            out.g = 0.0f;
            out.b = 0.0f;
            out.i = 0.0f;
        }

        out.u1 = std::clamp(unitFromU16(in.u1), 0.0f, 1.0f);
        out.u2 = std::clamp(unitFromU16(in.u2), 0.0f, 1.0f);
        return out;
    }

    void fillFromQueue(const PointFillRequest& req, std::vector<LaserPoint>& out) {
        if (req.maximumPointsRequired == 0) {
            return;
        }

        callbackCalls_.fetch_add(1, std::memory_order_relaxed);
        std::lock_guard<std::mutex> lock(queueMutex_);
        const std::size_t availableAtStart = pendingPoints_.size();
        const std::size_t targetPoints = targetBufferedPoints();
        if (buffering_.load(std::memory_order_relaxed) && availableAtStart < targetPoints) {
            const std::size_t missing = req.minimumPointsRequired;
            callbackUnderrunEvents_.fetch_add(1, std::memory_order_relaxed);
            callbackUnderrunPoints_.fetch_add(missing, std::memory_order_relaxed);
            logUnderrunIfDue(req, availableAtStart, missing, targetPoints, true);
            appendFallbackPoints(out, missing);
            blankFillPoints_.fetch_add(missing, std::memory_order_relaxed);
            return;
        }
        buffering_.store(false, std::memory_order_relaxed);

        const std::size_t toWrite = std::min<std::size_t>(req.maximumPointsRequired, pendingPoints_.size());
        for (std::size_t i = 0; i < toWrite; ++i) {
            out.push_back(pendingPoints_.front());
            pendingPoints_.pop_front();
        }
        emittedPoints_.fetch_add(toWrite, std::memory_order_relaxed);
        if (toWrite > 0) {
            lastOutputPoint_ = out.back();
            haveLastOutputPoint_ = true;
        }

        if (out.size() < req.minimumPointsRequired) {
            const std::size_t blankCount = req.minimumPointsRequired - out.size();
            callbackUnderrunEvents_.fetch_add(1, std::memory_order_relaxed);
            callbackUnderrunPoints_.fetch_add(blankCount, std::memory_order_relaxed);
            maybeIncreaseLatencyOnUnderrun(blankCount, req.minimumPointsRequired, targetPoints);
            logUnderrunIfDue(req, availableAtStart, blankCount, targetPoints, false);
            appendFallbackPoints(out, blankCount);
            blankFillPoints_.fetch_add(blankCount, std::memory_order_relaxed);
            buffering_.store(true, std::memory_order_relaxed);
        }
    }

    void appendFallbackPoints(std::vector<LaserPoint>& out, std::size_t count) {
        LaserPoint fallback{};
        if (haveLastOutputPoint_) {
            fallback = lastOutputPoint_;
            fallback.r = 0.0f;
            fallback.g = 0.0f;
            fallback.b = 0.0f;
            fallback.i = 0.0f;
        }
        out.insert(out.end(), count, fallback);
    }

    void maybeIncreaseLatencyOnUnderrun(std::size_t missingPoints,
                                        std::size_t minimumPointsRequired,
                                        std::size_t targetPoints) {
        if (!autoLatency_) {
            return;
        }

        if (minimumPointsRequired == 0) {
            return;
        }

        const auto now = std::chrono::steady_clock::now();
        if ((now - lastLatencyUnderrunSample_) > 2000ms) {
            latencyUnderrunAccumulator_ = 0;
        }
        lastLatencyUnderrunSample_ = now;
        latencyUnderrunAccumulator_ += missingPoints;

        if ((now - lastLatencyIncrease_) < 3000ms) {
            return;
        }

        const std::size_t threshold = std::max<std::size_t>(
            minimumPointsRequired / 2,
            std::max<std::size_t>(100, targetPoints / 3));
        if (latencyUnderrunAccumulator_ < threshold) {
            return;
        }

        const std::uint32_t before = latencyMs_.load(std::memory_order_relaxed);
        if (before >= maxLatencyMs_) {
            latencyUnderrunAccumulator_ = 0;
            return;
        }

        const std::uint32_t step = std::max<std::uint32_t>(10, before / 10);
        const std::uint32_t after = std::min<std::uint32_t>(maxLatencyMs_, before + step);
        latencyMs_.store(after, std::memory_order_relaxed);
        lastLatencyIncrease_ = now;
        latencyUnderrunAccumulator_ = 0;

        if (after != before && logger_) {
            std::ostringstream oss;
            oss << "[bridge:" << displayName_ << "] auto-latency "
                << before << "ms -> " << after << "ms";
            logger_->info(oss.str());
        }
    }

    void logUnderrunIfDue(const PointFillRequest& req,
                          std::size_t available,
                          std::size_t missing,
                          std::size_t targetPoints,
                          bool bufferingHold) {
        if (isAwaitingInputLocked(std::chrono::steady_clock::now(), 2000ms)) {
            return;
        }

        const auto now = std::chrono::steady_clock::now();
        if ((now - lastUnderrunLog_) < 1000ms) {
            return;
        }
        lastUnderrunLog_ = now;

        if (!logger_) {
            return;
        }

        std::ostringstream oss;
        oss << "[bridge:" << displayName_ << "] underrun"
            << " need_min=" << req.minimumPointsRequired
            << " need_max=" << req.maximumPointsRequired
            << " available=" << available
            << " missing=" << missing
            << " target=" << targetPoints
            << " queue_now=" << pendingPoints_.size()
            << " buffering=" << (bufferingHold ? 1 : 0);
        logger_->info(oss.str());
    }

    bool isAwaitingInputLocked(std::chrono::steady_clock::time_point now,
                               std::chrono::milliseconds idleThreshold) const {
        if (!hasSeenInput_) {
            return true;
        }
        if (!pendingPoints_.empty()) {
            return false;
        }
        return (now - lastInputAt_) >= idleThreshold;
    }

    void maybeUpdatePointRate(unsigned commandedPointRate) {
        if (commandedPointRate == 0) {
            return;
        }
        const std::uint32_t proposed = std::clamp<std::uint32_t>(commandedPointRate, 1000u, maxPointRateValue_);
        commandedInputPps_.store(proposed, std::memory_order_relaxed);
        if (proposed == currentPointRate_) {
            return;
        }
        controller_->setPointRate(proposed);
        currentPointRate_ = proposed;
    }

    std::shared_ptr<libera::core::LaserController> controller_;
    std::string displayName_;
    unsigned maxPointRateValue_;
    std::size_t maxQueuedPoints_;
    std::uint32_t currentPointRate_ = 30000;
    std::atomic<std::uint32_t> latencyMs_{300};
    std::uint32_t maxLatencyMs_ = 1500;
    bool autoLatency_ = true;
    LogSink logger_;
    std::atomic<std::uint32_t> commandedInputPps_{0};
    std::chrono::steady_clock::time_point lastLatencyIncrease_{};
    std::chrono::steady_clock::time_point lastLatencyUnderrunSample_{};
    std::size_t latencyUnderrunAccumulator_ = 0;
    mutable std::mutex queueMutex_;
    std::deque<LaserPoint> pendingPoints_;
    LaserPoint lastOutputPoint_{};
    bool haveLastOutputPoint_ = false;
    bool hasSeenInput_ = false;
    std::chrono::steady_clock::time_point lastInputAt_{};
    std::atomic<bool> buffering_{true};
    std::chrono::steady_clock::time_point lastUnderrunLog_{};
    std::atomic<std::uint64_t> receivedSlices_{0};
    std::atomic<std::uint64_t> receivedPoints_{0};
    std::atomic<std::uint64_t> receivedLitPoints_{0};
    std::atomic<std::uint64_t> callbackCalls_{0};
    std::atomic<std::uint64_t> callbackUnderrunEvents_{0};
    std::atomic<std::uint64_t> callbackUnderrunPoints_{0};
    std::atomic<std::uint64_t> emittedPoints_{0};
    std::atomic<std::uint64_t> blankFillPoints_{0};
    std::atomic<std::uint64_t> droppedPoints_{0};
};

class IdnBridgeEndpoint {
public:
    IdnBridgeEndpoint(std::shared_ptr<libera::core::LaserController> controller,
                      std::string dacLabel,
                      std::string dacId,
                      std::string dacType,
                      unsigned maxPps,
                      std::uint32_t sliceDurationUs,
                      std::size_t maxQueuedPoints,
                      std::uint32_t latencyMs,
                      std::uint32_t maxLatencyMs,
                      bool autoLatency,
                      std::uint8_t serviceId,
                      LogSink logger)
        : label_(std::move(dacLabel))
        , id_(std::move(dacId))
        , type_(std::move(dacType))
        , serviceId_(serviceId)
        , sliceDurationUs_(sliceDurationUs)
        , logger_(std::move(logger))
        , adapter_(std::make_shared<LiberaDacAdapter>(
              std::move(controller),
              label_,
              maxPps,
              maxQueuedPoints,
              latencyMs,
              maxLatencyMs,
              autoLatency,
              logger_))
        , output_(std::make_unique<V1LaproGraphicOutput>(adapter_)) {
        const std::string bridgedServiceName = std::string("IDNBridge ") + label_;
        std::vector<char> serviceName(bridgedServiceName.begin(), bridgedServiceName.end());
        serviceName.push_back('\0');
        const bool isDefault = (serviceId_ == 1);
        service_ = std::make_unique<IDNLaproService>(serviceId_, serviceName.data(), isDefault, output_.get());
    }

    ~IdnBridgeEndpoint() {
        stop();
    }

    void start() {
        if (running_.exchange(true)) {
            return;
        }
        driverThread_ = std::thread([this] { driverLoop(); });
    }

    void stop() {
        if (!running_.exchange(false)) {
            return;
        }
        if (driverThread_.joinable()) {
            driverThread_.join();
        }
    }

    void linkService(LLNode<ServiceNode>** firstService) {
        if (!service_ || !firstService) {
            return;
        }
        service_->linkinLast(firstService);
    }

    const std::string& label() const { return label_; }
    const std::string& id() const { return id_; }
    const std::string& type() const { return type_; }
    std::uint8_t serviceId() const { return serviceId_; }

    EndpointSnapshot snapshot() const {
        EndpointSnapshot snapshot;
        snapshot.label = label_;
        snapshot.id = id_;
        snapshot.type = type_;
        snapshot.serviceId = serviceId_;

        const auto stats = adapter_->getStatsSnapshot();
        snapshot.stats.receivedSlices = stats.receivedSlices;
        snapshot.stats.receivedPoints = stats.receivedPoints;
        snapshot.stats.receivedLitPoints = stats.receivedLitPoints;
        snapshot.stats.callbackCalls = stats.callbackCalls;
        snapshot.stats.callbackUnderrunEvents = stats.callbackUnderrunEvents;
        snapshot.stats.callbackUnderrunPoints = stats.callbackUnderrunPoints;
        snapshot.stats.emittedPoints = stats.emittedPoints;
        snapshot.stats.blankFillPoints = stats.blankFillPoints;
        snapshot.stats.droppedPoints = stats.droppedPoints;
        snapshot.stats.queuedPoints = stats.queuedPoints;
        snapshot.stats.outputPointRate = stats.outputPointRate;
        snapshot.stats.observedInputPointRate = stats.observedInputPointRate;
        snapshot.stats.latencyMs = stats.latencyMs;
        snapshot.stats.targetBufferedPoints = stats.targetBufferedPoints;
        snapshot.stats.buffering = stats.buffering;
        return snapshot;
    }

    void logStatsIfDue() {
        if (!logger_) {
            return;
        }

        const auto now = std::chrono::steady_clock::now();
        if ((now - lastStatsLog_) < 1s) {
            return;
        }
        lastStatsLog_ = now;

        const auto stats = adapter_->getStatsSnapshot();
        const bool awaitingInput = adapter_->isAwaitingInput();
        const auto deltaRxPts = stats.receivedPoints - lastStats_.receivedPoints;
        const auto deltaRxLitPts = stats.receivedLitPoints - lastStats_.receivedLitPoints;
        const auto deltaRxSlices = stats.receivedSlices - lastStats_.receivedSlices;
        const auto deltaCb = stats.callbackCalls - lastStats_.callbackCalls;
        const auto deltaUnderrunCb = stats.callbackUnderrunEvents - lastStats_.callbackUnderrunEvents;
        const auto deltaUnderrunPts = stats.callbackUnderrunPoints - lastStats_.callbackUnderrunPoints;
        const auto deltaOutPts = stats.emittedPoints - lastStats_.emittedPoints;
        const auto deltaBlank = stats.blankFillPoints - lastStats_.blankFillPoints;
        const auto deltaDropped = stats.droppedPoints - lastStats_.droppedPoints;

        if (awaitingInput && deltaRxSlices == 0 && deltaRxPts == 0 && deltaRxLitPts == 0) {
            lastStats_ = stats;
            return;
        }

        std::ostringstream oss;
        oss << "[bridge:" << label_ << "]"
            << " rx_slices/s=" << deltaRxSlices
            << " rx_pts/s=" << deltaRxPts
            << " rx_lit_pts/s=" << deltaRxLitPts
            << " cb/s=" << deltaCb
            << " und_cb/s=" << deltaUnderrunCb
            << " und_pts/s=" << deltaUnderrunPts
            << " out_pts/s=" << deltaOutPts
            << " pps=" << stats.outputPointRate
            << " in_pps=" << stats.observedInputPointRate
            << " lat_ms=" << stats.latencyMs
            << " lat_pts=" << stats.targetBufferedPoints
            << " buffering=" << (stats.buffering ? 1 : 0)
            << " queue=" << stats.queuedPoints
            << " blank/s=" << deltaBlank
            << " drop/s=" << deltaDropped;
        logger_->info(oss.str());

        lastStats_ = stats;
    }

private:
    void driverLoop() {
        TransformEnv tfEnv;
        tfEnv.usPerSlice = static_cast<double>(sliceDurationUs_);
        tfEnv.currentSliceTime = tfEnv.usPerSlice;

        unsigned driverMode = DRIVER_INACTIVE;
        auto currentBuffer = std::make_shared<SliceBuf>();

        while (running_.load(std::memory_order_relaxed)) {
            auto nextBuffer = adapter_->getNextBuffer(tfEnv, driverMode);
            if (nextBuffer && !nextBuffer->empty()) {
                currentBuffer = nextBuffer;
            }

            if (!currentBuffer || currentBuffer->empty()) {
                std::this_thread::sleep_for(1ms);
                continue;
            }

            const std::size_t iterationCount = currentBuffer->size();
            for (std::size_t i = 0; i < iterationCount && running_.load(std::memory_order_relaxed); ++i) {
                auto nextSlice = currentBuffer->front();
                currentBuffer->pop_front();

                if (!nextSlice) {
                    continue;
                }

                adapter_->writeFrame(*nextSlice, static_cast<double>(nextSlice->durationUs));

                if (driverMode == DRIVER_FRAMEMODE) {
                    currentBuffer->push_back(nextSlice);
                }
            }
        }
    }

    std::string label_;
    std::string id_;
    std::string type_;
    std::uint8_t serviceId_;
    std::uint32_t sliceDurationUs_;
    LogSink logger_;

    std::shared_ptr<LiberaDacAdapter> adapter_;
    std::unique_ptr<V1LaproGraphicOutput> output_;
    std::unique_ptr<IDNLaproService> service_;
    std::thread driverThread_;
    std::atomic<bool> running_{false};
    std::chrono::steady_clock::time_point lastStatsLog_{};
    LiberaDacAdapter::StatsSnapshot lastStats_{};
};

std::vector<std::unique_ptr<libera::core::ControllerInfo>> discoverControllers(
    libera::System& liberaSystem,
    std::uint32_t timeoutMs,
    const std::atomic<bool>& stopRequested) {
    auto started = std::chrono::steady_clock::now();
    std::vector<std::unique_ptr<libera::core::ControllerInfo>> discovered;

    while (!stopRequested.load(std::memory_order_relaxed)) {
        discovered = liberaSystem.discoverControllers();
        if (!discovered.empty()) {
            return discovered;
        }

        if (timeoutMs > 0) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started);
            if (elapsed.count() >= timeoutMs) {
                return discovered;
            }
        }

        std::this_thread::sleep_for(200ms);
    }

    return discovered;
}

bool shouldBridgeController(const libera::core::ControllerInfo& info, std::string& reason) {
    if (info.type() == "Helios") {
        const auto* heliosInfo = dynamic_cast<const libera::helios::HeliosControllerInfo*>(&info);
        if (heliosInfo != nullptr && !heliosInfo->isUsbController()) {
            reason = "already an IDN network Helios controller";
            return false;
        }
    }

    reason.clear();
    return true;
}

bool isControllerSelected(const std::set<std::string>& selectedControllerIds,
                          const libera::core::ControllerInfo& info) {
    return selectedControllerIds.empty() ||
           selectedControllerIds.find(info.idValue()) != selectedControllerIds.end();
}

const char* usageStateLabel(libera::core::ControllerUsageState state) {
    switch (state) {
    case libera::core::ControllerUsageState::Idle:
        return "Idle";
    case libera::core::ControllerUsageState::Active:
        return "Active";
    case libera::core::ControllerUsageState::BusyExclusive:
        return "Busy";
    case libera::core::ControllerUsageState::Unknown:
    default:
        return "Unknown";
    }
}

DiscoveredControllerSnapshot makeDiscoveredControllerSnapshot(
    const libera::core::ControllerInfo& info) {
    DiscoveredControllerSnapshot snapshot;
    snapshot.label = info.labelValue();
    snapshot.id = info.idValue();
    snapshot.type = info.type();
    snapshot.maxPointRate = info.maxPointRate();
    snapshot.usage = usageStateLabel(info.usageState());

    std::string reason;
    snapshot.bridgeable = shouldBridgeController(info, reason);
    snapshot.note = std::move(reason);
    return snapshot;
}

std::vector<DiscoveredControllerSnapshot> buildDiscoveredControllerSnapshots(
    const std::vector<std::unique_ptr<libera::core::ControllerInfo>>& discovered) {
    std::vector<DiscoveredControllerSnapshot> snapshots;
    snapshots.reserve(discovered.size());
    for (const auto& info : discovered) {
        if (!info) {
            continue;
        }
        snapshots.push_back(makeDiscoveredControllerSnapshot(*info));
    }
    return snapshots;
}

void stopStartedEndpoints(std::vector<std::unique_ptr<IdnBridgeEndpoint>>& endpoints) {
    for (auto& endpoint : endpoints) {
        endpoint->stop();
    }
    endpoints.clear();
}

} // namespace

struct BridgeRuntime::Impl {
    mutable std::mutex mutex;
    RuntimeState state = RuntimeState::Stopped;
    std::string statusMessage = "Stopped";
    bool hasDiscoveryResults = false;
    std::size_t discoveredControllers = 0;
    std::vector<DiscoveredControllerSnapshot> discovered;

    std::unique_ptr<libera::System> liberaSystem;
    std::unique_ptr<SockIDNServer> server;
    std::thread serverThread;
    std::thread monitorThread;
    std::vector<std::unique_ptr<IdnBridgeEndpoint>> endpoints;

    std::atomic<bool> stopRequested{false};
    LogSink logger = std::make_shared<RuntimeLogger>();

    void setState(RuntimeState nextState, std::string message) {
        std::lock_guard<std::mutex> lock(mutex);
        state = nextState;
        statusMessage = std::move(message);
    }
};

void printUsage(const char* exe) {
    printUsageImpl(exe, std::cout);
}

ParseResult parseOptions(int argc, char** argv, BridgeOptions& options) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help") {
            printUsage(argv[0]);
            return ParseResult::Help;
        }
        if (arg == "--no-auto-latency") {
            options.autoLatency = false;
            continue;
        }
        if (i + 1 >= argc) {
            std::cerr << "Missing value for " << arg << "\n";
            printUsage(argv[0]);
            return ParseResult::Error;
        }

        std::uint64_t raw = 0;
        if (!parseUnsigned(argv[i + 1], raw)) {
            std::cerr << "Invalid numeric value for " << arg << ": " << argv[i + 1] << "\n";
            return ParseResult::Error;
        }

        if (arg == "--discovery-timeout-ms") {
            options.discoveryTimeoutMs = static_cast<std::uint32_t>(
                std::min<std::uint64_t>(raw, std::numeric_limits<std::uint32_t>::max()));
        } else if (arg == "--max-dacs") {
            options.maxDacs = static_cast<std::size_t>(raw);
        } else if (arg == "--slice-us") {
            options.sliceDurationUs = static_cast<std::uint32_t>(std::max<std::uint64_t>(raw, 1000));
        } else if (arg == "--max-queue-points") {
            options.maxQueuedPoints = static_cast<std::size_t>(std::max<std::uint64_t>(raw, 1000));
        } else if (arg == "--latency-ms") {
            options.latencyMs = static_cast<std::uint32_t>(std::clamp<std::uint64_t>(raw, 0, 10000));
        } else if (arg == "--max-latency-ms") {
            options.maxLatencyMs = static_cast<std::uint32_t>(std::clamp<std::uint64_t>(raw, 0, 30000));
        } else {
            std::cerr << "Unknown option: " << arg << "\n";
            printUsage(argv[0]);
            return ParseResult::Error;
        }
        ++i;
    }

    if (options.maxLatencyMs < options.latencyMs) {
        options.maxLatencyMs = options.latencyMs;
    }

    return ParseResult::Ok;
}

const char* runtimeStateLabel(RuntimeState state) {
    switch (state) {
    case RuntimeState::Stopped:
        return "Stopped";
    case RuntimeState::Scanning:
        return "Scanning";
    case RuntimeState::Starting:
        return "Starting";
    case RuntimeState::Running:
        return "Running";
    case RuntimeState::StopRequested:
        return "Stopping";
    case RuntimeState::Failed:
        return "Failed";
    }
    return "Unknown";
}

BridgeRuntime::BridgeRuntime()
    : impl_(std::make_unique<Impl>()) {
    configureLiberaPluginDirectories();
    impl_->liberaSystem = std::make_unique<libera::System>();
}

BridgeRuntime::~BridgeRuntime() {
    stop();
}

void BridgeRuntime::setEchoLogsToStdStreams(bool enabled) {
    impl_->logger->setEcho(enabled);
}

bool BridgeRuntime::scan(const BridgeOptions& options) {
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (impl_->state == RuntimeState::Scanning ||
            impl_->state == RuntimeState::Starting ||
            impl_->state == RuntimeState::Running ||
            impl_->state == RuntimeState::StopRequested) {
            return false;
        }
        impl_->state = RuntimeState::Scanning;
        impl_->statusMessage = "Scanning for controllers...";
        impl_->hasDiscoveryResults = false;
        impl_->discoveredControllers = 0;
        impl_->discovered.clear();
    }

    impl_->logger->clear();
    impl_->stopRequested.store(false, std::memory_order_relaxed);
    impl_->logger->info("Scanning for controllers via Libera");

    if (!impl_->liberaSystem) {
        impl_->liberaSystem = std::make_unique<libera::System>();
    }
    auto* liberaSystem = impl_->liberaSystem.get();
    auto discovered = discoverControllers(*liberaSystem, options.discoveryTimeoutMs, impl_->stopRequested);
    auto discoveredSnapshots = buildDiscoveredControllerSnapshots(discovered);

    if (impl_->stopRequested.load(std::memory_order_relaxed)) {
        impl_->setState(RuntimeState::Stopped, "Stopped");
        impl_->logger->info("Controller scan cancelled.");
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->hasDiscoveryResults = true;
        impl_->discoveredControllers = discoveredSnapshots.size();
        impl_->discovered = std::move(discoveredSnapshots);
        impl_->state = RuntimeState::Stopped;
        impl_->statusMessage = discovered.empty() ? "No controllers found" : "Scan complete";
    }

    if (discovered.empty()) {
        impl_->logger->info("No controllers discovered via Libera.");
        return false;
    }

    {
        std::ostringstream oss;
        oss << "Discovered " << discovered.size() << " controller(s) via Libera.";
        impl_->logger->info(oss.str());
    }
    return true;
}

bool BridgeRuntime::start(const BridgeOptions& options) {
    static const std::set<std::string> allControllers;
    return start(options, allControllers);
}

bool BridgeRuntime::start(const BridgeOptions& options,
                          const std::set<std::string>& selectedControllerIds) {
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (impl_->state == RuntimeState::Scanning ||
            impl_->state == RuntimeState::Starting ||
            impl_->state == RuntimeState::Running ||
            impl_->state == RuntimeState::StopRequested) {
            return false;
        }
        impl_->state = RuntimeState::Starting;
        impl_->statusMessage = "Discovering controllers...";
    }

    impl_->logger->clear();
    impl_->stopRequested.store(false, std::memory_order_relaxed);
    if (selectedControllerIds.empty()) {
        impl_->logger->info("Starting IDN Bridge");
    } else {
        std::ostringstream oss;
        oss << "Starting IDN Bridge for " << selectedControllerIds.size()
            << " selected controller(s)";
        impl_->logger->info(oss.str());
    }

    if (!impl_->liberaSystem) {
        impl_->liberaSystem = std::make_unique<libera::System>();
    }
    auto* liberaSystem = impl_->liberaSystem.get();
    auto discovered = discoverControllers(*liberaSystem, options.discoveryTimeoutMs, impl_->stopRequested);
    auto discoveredSnapshots = buildDiscoveredControllerSnapshots(discovered);

    if (impl_->stopRequested.load(std::memory_order_relaxed)) {
        impl_->setState(RuntimeState::Stopped, "Stopped");
        impl_->logger->info("Bridge start cancelled.");
        return false;
    }

    if (discovered.empty()) {
        {
            std::lock_guard<std::mutex> lock(impl_->mutex);
            impl_->hasDiscoveryResults = true;
            impl_->discoveredControllers = 0;
            impl_->discovered.clear();
        }
        const std::string error = "No controllers discovered via Libera.";
        impl_->setState(RuntimeState::Failed, error);
        impl_->logger->error(error);
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->hasDiscoveryResults = true;
        impl_->discoveredControllers = discoveredSnapshots.size();
        impl_->discovered = std::move(discoveredSnapshots);
        impl_->statusMessage = "Connecting controllers...";
    }

    {
        std::ostringstream oss;
        oss << "Discovered " << discovered.size() << " controller(s) via Libera.";
        impl_->logger->info(oss.str());
    }

    LLNode<ServiceNode>* firstService = nullptr;
    std::vector<std::unique_ptr<IdnBridgeEndpoint>> endpoints;
    endpoints.reserve(discovered.size());

    std::size_t startedEndpoints = 0;
    for (const auto& info : discovered) {
        if (impl_->stopRequested.load(std::memory_order_relaxed)) {
            break;
        }
        if (!info) {
            continue;
        }

        {
            std::lock_guard<std::mutex> lock(impl_->mutex);
            std::ostringstream oss;
            oss << "Connecting " << info->labelValue() << "...";
            impl_->statusMessage = oss.str();
        }

        std::string skipReason;
        if (!shouldBridgeController(*info, skipReason)) {
            std::ostringstream oss;
            oss << "Skipping " << describeController(*info) << " (" << skipReason << ")";
            impl_->logger->info(oss.str());
            continue;
        }

        if (!isControllerSelected(selectedControllerIds, *info)) {
            continue;
        }

        if (options.maxDacs > 0 && startedEndpoints >= options.maxDacs) {
            break;
        }

        auto controller = liberaSystem->connectController(*info);
        if (!controller) {
            std::ostringstream oss;
            oss << "Skipping " << info->labelValue() << " (connect failed)";
            impl_->logger->error(oss.str());
            continue;
        }

        const auto serviceIdRaw = startedEndpoints + 1;
        if (serviceIdRaw > 255) {
            std::ostringstream oss;
            oss << "Service ID range exceeded. Stopping at " << startedEndpoints << " controller(s).";
            impl_->logger->error(oss.str());
            break;
        }

        auto endpoint = std::make_unique<IdnBridgeEndpoint>(
            controller,
            info->labelValue(),
            info->idValue(),
            info->type(),
            info->maxPointRate(),
            options.sliceDurationUs,
            options.maxQueuedPoints,
            options.latencyMs,
            options.maxLatencyMs,
            options.autoLatency,
            static_cast<std::uint8_t>(serviceIdRaw),
            impl_->logger);

        endpoint->linkService(&firstService);
        endpoint->start();

        {
            std::ostringstream oss;
            oss << "Bridged " << endpoint->label()
                << " [" << endpoint->type() << ":" << endpoint->id() << "]"
                << " -> IDN service " << static_cast<unsigned>(endpoint->serviceId());
            impl_->logger->info(oss.str());
        }

        endpoints.push_back(std::move(endpoint));
        ++startedEndpoints;
    }

    if (impl_->stopRequested.load(std::memory_order_relaxed)) {
        stopStartedEndpoints(endpoints);
        impl_->setState(RuntimeState::Stopped, "Stopped");
        impl_->logger->info("Bridge start cancelled.");
        return false;
    }

    if (endpoints.empty()) {
        const std::string error = selectedControllerIds.empty()
                                      ? "No bridge endpoints started."
                                      : "No selected controllers were started.";
        stopStartedEndpoints(endpoints);
        impl_->setState(RuntimeState::Failed, error);
        impl_->logger->error(error);
        return false;
    }

    auto server = std::make_unique<SockIDNServer>(firstService);
    std::thread serverThread([serverPtr = server.get()] { serverPtr->networkThreadFunc(); });

    std::string startupError;
    if (!server->waitUntilStarted(2000ms, startupError)) {
        if (startupError.empty()) {
            startupError = "IDN server failed to start.";
        }
        server->stopServer();
        if (serverThread.joinable()) {
            serverThread.join();
        }
        stopStartedEndpoints(endpoints);
        impl_->setState(RuntimeState::Failed, startupError);
        impl_->logger->error(startupError);
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->server = std::move(server);
        impl_->serverThread = std::move(serverThread);
        impl_->endpoints = std::move(endpoints);
        impl_->state = RuntimeState::Running;
        impl_->statusMessage = "Running";
    }

    impl_->monitorThread = std::thread([impl = impl_.get()] {
        while (!impl->stopRequested.load(std::memory_order_relaxed)) {
            {
                std::lock_guard<std::mutex> lock(impl->mutex);
                for (auto& endpoint : impl->endpoints) {
                    endpoint->logStatsIfDue();
                }
            }
            std::this_thread::sleep_for(200ms);
        }
    });

    impl_->logger->info("Bridge running.");
    return true;
}

void BridgeRuntime::requestStop() {
    impl_->stopRequested.store(true, std::memory_order_relaxed);

    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->state == RuntimeState::Starting || impl_->state == RuntimeState::Running) {
        impl_->state = RuntimeState::StopRequested;
        impl_->statusMessage = "Stopping...";
    }
}

void BridgeRuntime::stop() {
    requestStop();

    std::thread monitorThread;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        monitorThread = std::move(impl_->monitorThread);
    }
    if (monitorThread.joinable()) {
        monitorThread.join();
    }

    std::thread serverThread;
    std::unique_ptr<SockIDNServer> server;
    std::vector<std::unique_ptr<IdnBridgeEndpoint>> endpoints;
    bool hadActiveResources = false;

    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        hadActiveResources = impl_->server != nullptr ||
                             !impl_->endpoints.empty() ||
                             impl_->state != RuntimeState::Stopped;
        server = std::move(impl_->server);
        serverThread = std::move(impl_->serverThread);
        endpoints = std::move(impl_->endpoints);
        impl_->state = RuntimeState::Stopped;
        impl_->statusMessage = "Stopped";
    }

    if (server) {
        server->stopServer();
    }
    if (serverThread.joinable()) {
        serverThread.join();
    }

    stopStartedEndpoints(endpoints);

    impl_->stopRequested.store(false, std::memory_order_relaxed);
    if (hadActiveResources) {
        impl_->logger->info("Bridge stopped.");
    }
}

RuntimeSnapshot BridgeRuntime::snapshot() const {
    RuntimeSnapshot snapshot;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        snapshot.state = impl_->state;
        snapshot.statusMessage = impl_->statusMessage;
        snapshot.stopRequested = impl_->stopRequested.load(std::memory_order_relaxed);
        snapshot.hasDiscoveryResults = impl_->hasDiscoveryResults;
        snapshot.discoveredControllers = impl_->discoveredControllers;
        snapshot.discovered = impl_->discovered;
        snapshot.startedEndpoints = impl_->endpoints.size();
        snapshot.endpoints.reserve(impl_->endpoints.size());
        for (const auto& endpoint : impl_->endpoints) {
            snapshot.endpoints.push_back(endpoint->snapshot());
        }
    }

    snapshot.lastError = impl_->logger->lastError();
    snapshot.recentLogs = impl_->logger->lines();
    return snapshot;
}

} // namespace idn_bridge
