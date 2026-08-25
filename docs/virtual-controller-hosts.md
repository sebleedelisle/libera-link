# Writing a Virtual Controller Host

A virtual controller host presents linked Libera controllers to external
laser software. It receives control data from that software and submits
normalized Libera points to linked controllers.

This is the opposite side of a Libera controller backend:

- a controller backend talks to hardware
- a virtual controller host talks to software that wants to control the hardware

## The Mental Model

A virtual controller host should answer three questions:

1. How does external software see or address linked controllers?
2. How does external control data become `libera::core::LaserPoint` values?
3. Is the external data stream-like, frame-like, or both?

The virtual controller host should not discover DACs, connect hardware, or
implement Libera's output scheduler. `LinkRuntime` already does that.

## Main Types

The current API lives in [VirtualControllerHost.hpp](../src/virtual_controller/VirtualControllerHost.hpp).

- `VirtualControllerHost`: lifecycle object for one software-facing controller family.
- `TargetSink`: a linked output target that accepts points, frames, and
  transport-side control values such as scanner sync.
- `SliceSubmission`: one timed batch of normalized laser points.
- `FrameSubmission`: a whole frame expressed as one or more slices.
- `SubmissionResult`: accepted/dropped point counts and current target status after a submission.
- `TargetStatus`: queue, latency, rate, dropped-point, and underrun telemetry for a target.
- `VirtualControllerEndpoint`: user-facing mapping from a target to protocol-specific endpoint data.
- `VirtualControllerHostOption`: structured option metadata for CLI and GUI integration.
- `VirtualControllerHostConfig`: global options passed to the virtual controller host factory.

## Minimal Continuous Host

Use `submitContinuous()` when the protocol provides a stream of timed point
batches.

```cpp
class MyVirtualControllerHost final : public libera_link::virtual_controller::VirtualControllerHost {
public:
    explicit MyVirtualControllerHost(const VirtualControllerHostConfig& config)
        : sliceDurationUs(config.sliceDurationUs) {}

    std::string_view name() const override {
        return "my-controller";
    }

    std::string_view displayName() const override {
        return "My Controller";
    }

    bool start(const VirtualControllerHostContext& context, std::string& error) override {
        if (context.targets.empty()) {
            error = "No targets available.";
            return false;
        }

        targets = context.targets;
        runningFlag = true;
        worker = std::thread([this] { protocolLoop(); });
        return true;
    }

    void stop() override {
        runningFlag = false;
        closeProtocolSocket();
        if (worker.joinable()) {
            worker.join();
        }
        targets.clear();
        currentEndpoints.clear();
    }

    bool running() const override {
        return runningFlag;
    }

    std::vector<VirtualControllerEndpoint> endpoints() const override {
        return currentEndpoints;
    }

private:
    void protocolLoop() {
        while (runningFlag) {
            ExternalPacket packet = receiveExternalPacket();
            Target* target = lookupTarget(packet.destination);
            if (target == nullptr || !target->sink) {
                continue;
            }

            SliceSubmission submission;
            submission.durationUs = packet.durationUs;
            submission.effectivePointRate = packet.pointRate;
            submission.points = decodeExternalPoints(packet);

            const SubmissionResult result =
                target->sink->submitContinuous(std::move(submission));
            updateProtocolStatus(packet.client, result.status);
        }
    }

    std::uint32_t sliceDurationUs = 15000;
    std::atomic<bool> runningFlag{false};
    std::thread worker;
    std::vector<Target> targets;
    std::vector<VirtualControllerEndpoint> currentEndpoints;
};
```

## Minimal Frame Host

Use `replaceFrame()` when the external protocol sends a complete frame that
should replace what is currently queued for that target. Use `submitFrame()`
when the protocol sends complete frames that should be appended or scheduled
without clearing pending target data.

```cpp
void MyVirtualControllerHost::handleFramePacket(const ExternalFrame& frame) {
    Target* target = lookupTarget(frame.destination);
    if (target == nullptr || !target->sink) {
        return;
    }

    SliceSubmission slice;
    slice.durationUs = frame.durationUs;
    slice.effectivePointRate = frame.pointRate;
    slice.points = decodeFramePoints(frame);

    FrameSubmission submission;
    submission.clearTransportPrefetch = true;
    submission.slices.push_back(std::move(slice));

    const SubmissionResult result = target->sink->replaceFrame(std::move(submission));
    updateProtocolStatus(frame.client, result.status);
}
```

`clearTransportPrefetch` is useful when the new frame should replace pending
prefetched callback data in the Libera controller path.

## Target Status

`TargetSink` submissions return `SubmissionResult`. Hosts that emulate a DAC
with buffer/status replies can use this to answer the client without reaching
into `LinkRuntime`.

```cpp
TargetStatus status = target->sink->status();

EtherDreamStatus reply;
reply.fullness = status.totalBufferedPoints;
reply.pointRate = status.outputPointRate;
reply.underruns = status.underrunEvents;
sendStatus(reply);
```

