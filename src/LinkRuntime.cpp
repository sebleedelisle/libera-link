#include "LinkRuntime.hpp"
#include "LiberaPaths.hpp"
#include "ingest/IdnIngester.hpp"
#include "ingest/IngesterRegistry.hpp"

#include "libera/System.hpp"
#include "libera/core/LaserController.hpp"
#include "libera/etherdream/EtherDreamManager.hpp"
#include "libera/helios/HeliosControllerInfo.hpp"
#include "libera/helios/HeliosManager.hpp"
#include "libera/lasercubenet/LaserCubeNetManager.hpp"
#include "libera/lasercubeusb/LaserCubeUsbManager.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
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

namespace libera_link {
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
    ingest::ensureBuiltInIdnIngesterLinked();
    const auto defaultIngester = ingest::defaultIngester();
    const auto availableIngesters = ingest::availableIngesters();
    out << "Usage: " << exe << " [options]\n"
        << "  --discovery-timeout-ms <ms>    Libera discovery wait time (default 5000)\n"
        << "  --max-dacs <count>             Limit number of linked controllers (default all)\n"
        << "  --ingester <id>                Ingester to run (default "
        << (defaultIngester ? defaultIngester->id : "none") << ")\n"
        << "  --ingester-opt <key=value>     Pass a custom option to the selected ingester\n"
        << "  --slice-us <us>                Driver slice duration in microseconds (default 15000)\n"
        << "  --max-queue-points <count>     Max queued translated points per controller (default 300000)\n"
        << "  --latency-ms <ms>              Target buffered latency in milliseconds (default 50)\n"
        << "  --max-latency-ms <ms>          Max auto latency in milliseconds (default 1500)\n"
        << "  --no-auto-latency              Disable automatic latency increase on underrun\n"
        << "  --help                         Show this message\n";

    if (!availableIngesters.empty()) {
        out << "\nAvailable ingesters:\n";
        for (const auto& info : availableIngesters) {
            out << "  " << info.id << "  " << info.displayName;
            if (info.defaultSelection) {
                out << " [default]";
            }
            if (!info.description.empty()) {
                out << " - " << info.description;
            }
            out << "\n";
        }
    }
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

bool parseKeyValue(std::string_view text, std::string& key, std::string& value) {
    const auto equals = text.find('=');
    if (equals == std::string_view::npos || equals == 0 || equals + 1 >= text.size()) {
        return false;
    }

    key.assign(text.substr(0, equals));
    value.assign(text.substr(equals + 1));
    return true;
}

class LiberaTarget final : public ingest::TargetSink {
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
        std::size_t controllerPrefetchedPoints = 0;
        std::size_t controllerTransportBufferedPoints = 0;
        std::size_t controllerBufferedPoints = 0;
        std::size_t totalBufferedPoints = 0;
        std::uint32_t outputPointRate = 0;
        std::uint32_t observedInputPointRate = 0;
        std::uint32_t latencyMs = 0;
        std::size_t targetBufferedPoints = 0;
        bool buffering = false;
    };

    LiberaTarget(std::shared_ptr<libera::core::LaserController> controller,
                 ingest::TargetInfo info,
                 std::size_t maxQueuedPoints,
                 std::uint32_t latencyMs,
                 std::uint32_t maxLatencyMs,
                 bool autoLatency,
                 LogSink logger)
        : controller_(std::move(controller))
        , info_(std::move(info))
        , maxPointRateValue_(info_.maxPointRate > 0 ? info_.maxPointRate : 100000u)
        , maxQueuedPoints_(std::max<std::size_t>(maxQueuedPoints, 1000))
        , latencyMs_(latencyMs)
        , maxLatencyMs_(std::max(maxLatencyMs, latencyMs))
        , autoLatency_(autoLatency)
        , logger_(std::move(logger)) {
        const auto initialRate = std::min<std::uint32_t>(30000u, maxPointRateValue_);
        const auto startingRate = std::max<std::uint32_t>(initialRate, 1000u);
        currentPointRate_.store(startingRate, std::memory_order_relaxed);
        commandedInputPps_.store(startingRate, std::memory_order_relaxed);
        buffering_.store(true, std::memory_order_relaxed);

        controller_->setPointRate(startingRate);
        controller_->setArmed(true);
        controller_->setRequestPointsCallback(
            [this](const PointFillRequest& req, std::vector<LaserPoint>& out) {
                fillFromQueue(req, out);
            });
    }

