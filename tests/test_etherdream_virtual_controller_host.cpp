#include "virtual_controller/EtherDreamVirtualControllerHost.hpp"
#include "virtual_controller/VirtualControllerHostRegistry.hpp"

#include "libera/core/ByteBuffer.hpp"
#include "libera/core/ByteRead.hpp"
#include "libera/etherdream/EtherDreamResponse.hpp"
#include "libera/net/NetConfig.hpp"
#include "libera/net/TcpClient.hpp"
#include "libera/net/UdpSocket.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

using namespace std::chrono_literals;

namespace {

int g_failures = 0;

#define ASSERT_TRUE(cond, msg) \
    do { if (!(cond)) { std::fprintf(stderr, "ASSERT TRUE FAILED: %s @ %s:%d\n", (msg), __FILE__, __LINE__); ++g_failures; } } while (0)

#define ASSERT_EQ(a, b, msg) \
    do { auto _va = (a); auto _vb = (b); if (!((_va) == (_vb))) { \
        std::fprintf(stderr, "ASSERT EQ FAILED: %s lhs=%lld rhs=%lld @ %s:%d\n", \
                     (msg), static_cast<long long>(_va), static_cast<long long>(_vb), __FILE__, __LINE__); \
        ++g_failures; \
    } } while (0)

class RecordingSink final : public libera_link::virtual_controller::TargetSink {
public:
    explicit RecordingSink(std::string id) {
        info_.id = std::move(id);
        info_.label = "Recording target";
        info_.type = "test";
        info_.maxPointRate = 100000;
    }

    const libera_link::virtual_controller::TargetInfo& targetInfo() const override {
        return info_;
    }

    libera_link::virtual_controller::SubmissionResult
    submitContinuous(libera_link::virtual_controller::SliceSubmission submission) override {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto count = submission.points.size();
        received_.insert(received_.end(), submission.points.begin(), submission.points.end());
        observedRate_ = submission.effectivePointRate.value_or(0);
        cv_.notify_all();
        return result(true, count, 0);
    }

    libera_link::virtual_controller::SubmissionResult
    replaceFrame(libera_link::virtual_controller::FrameSubmission submission) override {
        std::lock_guard<std::mutex> lock(mutex_);
        std::size_t count = 0;
        received_.clear();
        for (auto& slice : submission.slices) {
            count += slice.points.size();
            received_.insert(received_.end(), slice.points.begin(), slice.points.end());
        }
        cv_.notify_all();
        return result(true, count, 0);
    }

    libera_link::virtual_controller::SubmissionResult
    submitFrame(libera_link::virtual_controller::FrameSubmission submission) override {
        return replaceFrame(std::move(submission));
    }

    libera_link::virtual_controller::TargetStatus status() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        libera_link::virtual_controller::TargetStatus status;
        status.outputPointRate = observedRate_;
        status.observedInputPointRate = observedRate_;
        status.receivedPoints = received_.size();
        return status;
    }

    void reset() override {
        std::lock_guard<std::mutex> lock(mutex_);
        received_.clear();
    }

    bool waitForPoints(std::size_t count, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        return cv_.wait_for(lock, timeout, [&] {
            return received_.size() >= count;
        });
    }

    std::vector<libera::core::LaserPoint> points() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return received_;
    }

private:
    libera_link::virtual_controller::SubmissionResult result(bool accepted,
                                                            std::size_t submitted,
                                                            std::size_t dropped) const {
        libera_link::virtual_controller::SubmissionResult value;
        value.accepted = accepted;
        value.submittedPoints = submitted;
        value.acceptedPoints = accepted ? submitted - std::min(submitted, dropped) : 0;
        value.droppedPoints = dropped;
        value.status.outputPointRate = observedRate_;
        value.status.observedInputPointRate = observedRate_;
        value.status.receivedPoints = received_.size();
        return value;
    }

    libera_link::virtual_controller::TargetInfo info_;
    mutable std::mutex mutex_;
    mutable std::condition_variable cv_;
    std::vector<libera::core::LaserPoint> received_;
    std::uint32_t observedRate_ = 0;
};

