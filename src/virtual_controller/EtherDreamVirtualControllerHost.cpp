#include "virtual_controller/EtherDreamVirtualControllerHost.hpp"

#include "virtual_controller/VirtualControllerHostRegistry.hpp"

#include "libera/core/ByteBuffer.hpp"
#include "libera/core/ByteRead.hpp"
#include "libera/etherdream/EtherDreamConfig.hpp"
#include "libera/etherdream/EtherDreamResponse.hpp"
#include "libera/log/Log.hpp"
#include "libera/net/NetConfig.hpp"

#if defined(__unix__) || defined(__APPLE__)
#  include <arpa/inet.h>
#  include <ifaddrs.h>
#  include <net/if.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#  include <sys/wait.h>
#  include <unistd.h>
#endif

#if defined(_WIN32)
#  include <winsock2.h>
#  include <ws2tcpip.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

namespace libera_link::virtual_controller {
namespace {

using libera::core::LaserPoint;
using libera::etherdream::LightEngineState;
using libera::etherdream::PlaybackState;
using libera::net::tcp;
using libera::net::udp;
namespace asio = libera::net::asio;
using namespace std::chrono_literals;

constexpr std::uint16_t defaultTcpPort =
    libera::etherdream::config::ETHERDREAM_DAC_PORT_DEFAULT;
constexpr std::uint16_t defaultDiscoveryPort =
    libera::etherdream::config::ETHERDREAM_DISCOVERY_PORT;
constexpr std::uint16_t defaultBufferCapacity = 3899;
constexpr std::uint32_t defaultMaxPointRate = 100000;
constexpr std::size_t bytesPerPoint = 18;
constexpr std::uint16_t rateChangeBit = 0x8000u;
constexpr std::uint16_t playbackFlagShutter = 0x0001u;
constexpr std::uint16_t playbackFlagUnderflow = 0x0002u;
constexpr std::uint16_t playbackFlagEstop = 0x0004u;
constexpr std::uint16_t lightEngineFlagEstopCommand = 0x0001u;

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
        const auto piece = comma == std::string_view::npos
            ? text
            : text.substr(0, comma);
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

bool parseBool(std::string_view text, bool fallback) {
    std::string value = trim(text);
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    if (value == "1" || value == "true" || value == "yes" || value == "on") {
        return true;
    }
    if (value == "0" || value == "false" || value == "no" || value == "off") {
        return false;
    }
    return fallback;
}

std::optional<std::uint32_t> ipv4ToHost(std::string_view text) {
    in_addr addr{};
    const std::string value(text);
    if (::inet_pton(AF_INET, value.c_str(), &addr) != 1) {
        return std::nullopt;
    }
    return ntohl(addr.s_addr);
}

std::string hostToIpv4(std::uint32_t value) {
    in_addr addr{};
    addr.s_addr = htonl(value);
    char buffer[INET_ADDRSTRLEN]{};
    if (::inet_ntop(AF_INET, &addr, buffer, sizeof(buffer)) == nullptr) {
        return {};
    }
    return std::string(buffer);
}

bool isValidIpv4(std::string_view text) {
    return ipv4ToHost(text).has_value();
}

std::uint8_t stableByteHash(std::string_view text, std::uint8_t salt) {
    std::uint32_t value = 2166136261u ^ salt;
    for (unsigned char ch : text) {
        value ^= ch;
        value *= 16777619u;
    }
    return static_cast<std::uint8_t>((value >> 16) ^ (value >> 8) ^ value);
}

std::array<std::uint8_t, 6> makeMacAddress(const TargetInfo& targetInfo, std::size_t index) {
    const std::string& seed = !targetInfo.id.empty() ? targetInfo.id : targetInfo.label;
    return {
        0x02u, // locally administered unicast
        0x4cu, // "LL"
        0x4cu,
        stableByteHash(seed, 0x11u),
        stableByteHash(seed, 0x37u),
        static_cast<std::uint8_t>((stableByteHash(seed, 0x73u) + index) & 0xffu),
    };
}

std::string formatMac(const std::array<std::uint8_t, 6>& mac) {
    std::ostringstream out;
    out << std::hex;
    for (std::size_t i = 0; i < mac.size(); ++i) {
        if (i) {
            out << ':';
        }
        out.width(2);
        out.fill('0');
        out << static_cast<unsigned>(mac[i]);
    }
    return out.str();
}

std::uint16_t revisionPortOffset(std::uint16_t tcpPort) {
    if (tcpPort < defaultTcpPort) {
        return 0;
    }
    const auto offset = static_cast<unsigned>(tcpPort) - defaultTcpPort;
    return offset <= std::numeric_limits<std::uint16_t>::max()
        ? static_cast<std::uint16_t>(offset)
        : 0;
}

float coordFromEd(std::int16_t value) {
    if (value == std::numeric_limits<std::int16_t>::min()) {
        return -1.0f;
    }
    return std::clamp(static_cast<float>(value) / 32767.0f, -1.0f, 1.0f);
}

float channelFromEd(std::uint16_t value) {
    return static_cast<float>(value) / 65535.0f;
}

struct HostOptions {
    std::vector<std::string> explicitAddresses;
    std::string ipMode = "auto";
    std::string interfaceName;
    std::string baseAddress;
    std::string netmask;
    std::vector<std::string> broadcastAddresses;
    bool autoManageAliases = true;
    bool allowPrivilegePrompt = true;
    bool discoveryEnabled = true;
    std::uint16_t tcpPort = defaultTcpPort;
    std::uint16_t discoveryPort = defaultDiscoveryPort;
    std::uint16_t bufferCapacity = defaultBufferCapacity;
    std::uint32_t maxPointRate = defaultMaxPointRate;
    std::uint32_t broadcastIntervalMs = 1000;
    std::uint32_t playbackSliceUs = 5000;
};

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

bool optionBool(const VirtualControllerHostConfig& config,
                std::string_view key,
                bool fallback) {
    const auto it = config.options.find(std::string(key));
    if (it == config.options.end()) {
        return fallback;
    }
    return parseBool(it->second, fallback);
}

HostOptions makeHostOptions(const VirtualControllerHostConfig& config) {
    HostOptions options;
    options.explicitAddresses = splitCsv(optionString(config, "addresses"));
    options.ipMode = optionString(config, "ip_mode", options.ipMode);
    options.interfaceName = optionString(config, "interface");
    options.baseAddress = optionString(config, "base_address");
    options.netmask = optionString(config, "netmask");
    options.broadcastAddresses = splitCsv(optionString(config, "broadcast_addresses"));
    if (options.broadcastAddresses.empty()) {
        options.broadcastAddresses.push_back("255.255.255.255");
    }
    options.autoManageAliases = optionBool(config, "auto_manage_aliases", options.autoManageAliases);
    options.allowPrivilegePrompt = optionBool(config, "allow_privilege_prompt", options.allowPrivilegePrompt);
    options.discoveryEnabled = optionBool(config, "discovery", options.discoveryEnabled);
    options.tcpPort = optionU16(config, "tcp_port", options.tcpPort);
    options.discoveryPort = optionU16(config, "discovery_port", options.discoveryPort);
    options.bufferCapacity = std::max<std::uint16_t>(
        1, optionU16(config, "buffer_capacity", options.bufferCapacity));
    options.maxPointRate = std::max<std::uint32_t>(
        1000u, optionU32(config, "max_point_rate", options.maxPointRate));
    options.broadcastIntervalMs = std::max<std::uint32_t>(
        20u, optionU32(config, "broadcast_interval_ms", options.broadcastIntervalMs));
    options.playbackSliceUs = std::max<std::uint32_t>(
        1000u,
        config.sliceDurationUs > 0 ? std::min(config.sliceDurationUs, 15000u)
                                   : options.playbackSliceUs);
    options.playbackSliceUs = optionU32(config, "playback_slice_us", options.playbackSliceUs);
    return options;
}

std::string shellQuote(std::string_view text) {
    std::string quoted = "'";
    for (const char ch : text) {
        if (ch == '\'') {
            quoted += "'\\''";
        } else {
            quoted += ch;
        }
    }
    quoted += "'";
    return quoted;
}

std::string appleScriptQuote(std::string_view text) {
    std::string quoted = "\"";
    for (const char ch : text) {
        if (ch == '\\' || ch == '"') {
            quoted += '\\';
        }
        quoted += ch;
    }
    quoted += '"';
    return quoted;
}

bool systemCommandSucceeded(const std::string& command) {
    const int status = std::system(command.c_str());
    if (status == -1) {
        return false;
    }
#if defined(__unix__) || defined(__APPLE__)
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status) == 0;
    }
    return status == 0;
#else
    return status == 0;
#endif
}

