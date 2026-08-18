#include "virtual_controller/LiberaProtocolVirtualControllerHost.hpp"

#include "libera/net/NetConfig.hpp"
#include "libera/protocol/Codec.hpp"
#include "libera/protocol/Sender.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

using namespace std::chrono_literals;

namespace {

namespace vc = libera_link::virtual_controller;
namespace protocol = libera::protocol;
using libera::net::tcp;
namespace asio = libera::net::asio;

int g_failures = 0;

#define ASSERT_TRUE(cond, msg) \
    do { if (!(cond)) { std::fprintf(stderr, "ASSERT TRUE FAILED: %s @ %s:%d\n", (msg), __FILE__, __LINE__); ++g_failures; } } while (0)

#define ASSERT_EQ(a, b, msg) \
    do { auto _va = (a); auto _vb = (b); if (!((_va) == (_vb))) { \
        std::fprintf(stderr, "ASSERT EQ FAILED: %s lhs=%lld rhs=%lld @ %s:%d\n", \
                     (msg), static_cast<long long>(_va), static_cast<long long>(_vb), __FILE__, __LINE__); \
        ++g_failures; \
    } } while (0)

class RecordingSink final : public vc::TargetSink {
public:
    RecordingSink() {
        info_.id = "protocol-test-target";
        info_.label = "Protocol test target";
        info_.type = "test";
        info_.maxPointRate = 60000;
    }

    const vc::TargetInfo& targetInfo() const override {
        return info_;
    }

    vc::SubmissionResult submitContinuous(vc::SliceSubmission submission) override {
        std::lock_guard<std::mutex> lock(mutex_);
        continuousPoints_ += submission.points.size();
        cv_.notify_all();
        return result(true, submission.points.size(), 0);
    }

    vc::SubmissionResult replaceFrame(vc::FrameSubmission submission) override {
        return submitFrame(std::move(submission));
    }

    vc::SubmissionResult submitFrame(vc::FrameSubmission submission) override {
        std::lock_guard<std::mutex> lock(mutex_);
        lastFrame_.clear();
        std::size_t count = 0;
        for (auto& slice : submission.slices) {
            count += slice.points.size();
            lastFrame_.insert(lastFrame_.end(), slice.points.begin(), slice.points.end());
        }
        ++frames_;
        cv_.notify_all();
        return result(true, count, 0);
    }

    void setScannerSync(std::int64_t offsetNs, bool enabled) override {
        std::lock_guard<std::mutex> lock(mutex_);
        lastScannerSyncOffsetNs_ = offsetNs;
        lastScannerSyncEnabled_ = enabled;
        ++scannerSyncUpdates_;
        cv_.notify_all();
    }

    vc::TargetStatus status() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        vc::TargetStatus status;
        status.receivedPoints = continuousPoints_ + lastFrame_.size();
        return status;
    }

    void reset() override {}

    bool waitForFrames(std::size_t count, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        return cv_.wait_for(lock, timeout, [&] {
            return frames_ >= count;
        });
    }

    bool waitForScannerSync(std::size_t count, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        return cv_.wait_for(lock, timeout, [&] {
            return scannerSyncUpdates_ >= count;
        });
    }

    std::size_t frames() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return frames_;
    }

    std::size_t continuousPoints() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return continuousPoints_;
    }

    std::vector<libera::core::LaserPoint> lastFrame() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return lastFrame_;
    }

    std::int64_t lastScannerSyncOffsetNs() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return lastScannerSyncOffsetNs_;
    }

    bool lastScannerSyncEnabled() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return lastScannerSyncEnabled_;
    }