    ~LiberaTarget() override {
        if (controller_) {
            controller_->setRequestPointsCallback({});
        }
    }

    const ingest::TargetInfo& targetInfo() const override {
        return info_;
    }

    void submitContinuous(ingest::SliceSubmission submission) override {
        const std::size_t pointCount = submission.points.size();
        if (pointCount == 0) {
            return;
        }

        receivedSlices_.fetch_add(1, std::memory_order_relaxed);
        receivedPoints_.fetch_add(pointCount, std::memory_order_relaxed);
        const auto downstreamBuffer = downstreamBufferSnapshot();
        const std::size_t litCount = std::count_if(
            submission.points.begin(), submission.points.end(),
            [](const LaserPoint& p) {
                return p.i > 0.0f && (p.r > 0.0f || p.g > 0.0f || p.b > 0.0f);
            });
        receivedLitPoints_.fetch_add(litCount, std::memory_order_relaxed);

        {
            std::lock_guard<std::mutex> lock(queueMutex_);
            hasSeenInput_ = true;
            lastInputAt_ = std::chrono::steady_clock::now();
            for (auto& point : submission.points) {
                pendingPoints_.push_back(point);
            }
            std::size_t dropCount = 0;
            while (!pendingPoints_.empty() &&
                   (pendingPoints_.size() + downstreamBuffer.pointsInBuffer) > maxQueuedPoints_) {
                pendingPoints_.pop_front();
                ++dropCount;
            }
            if (dropCount > 0) {
                droppedPoints_.fetch_add(dropCount, std::memory_order_relaxed);
            }
        }

        maybeUpdatePointRate(submission.effectivePointRate.value_or(
            inferCommandedPointRate(pointCount, static_cast<double>(submission.durationUs))));
    }

    void replaceFrame(ingest::FrameSubmission submission) override {
        if (submission.slices.empty()) {
            return;
        }

        std::deque<LaserPoint> replacementPoints;
        std::size_t totalPointCount = 0;
        std::size_t totalLitCount = 0;
        std::size_t validSliceCount = 0;
        std::uint64_t totalDurationUs = 0;

        for (const auto& slice : submission.slices) {
            if (slice.points.empty()) {
                continue;
            }

            ++validSliceCount;
            totalPointCount += slice.points.size();
            totalDurationUs += slice.durationUs;

            for (const auto& point : slice.points) {
                if (point.i > 0.0f &&
                    (point.r > 0.0f || point.g > 0.0f || point.b > 0.0f)) {
                    ++totalLitCount;
                }
                replacementPoints.push_back(point);
            }
        }

        if (replacementPoints.empty()) {
            return;
        }

        receivedSlices_.fetch_add(validSliceCount, std::memory_order_relaxed);
        receivedPoints_.fetch_add(totalPointCount, std::memory_order_relaxed);
        receivedLitPoints_.fetch_add(totalLitCount, std::memory_order_relaxed);

        const auto downstreamBuffer = downstreamBufferSnapshot();
        {
            std::lock_guard<std::mutex> lock(queueMutex_);
            hasSeenInput_ = true;
            lastInputAt_ = std::chrono::steady_clock::now();
            pendingPoints_.swap(replacementPoints);

            std::size_t dropCount = 0;
            while (!pendingPoints_.empty() &&
                   (pendingPoints_.size() + downstreamBuffer.pointsInBuffer) > maxQueuedPoints_) {
                pendingPoints_.pop_front();
                ++dropCount;
            }
            if (dropCount > 0) {
                droppedPoints_.fetch_add(dropCount, std::memory_order_relaxed);
            }
        }

        maybeUpdatePointRate(
            inferCommandedPointRate(totalPointCount, static_cast<double>(totalDurationUs)));
        if (submission.clearTransportPrefetch && controller_) {
            controller_->clearPointCallbackPrefetch();
        }
    }