std::optional<std::string> readCommandOutput(const char* command) {
#if defined(__unix__) || defined(__APPLE__)
    FILE* pipe = popen(command, "r");
    if (pipe == nullptr) {
        return std::nullopt;
    }
    std::string output;
    std::array<char, 256> buffer{};
    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
        output += buffer.data();
    }
    pclose(pipe);
    return output;
#else
    (void)command;
    return std::nullopt;
#endif
}

std::optional<std::string> defaultInterfaceName() {
#if defined(__APPLE__)
    auto output = readCommandOutput("/sbin/route -n get default 2>/dev/null");
    if (!output) {
        return std::nullopt;
    }
    std::istringstream input(*output);
    std::string line;
    while (std::getline(input, line)) {
        line = trim(line);
        constexpr std::string_view prefix = "interface:";
        if (line.rfind(prefix, 0) == 0) {
            auto value = trim(std::string_view(line).substr(prefix.size()));
            if (!value.empty()) {
                return value;
            }
        }
    }
    return std::nullopt;
#elif defined(__linux__)
    auto output = readCommandOutput("awk '$2 == \"00000000\" { print $1; exit }' /proc/net/route 2>/dev/null");
    if (!output) {
        return std::nullopt;
    }
    auto value = trim(*output);
    if (value.empty()) {
        return std::nullopt;
    }
    return value;
#else
    return std::nullopt;
#endif
}

struct InterfaceAddress {
    std::string name;
    std::string address;
    std::string netmask;
    std::string broadcast;
    std::uint32_t addressHost = 0;
    std::uint32_t netmaskHost = 0;
    std::uint32_t broadcastHost = 0;
    bool loopback = false;
};

std::vector<InterfaceAddress> interfaceAddresses() {
    std::vector<InterfaceAddress> result;
#if defined(__unix__) || defined(__APPLE__)
    ifaddrs* addrs = nullptr;
    if (getifaddrs(&addrs) != 0 || addrs == nullptr) {
        return result;
    }

    for (ifaddrs* it = addrs; it != nullptr; it = it->ifa_next) {
        if (it->ifa_addr == nullptr || it->ifa_addr->sa_family != AF_INET) {
            continue;
        }
        if ((it->ifa_flags & IFF_UP) == 0) {
            continue;
        }

        const auto* addr = reinterpret_cast<const sockaddr_in*>(it->ifa_addr);
        const auto* mask = reinterpret_cast<const sockaddr_in*>(it->ifa_netmask);
        const auto* bcast = reinterpret_cast<const sockaddr_in*>(it->ifa_broadaddr);

        char addressBuffer[INET_ADDRSTRLEN]{};
        char netmaskBuffer[INET_ADDRSTRLEN]{};
        char broadcastBuffer[INET_ADDRSTRLEN]{};
        if (::inet_ntop(AF_INET, &addr->sin_addr, addressBuffer, sizeof(addressBuffer)) == nullptr) {
            continue;
        }
        if (mask == nullptr ||
            ::inet_ntop(AF_INET, &mask->sin_addr, netmaskBuffer, sizeof(netmaskBuffer)) == nullptr) {
            continue;
        }
        if (bcast != nullptr) {
            (void)::inet_ntop(AF_INET, &bcast->sin_addr, broadcastBuffer, sizeof(broadcastBuffer));
        }

        InterfaceAddress info;
        info.name = it->ifa_name != nullptr ? it->ifa_name : "";
        info.address = addressBuffer;
        info.netmask = netmaskBuffer;
        info.broadcast = broadcastBuffer;
        info.addressHost = ntohl(addr->sin_addr.s_addr);
        info.netmaskHost = ntohl(mask->sin_addr.s_addr);
        info.broadcastHost = bcast != nullptr
            ? ntohl(bcast->sin_addr.s_addr)
            : ((info.addressHost & info.netmaskHost) | ~info.netmaskHost);
        info.loopback = (it->ifa_flags & IFF_LOOPBACK) != 0;
        result.push_back(std::move(info));
    }

    freeifaddrs(addrs);
#endif
    return result;
}

