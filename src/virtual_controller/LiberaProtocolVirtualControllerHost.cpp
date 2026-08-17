#include "virtual_controller/LiberaProtocolVirtualControllerHost.hpp"

#include "virtual_controller/VirtualControllerHostRegistry.hpp"

#include "libera/net/NetConfig.hpp"
#include "libera/protocol/Codec.hpp"
#include "libera/protocol/Sender.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace libera_link::virtual_controller {
namespace {

using libera::core::LaserPoint;
using libera::net::tcp;
using libera::net::udp;
namespace asio = libera::net::asio;
namespace protocol = libera::protocol;
using namespace std::chrono_literals;

constexpr std::uint32_t defaultMaxPointRate = 100000;
constexpr std::uint32_t defaultMinPointRate = 1000;
constexpr std::uint32_t defaultMaxFramePoints = 300000;
constexpr std::uint32_t defaultMaxRecordPayloadBytes = 4u * 1024u * 1024u;
constexpr std::uint8_t maxLaserPointUserChannels = 2;
constexpr std::uint32_t supportedFeatureFlags =
    protocol::FeatureTargetBeginTime |
    protocol::FeatureScannerSync |
    protocol::FeatureStatus;

std::string trim(std::string_view text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) {
        text.remove_prefix(1);
    }
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) {
        text.remove_suffix(1);
    }
    return std::string(text);
}

std::vector<std::string> splitCsv(std::string_view text) {
    std::vector<std::string> values;
    while (!text.empty()) {
        const auto comma = text.find(',');
        const auto piece = comma == std::string_view::npos ? text : text.substr(0, comma);
        auto value = trim(piece);
        if (!value.empty()) {
            values.push_back(std::move(value));
        }
        if (comma == std::string_view::npos) {
            break;
        }
        text.remove_prefix(comma + 1);
    }
    return values;
}

std::optional<std::uint32_t> parseU32(std::string_view text) {
    try {
        std::size_t parsed = 0;
        const auto raw = std::stoull(std::string(text), &parsed, 10);
        if (parsed != text.size() || raw > std::numeric_limits<std::uint32_t>::max()) {
            return std::nullopt;
        }
        return static_cast<std::uint32_t>(raw);
    } catch (...) {
        return std::nullopt;
    }
}

std::uint16_t optionU16(const VirtualControllerHostConfig& config,
                        std::string_view key,
                        std::uint16_t fallback) {
    const auto it = config.options.find(std::string(key));
    if (it == config.options.end()) {
        return fallback;
    }
    const auto parsed = parseU32(it->second);
    if (!parsed || *parsed > std::numeric_limits<std::uint16_t>::max()) {
        return fallback;
    }
    return static_cast<std::uint16_t>(*parsed);
}

std::uint32_t optionU32(const VirtualControllerHostConfig& config,
                        std::string_view key,
                        std::uint32_t fallback) {
    const auto it = config.options.find(std::string(key));
    if (it == config.options.end()) {
        return fallback;
    }
    return parseU32(it->second).value_or(fallback);
}

std::string optionString(const VirtualControllerHostConfig& config,
                         std::string_view key,
                         std::string fallback = {}) {
    const auto it = config.options.find(std::string(key));
    if (it == config.options.end()) {
        return fallback;
    }
    return trim(it->second);
}

float coordFromProtocol(std::int16_t value) {
    if (value == std::numeric_limits<std::int16_t>::min()) {
        return -1.0f;
    }
    return std::clamp(static_cast<float>(value) / 32767.0f, -1.0f, 1.0f);
}

float channelFromProtocol(std::uint16_t value) {
    return static_cast<float>(value) / 65535.0f;
}

