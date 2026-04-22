#include "ingest/IdnIngester.hpp"

#include "ingest/IngesterRegistry.hpp"

#include "output/V1LaproGraphOut.hpp"
#include "server/IDNLaproService.hpp"
#include "shared/DACHWInterface.hpp"
#include "stage/SockIDNServer.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

namespace libera_link::ingest {
namespace {

using libera::core::LaserPoint;
using namespace std::chrono_literals;

IngesterRegistrar gIdnIngesterRegistrar({
    {
        "idn",
        "OpenIDN",
        "Expose bridged controllers as OpenIDN / IDN services.",
        true,
    },
    [](const FactoryConfig& config) {
        return std::make_unique<IdnIngester>(config.sliceDurationUs);
    },
});

class IdnTransportAdapter final : public DACHWInterface {
public:
    explicit IdnTransportAdapter(std::shared_ptr<TargetSink> sink)
        : sink_(std::move(sink))
        , displayName_(sink_ ? sink_->targetInfo().label : std::string{})
        , maxPointRateValue_(
              sink_ && sink_->targetInfo().maxPointRate > 0
                  ? sink_->targetInfo().maxPointRate
                  : 100000u) {}

    int writeFrame(const TimeSlice& slice, double durationUs) override {
        if (!sink_) {
            return -1;
        }
        sink_->submitContinuous(makeSliceSubmission(slice, durationUs));
        return 0;
    }

    void replaceFrameBuffer(const SliceBuf& buffer, bool clearTransportPrefetch) {
        if (!sink_) {
            return;
        }
        sink_->replaceFrame(makeFrameSubmission(buffer, clearTransportPrefetch));
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
        maxPointRateValue_ = std::max<unsigned>(rate, 1000u);
    }

    void getName(char* nameBufferPtr, unsigned nameBufferSize) override {
        if (nameBufferPtr == nullptr || nameBufferSize == 0) {
            return;
        }
        std::snprintf(nameBufferPtr, nameBufferSize, "%s", displayName_.c_str());
    }

private:
    static std::optional<std::uint32_t> inferPointRate(std::size_t pointCount, double durationUs) {
        if (pointCount == 0 || durationUs <= 0.0) {
            return std::nullopt;
        }

        const double inferredRate =
            (1000000.0 * static_cast<double>(pointCount)) / durationUs;
        if (!std::isfinite(inferredRate) || inferredRate <= 0.0) {
            return std::nullopt;
        }

        return static_cast<std::uint32_t>(std::llround(inferredRate));
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

    static SliceSubmission makeSliceSubmission(const TimeSlice& slice, double durationUs) {
        SliceSubmission submission;
        submission.durationUs = durationUs > 0.0
            ? static_cast<std::uint32_t>(std::llround(durationUs))
            : slice.durationUs;

        const auto pointCount = slice.dataChunk.size() / sizeof(ISPDB25Point);
        submission.points.reserve(pointCount);

        const auto* points =
            reinterpret_cast<const ISPDB25Point*>(slice.dataChunk.data());
        for (std::size_t i = 0; i < pointCount; ++i) {
            submission.points.push_back(toLaserPoint(points[i]));
        }

        submission.effectivePointRate = inferPointRate(pointCount, durationUs);
        return submission;
    }

    static FrameSubmission makeFrameSubmission(const SliceBuf& buffer,
                                               bool clearTransportPrefetch) {
        FrameSubmission submission;
        submission.clearTransportPrefetch = clearTransportPrefetch;
        submission.slices.reserve(buffer.size());

        for (const auto& slice : buffer) {
            if (!slice) {
                continue;
            }
            submission.slices.push_back(
                makeSliceSubmission(*slice, static_cast<double>(slice->durationUs)));
        }

        return submission;
    }

    std::shared_ptr<TargetSink> sink_;
    std::string displayName_;
    unsigned maxPointRateValue_ = 100000u;
};

class IdnTargetSession {
public:
    IdnTargetSession(std::shared_ptr<TargetSink> sink,
                     std::uint8_t serviceId,
                     std::uint32_t sliceDurationUs)
        : sink_(std::move(sink))
        , serviceId_(serviceId)
        , sliceDurationUs_(sliceDurationUs)
        , adapter_(std::make_shared<IdnTransportAdapter>(sink_))
        , output_(std::make_unique<V1LaproGraphicOutput>(adapter_)) {
        const std::string bridgedServiceName =
            std::string("Libera Link ") + sink_->targetInfo().label;
        std::vector<char> serviceName(bridgedServiceName.begin(), bridgedServiceName.end());
        serviceName.push_back('\0');
        const bool isDefault = (serviceId_ == 1);
        service_ = std::make_unique<IDNLaproService>(
            serviceId_,
            serviceName.data(),
            isDefault,
            output_.get());
    }