std::unique_ptr<libera_link::virtual_controller::VirtualControllerHost>
createHost(std::uint16_t tcpPort,
           std::uint16_t discoveryPort,
           bool discovery,
           const std::shared_ptr<RecordingSink>& sink,
           std::string& error) {
    libera_link::virtual_controller::ensureBuiltInEtherDreamVirtualControllerHostLinked();

    libera_link::virtual_controller::VirtualControllerHostConfig config;
    config.sliceDurationUs = 1000;
    config.options["addresses"] = "127.0.0.1";
    config.options["tcp_port"] = std::to_string(tcpPort);
    config.options["discovery_port"] = std::to_string(discoveryPort);
    config.options["discovery"] = discovery ? "true" : "false";
    config.options["broadcast_addresses"] = "127.0.0.1";
    config.options["broadcast_interval_ms"] = "50";
    config.options["buffer_capacity"] = "64";
    config.options["playback_slice_us"] = "1000";

    auto host = libera_link::virtual_controller::createVirtualControllerHost(
        "etherdream", config, error);
    if (!host) {
        return nullptr;
    }

    libera_link::virtual_controller::VirtualControllerHostContext context;
    context.targets.push_back(libera_link::virtual_controller::Target{sink});
    if (!host->start(context, error)) {
        return nullptr;
    }
    return host;
}

bool sendBuffer(libera::net::TcpClient& client, const libera::core::ByteBuffer& buffer) {
    return !client.write_all(buffer.data(), buffer.size());
}

bool sendByte(libera::net::TcpClient& client, char command) {
    const auto byte = static_cast<std::uint8_t>(command);
    return !client.write_all(&byte, 1);
}

bool readResponse(libera::net::TcpClient& client,
                  libera::etherdream::EtherDreamResponse& response) {
    std::uint8_t raw[22]{};
    std::size_t transferred = 0;
    if (client.read_exact(raw, sizeof(raw), &transferred)) {
        return false;
    }
    return transferred == sizeof(raw) && response.decode(raw, sizeof(raw));
}

libera::core::ByteBuffer dataCommand(std::uint16_t pointCount) {
    libera::core::ByteBuffer buffer;
    buffer.appendChar('d');
    buffer.appendUInt16(pointCount);
    for (std::uint16_t i = 0; i < pointCount; ++i) {
        buffer.appendUInt16(0);
        buffer.appendInt16(16384);
        buffer.appendInt16(-16384);
        buffer.appendUInt16(65535);
        buffer.appendUInt16(32768);
        buffer.appendUInt16(0);
        buffer.appendUInt16(0);
        buffer.appendUInt16(0);
        buffer.appendUInt16(0);
    }
    return buffer;
}

libera::core::ByteBuffer beginCommand(std::uint32_t pointRate) {
    libera::core::ByteBuffer buffer;
    buffer.appendChar('b');
    buffer.appendUInt16(0);
    buffer.appendUInt32(pointRate);
    return buffer;
}