LaserPoint toLaserPoint(const protocol::PointSample& in) {
    LaserPoint out{};
    out.x = coordFromProtocol(in.x);
    out.y = coordFromProtocol(in.y);
    out.r = channelFromProtocol(in.r);
    out.g = channelFromProtocol(in.g);
    out.b = channelFromProtocol(in.b);
    out.i = channelFromProtocol(in.i);
    if (!in.user.empty()) {
        out.u1 = channelFromProtocol(in.user[0]);
    }
    if (in.user.size() > 1) {
        out.u2 = channelFromProtocol(in.user[1]);
    }
    return out;
}

std::vector<LaserPoint> toLaserPoints(const std::vector<protocol::PointSample>& samples) {
    std::vector<LaserPoint> points;
    points.reserve(samples.size());
    for (const auto& sample : samples) {
        points.push_back(toLaserPoint(sample));
    }
    return points;
}

std::uint32_t pointDurationUs(std::size_t pointCount, std::uint32_t pointRate) {
    if (pointCount == 0 || pointRate == 0) {
        return 0;
    }
    const double duration =
        (1000000.0 * static_cast<double>(pointCount)) / static_cast<double>(pointRate);
    if (!std::isfinite(duration) || duration <= 0.0) {
        return 0;
    }
    return static_cast<std::uint32_t>(std::llround(duration));
}

struct HostOptions {
    std::string listenAddress = "0.0.0.0";
    std::string advertisedAddress;
    std::vector<std::string> broadcastAddresses{"255.255.255.255"};
    std::uint16_t tcpPort = protocol::DEFAULT_SESSION_PORT;
    std::uint16_t discoveryPort = protocol::DEFAULT_DISCOVERY_PORT;
    std::uint32_t broadcastIntervalMs = 500;
    std::uint32_t minPointRate = defaultMinPointRate;
    std::uint32_t maxPointRate = defaultMaxPointRate;
    std::uint32_t maxFramePoints = defaultMaxFramePoints;
    std::uint32_t maxRecordPayloadBytes = defaultMaxRecordPayloadBytes;
    std::uint8_t maxUserChannels = maxLaserPointUserChannels;
    bool discoveryEnabled = true;
};

HostOptions makeHostOptions(const VirtualControllerHostConfig& config) {
    HostOptions options;
    options.listenAddress = optionString(config, "listen_address", options.listenAddress);
    options.advertisedAddress = optionString(config, "advertised_address", options.advertisedAddress);
    options.tcpPort = optionU16(config, "tcp_port", options.tcpPort);
    options.discoveryPort = optionU16(config, "discovery_port", options.discoveryPort);
    options.broadcastIntervalMs = std::max<std::uint32_t>(
        100u, optionU32(config, "broadcast_interval_ms", options.broadcastIntervalMs));
    options.minPointRate = std::max<std::uint32_t>(
        1u, optionU32(config, "min_point_rate", options.minPointRate));
    options.maxPointRate = std::max<std::uint32_t>(
        options.minPointRate, optionU32(config, "max_point_rate", options.maxPointRate));
    options.maxFramePoints = std::max<std::uint32_t>(
        1u, optionU32(config, "max_frame_points", options.maxFramePoints));
    options.maxRecordPayloadBytes = std::max<std::uint32_t>(
        1024u, optionU32(config, "max_record_payload_bytes", options.maxRecordPayloadBytes));
    options.maxUserChannels = static_cast<std::uint8_t>(std::min<std::uint32_t>(
        maxLaserPointUserChannels,
        optionU32(config, "max_user_channels", options.maxUserChannels)));

    auto broadcastAddresses = splitCsv(optionString(config, "broadcast_addresses"));
    if (!broadcastAddresses.empty()) {
        options.broadcastAddresses = std::move(broadcastAddresses);
    }
    const auto discovery = optionString(config, "discovery", options.discoveryEnabled ? "true" : "false");
    if (discovery == "0" || discovery == "false" || discovery == "off" || discovery == "no") {
        options.discoveryEnabled = false;
    }
    if (options.advertisedAddress.empty() && options.listenAddress != "0.0.0.0") {
        options.advertisedAddress = options.listenAddress;
    }
    return options;
}