    void reset() override {
        {
            std::lock_guard<std::mutex> lock(queueMutex_);
            pendingPoints_.clear();
            hasSeenInput_ = false;
            haveLastOutputPoint_ = false;
            lastInputAt_ = {};
        }
        buffering_.store(true, std::memory_order_relaxed);
        commandedInputPps_.store(
            currentPointRate_.load(std::memory_order_relaxed),
            std::memory_order_relaxed);
        lastObservedDownstreamBufferedPoints_.store(0, std::memory_order_relaxed);
        lastObservedDownstreamPrefetchedPoints_.store(0, std::memory_order_relaxed);
        lastObservedDownstreamTransportBufferedPoints_.store(0, std::memory_order_relaxed);
        lastObservedDownstreamBufferValid_.store(false, std::memory_order_relaxed);
        if (controller_) {
            controller_->clearPointCallbackPrefetch();
        }
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
        snapshot.outputPointRate = currentPointRate_.load(std::memory_order_relaxed);
        snapshot.observedInputPointRate = commandedInputPps_.load(std::memory_order_relaxed);
        snapshot.latencyMs = latencyMs_.load(std::memory_order_relaxed);
        snapshot.targetBufferedPoints = targetBufferedPoints();
        snapshot.buffering = buffering_.load(std::memory_order_relaxed);
        const auto downstreamBuffer = downstreamBufferSnapshot();
        snapshot.controllerPrefetchedPoints = downstreamBuffer.prefetchedPoints;
        snapshot.controllerTransportBufferedPoints = downstreamBuffer.transportBufferedPoints;
        snapshot.controllerBufferedPoints = downstreamBuffer.pointsInBuffer;
        {
            std::lock_guard<std::mutex> lock(queueMutex_);
            snapshot.queuedPoints = pendingPoints_.size();
        }
        snapshot.totalBufferedPoints =
            snapshot.queuedPoints + snapshot.controllerBufferedPoints;
        return snapshot;
    }

    bool isAwaitingInput(std::chrono::milliseconds idleThreshold = 2000ms) const {
        std::lock_guard<std::mutex> lock(queueMutex_);
        return isAwaitingInputLocked(std::chrono::steady_clock::now(), idleThreshold);
    }

    EndpointSnapshot snapshot(std::string_view ingesterId,
                              std::string_view ingesterDisplayName,
                              const ingest::BindingInfo* binding) const {
        EndpointSnapshot snapshot;
        snapshot.label = info_.label;
        snapshot.id = info_.id;
        snapshot.type = info_.type;
        snapshot.ingesterId = std::string(ingesterId);
        snapshot.ingesterDisplayName = std::string(ingesterDisplayName);
        if (binding != nullptr) {
            snapshot.bindingLabel = binding->label;
            snapshot.bindingValue = binding->value;
        }

        const auto stats = getStatsSnapshot();
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
        snapshot.stats.controllerPrefetchedPoints = stats.controllerPrefetchedPoints;
        snapshot.stats.controllerTransportBufferedPoints =
            stats.controllerTransportBufferedPoints;
        snapshot.stats.controllerBufferedPoints = stats.controllerBufferedPoints;
        snapshot.stats.totalBufferedPoints = stats.totalBufferedPoints;
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

        const auto stats = getStatsSnapshot();
        const bool awaitingInput = isAwaitingInput();
        const auto deltaRxPts = stats.receivedPoints - lastStats_.receivedPoints;
        const auto deltaRxLitPts = stats.receivedLitPoints - lastStats_.receivedLitPoints;
        const auto deltaRxSlices = stats.receivedSlices - lastStats_.receivedSlices;
        const auto deltaCb = stats.callbackCalls - lastStats_.callbackCalls;
        const auto deltaUnderrunCb =
            stats.callbackUnderrunEvents - lastStats_.callbackUnderrunEvents;
        const auto deltaUnderrunPts =
            stats.callbackUnderrunPoints - lastStats_.callbackUnderrunPoints;
        const auto deltaOutPts = stats.emittedPoints - lastStats_.emittedPoints;
        const auto deltaBlank = stats.blankFillPoints - lastStats_.blankFillPoints;
        const auto deltaDropped = stats.droppedPoints - lastStats_.droppedPoints;

        if (awaitingInput && deltaRxSlices == 0 && deltaRxPts == 0 && deltaRxLitPts == 0) {
            lastStats_ = stats;
            return;
        }

        std::ostringstream oss;
        oss << "[link:" << info_.label << "]"
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
            << " queue_local=" << stats.queuedPoints
            << " queue_prefetch=" << stats.controllerPrefetchedPoints
            << " queue_transport=" << stats.controllerTransportBufferedPoints
            << " queue_controller=" << stats.controllerBufferedPoints
            << " queue_total=" << stats.totalBufferedPoints
            << " blank/s=" << deltaBlank
            << " drop/s=" << deltaDropped;
        logger_->info(oss.str());

        lastStats_ = stats;
    }

private:
    struct DownstreamBufferSnapshot {
        std::size_t pointsInBuffer = 0;
        std::size_t prefetchedPoints = 0;
        std::size_t transportBufferedPoints = 0;
        bool valid = false;
    };