private:
    vc::SubmissionResult result(bool accepted,
                                std::size_t submitted,
                                std::size_t dropped) const {
        vc::SubmissionResult value;
        value.accepted = accepted;
        value.submittedPoints = submitted;
        value.acceptedPoints = accepted ? submitted - std::min(submitted, dropped) : 0;
        value.droppedPoints = dropped;
        value.status.receivedPoints = continuousPoints_ + lastFrame_.size();
        return value;
    }

    vc::TargetInfo info_;
    mutable std::mutex mutex_;
    mutable std::condition_variable cv_;
    std::size_t frames_ = 0;
    std::size_t continuousPoints_ = 0;
    std::size_t scannerSyncUpdates_ = 0;
    std::int64_t lastScannerSyncOffsetNs_ = 0;
    bool lastScannerSyncEnabled_ = false;
    std::vector<libera::core::LaserPoint> lastFrame_;
};

std::uint16_t freeTcpPort() {
    asio::io_context io;
    tcp::acceptor acceptor(io, tcp::endpoint(tcp::v4(), 0));
    return acceptor.local_endpoint().port();
}

bool writeBytes(tcp::socket& socket, const std::vector<std::uint8_t>& bytes) {
    std::error_code ec;
    asio::write(socket, asio::buffer(bytes), ec);
    return !ec;
}

bool readRecord(tcp::socket& socket, protocol::Record& record) {
    std::array<std::uint8_t, protocol::RECORD_HEADER_SIZE> headerBytes{};
    std::error_code ec;
    asio::read(socket, asio::buffer(headerBytes), ec);
    if (ec) {
        return false;
    }
    protocol::RecordHeader header;
    std::string error;
    if (!protocol::decodeRecordHeader(headerBytes.data(), headerBytes.size(), header, error)) {
        return false;
    }
    std::vector<std::uint8_t> payload(header.payloadSize);
    if (!payload.empty()) {
        asio::read(socket, asio::buffer(payload), ec);
        if (ec) {
            return false;
        }
    }
    record.type = header.type;
    record.flags = header.flags;
    record.sequence = header.sequence;
    record.payload = std::move(payload);
    return true;
}

} // namespace