std::string targetEndpointId(const TargetInfo& info, std::size_t index) {
    if (!info.id.empty()) {
        return info.id;
    }
    if (!info.label.empty()) {
        return info.label;
    }
    return "target-" + std::to_string(index + 1);
}

std::string targetDisplayName(const TargetInfo& info, std::size_t index) {
    std::string name;
    if (!info.label.empty()) {
        name = info.label;
    } else if (!info.id.empty()) {
        name = info.id;
    } else {
        name = "Libera target " + std::to_string(index + 1);
    }
    if (name.rfind("LL - ", 0) == 0) {
        return name;
    }
    return "LL - " + name;
}

protocol::StreamMode acceptedMode(protocol::StreamMode requested) {
    if (requested == protocol::StreamMode::FrameByCount ||
        requested == protocol::StreamMode::MarkedPointStream) {
        return protocol::StreamMode::FrameByCount;
    }
    return protocol::StreamMode::RawPointStream;
}

struct PendingFrame {
    protocol::FrameMarker marker;
    std::vector<LaserPoint> points;
    std::optional<std::chrono::steady_clock::time_point> targetBeginTime;
};

struct SessionState {
    protocol::Sender sender;
    protocol::StreamMode streamMode = protocol::StreamMode::RawPointStream;
    std::uint8_t userChannelCount = 0;
    std::uint32_t pointRate = 30000;
    std::chrono::steady_clock::time_point sessionStartedAt = std::chrono::steady_clock::now();
    std::optional<PendingFrame> pendingFrame;
};

class TargetServer final {
public:
    TargetServer(Target target,
                 HostOptions options,
                 std::uint16_t tcpPort,
                 std::size_t index)
        : target_(std::move(target))
        , options_(std::move(options))
        , tcpPort_(tcpPort)
        , index_(index) {}

    ~TargetServer() {
        stop();
    }

    bool start(std::string& error) {
        if (!target_.sink) {
            error = "Libera protocol target is missing a sink.";
            return false;
        }

        std::error_code ec;
        const auto address = asio::ip::make_address(options_.listenAddress, ec);
        if (ec) {
            error = "Invalid Libera protocol listen address " + options_.listenAddress +
                    ": " + ec.message();
            return false;
        }

        acceptor_ = std::make_unique<tcp::acceptor>(io_);
        acceptor_->open(tcp::v4(), ec);
        if (ec) {
            error = "Libera protocol TCP acceptor open failed: " + ec.message();
            return false;
        }
        acceptor_->set_option(asio::socket_base::reuse_address(true), ec);
        acceptor_->bind(tcp::endpoint(address, tcpPort_), ec);
        if (ec) {
            error = "Libera protocol TCP bind failed on " + options_.listenAddress + ":" +
                    std::to_string(tcpPort_) + ": " + ec.message();
            return false;
        }
        acceptor_->listen(asio::socket_base::max_listen_connections, ec);
        if (ec) {
            error = "Libera protocol TCP listen failed: " + ec.message();
            return false;
        }

        running_.store(true, std::memory_order_release);
        acceptThread_ = std::thread([this] { acceptLoop(); });
        return true;
    }

    void stop() {
        if (!running_.exchange(false, std::memory_order_acq_rel)) {
            return;
        }

        std::error_code ignored;
        if (acceptor_) {
            acceptor_->close(ignored);
        }

        {
            std::lock_guard<std::mutex> lock(activeSocketsMutex_);
            for (auto& socket : activeSockets_) {
                if (socket && socket->is_open()) {
                    socket->close(ignored);
                }
            }
        }

        if (acceptThread_.joinable()) {
            acceptThread_.join();
        }

        for (auto& thread : sessionThreads_) {
            if (thread.joinable()) {
                thread.join();
            }
        }
        sessionThreads_.clear();
        activeSockets_.clear();
    }