void testProtocolStreaming() {
    constexpr std::uint16_t tcpPort = 19765;
    constexpr std::uint16_t discoveryPort = 17654;
    auto sink = std::make_shared<RecordingSink>("target-a");
    std::string error;
    auto host = createHost(tcpPort, discoveryPort, false, sink, error);
    ASSERT_TRUE(host != nullptr, error.empty() ? "host should start" : error.c_str());
    if (!host) {
        return;
    }

    libera::net::TcpClient client;
    client.setDefaultTimeout(500ms);
    client.setConnectTimeout(500ms);
    const auto endpoint = libera::net::tcp::endpoint(
        libera::net::asio::ip::make_address("127.0.0.1"), tcpPort);
    ASSERT_TRUE(!client.connect(endpoint), "client connects to virtual Ether Dream");

    libera::etherdream::EtherDreamResponse response;
    ASSERT_TRUE(readResponse(client, response), "initial response");
    ASSERT_EQ(response.response, static_cast<std::uint8_t>('a'), "initial ack");
    ASSERT_EQ(response.command, static_cast<std::uint8_t>('?'), "initial command");
    ASSERT_EQ(static_cast<int>(response.status.playbackState), 0, "initial idle");

    ASSERT_TRUE(sendByte(client, 'p'), "send prepare");
    ASSERT_TRUE(readResponse(client, response), "prepare response");
    ASSERT_EQ(response.response, static_cast<std::uint8_t>('a'), "prepare ack");
    ASSERT_EQ(static_cast<int>(response.status.playbackState), 1, "prepared state");

    auto data = dataCommand(50);
    ASSERT_TRUE(sendBuffer(client, data), "send data");
    ASSERT_TRUE(readResponse(client, response), "data response");
    ASSERT_EQ(response.response, static_cast<std::uint8_t>('a'), "data ack");
    ASSERT_EQ(response.status.bufferFullness, static_cast<std::uint16_t>(50), "buffer fullness after data");

    auto begin = beginCommand(30000);
    ASSERT_TRUE(sendBuffer(client, begin), "send begin");
    ASSERT_TRUE(readResponse(client, response), "begin response");
    ASSERT_EQ(response.response, static_cast<std::uint8_t>('a'), "begin ack");
    ASSERT_EQ(static_cast<int>(response.status.playbackState), 2, "playing state");

    ASSERT_TRUE(sink->waitForPoints(20, 1000ms), "playback should submit points to target sink");
    const auto points = sink->points();
    ASSERT_TRUE(!points.empty(), "received point list is non-empty");
    if (!points.empty()) {
        ASSERT_TRUE(std::fabs(points.front().x - 0.5f) < 0.01f, "x decoded");
        ASSERT_TRUE(std::fabs(points.front().y + 0.5f) < 0.01f, "y decoded");
        ASSERT_TRUE(std::fabs(points.front().r - 1.0f) < 0.01f, "red decoded");
        ASSERT_TRUE(std::fabs(points.front().g - 0.5f) < 0.01f, "green decoded");
        ASSERT_TRUE(points.front().i > 0.9f, "RGB-lit point gets non-zero intensity");
    }

    host->stop();
}

void testNaksAndBroadcast() {
    constexpr std::uint16_t tcpPort = 19766;
    constexpr std::uint16_t discoveryPort = 17655;

    auto io = libera::net::shared_io_context();
    libera::net::UdpSocket udp(*io);
    ASSERT_TRUE(!udp.open_v4(false), "open UDP listener");
    ASSERT_TRUE(!udp.bind_any(discoveryPort, false), "bind UDP discovery listener");

    auto sink = std::make_shared<RecordingSink>("target-b");
    std::string error;
    auto host = createHost(tcpPort, discoveryPort, true, sink, error);
    ASSERT_TRUE(host != nullptr, error.empty() ? "host should start" : error.c_str());
    if (!host) {
        return;
    }

    std::array<std::uint8_t, 128> packet{};
    libera::net::udp::endpoint sender;
    std::size_t received = 0;
    ASSERT_TRUE(!udp.recv_from(packet.data(), packet.size(), sender, received, 1000ms, false),
                "receive discovery broadcast");
    ASSERT_TRUE(received >= 36, "discovery packet size");
    ASSERT_EQ(libera::core::bytes::readLe16(packet.data() + 6), static_cast<std::uint16_t>(0),
              "virtual hardware revision");
    ASSERT_EQ(libera::core::bytes::readLe16(packet.data() + 8),
              static_cast<std::uint16_t>(tcpPort - 7765),
              "software revision carries test port offset");

    libera::net::TcpClient client;
    client.setDefaultTimeout(500ms);
    client.setConnectTimeout(500ms);
    const auto endpoint = libera::net::tcp::endpoint(
        libera::net::asio::ip::make_address("127.0.0.1"), tcpPort);
    ASSERT_TRUE(!client.connect(endpoint), "client connects");

    libera::etherdream::EtherDreamResponse response;
    ASSERT_TRUE(readResponse(client, response), "initial response");
    ASSERT_TRUE(sendByte(client, 'p'), "send prepare");
    ASSERT_TRUE(readResponse(client, response), "prepare response");

    auto tooLarge = dataCommand(65);
    ASSERT_TRUE(sendBuffer(client, tooLarge), "send too-large data");
    ASSERT_TRUE(readResponse(client, response), "too-large data response");
    ASSERT_EQ(response.response, static_cast<std::uint8_t>('F'), "buffer full NAK");
    ASSERT_EQ(response.status.bufferFullness, static_cast<std::uint16_t>(0), "full NAK does not enqueue");

    ASSERT_TRUE(sendByte(client, 's'), "send stop");
    ASSERT_TRUE(readResponse(client, response), "stop response");
    ASSERT_EQ(response.response, static_cast<std::uint8_t>('a'), "stop ack from prepared");
    ASSERT_EQ(static_cast<int>(response.status.playbackState), 0, "idle after stop");

    auto zeroData = dataCommand(0);
    ASSERT_TRUE(sendBuffer(client, zeroData), "send idle data");
    ASSERT_TRUE(readResponse(client, response), "idle data response");
    ASSERT_EQ(response.response, static_cast<std::uint8_t>('I'), "idle data invalid NAK");

    host->stop();
    udp.close();
}