bool canBindAddress(std::string_view address, std::uint16_t port) {
    std::error_code ec;
    asio::io_context io;
    tcp::acceptor acceptor(io);
    acceptor.open(tcp::v4(), ec);
    if (ec) {
        return false;
    }
    acceptor.set_option(asio::socket_base::reuse_address(true), ec);
    const auto ip = asio::ip::make_address(std::string(address), ec);
    if (ec) {
        return false;
    }
    acceptor.bind(tcp::endpoint(ip, port), ec);
    return !ec;
}

class IpAliasManager {
public:
    explicit IpAliasManager(HostOptions options)
        : options_(std::move(options)) {}

    ~IpAliasManager() {
        cleanup();
    }

    IpAliasManager(const IpAliasManager&) = delete;
    IpAliasManager& operator=(const IpAliasManager&) = delete;

    bool allocate(std::size_t count, std::vector<std::string>& addresses, std::string& error) {
        addresses.clear();
        cleanup();

        if (!options_.explicitAddresses.empty()) {
            if (options_.explicitAddresses.size() < count) {
                error = "Ether Dream explicit address list has fewer addresses than targets.";
                return false;
            }
            for (std::size_t i = 0; i < count; ++i) {
                if (!isValidIpv4(options_.explicitAddresses[i])) {
                    error = "Invalid Ether Dream explicit IPv4 address: " + options_.explicitAddresses[i];
                    return false;
                }
                addresses.push_back(options_.explicitAddresses[i]);
            }
            return true;
        }

        if (options_.ipMode == "explicit") {
            error = "Ether Dream ip_mode=explicit requires addresses=ip1,ip2,...";
            return false;
        }

        if (count == 0) {
            return true;
        }

        if (options_.ipMode == "loopback") {
            return allocateLoopback(count, addresses, error);
        }

        return allocateLan(count, addresses, error);
    }

    void cleanup() {
        for (auto it = addedAliases_.rbegin(); it != addedAliases_.rend(); ++it) {
            removeAlias(*it);
        }
        addedAliases_.clear();
    }

private:
    struct AliasRecord {
        std::string interfaceName;
        std::string address;
        std::string netmask;
        bool loopback = false;
    };

    bool allocateLoopback(std::size_t count, std::vector<std::string>& addresses, std::string& error) {
#if defined(_WIN32)
        if (count > 1) {
            error = "Automatic loopback IP aliases are not implemented on Windows.";
            return false;
        }
#endif
        const std::string interfaceName =
#if defined(__APPLE__)
            "lo0";
#elif defined(__linux__)
            "lo";
#else
            "lo0";
#endif
        for (std::size_t i = 0; i < count; ++i) {
            const std::string address = "127.0.0." + std::to_string(static_cast<unsigned>(i + 1));
            AliasRecord record{interfaceName, address, "255.0.0.0", true};
            if (!ensureAlias(record, error)) {
                return false;
            }
            addresses.push_back(address);
        }
        return true;
    }

    bool allocateLan(std::size_t count, std::vector<std::string>& addresses, std::string& error) {
        auto allInterfaces = interfaceAddresses();
        if (allInterfaces.empty()) {
            if (count == 1) {
                addresses.push_back("127.0.0.1");
                return true;
            }
            error = "No IPv4 interface is available for Ether Dream aliases.";
            return false;
        }

        std::unordered_set<std::uint32_t> existing;
        for (const auto& iface : allInterfaces) {
            existing.insert(iface.addressHost);
        }

        std::vector<InterfaceAddress> candidates;
        candidates.reserve(allInterfaces.size());
        const auto defaultIface = defaultInterfaceName();
        for (const auto& iface : allInterfaces) {
            if (iface.loopback) {
                continue;
            }
            if (!options_.interfaceName.empty() && iface.name != options_.interfaceName) {
                continue;
            }
            candidates.push_back(iface);
        }

        if (candidates.empty()) {
            if (count == 1) {
                addresses.push_back("127.0.0.1");
                return true;
            }
            error = options_.interfaceName.empty()
                ? "No non-loopback IPv4 interface is available for Ether Dream aliases."
                : "Configured Ether Dream interface is not available: " + options_.interfaceName;
            return false;
        }

        if (defaultIface) {
            std::stable_sort(candidates.begin(), candidates.end(),
                             [&](const InterfaceAddress& a, const InterfaceAddress& b) {
                                 return (a.name == *defaultIface) > (b.name == *defaultIface);
                             });
        }

        const InterfaceAddress& iface = candidates.front();
        if (count == 1 && options_.baseAddress.empty()) {
            addresses.push_back(iface.address);
            return true;
        }

        std::uint32_t network = iface.addressHost & iface.netmaskHost;
        std::uint32_t broadcast = iface.broadcastHost != 0
            ? iface.broadcastHost
            : (network | ~iface.netmaskHost);
        if (!options_.baseAddress.empty()) {
            const auto base = ipv4ToHost(options_.baseAddress);
            if (!base) {
                error = "Invalid Ether Dream base_address: " + options_.baseAddress;
                return false;
            }
            network = *base & iface.netmaskHost;
            broadcast = network | ~iface.netmaskHost;
        }

        if (broadcast <= network + 1) {
            error = "Ether Dream interface subnet has no usable alias addresses.";
            return false;
        }

        std::vector<std::string> allocated;
        allocated.reserve(count);
        for (std::uint32_t candidate = broadcast - 1; candidate > network; --candidate) {
            if (existing.count(candidate) > 0 || candidate == iface.addressHost) {
                continue;
            }
            const std::string address = hostToIpv4(candidate);
            if (address.empty()) {
                continue;
            }
            AliasRecord record{iface.name, address, iface.netmask, false};
            if (!ensureAlias(record, error)) {
                return false;
            }
            allocated.push_back(address);
            existing.insert(candidate);
            if (allocated.size() == count) {
                addresses = std::move(allocated);
                return true;
            }
        }

        error = "Could not allocate enough Ether Dream alias addresses on interface " + iface.name + ".";
        return false;
    }