    VirtualControllerEndpoint endpoint() const {
        const auto& info = target_.sink->targetInfo();
        VirtualControllerEndpoint endpoint;
        endpoint.targetId = info.id;
        endpoint.label = targetDisplayName(info, index_);
        endpoint.value = options_.advertisedAddress.empty()
            ? std::to_string(tcpPort_)
            : options_.advertisedAddress + ":" + std::to_string(tcpPort_);
        endpoint.kind = "virtual-dac";
        endpoint.protocol = "libera";
        endpoint.transport = "tcp";
        endpoint.address = options_.advertisedAddress.empty()
            ? options_.listenAddress
            : options_.advertisedAddress;
        endpoint.port = tcpPort_;
        endpoint.channels = options_.maxUserChannels;
        endpoint.attributes["stream_modes"] = "raw,frame-by-count";
        endpoint.attributes["discovery_port"] = std::to_string(options_.discoveryPort);
        return endpoint;
    }

    protocol::DiscoveryAdvertisement advertisement() const {
        const auto& info = target_.sink->targetInfo();
        protocol::DiscoveryAdvertisement advertisement;
        advertisement.endpointId = targetEndpointId(info, index_);
        advertisement.displayName = targetDisplayName(info, index_);
        advertisement.endpointType = "virtual";
        advertisement.address = options_.advertisedAddress;
        advertisement.tcpPort = tcpPort_;
        advertisement.supportedStreamModes =
            protocol::streamModeMask(protocol::StreamMode::RawPointStream) |
            protocol::streamModeMask(protocol::StreamMode::FrameByCount);
        advertisement.maxUserChannelCount = options_.maxUserChannels;
        advertisement.minPointRate = options_.minPointRate;
        advertisement.maxPointRate = info.maxPointRate > 0
            ? std::min<std::uint32_t>(info.maxPointRate, options_.maxPointRate)
            : options_.maxPointRate;
        advertisement.maxFramePointCount = options_.maxFramePoints;
        advertisement.featureFlags = supportedFeatureFlags;
        return advertisement;
    }

private:
    void acceptLoop() {
        while (running_.load(std::memory_order_acquire)) {
            auto socket = std::make_shared<tcp::socket>(io_);
            std::error_code ec;
            acceptor_->accept(*socket, ec);
            if (ec) {
                if (running_.load(std::memory_order_acquire)) {
                    std::this_thread::sleep_for(25ms);
                }
                continue;
            }

            {
                std::lock_guard<std::mutex> lock(activeSocketsMutex_);
                activeSockets_.push_back(socket);
            }
            sessionThreads_.emplace_back([this, socket] {
                runSession(socket);
                removeActiveSocket(socket);
            });
        }
    }

    void removeActiveSocket(const std::shared_ptr<tcp::socket>& socket) {
        std::lock_guard<std::mutex> lock(activeSocketsMutex_);
        activeSockets_.erase(
            std::remove(activeSockets_.begin(), activeSockets_.end(), socket),
            activeSockets_.end());
    }

    bool readRecord(tcp::socket& socket, protocol::Record& record, std::string& error) const {
        std::array<std::uint8_t, protocol::RECORD_HEADER_SIZE> headerBytes{};
        std::error_code ec;
        asio::read(socket, asio::buffer(headerBytes), ec);
        if (ec) {
            error = ec.message();
            return false;
        }

        protocol::RecordHeader header;
        if (!protocol::decodeRecordHeader(headerBytes.data(), headerBytes.size(), header, error)) {
            return false;
        }
        if (header.payloadSize > options_.maxRecordPayloadBytes) {
            error = "Libera protocol record payload exceeds configured limit.";
            return false;
        }

        std::vector<std::uint8_t> payload(header.payloadSize);
        if (!payload.empty()) {
            asio::read(socket, asio::buffer(payload), ec);
            if (ec) {
                error = ec.message();
                return false;
            }
        }

        record.type = header.type;
        record.flags = header.flags;
        record.sequence = header.sequence;
        record.payload = std::move(payload);
        return true;
    }