    static unsigned inferCommandedPointRate(std::size_t pointCount,
                                            double durationUs) {
        if (pointCount == 0 || durationUs <= 0.0) {
            return 0;
        }

        const double inferredRate =
            (1000000.0 * static_cast<double>(pointCount)) / durationUs;
        if (!std::isfinite(inferredRate) || inferredRate <= 0.0) {
            return 0;
        }
        return static_cast<unsigned>(std::llround(inferredRate));
    }

    std::size_t targetBufferedPoints() const {
        const double latencyMs = static_cast<double>(latencyMs_.load(std::memory_order_relaxed));
        const double rawPoints =
            (static_cast<double>(currentPointRate_.load(std::memory_order_relaxed)) * latencyMs) /
            1000.0;
        const std::size_t minPoints = 1000;
        const std::size_t maxPoints = std::max<std::size_t>(minPoints, maxQueuedPoints_ - 1);
        return std::clamp<std::size_t>(
            static_cast<std::size_t>(std::llround(rawPoints)),
            minPoints,
            maxPoints);
    }

    DownstreamBufferSnapshot downstreamBufferSnapshot(bool allowControllerQuery = true) const {
        DownstreamBufferSnapshot snapshot;
        if (!allowControllerQuery) {
            snapshot.pointsInBuffer =
                lastObservedDownstreamBufferedPoints_.load(std::memory_order_relaxed);
            snapshot.prefetchedPoints =
                lastObservedDownstreamPrefetchedPoints_.load(std::memory_order_relaxed);
            snapshot.transportBufferedPoints =
                lastObservedDownstreamTransportBufferedPoints_.load(std::memory_order_relaxed);
            snapshot.valid =
                lastObservedDownstreamBufferValid_.load(std::memory_order_relaxed);
            return snapshot;
        }

        if (!controller_) {
            lastObservedDownstreamBufferedPoints_.store(0, std::memory_order_relaxed);
            lastObservedDownstreamPrefetchedPoints_.store(0, std::memory_order_relaxed);
            lastObservedDownstreamTransportBufferedPoints_.store(0, std::memory_order_relaxed);
            lastObservedDownstreamBufferValid_.store(false, std::memory_order_relaxed);
            return snapshot;
        }

        const auto breakdown = controller_->getPointCallbackBufferBreakdown();
        if (!breakdown) {
            lastObservedDownstreamBufferedPoints_.store(0, std::memory_order_relaxed);
            lastObservedDownstreamPrefetchedPoints_.store(0, std::memory_order_relaxed);
            lastObservedDownstreamTransportBufferedPoints_.store(0, std::memory_order_relaxed);
            lastObservedDownstreamBufferValid_.store(false, std::memory_order_relaxed);
            return snapshot;
        }

        snapshot.pointsInBuffer = breakdown->totalBufferedPoints;
        snapshot.prefetchedPoints = breakdown->prefetchedPoints;
        snapshot.transportBufferedPoints = breakdown->transportBufferedPoints;
        snapshot.valid = true;
        lastObservedDownstreamBufferedPoints_.store(snapshot.pointsInBuffer,
                                                    std::memory_order_relaxed);
        lastObservedDownstreamPrefetchedPoints_.store(snapshot.prefetchedPoints,
                                                      std::memory_order_relaxed);
        lastObservedDownstreamTransportBufferedPoints_.store(
            snapshot.transportBufferedPoints,
            std::memory_order_relaxed);
        lastObservedDownstreamBufferValid_.store(true, std::memory_order_relaxed);
        return snapshot;
    }

