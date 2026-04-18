#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace idn_bridge {

struct BridgeOptions {
    std::uint32_t discoveryTimeoutMs = 5000;
    std::size_t maxDacs = 0;
    std::uint32_t sliceDurationUs = 15000;
    std::size_t maxQueuedPoints = 300000;
    std::uint32_t latencyMs = 50;
    std::uint32_t maxLatencyMs = 1500;
    bool autoLatency = true;
};

void printUsage(const char* exe);

enum class ParseResult {
    Ok,
    Help,
    Error
};

ParseResult parseOptions(int argc, char** argv, BridgeOptions& options);

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
    std::uint8_t serviceId = 0;
    EndpointStatsSnapshot stats;
};

struct DiscoveredControllerSnapshot {
    std::string label;
    std::string id;
    std::string type;
    std::uint32_t maxPointRate = 0;
    std::string usage;
    bool bridgeable = true;
    std::string note;
};

struct RuntimeSnapshot {
    RuntimeState state = RuntimeState::Stopped;
    std::string statusMessage = "Stopped";
    std::string lastError;
    bool stopRequested = false;
    bool hasDiscoveryResults = false;
    std::size_t discoveredControllers = 0;
    std::size_t startedEndpoints = 0;
    std::vector<DiscoveredControllerSnapshot> discovered;
    std::vector<EndpointSnapshot> endpoints;
    std::vector<std::string> recentLogs;
};

class BridgeRuntime {
public:
    BridgeRuntime();
    ~BridgeRuntime();

    BridgeRuntime(const BridgeRuntime&) = delete;
    BridgeRuntime& operator=(const BridgeRuntime&) = delete;

    void setEchoLogsToStdStreams(bool enabled);

    bool scan(const BridgeOptions& options);
    bool start(const BridgeOptions& options);
    bool start(const BridgeOptions& options, const std::set<std::string>& selectedControllerIds);
    void requestStop();
    void stop();

    RuntimeSnapshot snapshot() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace idn_bridge