    bool ensureAlias(const AliasRecord& record, std::string& error) {
        if (canBindAddress(record.address, options_.tcpPort)) {
            return true;
        }

        if (!options_.autoManageAliases) {
            error = "Ether Dream address " + record.address +
                    " is not assigned locally and auto_manage_aliases is disabled.";
            return false;
        }

        if (!addAlias(record)) {
            error = "Failed to add Ether Dream IP alias " + record.address +
                    " on " + record.interfaceName + ". Run with privileges or set addresses=... to preconfigured IPs.";
            return false;
        }

        if (!canBindAddress(record.address, options_.tcpPort)) {
            removeAlias(record);
            error = "Ether Dream IP alias " + record.address +
                    " was added but the app still cannot bind to it.";
            return false;
        }

        addedAliases_.push_back(record);
        return true;
    }

    bool addAlias(const AliasRecord& record) {
#if defined(__APPLE__)
        const std::string command = "/sbin/ifconfig " + shellQuote(record.interfaceName) +
                                    " alias " + shellQuote(record.address) + " " +
                                    shellQuote(record.netmask);
        if (systemCommandSucceeded(command + " >/dev/null 2>&1")) {
            return true;
        }
        if (!options_.allowPrivilegePrompt) {
            return false;
        }
        const std::string privileged = "/usr/bin/osascript -e " +
            shellQuote("do shell script " + appleScriptQuote(command) + " with administrator privileges");
        return systemCommandSucceeded(privileged + " >/dev/null 2>&1");
#elif defined(__linux__)
        const auto prefix = prefixLength(record.netmask).value_or(24);
        const std::string command = "/sbin/ip addr add " + shellQuote(record.address + "/" + std::to_string(prefix)) +
                                    " dev " + shellQuote(record.interfaceName);
        return systemCommandSucceeded(command + " >/dev/null 2>&1");
#else
        (void)record;
        return false;
#endif
    }

    void removeAlias(const AliasRecord& record) {
#if defined(__APPLE__)
        const std::string command = "/sbin/ifconfig " + shellQuote(record.interfaceName) +
                                    " -alias " + shellQuote(record.address);
        if (systemCommandSucceeded(command + " >/dev/null 2>&1")) {
            return;
        }
        if (options_.allowPrivilegePrompt) {
            const std::string privileged = "/usr/bin/osascript -e " +
                shellQuote("do shell script " + appleScriptQuote(command) + " with administrator privileges");
            (void)systemCommandSucceeded(privileged + " >/dev/null 2>&1");
        }
#elif defined(__linux__)
        const auto prefix = prefixLength(record.netmask).value_or(24);
        const std::string command = "/sbin/ip addr del " + shellQuote(record.address + "/" + std::to_string(prefix)) +
                                    " dev " + shellQuote(record.interfaceName);
        (void)systemCommandSucceeded(command + " >/dev/null 2>&1");
#else
        (void)record;
#endif
    }

    static std::optional<unsigned> prefixLength(const std::string& netmask) {
        const auto mask = ipv4ToHost(netmask);
        if (!mask) {
            return std::nullopt;
        }
        unsigned prefix = 0;
        bool sawZero = false;
        for (int bit = 31; bit >= 0; --bit) {
            const bool set = ((*mask >> bit) & 1u) != 0;
            if (set && sawZero) {
                return std::nullopt;
            }
            if (set) {
                ++prefix;
            } else {
                sawZero = true;
            }
        }
        return prefix;
    }

    HostOptions options_;
    std::vector<AliasRecord> addedAliases_;
};

struct BufferedPoint {
    LaserPoint point;
    std::uint16_t control = 0;
};

class VirtualEtherDreamDevice {
public:
    VirtualEtherDreamDevice(Target target,
                            std::string address,
                            std::array<std::uint8_t, 6> mac,
                            HostOptions options)
        : target_(std::move(target))
        , address_(std::move(address))
        , mac_(mac)
        , options_(std::move(options))
        , io_()
        , acceptor_(io_)
        , broadcastSocket_(io_)
        , broadcastTimer_(io_)
        , playbackTimer_(io_) {}

    ~VirtualEtherDreamDevice() {
        stop();
    }

    VirtualEtherDreamDevice(const VirtualEtherDreamDevice&) = delete;
    VirtualEtherDreamDevice& operator=(const VirtualEtherDreamDevice&) = delete;

    bool start(std::string& error) {
        if (!target_.sink) {
            error = "Ether Dream target has no sink.";
            return false;
        }

        std::error_code ec;
        const auto ip = asio::ip::make_address(address_, ec);
        if (ec) {
            error = "Invalid Ether Dream listen address " + address_ + ": " + ec.message();
            return false;
        }

        acceptor_.open(tcp::v4(), ec);
        if (ec) {
            error = "Ether Dream TCP open failed: " + ec.message();
            return false;
        }
        acceptor_.set_option(asio::socket_base::reuse_address(true), ec);
        acceptor_.bind(tcp::endpoint(ip, options_.tcpPort), ec);
        if (ec) {
            error = "Ether Dream TCP bind failed for " + address_ + ":" +
                    std::to_string(options_.tcpPort) + ": " + ec.message();
            return false;
        }
        acceptor_.listen(asio::socket_base::max_listen_connections, ec);
        if (ec) {
            error = "Ether Dream TCP listen failed for " + address_ + ": " + ec.message();
            return false;
        }

        if (options_.discoveryEnabled) {
            broadcastSocket_.open(udp::v4(), ec);
            if (!ec) {
                broadcastSocket_.set_option(asio::socket_base::broadcast(true), ec);
            }
            if (!ec) {
                broadcastSocket_.bind(udp::endpoint(ip, 0), ec);
            }
            if (ec) {
                error = "Ether Dream UDP broadcast socket failed for " + address_ + ": " + ec.message();
                return false;
            }
        }

        running_.store(true, std::memory_order_release);
        startAccept();
        startPlaybackTimer();
        if (options_.discoveryEnabled) {
            startBroadcastTimer(0ms);
        }
        ioThread_ = std::thread([this] { io_.run(); });
        return true;
    }