    void fillFromQueue(const PointFillRequest& req, std::vector<LaserPoint>& out) {
        if (req.maximumPointsRequired == 0) {
            return;
        }

        callbackCalls_.fetch_add(1, std::memory_order_relaxed);
        const auto downstreamBuffer = downstreamBufferSnapshot(false);
        std::lock_guard<std::mutex> lock(queueMutex_);
        const std::size_t availableAtStart = pendingPoints_.size();
        const std::size_t targetPoints = targetBufferedPoints();
        const std::size_t totalBufferedAtStart =
            availableAtStart + downstreamBuffer.pointsInBuffer;
        if (buffering_.load(std::memory_order_relaxed) &&
            totalBufferedAtStart < targetPoints) {
            const std::size_t missing = req.minimumPointsRequired;
            callbackUnderrunEvents_.fetch_add(1, std::memory_order_relaxed);
            callbackUnderrunPoints_.fetch_add(missing, std::memory_order_relaxed);
            logUnderrunIfDue(req,
                             availableAtStart,
                             downstreamBuffer.pointsInBuffer,
                             missing,
                             targetPoints,
                             true);
            appendFallbackPoints(out, missing);
            blankFillPoints_.fetch_add(missing, std::memory_order_relaxed);
            return;
        }
        buffering_.store(false, std::memory_order_relaxed);

        const std::size_t toWrite =
            std::min<std::size_t>(req.maximumPointsRequired, pendingPoints_.size());
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
            logUnderrunIfDue(req,
                             availableAtStart,
                             downstreamBuffer.pointsInBuffer,
                             blankCount,
                             targetPoints,
                             false);
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
        if (!autoLatency_ || minimumPointsRequired == 0) {
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
            oss << "[link:" << info_.label << "] auto-latency "
                << before << "ms -> " << after << "ms";
            logger_->info(oss.str());
        }
    }

    void logUnderrunIfDue(const PointFillRequest& req,
                          std::size_t available,
                          std::size_t downstreamBufferedPoints,
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
        oss << "[link:" << info_.label << "] underrun"
            << " need_min=" << req.minimumPointsRequired
            << " need_max=" << req.maximumPointsRequired
            << " queue_local=" << available
            << " queue_controller=" << downstreamBufferedPoints
            << " queue_total=" << (available + downstreamBufferedPoints)
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
        const std::uint32_t proposed =
            std::clamp<std::uint32_t>(commandedPointRate, 1000u, maxPointRateValue_);
        commandedInputPps_.store(proposed, std::memory_order_relaxed);
        if (proposed == currentPointRate_.load(std::memory_order_relaxed)) {
            return;
        }
        controller_->setPointRate(proposed);
        currentPointRate_.store(proposed, std::memory_order_relaxed);
    }

    std::shared_ptr<libera::core::LaserController> controller_;
    ingest::TargetInfo info_;
    unsigned maxPointRateValue_;
    std::size_t maxQueuedPoints_;
    std::atomic<std::uint32_t> currentPointRate_{30000};
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
    mutable std::atomic<std::size_t> lastObservedDownstreamBufferedPoints_{0};
    mutable std::atomic<std::size_t> lastObservedDownstreamPrefetchedPoints_{0};
    mutable std::atomic<std::size_t> lastObservedDownstreamTransportBufferedPoints_{0};
    mutable std::atomic<bool> lastObservedDownstreamBufferValid_{false};
    std::chrono::steady_clock::time_point lastStatsLog_{};
    StatsSnapshot lastStats_{};
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

bool shouldLinkController(const libera::core::ControllerInfo& info, std::string& reason) {
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
    snapshot.linkable = shouldLinkController(info, reason);
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

const ingest::BindingInfo* bindingForTarget(
    const std::vector<ingest::BindingInfo>& bindings,
    const std::string& targetId) {
    for (const auto& binding : bindings) {
        if (binding.targetId == targetId) {
            return &binding;
        }
    }
    return nullptr;
}

} // namespace

struct LinkRuntime::Impl {
    mutable std::mutex mutex;
    RuntimeState state = RuntimeState::Stopped;
    std::string statusMessage = "Stopped";
    bool hasDiscoveryResults = false;
    std::size_t discoveredControllers = 0;
    std::vector<DiscoveredControllerSnapshot> discovered;