    bool writeBytes(tcp::socket& socket, const std::vector<std::uint8_t>& bytes) const {
        std::error_code ec;
        asio::write(socket, asio::buffer(bytes), ec);
        return !ec;
    }

    void runSession(const std::shared_ptr<tcp::socket>& socket) {
        if (!socket || !target_.sink) {
            return;
        }

        target_.sink->reset();

        SessionState state;
        protocol::Record record;
        std::string error;
        if (!readRecord(*socket, record, error) || record.type != protocol::RecordType::Hello) {
            target_.sink->reset();
            return;
        }

        protocol::Hello hello;
        if (!protocol::decodeHello(record.payload.data(), record.payload.size(), hello, error)) {
            target_.sink->reset();
            return;
        }

        state.streamMode = acceptedMode(hello.requestedStreamMode);
        state.userChannelCount = static_cast<std::uint8_t>(std::min<std::uint32_t>(
            options_.maxUserChannels,
            hello.requestedUserChannelCount));
        state.pointRate = std::clamp<std::uint32_t>(
            hello.defaultPointRate == 0 ? 30000u : hello.defaultPointRate,
            options_.minPointRate,
            options_.maxPointRate);
        state.sender.setUserChannelCount(state.userChannelCount);
        state.sessionStartedAt = std::chrono::steady_clock::now();

        protocol::Accept accept;
        accept.acceptedStreamMode = state.streamMode;
        accept.acceptedUserChannelCount = state.userChannelCount;
        accept.defaultPointRate = state.pointRate;
        accept.maxPointRate = options_.maxPointRate;
        accept.maxFramePointCount = options_.maxFramePoints;
        accept.maxRecordPayloadSize = options_.maxRecordPayloadBytes;
        accept.sessionId = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                state.sessionStartedAt.time_since_epoch()).count());
        accept.featureFlags = supportedFeatureFlags;
        if (!writeBytes(*socket, state.sender.makeAccept(accept))) {
            target_.sink->reset();
            return;
        }

        if (!readRecord(*socket, record, error) || record.type != protocol::RecordType::Ready) {
            target_.sink->reset();
            return;
        }

        while (running_.load(std::memory_order_acquire) && socket->is_open()) {
            if (!readRecord(*socket, record, error)) {
                break;
            }
            if (!handleRecord(*socket, state, record)) {
                break;
            }
        }

        target_.sink->reset();
    }

    bool handleRecord(tcp::socket& socket,
                      SessionState& state,
                      const protocol::Record& record) {
        std::string error;
        switch (record.type) {
        case protocol::RecordType::StreamConfig:
            return handleStreamConfig(state, record, error);
        case protocol::RecordType::SetScannerSync:
            return handleScannerSync(record, error);
        case protocol::RecordType::FrameMarker:
            return handleFrameMarker(state, record, error);
        case protocol::RecordType::Points:
            return handlePoints(state, record, error);
        case protocol::RecordType::Ping:
            if (record.payload.size() == 8) {
                const auto timestamp = protocol::readUInt64(record.payload.data());
                writeBytes(socket, state.sender.makePong(timestamp));
            }
            return true;
        case protocol::RecordType::Close:
            return false;
        default:
            return true;
        }
    }

    bool handleStreamConfig(SessionState& state,
                            const protocol::Record& record,
                            std::string& error) const {
        protocol::StreamConfig config;
        if (!protocol::decodeStreamConfig(record.payload.data(),
                                          record.payload.size(),
                                          config,
                                          error)) {
            return false;
        }

        state.streamMode = acceptedMode(config.streamMode);
        state.userChannelCount = static_cast<std::uint8_t>(std::min<std::uint32_t>(
            options_.maxUserChannels,
            config.userChannelCount));
        state.sender.setUserChannelCount(state.userChannelCount);
        if (config.defaultPointRate != 0) {
            state.pointRate = std::clamp<std::uint32_t>(
                config.defaultPointRate,
                options_.minPointRate,
                options_.maxPointRate);
        }
        state.pendingFrame.reset();
        return true;
    }

    bool handleScannerSync(const protocol::Record& record,
                           std::string& error) const {
        protocol::ScannerSync scannerSync;
        if (!protocol::decodeScannerSync(record.payload.data(),
                                         record.payload.size(),
                                         scannerSync,
                                         error)) {
            return false;
        }
        if (target_.sink) {
            target_.sink->setScannerSync(scannerSync.offsetNs, scannerSync.enabled);
        }
        return true;
    }

    bool handleFrameMarker(SessionState& state,
                           const protocol::Record& record,
                           std::string& error) {
        protocol::FrameMarker marker;
        if (!protocol::decodeFrameMarker(record.payload.data(),
                                         record.payload.size(),
                                         marker,
                                         error)) {
            return false;
        }
        if (marker.framePointCount > options_.maxFramePoints) {
            return false;
        }

        if (state.pendingFrame && !state.pendingFrame->points.empty() &&
            state.pendingFrame->marker.framePointCount == 0) {
            submitPendingFrame(state);
        }
        state.pendingFrame.reset();

        PendingFrame pending;
        pending.marker = marker;
        pending.points.reserve(marker.framePointCount);
        if (marker.targetBeginTimeNs != 0) {
            pending.targetBeginTime =
                state.sessionStartedAt +
                std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                    std::chrono::nanoseconds(marker.targetBeginTimeNs));
        }
        state.pendingFrame = std::move(pending);
        if (marker.pointRate != 0) {
            state.pointRate = std::clamp<std::uint32_t>(
                marker.pointRate,
                options_.minPointRate,
                options_.maxPointRate);
        }
        return true;
    }

    bool handlePoints(SessionState& state,
                      const protocol::Record& record,
                      std::string& error) {
        std::vector<protocol::PointSample> samples;
        if (!protocol::decodePointSamples(record.payload.data(),
                                          record.payload.size(),
                                          state.userChannelCount,
                                          samples,
                                          error)) {
            return false;
        }
        auto points = toLaserPoints(samples);
        if (state.pendingFrame) {
            appendFramePoints(state, std::move(points));
            return true;
        }

        submitContinuousPoints(std::move(points), state.pointRate);
        return true;
    }

    void appendFramePoints(SessionState& state, std::vector<LaserPoint> points) {
        std::size_t cursor = 0;
        while (cursor < points.size()) {
            if (!state.pendingFrame) {
                std::vector<LaserPoint> remaining(
                    points.begin() + static_cast<std::ptrdiff_t>(cursor),
                    points.end());
                submitContinuousPoints(std::move(remaining), state.pointRate);
                return;
            }

            auto& pending = *state.pendingFrame;
            const std::uint32_t expected = pending.marker.framePointCount;
            const std::size_t available = points.size() - cursor;
            const std::size_t remaining = expected > 0
                ? expected - std::min<std::size_t>(pending.points.size(), expected)
                : available;
            const std::size_t toCopy = std::min<std::size_t>(available, remaining);
            pending.points.insert(
                pending.points.end(),
                points.begin() + static_cast<std::ptrdiff_t>(cursor),
                points.begin() + static_cast<std::ptrdiff_t>(cursor + toCopy));
            cursor += toCopy;

            if (expected > 0 && pending.points.size() >= expected) {
                submitPendingFrame(state);
            } else if (toCopy == 0) {
                state.pendingFrame.reset();
            }
        }
    }

    void submitContinuousPoints(std::vector<LaserPoint> points, std::uint32_t pointRate) {
        if (!target_.sink || points.empty()) {
            return;
        }
        SliceSubmission submission;
        submission.effectivePointRate = pointRate;
        submission.durationUs = pointDurationUs(points.size(), pointRate);
        submission.points = std::move(points);
        target_.sink->submitContinuous(std::move(submission));
    }

    void submitPendingFrame(SessionState& state) {
        if (!target_.sink || !state.pendingFrame || state.pendingFrame->points.empty()) {
            state.pendingFrame.reset();
            return;
        }

        auto pending = std::move(*state.pendingFrame);
        state.pendingFrame.reset();

        const std::uint32_t pointRate = pending.marker.pointRate != 0
            ? std::clamp<std::uint32_t>(pending.marker.pointRate,
                                        options_.minPointRate,
                                        options_.maxPointRate)
            : state.pointRate;

        SliceSubmission slice;
        slice.effectivePointRate = pointRate;
        slice.durationUs = pointDurationUs(pending.points.size(), pointRate);
        slice.points = std::move(pending.points);

        FrameSubmission frame;
        frame.effectivePointRate = pointRate;
        frame.targetBeginTime = pending.targetBeginTime;
        frame.slices.push_back(std::move(slice));
        target_.sink->submitFrame(std::move(frame));
    }

    Target target_;
    HostOptions options_;
    std::uint16_t tcpPort_ = protocol::DEFAULT_SESSION_PORT;
    std::size_t index_ = 0;
    asio::io_context io_;
    std::unique_ptr<tcp::acceptor> acceptor_;
    std::thread acceptThread_;
    std::vector<std::thread> sessionThreads_;
    std::vector<std::shared_ptr<tcp::socket>> activeSockets_;
    std::mutex activeSocketsMutex_;
    std::atomic<bool> running_{false};
};