    void stop() {
        const bool wasRunning = running_.exchange(false, std::memory_order_acq_rel);
        if (!wasRunning && !ioThread_.joinable()) {
            return;
        }

        std::error_code ignored;
        acceptor_.close(ignored);
        broadcastSocket_.close(ignored);
        (void)broadcastTimer_.cancel();
        (void)playbackTimer_.cancel();
        if (sessionSocket_) {
            sessionSocket_->close(ignored);
        }
        io_.stop();
        if (ioThread_.joinable()) {
            ioThread_.join();
        }
        io_.restart();
        resetState();
    }

    VirtualControllerEndpoint endpoint() const {
        VirtualControllerEndpoint endpoint;
        const auto& info = target_.sink->targetInfo();
        endpoint.targetId = info.id;
        endpoint.label = "Ether Dream " + formatMac(mac_);
        endpoint.value = address_ + ":" + std::to_string(options_.tcpPort);
        endpoint.kind = "network-socket";
        endpoint.protocol = "Ether Dream";
        endpoint.transport = "tcp";
        endpoint.address = address_;
        endpoint.port = options_.tcpPort;
        endpoint.attributes["mac"] = formatMac(mac_);
        endpoint.attributes["discovery_port"] = std::to_string(options_.discoveryPort);
        endpoint.attributes["hardware_revision"] = "0";
        endpoint.attributes["software_revision"] =
            std::to_string(revisionPortOffset(options_.tcpPort));
        return endpoint;
    }

private:
    void startAccept() {
        auto socket = std::make_shared<tcp::socket>(io_);
        acceptor_.async_accept(*socket, [this, socket](const std::error_code& ec) {
            if (!running_.load(std::memory_order_acquire)) {
                return;
            }

            if (!ec) {
                handleAccepted(socket);
            }
            startAccept();
        });
    }

    void handleAccepted(const std::shared_ptr<tcp::socket>& socket) {
        if (sessionSocket_ && sessionSocket_->is_open()) {
            std::error_code ignored;
            socket->close(ignored);
            return;
        }

        std::error_code ignored;
        socket->set_option(tcp::no_delay(true), ignored);
        socket->set_option(asio::socket_base::keep_alive(true), ignored);
        sessionSocket_ = socket;
        resetState();
        sendResponse('?', 'a');
        startReadCommand();
    }

    void startReadCommand() {
        if (!sessionSocket_ || !sessionSocket_->is_open()) {
            clearSession();
            return;
        }

        auto command = std::make_shared<std::array<std::uint8_t, 1>>();
        asio::async_read(*sessionSocket_, asio::buffer(*command),
                         [this, command](const std::error_code& ec, std::size_t) {
                             if (ec) {
                                 clearSession();
                                 return;
                             }
                             handleCommand(static_cast<char>((*command)[0]));
                         });
    }

    void handleCommand(char command) {
        switch (command) {
        case '?':
            sendResponse(command, 'a');
            startReadCommand();
            break;
        case 'v':
            sendVersion();
            startReadCommand();
            break;
        case 'p':
            handlePrepare(command);
            startReadCommand();
            break;
        case 'b':
        case 'u':
            readBegin(command);
            break;
        case 'q':
            readRateChange(command);
            break;
        case 'd':
            readDataHeader();
            break;
        case 's':
            handleStop(command);
            startReadCommand();
            break;
        case 'c':
            handleClear(command);
            startReadCommand();
            break;
        case '\0':
        case static_cast<char>(0xff):
            handleEmergencyStop(command);
            startReadCommand();
            break;
        default:
            handleEmergencyStop(command);
            startReadCommand();
            break;
        }
    }

    void readBegin(char command) {
        readBytes(6, [this, command](std::vector<std::uint8_t> bytes) {
            const auto rate = libera::core::bytes::readLe32(bytes.data() + 2);
            std::uint8_t response = 'I';
            {
                std::lock_guard<std::mutex> lock(stateMutex_);
                if (rate > 0 && rate <= options_.maxPointRate &&
                    lightEngineState_ == LightEngineState::Ready &&
                    (command == 'u' || playbackState_ == PlaybackState::Prepared) &&
                    !pointBuffer_.empty()) {
                    pointRate_ = rate;
                    playbackState_ = PlaybackState::Playing;
                    playbackFlags_ |= playbackFlagShutter;
                    playbackClockValid_ = false;
                    response = 'a';
                }
            }
            sendResponse(command, response);
            startReadCommand();
        });
    }

    void readRateChange(char command) {
        readBytes(4, [this, command](std::vector<std::uint8_t> bytes) {
            const auto rate = libera::core::bytes::readLe32(bytes.data());
            std::uint8_t response = 'I';
            {
                std::lock_guard<std::mutex> lock(stateMutex_);
                if (playbackState_ != PlaybackState::Idle && rate > 0 && rate <= options_.maxPointRate) {
                    if (pointRateQueue_.size() >= maxPointRateQueue_) {
                        response = 'F';
                    } else {
                        pointRateQueue_.push_back(rate);
                        response = 'a';
                    }
                }
            }
            sendResponse(command, response);
            startReadCommand();
        });
    }

    void readDataHeader() {
        readBytes(2, [this](std::vector<std::uint8_t> bytes) {
            const auto pointCount = libera::core::bytes::readLe16(bytes.data());
            readDataPayload(pointCount);
        });
    }