`TargetSink` calls are designed to be thread-safe. A host may submit to
different targets from different protocol threads. Submissions to the same
target are serialized internally by the target sink.

## Registration

Source-linked virtual controller hosts register a factory with
`VirtualControllerHostRegistrar`.

```cpp
#include "virtual_controller/VirtualControllerHostRegistry.hpp"

using namespace libera_link::virtual_controller;

static VirtualControllerHostRegistrar gMyVirtualControllerHost({
    {
        "my-controller",
        "My Controller",
        "Expose linked controllers through My Controller.",
        {
            {
                "port",
                "Port",
                "Network port to listen on.",
                VirtualControllerHostOptionType::Integer,
                "7654",
            },
        },
        false, // default GUI/CLI selection
        false, // shared instance; set true for one host instance per target
    },
    [](const VirtualControllerHostConfig& config) -> std::unique_ptr<VirtualControllerHost> {
        return std::make_unique<MyVirtualControllerHost>(config);
    },
});
```

Once the object file is linked into `libera_link_core`, the virtual controller
host appears in the GUI selector and can be selected from the CLI:

```bash
./build/libera_link --virtual-controller my-controller
```

When a host registration lives in a static library, make sure the object file is
not discarded by the linker. The built-in hosts use a small
`ensureBuiltIn...Linked()` function and call it before querying the registry.

## Controller Routes

`LinkOptions::virtualControllerHostId` is the default host for selected
controllers. `LinkOptions::virtualControllerRoutes` can override that per
controller:

```cpp
LinkOptions options;
options.virtualControllerHostId = "idn";

VirtualControllerRoute route;
route.controllerId = "etherdream:01020304";
route.hostId = "libera";
options.virtualControllerRoutes.push_back(std::move(route));
```

Routes are grouped into host instances before startup. By default, controllers
using the same host and options share one host instance. If a host type needs a
separate server or virtual device per physical target, register it with
`separateInstancePerTarget = true`. A route can also set `hostInstanceKey`
explicitly when a host needs custom grouping.

If a running host returns `true` from `supportsDynamicTargets()`, `LinkRuntime`
may call `addTarget()` or `removeTarget()` when the selected controller set
changes. Hosts that keep the default `false` are restarted when their target
membership changes.

## Endpoint Information

Endpoints explain how the external protocol exposes each linked target. For IDN,
this is the service ID.

```cpp
bool MyVirtualControllerHost::start(const VirtualControllerHostContext& context, std::string& error) {
    currentEndpoints.clear();

    for (std::size_t i = 0; i < context.targets.size(); ++i) {
        const auto& target = context.targets[i];
        if (!target.sink) {
            continue;
        }

        const std::string endpointValue = allocateProtocolEndpoint(i);

        VirtualControllerEndpoint exposedEndpoint;
        exposedEndpoint.targetId = target.sink->targetInfo().id;
        exposedEndpoint.label = "My Controller endpoint";
        exposedEndpoint.value = endpointValue;
        exposedEndpoint.kind = "network-socket";
        exposedEndpoint.protocol = "My Controller";
        exposedEndpoint.transport = "tcp";
        exposedEndpoint.address = "0.0.0.0";
        exposedEndpoint.port = configuredPort;
        currentEndpoints.push_back(std::move(exposedEndpoint));
    }

    startProtocolServer();
    return true;
}
```

## Lifecycle

```text
app starts
+-- LinkRuntime discovers and connects controllers
+-- LinkRuntime creates one TargetSink per controller
+-- createVirtualControllerHost(id, config)
+-- host.start(context)
|   +-- store targets
|   +-- expose protocol endpoints
|   +-- start protocol/network threads
|   +-- publish endpoints
+-- external protocol submits slices or frames
+-- LinkRuntime drains target queues into Libera callbacks
+-- host.stop()
    +-- stop protocol/network threads
    +-- reset protocol state
    +-- release target references
```

## Practical Rules

- Treat `TargetSink` as the only way to send points to hardware.
- Keep protocol parsing and hardware output separate.
- Call `reset()` when your protocol session closes or changes ownership.
- Provide useful `VirtualControllerEndpoint`; it is what users see in logs and the GUI.
- Provide structured `VirtualControllerHostOption` entries for settings that should appear in UI or CLI.
- Use `SubmissionResult` and `TargetStatus` when your emulated protocol needs buffer or readiness replies.
- Clamp and validate protocol data before constructing `LaserPoint` values.
- Implement dynamic target add/remove only when the protocol can publish new or
  removed endpoints without invalidating active clients.
- Keep `stop()` prompt. It may be called while network or protocol threads are active.

The current API is intentionally small. If a virtual controller host needs
richer events, client-session state, or OS-specific device lifecycle hooks, that
should be added to the shared virtual controller host contract rather than
hidden inside one virtual controller host implementation.
