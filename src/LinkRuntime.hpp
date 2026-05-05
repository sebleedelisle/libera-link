#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace libera_link {

struct VirtualControllerRoute {
    std::string controllerId;
    std::string hostId;
    std::string hostInstanceKey;
    std::unordered_map<std::string, std::string> options;
};

struct LinkOptions {
    std::uint32_t discoveryTimeoutMs = 5000;
    std::size_t maxDacs = 0;
    std::uint32_t sliceDurationUs = 15000;
    std::size_t maxQueuedPoints = 300000;
    std::uint32_t latencyMs = 50;
    std::uint32_t maxLatencyMs = 1500;
    bool autoLatency = true;
    std::string virtualControllerHostId;
    std::unordered_map<std::string, std::string> virtualControllerHostOptions;
    std::vector<VirtualControllerRoute> virtualControllerRoutes;
    std::set<std::string> disabledControllerTypes{"IDN", "EtherDream"};
};

void printUsage(const char* exe);

enum class ParseResult {
    Ok,
    Help,
    Error
};

ParseResult parseOptions(int argc, char** argv, LinkOptions& options);

enum class RuntimeState {
    Stopped,
    Scanning,
    Starting,
    Running,
    StopRequested,
    Failed
};

const char* runtimeStateLabel(RuntimeState state);

struct EndpointStatsSnapshot {
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

struct EndpointSnapshot {
    std::string label;
    std::string id;
    std::string type;
    std::string virtualControllerHostId;
    std::string virtualControllerHostDisplayName;
    std::string virtualControllerEndpointLabel;
    std::string virtualControllerEndpointValue;
    std::string virtualControllerEndpointKind;
    std::string virtualControllerEndpointProtocol;
    std::string virtualControllerEndpointTransport;
    std::string virtualControllerEndpointAddress;
    std::uint16_t virtualControllerEndpointPort = 0;
    std::uint32_t virtualControllerEndpointChannels = 0;
    std::unordered_map<std::string, std::string> virtualControllerEndpointAttributes;
    EndpointStatsSnapshot stats;
};

struct DiscoveredControllerSnapshot {
    std::string label;
    std::string id;
    std::string type;
    std::uint32_t maxPointRate = 0;
    std::string usage;
    bool linkable = true;
    std::string note;
};

struct RuntimeSnapshot {
    RuntimeState state = RuntimeState::Stopped;
    std::string statusMessage = "Stopped";
    std::string lastError;
    std::string activeVirtualControllerHostId;
    std::string activeVirtualControllerHostDisplayName;
    bool stopRequested = false;
    bool hasDiscoveryResults = false;
    std::size_t discoveredControllers = 0;
    std::size_t startedEndpoints = 0;
    std::vector<DiscoveredControllerSnapshot> discovered;
    std::vector<EndpointSnapshot> endpoints;
    std::vector<std::string> recentLogs;
};

class LinkRuntime {
public:
    LinkRuntime();
    ~LinkRuntime();

    LinkRuntime(const LinkRuntime&) = delete;
    LinkRuntime& operator=(const LinkRuntime&) = delete;

    void setEchoLogsToStdStreams(bool enabled);

    bool scan(const LinkOptions& options);
    bool start(const LinkOptions& options);
    bool start(const LinkOptions& options, const std::set<std::string>& selectedControllerIds);
    void requestStop();
    void stop();

    RuntimeSnapshot snapshot() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace libera_link