    void readDataPayload(std::uint16_t pointCount) {
        readBytes(static_cast<std::size_t>(pointCount) * bytesPerPoint,
                  [this, pointCount](std::vector<std::uint8_t> bytes) {
                      std::vector<BufferedPoint> decoded;
                      decoded.reserve(pointCount);
                      const std::uint8_t* cursor = bytes.data();
                      for (std::uint16_t i = 0; i < pointCount; ++i) {
                          BufferedPoint point;
                          point.control = libera::core::bytes::readLe16(cursor);
                          cursor += 2;
                          const auto x = static_cast<std::int16_t>(libera::core::bytes::readLe16(cursor));
                          cursor += 2;
                          const auto y = static_cast<std::int16_t>(libera::core::bytes::readLe16(cursor));
                          cursor += 2;
                          const auto r = libera::core::bytes::readLe16(cursor);
                          cursor += 2;
                          const auto g = libera::core::bytes::readLe16(cursor);
                          cursor += 2;
                          const auto b = libera::core::bytes::readLe16(cursor);
                          cursor += 2;
                          const auto iValue = libera::core::bytes::readLe16(cursor);
                          cursor += 2;
                          const auto u1 = libera::core::bytes::readLe16(cursor);
                          cursor += 2;
                          const auto u2 = libera::core::bytes::readLe16(cursor);
                          cursor += 2;

                          point.point.x = coordFromEd(x);
                          point.point.y = coordFromEd(y);
                          point.point.r = channelFromEd(r);
                          point.point.g = channelFromEd(g);
                          point.point.b = channelFromEd(b);
                          point.point.i = iValue > 0
                              ? channelFromEd(iValue)
                              : ((r != 0 || g != 0 || b != 0) ? 1.0f : 0.0f);
                          point.point.u1 = channelFromEd(u1);
                          point.point.u2 = channelFromEd(u2);
                          decoded.push_back(point);
                      }

                      std::uint8_t response = 'I';
                      {
                          std::lock_guard<std::mutex> lock(stateMutex_);
                          if (playbackState_ != PlaybackState::Idle &&
                              lightEngineState_ == LightEngineState::Ready) {
                              if (pointBuffer_.size() + decoded.size() > options_.bufferCapacity) {
                                  response = 'F';
                              } else {
                                  for (auto& point : decoded) {
                                      pointBuffer_.push_back(std::move(point));
                                  }
                                  response = 'a';
                              }
                          }
                      }
                      sendResponse('d', response);
                      startReadCommand();
                  });
    }

    template <typename Handler>
    void readBytes(std::size_t size, Handler handler) {
        auto bytes = std::make_shared<std::vector<std::uint8_t>>(size);
        if (size == 0) {
            handler({});
            return;
        }
        asio::async_read(*sessionSocket_, asio::buffer(*bytes),
                         [this, bytes, handler = std::move(handler)](const std::error_code& ec, std::size_t) mutable {
                             if (ec) {
                                 clearSession();
                                 return;
                             }
                             handler(std::move(*bytes));
                         });
    }

