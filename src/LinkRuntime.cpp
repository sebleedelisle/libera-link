#include "LinkRuntime.hpp"
#include "LiberaPaths.hpp"
#include "virtual_controller/IdnVirtualControllerHost.hpp"
#include "virtual_controller/VirtualControllerHostRegistry.hpp"

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
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
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
    virtual_controller::ensureBuiltInIdnVirtualControllerHostLinked();
    const auto defaultVirtualControllerHost = virtual_controller::defaultVirtualControllerHost();
    const auto availableVirtualControllerHosts = virtual_controller::availableVirtualControllerHosts();
    out << "Usage: " << exe << " [options]\n"
        << "  --discovery-timeout-ms <ms>    Libera discovery wait time (default 5000)\n"
        << "  --max-dacs <count>             Limit number of linked controllers (default all)\n"
        << "  --virtual-controller <id>      Virtual controller to expose (default "
        << (defaultVirtualControllerHost ? defaultVirtualControllerHost->id : "none") << ")\n"
        << "  --virtual-controller-opt <key=value>\n"
        << "                                  Pass a custom option to the selected virtual controller\n"
        << "  --slice-us <us>                Driver slice duration in microseconds (default 15000)\n"
        << "  --max-queue-points <count>     Max queued translated points per controller (default 300000)\n"
        << "  --latency-ms <ms>              Target buffered latency in milliseconds (default 50)\n"
        << "  --max-latency-ms <ms>          Max auto latency in milliseconds (default 1500)\n"
        << "  --no-auto-latency              Disable automatic latency increase on underrun\n"
        << "  --help                         Show this message\n";

    if (!availableVirtualControllerHosts.empty()) {
        out << "\nAvailable virtual controllers:\n";
        for (const auto& info : availableVirtualControllerHosts) {
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

class LiberaTarget final : public virtual_controller::TargetSink {
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
                 virtual_controller::TargetInfo info,
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

    const virtual_controller::TargetInfo& targetInfo() const override {
        return info_;
    }

    virtual_controller::SubmissionResult submitContinuous(virtual_controller::SliceSubmission submission) override {
        const std::size_t pointCount = submission.points.size();
        if (pointCount == 0) {
            return makeSubmissionResult(false, 0, 0);
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

        std::size_t dropCount = 0;
        {
            std::lock_guard<std::mutex> lock(queueMutex_);
            hasSeenInput_ = true;
            lastInputAt_ = std::chrono::steady_clock::now();
            for (auto& point : submission.points) {
                pendingPoints_.push_back(point);
            }
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
        return makeSubmissionResult(true, pointCount, dropCount);
    }

    virtual_controller::SubmissionResult replaceFrame(virtual_controller::FrameSubmission submission) override {
        if (submission.slices.empty()) {
            return makeSubmissionResult(false, 0, 0);
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
            return makeSubmissionResult(false, 0, 0);
        }

        receivedSlices_.fetch_add(validSliceCount, std::memory_order_relaxed);
        receivedPoints_.fetch_add(totalPointCount, std::memory_order_relaxed);
        receivedLitPoints_.fetch_add(totalLitCount, std::memory_order_relaxed);

        const auto downstreamBuffer = downstreamBufferSnapshot();
        std::size_t dropCount = 0;
        {
            std::lock_guard<std::mutex> lock(queueMutex_);
            hasSeenInput_ = true;
            lastInputAt_ = std::chrono::steady_clock::now();
            pendingPoints_.swap(replacementPoints);

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
        return makeSubmissionResult(true, totalPointCount, dropCount);
    }

    virtual_controller::TargetStatus status() const override {
        return targetStatusSnapshot();
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

    virtual_controller::TargetStatus targetStatusSnapshot() const {
        const auto stats = getStatsSnapshot();
        virtual_controller::TargetStatus status;
        status.queuedPoints = stats.queuedPoints;
        status.maxQueuedPoints = maxQueuedPoints_;
        status.controllerPrefetchedPoints = stats.controllerPrefetchedPoints;
        status.controllerTransportBufferedPoints = stats.controllerTransportBufferedPoints;
        status.controllerBufferedPoints = stats.controllerBufferedPoints;
        status.totalBufferedPoints = stats.totalBufferedPoints;
        status.targetBufferedPoints = stats.targetBufferedPoints;
        status.outputPointRate = stats.outputPointRate;
        status.observedInputPointRate = stats.observedInputPointRate;
        status.latencyMs = stats.latencyMs;
        status.receivedPoints = stats.receivedPoints;
        status.droppedPoints = stats.droppedPoints;
        status.underrunEvents = stats.callbackUnderrunEvents;
        status.underrunPoints = stats.callbackUnderrunPoints;
        status.buffering = stats.buffering;
        return status;
    }

    virtual_controller::SubmissionResult makeSubmissionResult(bool accepted,
                                                              std::size_t submittedPoints,
                                                              std::size_t droppedPoints) const {
        virtual_controller::SubmissionResult result;
        result.accepted = accepted;
        result.submittedPoints = submittedPoints;
        result.droppedPoints = droppedPoints;
        result.acceptedPoints = accepted
            ? submittedPoints - std::min(submittedPoints, droppedPoints)
            : 0;
        result.status = targetStatusSnapshot();
        return result;
    }

    bool isAwaitingInput(std::chrono::milliseconds idleThreshold = 2000ms) const {
        std::lock_guard<std::mutex> lock(queueMutex_);
        return isAwaitingInputLocked(std::chrono::steady_clock::now(), idleThreshold);
    }

    EndpointSnapshot snapshot(std::string_view virtualControllerHostId,
                              std::string_view virtualControllerHostDisplayName,
                              const virtual_controller::VirtualControllerEndpoint* endpoint) const {
        EndpointSnapshot snapshot;
        snapshot.label = info_.label;
        snapshot.id = info_.id;
        snapshot.type = info_.type;
        snapshot.virtualControllerHostId = std::string(virtualControllerHostId);
        snapshot.virtualControllerHostDisplayName = std::string(virtualControllerHostDisplayName);
        if (endpoint != nullptr) {
            snapshot.virtualControllerEndpointLabel = endpoint->label;
            snapshot.virtualControllerEndpointValue = endpoint->value;
            snapshot.virtualControllerEndpointKind = endpoint->kind;
            snapshot.virtualControllerEndpointProtocol = endpoint->protocol;
            snapshot.virtualControllerEndpointTransport = endpoint->transport;
            snapshot.virtualControllerEndpointAddress = endpoint->address;
            snapshot.virtualControllerEndpointPort = endpoint->port;
            snapshot.virtualControllerEndpointChannels = endpoint->channels;
            snapshot.virtualControllerEndpointAttributes = endpoint->attributes;
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
    virtual_controller::TargetInfo info_;
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

const virtual_controller::VirtualControllerEndpoint* endpointForTarget(
    const std::vector<virtual_controller::VirtualControllerEndpoint>& endpoints,
    const std::string& targetId) {
    for (const auto& endpoint : endpoints) {
        if (endpoint.targetId == targetId) {
            return &endpoint;
        }
    }
    return nullptr;
}

struct ActiveVirtualControllerHost {
    std::string id;
    std::string displayName;
    std::string instanceKey;
    std::unique_ptr<virtual_controller::VirtualControllerHost> host;
    std::vector<std::shared_ptr<LiberaTarget>> targets;
    std::vector<virtual_controller::VirtualControllerEndpoint> endpoints;
};

const ActiveVirtualControllerHost* activeHostForTarget(
    const std::vector<ActiveVirtualControllerHost>& activeHosts,
    const std::string& targetId) {
    for (const auto& activeHost : activeHosts) {
        for (const auto& target : activeHost.targets) {
            if (target && target->targetInfo().id == targetId) {
                return &activeHost;
            }
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
    std::vector<ActiveVirtualControllerHost> virtualControllerHosts;
    std::thread monitorThread;
    std::vector<std::shared_ptr<LiberaTarget>> targets;
    std::string activeVirtualControllerHostId;
    std::string activeVirtualControllerHostDisplayName;

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
    virtual_controller::ensureBuiltInIdnVirtualControllerHostLinked();
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

        if (arg == "--virtual-controller") {
            options.virtualControllerHostId = argv[i + 1];
            ++i;
            continue;
        }

        if (arg == "--virtual-controller-opt") {
            std::string key;
            std::string value;
            if (!parseKeyValue(argv[i + 1], key, value)) {
                std::cerr << "Invalid virtual controller host option for " << arg
                          << ": expected key=value, got " << argv[i + 1] << "\n";
                return ParseResult::Error;
            }
            options.virtualControllerHostOptions[key] = value;
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

    if (!options.virtualControllerHostId.empty() && !virtual_controller::findVirtualControllerHost(options.virtualControllerHostId)) {
        std::cerr << "Unknown virtual controller host: " << options.virtualControllerHostId << "\n";
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
        impl_->activeVirtualControllerHostId.clear();
        impl_->activeVirtualControllerHostDisplayName.clear();
    }

    impl_->logger->clear();
    impl_->stopRequested.store(false, std::memory_order_relaxed);

    const auto fallbackVirtualControllerHostInfo = [&]() -> std::optional<virtual_controller::VirtualControllerHostInfo> {
        if (!options.virtualControllerHostId.empty()) {
            return virtual_controller::findVirtualControllerHost(options.virtualControllerHostId);
        }
        return virtual_controller::defaultVirtualControllerHost();
    }();

    if (!options.virtualControllerHostId.empty() && !fallbackVirtualControllerHostInfo) {
        const std::string error = "Unknown virtual controller host \"" +
                                  options.virtualControllerHostId + "\".";
        impl_->setState(RuntimeState::Failed, error);
        impl_->logger->error(error);
        return false;
    }

    libera::core::LaserController::setTargetLatency(std::chrono::milliseconds(0));
    libera::core::LaserController::setMaxFrameHoldTime(std::chrono::milliseconds(0));

    if (selectedControllerIds.empty()) {
        impl_->logger->info("Starting Libera Link");
    } else {
        std::ostringstream oss;
        oss << "Starting Libera Link for " << selectedControllerIds.size()
            << " selected controller(s)";
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

        virtual_controller::TargetInfo targetInfo;
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

    struct PendingVirtualControllerHostGroup {
        virtual_controller::VirtualControllerHostInfo info;
        std::string instanceKey;
        std::unordered_map<std::string, std::string> options;
        std::vector<std::shared_ptr<LiberaTarget>> targets;
    };

    std::unordered_map<std::string, VirtualControllerRoute> routesByControllerId;
    routesByControllerId.reserve(options.virtualControllerRoutes.size());
    for (const auto& route : options.virtualControllerRoutes) {
        if (!route.controllerId.empty()) {
            routesByControllerId[route.controllerId] = route;
        }
    }

    std::vector<PendingVirtualControllerHostGroup> hostGroups;
    hostGroups.reserve(targets.size());
    for (const auto& target : targets) {
        const auto routeIt = routesByControllerId.find(target->targetInfo().id);
        const VirtualControllerRoute* route =
            routeIt != routesByControllerId.end() ? &routeIt->second : nullptr;

        std::optional<virtual_controller::VirtualControllerHostInfo> hostInfo;
        if (route != nullptr && !route->hostId.empty()) {
            hostInfo = virtual_controller::findVirtualControllerHost(route->hostId);
        } else {
            hostInfo = fallbackVirtualControllerHostInfo;
        }

        if (!hostInfo) {
            const std::string hostId =
                route != nullptr && !route->hostId.empty() ? route->hostId : options.virtualControllerHostId;
            const std::string error = hostId.empty()
                                          ? "No virtual controller hosts are registered."
                                          : "Unknown virtual controller host \"" + hostId + "\".";
            targets.clear();
            impl_->setState(RuntimeState::Failed, error);
            impl_->logger->error(error);
            return false;
        }

        const std::string instanceKey =
            route != nullptr && !route->hostInstanceKey.empty()
                ? route->hostInstanceKey
                : hostInfo->separateInstancePerTarget
                      ? target->targetInfo().id
                      : hostInfo->id;
        const auto& hostOptions =
            route != nullptr ? route->options : options.virtualControllerHostOptions;

        auto groupIt = std::find_if(
            hostGroups.begin(), hostGroups.end(),
            [&](const PendingVirtualControllerHostGroup& group) {
                return group.info.id == hostInfo->id &&
                       group.instanceKey == instanceKey &&
                       group.options == hostOptions;
            });
        if (groupIt == hostGroups.end()) {
            PendingVirtualControllerHostGroup group;
            group.info = *hostInfo;
            group.instanceKey = instanceKey;
            group.options = hostOptions;
            hostGroups.push_back(std::move(group));
            groupIt = std::prev(hostGroups.end());
        }
        groupIt->targets.push_back(target);
    }

    std::vector<ActiveVirtualControllerHost> activeVirtualControllerHosts;
    activeVirtualControllerHosts.reserve(hostGroups.size());
    auto failStart = [&](const std::string& error) {
        for (auto& activeHost : activeVirtualControllerHosts) {
            if (activeHost.host) {
                activeHost.host->stop();
            }
        }
        targets.clear();
        impl_->setState(RuntimeState::Failed, error);
        impl_->logger->error(error);
        return false;
    };

    for (const auto& group : hostGroups) {
        {
            std::lock_guard<std::mutex> lock(impl_->mutex);
            std::ostringstream oss;
            oss << "Starting " << group.info.displayName << "...";
            impl_->statusMessage = oss.str();
        }

        virtual_controller::VirtualControllerHostContext startContext;
        startContext.targets.reserve(group.targets.size());
        for (const auto& target : group.targets) {
            startContext.targets.push_back(virtual_controller::Target{target});
        }

        virtual_controller::VirtualControllerHostConfig virtualControllerHostConfig;
        virtualControllerHostConfig.sliceDurationUs = options.sliceDurationUs;
        virtualControllerHostConfig.options = group.options;

        std::string startupError;
        auto virtualControllerHost =
            virtual_controller::createVirtualControllerHost(group.info.id,
                                                            virtualControllerHostConfig,
                                                            startupError);
        if (!virtualControllerHost) {
            return failStart(startupError);
        }

        if (!virtualControllerHost->start(startContext, startupError)) {
            if (startupError.empty()) {
                startupError = group.info.displayName + " virtual controller host failed to start.";
            }
            return failStart(startupError);
        }

        if (impl_->stopRequested.load(std::memory_order_relaxed)) {
            virtualControllerHost->stop();
            for (auto& activeHost : activeVirtualControllerHosts) {
                if (activeHost.host) {
                    activeHost.host->stop();
                }
            }
            targets.clear();
            impl_->setState(RuntimeState::Stopped, "Stopped");
            impl_->logger->info("Link start cancelled.");
            return false;
        }

        ActiveVirtualControllerHost activeHost;
        activeHost.id = group.info.id;
        activeHost.displayName = std::string(virtualControllerHost->displayName());
        activeHost.instanceKey = group.instanceKey;
        activeHost.targets = group.targets;
        activeHost.endpoints = virtualControllerHost->endpoints();
        activeHost.host = std::move(virtualControllerHost);
        activeVirtualControllerHosts.push_back(std::move(activeHost));
    }

    std::string activeVirtualControllerHostId;
    std::string activeVirtualControllerHostDisplayName;
    if (activeVirtualControllerHosts.size() == 1) {
        activeVirtualControllerHostId = activeVirtualControllerHosts.front().id;
        activeVirtualControllerHostDisplayName = activeVirtualControllerHosts.front().displayName;
    } else if (!activeVirtualControllerHosts.empty()) {
        const bool sameHostType = std::all_of(
            activeVirtualControllerHosts.begin(), activeVirtualControllerHosts.end(),
            [&](const ActiveVirtualControllerHost& activeHost) {
                return activeHost.id == activeVirtualControllerHosts.front().id;
            });
        activeVirtualControllerHostId = sameHostType
            ? activeVirtualControllerHosts.front().id
            : "mixed";
        activeVirtualControllerHostDisplayName = sameHostType
            ? activeVirtualControllerHosts.front().displayName + " (" +
                  std::to_string(activeVirtualControllerHosts.size()) + " instances)"
            : "Multiple virtual controllers";
    }

    for (const auto& activeHost : activeVirtualControllerHosts) {
        for (const auto& target : activeHost.targets) {
            const auto* endpoint = endpointForTarget(activeHost.endpoints, target->targetInfo().id);
            std::ostringstream oss;
            oss << "Linked " << target->targetInfo().label
                << " [" << target->targetInfo().type << ":" << target->targetInfo().id << "]"
                << " via " << activeHost.displayName;
            if (endpoint != nullptr && !endpoint->label.empty()) {
                oss << " -> " << endpoint->label;
            }
            impl_->logger->info(oss.str());
        }
    }

    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->virtualControllerHosts = std::move(activeVirtualControllerHosts);
        impl_->targets = std::move(targets);
        impl_->activeVirtualControllerHostId = activeVirtualControllerHostId;
        impl_->activeVirtualControllerHostDisplayName = activeVirtualControllerHostDisplayName;
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

    std::vector<ActiveVirtualControllerHost> activeVirtualControllerHosts;
    std::vector<std::shared_ptr<LiberaTarget>> targets;
    bool hadActiveResources = false;

    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        hadActiveResources = !impl_->virtualControllerHosts.empty() ||
                             !impl_->targets.empty() ||
                             impl_->state != RuntimeState::Stopped;
        activeVirtualControllerHosts = std::move(impl_->virtualControllerHosts);
        targets = std::move(impl_->targets);
        impl_->activeVirtualControllerHostId.clear();
        impl_->activeVirtualControllerHostDisplayName.clear();
        impl_->state = RuntimeState::Stopped;
        impl_->statusMessage = "Stopped";
    }

    for (auto& activeHost : activeVirtualControllerHosts) {
        if (activeHost.host) {
            activeHost.host->stop();
        }
    }
    activeVirtualControllerHosts.clear();
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
        snapshot.activeVirtualControllerHostId = impl_->activeVirtualControllerHostId;
        snapshot.activeVirtualControllerHostDisplayName = impl_->activeVirtualControllerHostDisplayName;
        snapshot.hasDiscoveryResults = impl_->hasDiscoveryResults;
        snapshot.discoveredControllers = impl_->discoveredControllers;
        snapshot.discovered = impl_->discovered;
        snapshot.startedEndpoints = impl_->targets.size();
        snapshot.endpoints.reserve(impl_->targets.size());
        for (const auto& target : impl_->targets) {
            const auto* activeHost =
                activeHostForTarget(impl_->virtualControllerHosts, target->targetInfo().id);
            const std::string_view activeHostId =
                activeHost != nullptr ? std::string_view(activeHost->id) : std::string_view{};
            const std::string_view activeHostDisplayName =
                activeHost != nullptr ? std::string_view(activeHost->displayName) : std::string_view{};
            snapshot.endpoints.push_back(target->snapshot(
                activeHostId,
                activeHostDisplayName,
                activeHost != nullptr
                    ? endpointForTarget(activeHost->endpoints, target->targetInfo().id)
                    : nullptr));
        }
    }

    snapshot.lastError = impl_->logger->lastError();
    snapshot.recentLogs = impl_->logger->lines();
    return snapshot;
}

} // namespace libera_link