    std::unique_ptr<libera::System> liberaSystem;
    std::unique_ptr<ingest::Ingester> ingester;
    std::thread monitorThread;
    std::vector<std::shared_ptr<LiberaTarget>> targets;
    std::vector<ingest::BindingInfo> bindings;
    std::string activeIngesterId;
    std::string activeIngesterDisplayName;

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

ParseResult parseOptions(int argc, char** argv, LinkOptions& options) {
    ingest::ensureBuiltInIdnIngesterLinked();
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

        if (arg == "--ingester") {
            options.ingesterId = argv[i + 1];
            ++i;
            continue;
        }

        if (arg == "--ingester-opt") {
            std::string key;
            std::string value;
            if (!parseKeyValue(argv[i + 1], key, value)) {
                std::cerr << "Invalid ingester option for " << arg
                          << ": expected key=value, got " << argv[i + 1] << "\n";
                return ParseResult::Error;
            }
            options.ingesterOptions[key] = value;
            ++i;
            continue;
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
            options.sliceDurationUs =
                static_cast<std::uint32_t>(std::max<std::uint64_t>(raw, 1000));
        } else if (arg == "--max-queue-points") {
            options.maxQueuedPoints =
                static_cast<std::size_t>(std::max<std::uint64_t>(raw, 1000));
        } else if (arg == "--latency-ms") {
            options.latencyMs =
                static_cast<std::uint32_t>(std::clamp<std::uint64_t>(raw, 0, 10000));
        } else if (arg == "--max-latency-ms") {
            options.maxLatencyMs =
                static_cast<std::uint32_t>(std::clamp<std::uint64_t>(raw, 0, 30000));
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

    if (!options.ingesterId.empty() && !ingest::findIngester(options.ingesterId)) {
        std::cerr << "Unknown ingester: " << options.ingesterId << "\n";
        printUsage(argv[0]);
        return ParseResult::Error;
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

LinkRuntime::LinkRuntime()
    : impl_(std::make_unique<Impl>()) {
    configureLiberaPluginDirectories();
    impl_->liberaSystem = std::make_unique<libera::System>();
}

LinkRuntime::~LinkRuntime() {
    stop();
}

void LinkRuntime::setEchoLogsToStdStreams(bool enabled) {
    impl_->logger->setEcho(enabled);
}

bool LinkRuntime::scan(const LinkOptions& options) {
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
    auto discovered =
        discoverControllers(*liberaSystem, options.discoveryTimeoutMs, impl_->stopRequested);
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

bool LinkRuntime::start(const LinkOptions& options) {
    static const std::set<std::string> allControllers;
    return start(options, allControllers);
}

bool LinkRuntime::start(const LinkOptions& options,
                          const std::set<std::string>& selectedControllerIds) {
    const auto selectedIngesterInfo = [&]() -> std::optional<ingest::RegistrationInfo> {
        if (!options.ingesterId.empty()) {
            return ingest::findIngester(options.ingesterId);
        }
        return ingest::defaultIngester();
    }();

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
        impl_->activeIngesterId.clear();
        impl_->activeIngesterDisplayName.clear();
    }

    impl_->logger->clear();
    impl_->stopRequested.store(false, std::memory_order_relaxed);

    if (!selectedIngesterInfo) {
        const std::string error = options.ingesterId.empty()
                                      ? "No ingesters are registered."
                                      : "Unknown ingester \"" + options.ingesterId + "\".";
        impl_->setState(RuntimeState::Failed, error);
        impl_->logger->error(error);
        return false;
    }

    libera::core::LaserController::setTargetLatency(std::chrono::milliseconds(0));
    libera::core::LaserController::setMaxFrameHoldTime(std::chrono::milliseconds(0));

    if (selectedControllerIds.empty()) {
        std::ostringstream oss;
        oss << "Starting Libera Link via " << selectedIngesterInfo->displayName;
        impl_->logger->info(oss.str());
    } else {
        std::ostringstream oss;
        oss << "Starting Libera Link via " << selectedIngesterInfo->displayName
            << " for " << selectedControllerIds.size() << " selected controller(s)";
        impl_->logger->info(oss.str());
    }

    if (!impl_->liberaSystem) {
        impl_->liberaSystem = std::make_unique<libera::System>();
    }
    auto* liberaSystem = impl_->liberaSystem.get();
    auto discovered =
        discoverControllers(*liberaSystem, options.discoveryTimeoutMs, impl_->stopRequested);
    auto discoveredSnapshots = buildDiscoveredControllerSnapshots(discovered);

    if (impl_->stopRequested.load(std::memory_order_relaxed)) {
        impl_->setState(RuntimeState::Stopped, "Stopped");
        impl_->logger->info("Link start cancelled.");
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

    std::vector<std::shared_ptr<LiberaTarget>> targets;
    targets.reserve(discovered.size());

    std::size_t startedTargets = 0;
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
        if (!shouldLinkController(*info, skipReason)) {
            std::ostringstream oss;
            oss << "Skipping " << describeController(*info) << " (" << skipReason << ")";
            impl_->logger->info(oss.str());
            continue;
        }

        if (!isControllerSelected(selectedControllerIds, *info)) {
            continue;
        }

        if (options.maxDacs > 0 && startedTargets >= options.maxDacs) {
            break;
        }

        auto controller = liberaSystem->connectController(*info);
        if (!controller) {
            std::ostringstream oss;
            oss << "Skipping " << info->labelValue() << " (connect failed)";
            impl_->logger->error(oss.str());
            continue;
        }

        ingest::TargetInfo targetInfo;
        targetInfo.id = info->idValue();
        targetInfo.label = info->labelValue();
        targetInfo.type = info->type();
        targetInfo.maxPointRate = info->maxPointRate();

        auto target = std::make_shared<LiberaTarget>(
            controller,
            std::move(targetInfo),
            options.maxQueuedPoints,
            options.latencyMs,
            options.maxLatencyMs,
            options.autoLatency,
            impl_->logger);

        targets.push_back(std::move(target));
        ++startedTargets;
    }

    if (impl_->stopRequested.load(std::memory_order_relaxed)) {
        targets.clear();
        impl_->setState(RuntimeState::Stopped, "Stopped");
        impl_->logger->info("Link start cancelled.");
        return false;
    }

    if (targets.empty()) {
        const std::string error = selectedControllerIds.empty()
                                      ? "No link endpoints started."
                                      : "No selected controllers were started.";
        impl_->setState(RuntimeState::Failed, error);
        impl_->logger->error(error);
        return false;
    }

    ingest::StartContext startContext;
    startContext.targets.reserve(targets.size());
    for (const auto& target : targets) {
        startContext.targets.push_back(ingest::Target{target});
    }

    ingest::FactoryConfig ingesterConfig;
    ingesterConfig.sliceDurationUs = options.sliceDurationUs;
    ingesterConfig.options = options.ingesterOptions;

    std::string startupError;
    auto ingester = ingest::createIngester(selectedIngesterInfo->id, ingesterConfig, startupError);
    if (!ingester) {
        targets.clear();
        impl_->setState(RuntimeState::Failed, startupError);
        impl_->logger->error(startupError);
        return false;
    }

    if (!ingester->start(startContext, startupError)) {
        if (startupError.empty()) {
            startupError = std::string(selectedIngesterInfo->displayName) +
                " ingester failed to start.";
        }
        targets.clear();
        impl_->setState(RuntimeState::Failed, startupError);
        impl_->logger->error(startupError);
        return false;
    }

    if (impl_->stopRequested.load(std::memory_order_relaxed)) {
        ingester->stop();
        targets.clear();
        impl_->setState(RuntimeState::Stopped, "Stopped");
        impl_->logger->info("Link start cancelled.");
        return false;
    }

    auto bindings = ingester->bindings();
    const std::string activeIngesterId = selectedIngesterInfo->id;
    const std::string activeIngesterDisplayName = std::string(ingester->displayName());
    for (const auto& target : targets) {
        const auto* binding = bindingForTarget(bindings, target->targetInfo().id);
        std::ostringstream oss;
        oss << "Linked " << target->targetInfo().label
            << " [" << target->targetInfo().type << ":" << target->targetInfo().id << "]"
            << " via " << activeIngesterDisplayName;
        if (binding != nullptr && !binding->label.empty()) {
            oss << " -> " << binding->label;
        }
        impl_->logger->info(oss.str());
    }

    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->ingester = std::move(ingester);
        impl_->targets = std::move(targets);
        impl_->bindings = std::move(bindings);
        impl_->activeIngesterId = activeIngesterId;
        impl_->activeIngesterDisplayName = activeIngesterDisplayName;
        impl_->state = RuntimeState::Running;
        impl_->statusMessage = "Running";
    }

    impl_->monitorThread = std::thread([impl = impl_.get()] {
        while (!impl->stopRequested.load(std::memory_order_relaxed)) {
            {
                std::lock_guard<std::mutex> lock(impl->mutex);
                for (auto& target : impl->targets) {
                    target->logStatsIfDue();
                }
            }
            std::this_thread::sleep_for(200ms);
        }
    });

    impl_->logger->info("Link running.");
    return true;
}

void LinkRuntime::requestStop() {
    impl_->stopRequested.store(true, std::memory_order_relaxed);

    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->state == RuntimeState::Starting || impl_->state == RuntimeState::Running) {
        impl_->state = RuntimeState::StopRequested;
        impl_->statusMessage = "Stopping...";
    }
}

void LinkRuntime::stop() {
    requestStop();

    std::thread monitorThread;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        monitorThread = std::move(impl_->monitorThread);
    }
    if (monitorThread.joinable()) {
        monitorThread.join();
    }

    std::unique_ptr<ingest::Ingester> ingester;
    std::vector<std::shared_ptr<LiberaTarget>> targets;
    bool hadActiveResources = false;

    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        hadActiveResources = impl_->ingester != nullptr ||
                             !impl_->targets.empty() ||
                             impl_->state != RuntimeState::Stopped;
        ingester = std::move(impl_->ingester);
        targets = std::move(impl_->targets);
        impl_->bindings.clear();
        impl_->activeIngesterId.clear();
        impl_->activeIngesterDisplayName.clear();
        impl_->state = RuntimeState::Stopped;
        impl_->statusMessage = "Stopped";
    }