VirtualControllerHostRegistrar gLiberaProtocolVirtualControllerHostRegistrar({
    {
        "libera",
        "Libera Protocol",
        "Expose linked controllers through the Libera protocol.",
        {
            {
                "listen_address",
                "Listen Address",
                "Local address for TCP sessions.",
                VirtualControllerHostOptionType::String,
                "0.0.0.0",
                {},
                false,
            },
            {
                "advertised_address",
                "Advertised Address",
                "Address advertised to senders. Leave empty to let senders use the UDP source address.",
                VirtualControllerHostOptionType::String,
                "",
                {},
                false,
            },
            {
                "tcp_port",
                "TCP Port",
                "Base TCP port for Libera protocol sessions.",
                VirtualControllerHostOptionType::Integer,
                std::to_string(protocol::DEFAULT_SESSION_PORT),
                {},
                false,
            },
            {
                "discovery_port",
                "Discovery Port",
                "UDP discovery advertisement port.",
                VirtualControllerHostOptionType::Integer,
                std::to_string(protocol::DEFAULT_DISCOVERY_PORT),
                {},
                false,
            },
            {
                "broadcast_addresses",
                "Broadcast Addresses",
                "Comma-separated UDP advertisement destinations.",
                VirtualControllerHostOptionType::String,
                "255.255.255.255",
                {},
                false,
            },
            {
                "broadcast_interval_ms",
                "Broadcast Interval",
                "Discovery advertisement interval in milliseconds.",
                VirtualControllerHostOptionType::Integer,
                "500",
                {},
                false,
            },
            {
                "max_frame_points",
                "Max Frame Points",
                "Maximum accepted points in one counted frame.",
                VirtualControllerHostOptionType::Integer,
                std::to_string(defaultMaxFramePoints),
                {},
                false,
            },
            {
                "max_user_channels",
                "User Channels",
                "Maximum accepted user channels. Libera Link currently maps u1 and u2.",
                VirtualControllerHostOptionType::Integer,
                std::to_string(maxLaserPointUserChannels),
                {},
                false,
            },
        },
        false,
    },
    [](const VirtualControllerHostConfig& config) {
        return std::make_unique<LiberaProtocolVirtualControllerHost>(config);
    },
});

} // namespace