void testLoopbackSingleUsesLocalhostWithoutAlias() {
    constexpr std::uint16_t tcpPort = 19767;

    libera_link::virtual_controller::ensureBuiltInEtherDreamVirtualControllerHostLinked();

    libera_link::virtual_controller::VirtualControllerHostConfig config;
    config.sliceDurationUs = 1000;
    config.options["ip_mode"] = "loopback";
    config.options["allow_privilege_prompt"] = "false";
    config.options["tcp_port"] = std::to_string(tcpPort);
    config.options["discovery"] = "false";
    config.options["playback_slice_us"] = "1000";

    std::string error;
    auto host = libera_link::virtual_controller::createVirtualControllerHost(
        "etherdream", config, error);
    ASSERT_TRUE(host != nullptr, error.empty() ? "loopback host should construct" : error.c_str());
    if (!host) {
        return;
    }

    auto sink = std::make_shared<RecordingSink>("target-loopback");
    libera_link::virtual_controller::VirtualControllerHostContext context;
    context.targets.push_back(libera_link::virtual_controller::Target{sink});
    ASSERT_TRUE(host->start(context, error),
                error.empty() ? "loopback host should start without adding aliases" : error.c_str());
    if (!host->running()) {
        return;
    }

    const auto endpoints = host->endpoints();
    ASSERT_EQ(endpoints.size(), static_cast<std::size_t>(1), "one loopback endpoint");
    if (!endpoints.empty()) {
        ASSERT_TRUE(endpoints.front().address == "127.0.0.1",
                    "single loopback endpoint uses localhost");
    }

    libera::net::TcpClient client;
    client.setDefaultTimeout(500ms);
    client.setConnectTimeout(500ms);
    const auto endpoint = libera::net::tcp::endpoint(
        libera::net::asio::ip::make_address("127.0.0.1"), tcpPort);
    ASSERT_TRUE(!client.connect(endpoint), "client connects to single loopback virtual Ether Dream");

    libera::etherdream::EtherDreamResponse response;
    ASSERT_TRUE(readResponse(client, response), "single loopback initial response");
    ASSERT_EQ(response.response, static_cast<std::uint8_t>('a'), "single loopback initial ack");

    host->stop();
}

} // namespace

int main() {
    testProtocolStreaming();
    testNaksAndBroadcast();
    testLoopbackSingleUsesLocalhostWithoutAlias();

    if (g_failures > 0) {
        std::fprintf(stderr, "Ether Dream virtual controller host tests failed: %d\n", g_failures);
        return 1;
    }

    std::puts("Ether Dream virtual controller host tests passed.");
    return 0;
}
