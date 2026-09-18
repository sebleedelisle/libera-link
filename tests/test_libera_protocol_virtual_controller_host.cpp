#include "virtual_controller/LiberaProtocolVirtualControllerHost.hpp"

#include "libera/net/NetConfig.hpp"
#include "libera/liberaprotocol/LiberaProtocolController.hpp"
#include "libera/liberaprotocol/LiberaProtocolControllerInfo.hpp"
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
#include <thread>
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

    void reset() override {
        std::lock_guard<std::mutex> lock(mutex_);
        ++resets_;
        cv_.notify_all();
    }

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

    bool waitForResets(std::size_t count, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        return cv_.wait_for(lock, timeout, [&] {
            return resets_ >= count;
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
    std::size_t resets_ = 0;
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

bool waitForAvailability(vc::LiberaProtocolVirtualControllerHost& host,
                         std::string_view expected,
                         std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto endpoints = host.endpoints();
        if (!endpoints.empty() &&
            endpoints.front().attributes.at("availability") == expected) {
            return true;
        }
        std::this_thread::sleep_for(10ms);
    }
    return false;
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
    config.options["handshake_timeout_ms"] = "250";
    config.options["session_timeout_ms"] = "800";

    vc::VirtualControllerHostContext context;
    context.targets.push_back(vc::Target{sink});

    vc::LiberaProtocolVirtualControllerHost host(config);
    std::string error;
    ASSERT_TRUE(host.start(context, error), error.c_str());
    const auto endpoints = host.endpoints();
    ASSERT_EQ(endpoints.size(), 1, "one endpoint");
    ASSERT_TRUE(endpoints[0].label == "LL - Protocol test target", "endpoint label is prefixed");
    ASSERT_TRUE(endpoints[0].address == "127.0.0.1", "explicit advertised address is reported");
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
    ASSERT_TRUE((accept.featureFlags & protocol::FeatureStatus) == 0,
                "ACCEPT does not advertise unimplemented status records");
    ASSERT_EQ(accept.maxPointRate, 60000, "ACCEPT honors target maximum point rate");
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

    // Simulate a half-open client: leave its socket open but send no more
    // records. The endpoint must expire the stale session and become available.
    ASSERT_TRUE(sink->waitForResets(2, 2000ms), "inactive session resets target");
    ASSERT_TRUE(waitForAvailability(host, "available", 500ms),
                "endpoint becomes available after inactivity timeout");

    tcp::socket recoveredSocket(io);
    recoveredSocket.connect(tcp::endpoint(asio::ip::make_address("127.0.0.1"),
                                          endpoints[0].port),
                            ec);
    ASSERT_TRUE(!ec, "connect after stale session expires");
    protocol::Sender recoveredSender(2);
    hello.senderName = "recovered-protocol-host-test";
    ASSERT_TRUE(writeBytes(recoveredSocket, recoveredSender.makeHello(hello)),
                "send recovered HELLO");
    ASSERT_TRUE(readRecord(recoveredSocket, record), "read recovered ACCEPT");
    ASSERT_TRUE(record.type == protocol::RecordType::Accept, "recovered session accepted");
    ASSERT_TRUE(writeBytes(recoveredSocket, recoveredSender.makeReady()),
                "send recovered READY");

    // Frame-by-count cannot safely accept a zero count because its pending
    // point storage otherwise has no bound.
    marker.frameId = 2;
    marker.framePointCount = 0;
    ASSERT_TRUE(writeBytes(recoveredSocket, recoveredSender.makeFrameMarker(marker)),
                "send invalid zero-count frame marker");
    ASSERT_TRUE(sink->waitForResets(4, 300ms),
                "zero-count frame closes and resets session immediately");
    ASSERT_TRUE(waitForAvailability(host, "available", 300ms),
                "endpoint released after invalid frame marker");

    // The real client sends heartbeats even when it has no content callback.
    // Keeping this idle connection alive verifies both halves of the heartbeat.
    protocol::DiscoveryAdvertisement heartbeatAdvertisement;
    heartbeatAdvertisement.endpointId = endpoints[0].targetId;
    heartbeatAdvertisement.displayName = endpoints[0].label;
    heartbeatAdvertisement.address = endpoints[0].address;
    heartbeatAdvertisement.tcpPort = endpoints[0].port;
    heartbeatAdvertisement.supportedStreamModes =
        protocol::streamModeMask(protocol::StreamMode::FrameByCount);
    heartbeatAdvertisement.availability = protocol::EndpointAvailability::Available;
    heartbeatAdvertisement.maxUserChannelCount = 2;
    heartbeatAdvertisement.minPointRate = 1000;
    heartbeatAdvertisement.maxPointRate = 60000;
    heartbeatAdvertisement.maxFramePointCount = 300000;
    heartbeatAdvertisement.featureFlags = protocol::FeatureScannerSync;
    libera::liberaprotocol::LiberaProtocolControllerInfo heartbeatInfo(
        heartbeatAdvertisement,
        "127.0.0.1");
    libera::liberaprotocol::LiberaProtocolController heartbeatClient(heartbeatInfo);
    ASSERT_TRUE(static_cast<bool>(heartbeatClient.connect(heartbeatInfo)),
                "real protocol client connects for heartbeat test");
    heartbeatClient.startThread();
    std::this_thread::sleep_for(1800ms);
    ASSERT_TRUE(waitForAvailability(host, "busy", 200ms),
                "heartbeats keep an otherwise idle session alive");
    heartbeatClient.stopThread();
    heartbeatClient.close();
    ASSERT_TRUE(waitForAvailability(host, "available", 500ms),
                "orderly client close releases heartbeat session");

    host.stop();

    vc::VirtualControllerHostConfig automaticAddressConfig;
    automaticAddressConfig.options["listen_address"] = "0.0.0.0";
    automaticAddressConfig.options["tcp_port"] = std::to_string(freeTcpPort());
    automaticAddressConfig.options["discovery"] = "false";
    vc::LiberaProtocolVirtualControllerHost automaticAddressHost(automaticAddressConfig);
    ASSERT_TRUE(automaticAddressHost.start(context, error), error.c_str());
    const auto automaticAddressEndpoints = automaticAddressHost.endpoints();
    ASSERT_EQ(automaticAddressEndpoints.size(), 1, "one automatic-address endpoint");
    ASSERT_TRUE(automaticAddressEndpoints[0].address.empty(),
                "wildcard bind is not reported as a reachable address");
    ASSERT_TRUE(automaticAddressEndpoints[0].value.find("UDP source address") !=
                    std::string::npos,
                "automatic address explains discovery-source behavior");
    automaticAddressHost.stop();

    vc::VirtualControllerHostConfig invalidPortConfig;
    invalidPortConfig.options["listen_address"] = "127.0.0.1";
    invalidPortConfig.options["tcp_port"] = "0";
    invalidPortConfig.options["discovery"] = "false";
    vc::LiberaProtocolVirtualControllerHost invalidPortHost(invalidPortConfig);
    error.clear();
    ASSERT_TRUE(!invalidPortHost.start(context, error), "zero TCP base port is rejected");
    ASSERT_TRUE(error.find("between 1 and 65535") != std::string::npos,
                "invalid port reports an actionable error");

    if (g_failures == 0) {
        std::printf("Libera Protocol virtual controller host tests passed.\n");
    }
    return g_failures == 0 ? 0 : 1;
}