void ensureBuiltInLiberaProtocolVirtualControllerHostLinked() {}

struct LiberaProtocolVirtualControllerHost::Impl {
    HostOptions options;
    std::vector<std::unique_ptr<TargetServer>> servers;
    std::thread advertiserThread;
    std::atomic<bool> active{false};

    void stop() {
        if (!active.exchange(false, std::memory_order_acq_rel)) {
            return;
        }

        for (auto& server : servers) {
            if (server) {
                server->stop();
            }
        }
        if (advertiserThread.joinable()) {
            advertiserThread.join();
        }
        servers.clear();
    }

    void advertiserLoop() {
        asio::io_context io;
        udp::socket socket(io);
        std::error_code ec;
        socket.open(udp::v4(), ec);
        if (ec) {
            return;
        }
        socket.set_option(asio::socket_base::broadcast(true), ec);

        while (active.load(std::memory_order_acquire)) {
            for (const auto& server : servers) {
                if (!server) {
                    continue;
                }
                const auto payload =
                    protocol::encodeDiscoveryAdvertisement(server->advertisement());
                for (const auto& address : options.broadcastAddresses) {
                    std::error_code addressError;
                    const auto ip = asio::ip::make_address(address, addressError);
                    if (addressError) {
                        continue;
                    }
                    udp::endpoint endpoint(ip, options.discoveryPort);
                    std::error_code sendError;
                    socket.send_to(asio::buffer(payload), endpoint, 0, sendError);
                }
            }

            const auto interval = std::chrono::milliseconds(options.broadcastIntervalMs);
            const auto slices = std::max<int>(1, static_cast<int>(interval / 50ms));
            for (int i = 0; i < slices && active.load(std::memory_order_acquire); ++i) {
                std::this_thread::sleep_for(interval / slices);
            }
        }
    }
};