    void handlePrepare(char command) {
        std::uint8_t response = 'I';
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            if (lightEngineState_ == LightEngineState::Ready &&
                playbackState_ == PlaybackState::Idle) {
                pointBuffer_.clear();
                pointRateQueue_.clear();
                pointCount_ = 0;
                playbackFlags_ &= ~(playbackFlagUnderflow | playbackFlagEstop | playbackFlagShutter);
                playbackState_ = PlaybackState::Prepared;
                playbackClockValid_ = false;
                response = 'a';
            }
        }
        if (response == 'a' && target_.sink) {
            target_.sink->reset();
        }
        sendResponse(command, response);
    }

    void handleStop(char command) {
        std::uint8_t response = 'I';
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            if (playbackState_ == PlaybackState::Prepared ||
                playbackState_ == PlaybackState::Playing) {
                stopPlaybackLocked();
                response = 'a';
            }
        }
        if (response == 'a' && target_.sink) {
            target_.sink->reset();
        }
        sendResponse(command, response);
    }

    void handleClear(char command) {
        bool shouldResetTarget = false;
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            lightEngineState_ = LightEngineState::Ready;
            lightEngineFlags_ = 0;
            playbackFlags_ &= ~(playbackFlagUnderflow | playbackFlagEstop);
            if (playbackState_ == PlaybackState::Idle) {
                pointBuffer_.clear();
                pointRateQueue_.clear();
                playbackClockValid_ = false;
                shouldResetTarget = true;
            }
        }
        if (shouldResetTarget && target_.sink) {
            target_.sink->reset();
        }
        sendResponse(command, 'a');
    }

    void handleEmergencyStop(char command) {
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            lightEngineState_ = LightEngineState::Estop;
            lightEngineFlags_ |= lightEngineFlagEstopCommand;
            playbackFlags_ |= playbackFlagEstop;
            stopPlaybackLocked();
        }
        if (target_.sink) {
            target_.sink->reset();
        }
        sendResponse(command, 'a');
    }

    void stopPlaybackLocked() {
        playbackState_ = PlaybackState::Idle;
        playbackFlags_ &= ~playbackFlagShutter;
        pointBuffer_.clear();
        pointRateQueue_.clear();
        playbackClockValid_ = false;
        playbackAccumulator_ = 0.0;
    }

    void clearSession() {
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            stopPlaybackLocked();
        }
        if (target_.sink) {
            target_.sink->reset();
        }
        std::error_code ignored;
        if (sessionSocket_) {
            sessionSocket_->close(ignored);
            sessionSocket_.reset();
        }
    }

    void sendVersion() {
        if (!sessionSocket_ || !sessionSocket_->is_open()) {
            return;
        }
        auto bytes = std::make_shared<std::array<std::uint8_t, 32>>();
        bytes->fill(0);
        const std::string version = "r3 libera-link";
        std::copy(version.begin(), version.end(), bytes->begin());
        std::error_code ignored;
        (void)asio::write(*sessionSocket_, asio::buffer(*bytes), ignored);
    }

    void sendResponse(char command, std::uint8_t response) {
        if (!sessionSocket_ || !sessionSocket_->is_open()) {
            return;
        }
        const auto buffer = buildResponse(command, response);
        std::error_code ignored;
        (void)asio::write(*sessionSocket_, asio::buffer(buffer), ignored);
    }

    std::vector<std::uint8_t> buildResponse(char command, std::uint8_t response) const {
        libera::core::ByteBuffer buffer;
        buffer.appendUInt8(response);
        buffer.appendUInt8(static_cast<std::uint8_t>(command));
        appendStatus(buffer);
        return std::vector<std::uint8_t>(buffer.data(), buffer.data() + buffer.size());
    }

    void appendStatus(libera::core::ByteBuffer& buffer) const {
        std::lock_guard<std::mutex> lock(stateMutex_);
        appendStatusLocked(buffer);
    }

    void appendStatusLocked(libera::core::ByteBuffer& buffer) const {
        buffer.appendUInt8(0);
        buffer.appendUInt8(static_cast<std::uint8_t>(lightEngineState_));
        buffer.appendUInt8(static_cast<std::uint8_t>(playbackState_));
        buffer.appendUInt8(0);
        buffer.appendUInt16(lightEngineFlags_);
        buffer.appendUInt16(playbackFlags_);
        buffer.appendUInt16(0);
        buffer.appendUInt16(static_cast<std::uint16_t>(
            std::min<std::size_t>(pointBuffer_.size(), std::numeric_limits<std::uint16_t>::max())));
        buffer.appendUInt32(playbackState_ == PlaybackState::Idle ? 0 : pointRate_);
        buffer.appendUInt32(playbackState_ == PlaybackState::Playing ? pointCount_ : 0);
    }

    std::vector<std::uint8_t> buildBroadcastPacket() const {
        libera::core::ByteBuffer buffer;
        for (std::uint8_t byte : mac_) {
            buffer.appendUInt8(byte);
        }
        buffer.appendUInt16(0); // hardware revision: virtual Ether Dream
        buffer.appendUInt16(revisionPortOffset(options_.tcpPort));
        buffer.appendUInt16(options_.bufferCapacity);
        buffer.appendUInt32(options_.maxPointRate);
        appendStatus(buffer);
        return std::vector<std::uint8_t>(buffer.data(), buffer.data() + buffer.size());
    }

    void startBroadcastTimer(std::chrono::milliseconds delay) {
        broadcastTimer_.expires_after(delay);
        broadcastTimer_.async_wait([this](const std::error_code& ec) {
            if (ec || !running_.load(std::memory_order_acquire)) {
                return;
            }
            sendBroadcast();
            startBroadcastTimer(std::chrono::milliseconds(options_.broadcastIntervalMs));
        });
    }

    void sendBroadcast() {
        if (!broadcastSocket_.is_open()) {
            return;
        }
        auto packet = std::make_shared<std::vector<std::uint8_t>>(buildBroadcastPacket());
        for (const auto& address : options_.broadcastAddresses) {
            std::error_code ec;
            const auto ip = asio::ip::make_address(address, ec);
            if (ec) {
                continue;
            }
            auto endpoint = std::make_shared<udp::endpoint>(ip, options_.discoveryPort);
            broadcastSocket_.async_send_to(asio::buffer(*packet), *endpoint,
                                           [packet, endpoint](const std::error_code&, std::size_t) {});
        }
    }

    void startPlaybackTimer() {
        playbackTimer_.expires_after(std::chrono::microseconds(options_.playbackSliceUs));
        playbackTimer_.async_wait([this](const std::error_code& ec) {
            if (ec || !running_.load(std::memory_order_acquire)) {
                return;
            }
            playbackTick();
            startPlaybackTimer();
        });
    }

    void playbackTick() {
        std::vector<LaserPoint> points;
        std::uint32_t effectiveRate = 0;

        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            if (playbackState_ != PlaybackState::Playing || pointRate_ == 0) {
                return;
            }

            const auto now = std::chrono::steady_clock::now();
            if (!playbackClockValid_) {
                lastPlaybackAt_ = now;
                playbackClockValid_ = true;
                return;
            }

            const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                now - lastPlaybackAt_);
            lastPlaybackAt_ = now;
            playbackAccumulator_ +=
                static_cast<double>(elapsed.count()) * static_cast<double>(pointRate_) / 1000000.0;
            const auto pointsDue = static_cast<std::size_t>(std::floor(playbackAccumulator_));
            if (pointsDue == 0) {
                return;
            }
            playbackAccumulator_ -= static_cast<double>(pointsDue);

            const auto toDrain = std::min<std::size_t>(pointsDue, pointBuffer_.size());
            points.reserve(toDrain);
            effectiveRate = pointRate_;
            for (std::size_t i = 0; i < toDrain; ++i) {
                BufferedPoint point = pointBuffer_.front();
                pointBuffer_.pop_front();
                if ((point.control & rateChangeBit) != 0 && !pointRateQueue_.empty()) {
                    pointRate_ = pointRateQueue_.front();
                    pointRateQueue_.pop_front();
                    effectiveRate = pointRate_;
                    playbackClockValid_ = false;
                    playbackAccumulator_ = 0.0;
                }
                points.push_back(point.point);
                ++pointCount_;
            }

            if (toDrain < pointsDue) {
                playbackState_ = PlaybackState::Idle;
                playbackFlags_ &= ~playbackFlagShutter;
                playbackFlags_ |= playbackFlagUnderflow;
                playbackClockValid_ = false;
                playbackAccumulator_ = 0.0;
            }
        }

        if (!points.empty() && target_.sink) {
            SliceSubmission submission;
            submission.effectivePointRate = effectiveRate > 0 ? effectiveRate : options_.maxPointRate;
            submission.durationUs = static_cast<std::uint32_t>(
                std::max<double>(1.0,
                                 (1000000.0 * static_cast<double>(points.size())) /
                                     static_cast<double>(*submission.effectivePointRate)));
            submission.points = std::move(points);
            target_.sink->submitContinuous(std::move(submission));
        }
    }

    void resetState() {
        std::lock_guard<std::mutex> lock(stateMutex_);
        lightEngineState_ = LightEngineState::Ready;
        playbackState_ = PlaybackState::Idle;
        lightEngineFlags_ = 0;
        playbackFlags_ = 0;
        pointRate_ = std::min<std::uint32_t>(30000u, options_.maxPointRate);
        pointCount_ = 0;
        pointBuffer_.clear();
        pointRateQueue_.clear();
        playbackAccumulator_ = 0.0;
        playbackClockValid_ = false;
    }

    Target target_;
    std::string address_;
    std::array<std::uint8_t, 6> mac_;
    HostOptions options_;

    asio::io_context io_;
    tcp::acceptor acceptor_;
    udp::socket broadcastSocket_;
    asio::steady_timer broadcastTimer_;
    asio::steady_timer playbackTimer_;
    std::thread ioThread_;
    std::shared_ptr<tcp::socket> sessionSocket_;
    std::atomic<bool> running_{false};

    mutable std::mutex stateMutex_;
    LightEngineState lightEngineState_ = LightEngineState::Ready;
    PlaybackState playbackState_ = PlaybackState::Idle;
    std::uint16_t lightEngineFlags_ = 0;
    std::uint16_t playbackFlags_ = 0;
    std::uint32_t pointRate_ = 30000;
    std::uint32_t pointCount_ = 0;
    std::deque<BufferedPoint> pointBuffer_;
    std::deque<std::uint32_t> pointRateQueue_;
    std::chrono::steady_clock::time_point lastPlaybackAt_{};
    double playbackAccumulator_ = 0.0;
    bool playbackClockValid_ = false;

    static constexpr std::size_t maxPointRateQueue_ = 100;
};