    ~IdnTargetSession() {
        stop();
    }

    void start() {
        if (running_.exchange(true, std::memory_order_relaxed)) {
            return;
        }
        sink_->reset();
        driverThread_ = std::thread([this] { driverLoop(); });
    }

    void stop() {
        if (!running_.exchange(false, std::memory_order_relaxed)) {
            return;
        }
        if (driverThread_.joinable()) {
            driverThread_.join();
        }
        sink_->reset();
    }

    void linkService(LLNode<ServiceNode>** firstService) {
        if (!service_ || !firstService) {
            return;
        }
        service_->linkinLast(firstService);
    }

    std::uint8_t serviceId() const {
        return serviceId_;
    }

    const std::string& targetId() const {
        return sink_->targetInfo().id;
    }

private:
    static std::chrono::microseconds bufferReplayInterval(const SliceBuf& buffer) {
        auto total = 0us;
        for (const auto& slice : buffer) {
            if (!slice) {
                continue;
            }
            total += std::chrono::microseconds(slice->durationUs);
        }
        return total;
    }

    void driverLoop() {
        TransformEnv tfEnv;
        tfEnv.usPerSlice = static_cast<double>(sliceDurationUs_);
        tfEnv.currentSliceTime = tfEnv.usPerSlice;

        unsigned driverMode = DRIVER_INACTIVE;
        auto currentBuffer = std::make_shared<SliceBuf>();
        auto nextFrameReplayAt = std::chrono::steady_clock::time_point{};

        while (running_.load(std::memory_order_relaxed)) {
            bool hasFreshFrameInput = false;
            auto nextBuffer = adapter_->getNextBuffer(tfEnv, driverMode);
            if (nextBuffer && !nextBuffer->empty()) {
                currentBuffer = nextBuffer;
                if (driverMode == DRIVER_FRAMEMODE) {
                    nextFrameReplayAt = std::chrono::steady_clock::now();
                    hasFreshFrameInput = true;
                }
            }

            if (!currentBuffer || currentBuffer->empty()) {
                std::this_thread::sleep_for(1ms);
                continue;
            }

            if (driverMode == DRIVER_FRAMEMODE &&
                nextFrameReplayAt != std::chrono::steady_clock::time_point{}) {
                const auto now = std::chrono::steady_clock::now();
                if (now < nextFrameReplayAt) {
                    const auto sleepFor = std::min(
                        1ms,
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            nextFrameReplayAt - now));
                    if (sleepFor > 0ms) {
                        std::this_thread::sleep_for(sleepFor);
                    }
                    continue;
                }
            }

            if (driverMode == DRIVER_FRAMEMODE) {
                adapter_->replaceFrameBuffer(*currentBuffer, hasFreshFrameInput);
                auto replayInterval = bufferReplayInterval(*currentBuffer);
                if (replayInterval <= 0us) {
                    replayInterval = 1ms;
                }
                nextFrameReplayAt = std::chrono::steady_clock::now() + replayInterval;
                continue;
            }

            const std::size_t iterationCount = currentBuffer->size();
            for (std::size_t i = 0;
                 i < iterationCount && running_.load(std::memory_order_relaxed);
                 ++i) {
                auto nextSlice = currentBuffer->front();
                currentBuffer->pop_front();

                if (!nextSlice) {
                    continue;
                }

                adapter_->writeFrame(
                    *nextSlice,
                    static_cast<double>(nextSlice->durationUs));
            }
            nextFrameReplayAt = std::chrono::steady_clock::time_point{};
        }
    }