LiberaProtocolVirtualControllerHost::LiberaProtocolVirtualControllerHost(
    const VirtualControllerHostConfig& config)
    : impl_(std::make_unique<Impl>())
    , config_(config) {}

LiberaProtocolVirtualControllerHost::~LiberaProtocolVirtualControllerHost() {
    stop();
}

std::string_view LiberaProtocolVirtualControllerHost::name() const {
    return "libera";
}

std::string_view LiberaProtocolVirtualControllerHost::displayName() const {
    return "Libera Protocol";
}

bool LiberaProtocolVirtualControllerHost::start(const VirtualControllerHostContext& context,
                                                std::string& error) {
    if (impl_->active.load(std::memory_order_acquire)) {
        return true;
    }
    if (context.targets.empty()) {
        error = "Libera protocol host requires at least one target.";
        return false;
    }

    impl_->options = makeHostOptions(config_);
    impl_->active.store(true, std::memory_order_release);

    std::size_t index = 0;
    for (const auto& target : context.targets) {
        const auto port = static_cast<std::uint16_t>(
            std::min<unsigned>(
                std::numeric_limits<std::uint16_t>::max(),
                static_cast<unsigned>(impl_->options.tcpPort) +
                    static_cast<unsigned>(index)));
        auto server = std::make_unique<TargetServer>(target, impl_->options, port, index);
        if (!server->start(error)) {
            impl_->stop();
            return false;
        }
        impl_->servers.push_back(std::move(server));
        ++index;
    }

    if (impl_->options.discoveryEnabled) {
        impl_->advertiserThread = std::thread([this] { impl_->advertiserLoop(); });
    }
    return true;
}

void LiberaProtocolVirtualControllerHost::stop() {
    impl_->stop();
}

bool LiberaProtocolVirtualControllerHost::running() const {
    return impl_->active.load(std::memory_order_acquire);
}

std::vector<VirtualControllerEndpoint> LiberaProtocolVirtualControllerHost::endpoints() const {
    std::vector<VirtualControllerEndpoint> endpoints;
    endpoints.reserve(impl_->servers.size());
    for (const auto& server : impl_->servers) {
        if (server) {
            endpoints.push_back(server->endpoint());
        }
    }
    return endpoints;
}

} // namespace libera_link::virtual_controller