VirtualControllerHostRegistrar gEtherDreamVirtualControllerHostRegistrar({
    {
        "etherdream",
        "Ether Dream",
        "Expose linked controllers as virtual Ether Dream DACs.",
        {
            {
                "ip_mode",
                "IP Mode",
                "Address selection mode: auto, loopback, or explicit addresses.",
                VirtualControllerHostOptionType::Choice,
                "auto",
                {"auto", "loopback", "explicit"},
                false,
            },
            {
                "addresses",
                "Addresses",
                "Comma-separated local IPv4 addresses to use instead of automatic aliases.",
                VirtualControllerHostOptionType::String,
                "",
                {},
                false,
            },
            {
                "interface",
                "Interface",
                "Network interface to use for automatic LAN aliases.",
                VirtualControllerHostOptionType::String,
                "",
                {},
                false,
            },
            {
                "auto_manage_aliases",
                "Auto IP Aliases",
                "Automatically add and remove local IP aliases when needed.",
                VirtualControllerHostOptionType::Boolean,
                "true",
                {},
                false,
            },
        },
        false,
    },
    [](const VirtualControllerHostConfig& config) {
        return std::make_unique<EtherDreamVirtualControllerHost>(config);
    },
});

} // namespace

void ensureBuiltInEtherDreamVirtualControllerHostLinked() {}

struct EtherDreamVirtualControllerHost::Impl {
    explicit Impl(HostOptions hostOptions)
        : options(std::move(hostOptions))
        , aliasManager(options) {}

    HostOptions options;
    IpAliasManager aliasManager;
    std::vector<std::unique_ptr<VirtualEtherDreamDevice>> devices;
    std::vector<VirtualControllerEndpoint> endpoints;
    std::atomic<bool> active{false};
};

EtherDreamVirtualControllerHost::EtherDreamVirtualControllerHost(const VirtualControllerHostConfig& config)
    : impl_(std::make_unique<Impl>(makeHostOptions(config)))
    , config_(config) {}

EtherDreamVirtualControllerHost::~EtherDreamVirtualControllerHost() {
    stop();
}

std::string_view EtherDreamVirtualControllerHost::name() const {
    return "etherdream";
}

std::string_view EtherDreamVirtualControllerHost::displayName() const {
    return "Ether Dream";
}

bool EtherDreamVirtualControllerHost::start(const VirtualControllerHostContext& context,
                                            std::string& error) {
    if (!impl_) {
        error = "Ether Dream virtual controller host not initialized.";
        return false;
    }
    if (running()) {
        error = "Ether Dream virtual controller host is already running.";
        return false;
    }

    std::vector<Target> validTargets;
    validTargets.reserve(context.targets.size());
    for (const auto& target : context.targets) {
        if (target.sink) {
            validTargets.push_back(target);
        }
    }

    if (validTargets.empty()) {
        error = "No Ether Dream targets were created.";
        return false;
    }

    std::vector<std::string> addresses;
    if (!impl_->aliasManager.allocate(validTargets.size(), addresses, error)) {
        return false;
    }

    std::vector<std::unique_ptr<VirtualEtherDreamDevice>> devices;
    std::vector<VirtualControllerEndpoint> endpoints;
    devices.reserve(validTargets.size());
    endpoints.reserve(validTargets.size());

    for (std::size_t i = 0; i < validTargets.size(); ++i) {
        HostOptions deviceOptions = impl_->options;
        const auto mac = makeMacAddress(validTargets[i].sink->targetInfo(), i);
        auto device = std::make_unique<VirtualEtherDreamDevice>(
            validTargets[i], addresses[i], mac, deviceOptions);
        if (!device->start(error)) {
            for (auto& started : devices) {
                started->stop();
            }
            impl_->aliasManager.cleanup();
            return false;
        }
        endpoints.push_back(device->endpoint());
        devices.push_back(std::move(device));
    }

    impl_->devices = std::move(devices);
    impl_->endpoints = std::move(endpoints);
    impl_->active.store(true, std::memory_order_release);
    return true;
}

void EtherDreamVirtualControllerHost::stop() {
    if (!impl_) {
        return;
    }
    for (auto& device : impl_->devices) {
        if (device) {
            device->stop();
        }
    }
    impl_->devices.clear();
    impl_->endpoints.clear();
    impl_->aliasManager.cleanup();
    impl_->active.store(false, std::memory_order_release);
}

bool EtherDreamVirtualControllerHost::running() const {
    return impl_ && impl_->active.load(std::memory_order_acquire);
}

std::vector<VirtualControllerEndpoint> EtherDreamVirtualControllerHost::endpoints() const {
    return impl_ ? impl_->endpoints : std::vector<VirtualControllerEndpoint>{};
}

} // namespace libera_link::virtual_controller