    if (ingester) {
        ingester->stop();
    }
    targets.clear();

    impl_->stopRequested.store(false, std::memory_order_relaxed);
    if (hadActiveResources) {
        impl_->logger->info("Link stopped.");
    }
}

RuntimeSnapshot LinkRuntime::snapshot() const {
    RuntimeSnapshot snapshot;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        snapshot.state = impl_->state;
        snapshot.statusMessage = impl_->statusMessage;
        snapshot.stopRequested = impl_->stopRequested.load(std::memory_order_relaxed);
        snapshot.activeIngesterId = impl_->activeIngesterId;
        snapshot.activeIngesterDisplayName = impl_->activeIngesterDisplayName;
        snapshot.hasDiscoveryResults = impl_->hasDiscoveryResults;
        snapshot.discoveredControllers = impl_->discoveredControllers;
        snapshot.discovered = impl_->discovered;
        snapshot.startedEndpoints = impl_->targets.size();
        snapshot.endpoints.reserve(impl_->targets.size());
        for (const auto& target : impl_->targets) {
            snapshot.endpoints.push_back(target->snapshot(
                impl_->activeIngesterId,
                impl_->activeIngesterDisplayName,
                bindingForTarget(impl_->bindings, target->targetInfo().id)));
        }
    }

    snapshot.lastError = impl_->logger->lastError();
    snapshot.recentLogs = impl_->logger->lines();
    return snapshot;
}

} // namespace libera_link