int main() {
    vc::ensureBuiltInLiberaProtocolVirtualControllerHostLinked();

    const auto sink = std::make_shared<RecordingSink>();
    vc::VirtualControllerHostConfig config;
    config.options["listen_address"] = "127.0.0.1";
    config.options["advertised_address"] = "127.0.0.1";
    config.options["tcp_port"] = std::to_string(freeTcpPort());
    config.options["discovery"] = "false";

    vc::VirtualControllerHostContext context;
    context.targets.push_back(vc::Target{sink});

    vc::LiberaProtocolVirtualControllerHost host(config);
    std::string error;
    ASSERT_TRUE(host.start(context, error), error.c_str());
    const auto endpoints = host.endpoints();
    ASSERT_EQ(endpoints.size(), 1, "one endpoint");
    ASSERT_TRUE(endpoints[0].label == "LL - Protocol test target", "endpoint label is prefixed");
    ASSERT_TRUE(endpoints[0].attributes.at("availability") == "available",
                "endpoint starts available");

    asio::io_context io;
    tcp::socket socket(io);
    std::error_code ec;
    socket.connect(tcp::endpoint(asio::ip::make_address("127.0.0.1"), endpoints[0].port), ec);
    ASSERT_TRUE(!ec, "connect to protocol host");

    protocol::Sender sender(2);
    protocol::Hello hello;
    hello.senderName = "protocol-host-test";
    hello.requestedStreamMode = protocol::StreamMode::FrameByCount;
    hello.requestedUserChannelCount = 2;
    hello.defaultPointRate = 30000;
    ASSERT_TRUE(writeBytes(socket, sender.makeHello(hello)), "send HELLO");

    protocol::Record record;
    ASSERT_TRUE(readRecord(socket, record), "read ACCEPT");
    ASSERT_TRUE(record.type == protocol::RecordType::Accept, "ACCEPT record type");
    protocol::Accept accept;
    ASSERT_TRUE(protocol::decodeAccept(record.payload.data(), record.payload.size(), accept, error),
                "decode ACCEPT");
    ASSERT_TRUE(accept.acceptedStreamMode == protocol::StreamMode::FrameByCount,
                "accepted frame-by-count");
    ASSERT_TRUE((accept.featureFlags & protocol::FeatureScannerSync) != 0,
                "ACCEPT advertises scanner sync");
    sender.setUserChannelCount(accept.acceptedUserChannelCount);
    ASSERT_TRUE(writeBytes(socket, sender.makeReady()), "send READY");

    const auto busyEndpoints = host.endpoints();
    ASSERT_EQ(busyEndpoints.size(), 1, "one busy endpoint");
    ASSERT_TRUE(busyEndpoints[0].attributes.at("availability") == "busy",
                "endpoint becomes busy after accepted session");

    tcp::socket secondSocket(io);
    secondSocket.connect(tcp::endpoint(asio::ip::make_address("127.0.0.1"),
                                       endpoints[0].port),
                         ec);
    ASSERT_TRUE(!ec, "connect second protocol client");
    protocol::Sender secondSender(2);
    hello.senderName = "second-protocol-host-test";
    ASSERT_TRUE(writeBytes(secondSocket, secondSender.makeHello(hello)), "send second HELLO");
    ASSERT_TRUE(readRecord(secondSocket, record), "read second response");
    ASSERT_TRUE(record.type == protocol::RecordType::Reject, "second client is rejected");
    protocol::Reject reject;
    ASSERT_TRUE(protocol::decodeReject(record.payload.data(), record.payload.size(), reject, error),
                "decode reject");
    ASSERT_TRUE(reject.code == protocol::RejectCode::Busy, "busy reject code");

    protocol::StreamConfig streamConfig;
    streamConfig.defaultPointRate = 30000;
    streamConfig.streamMode = protocol::StreamMode::FrameByCount;
    streamConfig.userChannelCount = accept.acceptedUserChannelCount;
    ASSERT_TRUE(writeBytes(socket, sender.makeStreamConfig(streamConfig)), "send stream config");

    protocol::ScannerSync scannerSync;
    scannerSync.offsetNs = 225000;
    scannerSync.enabled = true;
    ASSERT_TRUE(writeBytes(socket, sender.makeScannerSync(scannerSync)), "send scanner sync");
    ASSERT_TRUE(sink->waitForScannerSync(1, 1000ms), "scanner sync applied");
    ASSERT_EQ(sink->lastScannerSyncOffsetNs(), 225000, "scanner sync offset");
    ASSERT_TRUE(sink->lastScannerSyncEnabled(), "scanner sync enabled");

    protocol::FrameMarker marker;
    marker.frameId = 1;
    marker.pointRate = 30000;
    marker.framePointCount = 3;
    ASSERT_TRUE(writeBytes(socket, sender.makeFrameMarker(marker)), "send frame marker");

    protocol::PointSample a;
    a.x = -32768;
    a.r = 65535;
    a.i = 65535;
    a.user = {100, 200};
    protocol::PointSample b;
    b.y = 32767;
    b.g = 65535;
    b.i = 65535;
    b.user = {300, 400};
    protocol::PointSample c;
    c.x = 1000;
    c.y = -1000;
    c.b = 65535;
    c.i = 65535;
    c.user = {500, 600};
    ASSERT_TRUE(writeBytes(socket, sender.makePoints({a, b, c})), "send points");

    ASSERT_TRUE(sink->waitForFrames(1, 1000ms), "one complete frame submitted");
    ASSERT_EQ(sink->frames(), 1, "frame count");
    ASSERT_EQ(sink->continuousPoints(), 0, "no continuous fallback");
    const auto frame = sink->lastFrame();
    ASSERT_EQ(frame.size(), 3, "frame point count");
    ASSERT_TRUE(frame[0].x == -1.0f, "first point x");
    ASSERT_TRUE(frame[1].y == 1.0f, "second point y");
    ASSERT_TRUE(frame[2].b > 0.99f, "third point blue");

    host.stop();
    if (g_failures == 0) {
        std::printf("Libera Protocol virtual controller host tests passed.\n");
    }
    return g_failures == 0 ? 0 : 1;
}