    std::shared_ptr<TargetSink> sink_;
    std::uint8_t serviceId_ = 0;
    std::uint32_t sliceDurationUs_ = 0;
    std::shared_ptr<IdnTransportAdapter> adapter_;
    std::unique_ptr<V1LaproGraphicOutput> output_;
    std::unique_ptr<IDNLaproService> service_;
    std::thread driverThread_;
    std::atomic<bool> running_{false};
};

void stopSessions(std::vector<std::unique_ptr<IdnTargetSession>>& sessions) {
    for (auto& session : sessions) {
        session->stop();
    }
    sessions.clear();
}

} // namespace

void ensureBuiltInIdnIngesterLinked() {}

struct IdnIngester::Impl {
    std::unique_ptr<SockIDNServer> server;
    std::thread serverThread;
    std::vector<std::unique_ptr<IdnTargetSession>> sessions;
    std::atomic<bool> active{false};
};

IdnIngester::IdnIngester(std::uint32_t sliceDurationUs)
    : impl_(std::make_unique<Impl>())
    , sliceDurationUs_(sliceDurationUs) {}

IdnIngester::~IdnIngester() {
    stop();
}

std::string_view IdnIngester::name() const {
    return "idn";
}

std::string_view IdnIngester::displayName() const {
    return "OpenIDN";
}

bool IdnIngester::start(const StartContext& context, std::string& error) {
    if (!impl_) {
        error = "IDN ingester not initialized.";
        return false;
    }
    if (running()) {
        error = "IDN ingester is already running.";
        return false;
    }

    bindings_.clear();

    LLNode<ServiceNode>* firstService = nullptr;
    std::vector<std::unique_ptr<IdnTargetSession>> sessions;
    sessions.reserve(context.targets.size());

    std::size_t startedTargets = 0;
    for (const auto& target : context.targets) {
        if (!target.sink) {
            continue;
        }

        const auto serviceIdRaw = startedTargets + 1;
        if (serviceIdRaw > 255) {
            error = "IDN service ID range exceeded.";
            stopSessions(sessions);
            return false;
        }

        auto session = std::make_unique<IdnTargetSession>(
            target.sink,
            static_cast<std::uint8_t>(serviceIdRaw),
            sliceDurationUs_);
        session->linkService(&firstService);
        session->start();

        BindingInfo binding;
        binding.targetId = target.sink->targetInfo().id;
        binding.label = "IDN service " + std::to_string(serviceIdRaw);
        binding.value = std::to_string(serviceIdRaw);
        bindings_.push_back(std::move(binding));
        sessions.push_back(std::move(session));
        ++startedTargets;
    }

    if (sessions.empty()) {
        error = "No IDN targets were created.";
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
        stopSessions(sessions);
        bindings_.clear();
        error = startupError;
        return false;
    }

    impl_->server = std::move(server);
    impl_->serverThread = std::move(serverThread);
    impl_->sessions = std::move(sessions);
    impl_->active.store(true, std::memory_order_relaxed);
    return true;
}

void IdnIngester::stop() {
    if (!impl_) {
        return;
    }

    auto server = std::move(impl_->server);
    auto serverThread = std::move(impl_->serverThread);
    auto sessions = std::move(impl_->sessions);
    impl_->active.store(false, std::memory_order_relaxed);

    if (server) {
        server->stopServer();
    }
    if (serverThread.joinable()) {
        serverThread.join();
    }

    stopSessions(sessions);
    bindings_.clear();
}

bool IdnIngester::running() const {
    return impl_ && impl_->active.load(std::memory_order_relaxed);
}

std::vector<BindingInfo> IdnIngester::bindings() const {
    return bindings_;
}

} // namespace libera_link::ingest
