# Libera Link Internals

This note is for people working on Libera Link itself. If you only want to run
the app, start with the top-level [README](../README.md).

## Build

Clone submodules first:

```bash
git submodule update --init --recursive
```

```bash
cmake -S . -B build
cmake --build build -j
```

Preferred preset-based build:

```bash
cmake --preset release
cmake --build --preset release --parallel
```

Important CMake options:

- `LIBERA_LINK_BUILD_GUI`: builds the native GUI target.
- `LIBERA_LINK_USE_BUNDLED_LIBUSB`: uses bundled libusb from the
  `libera-laser` dependency tree. It defaults to `ON` for macOS and Windows,
  and `OFF` for Linux.
- `LIBERA_LINK_OPENIDN_LINK_MODE`: enables Libera Link IDN behavior paths. The
  vendored IDN subset currently requires this to stay `ON`.
- `LIBERA_LINK_BUILD_HARDWARE_TESTS`: enables tests that require hardware.
- `LIBERA_LINK_GUI_GLFW_SOURCE_DIR`: optional path to an existing GLFW source
  tree.
- `LIBERA_LINK_GUI_IMGUI_SOURCE_DIR`: optional path to an existing Dear ImGui
  source tree.

If the GUI dependency paths are not set or are invalid, CMake fetches pinned
GLFW and Dear ImGui revisions through FetchContent.

## Source Layout

- `src/LinkRuntime.*`: runtime orchestration, discovery, linking, queueing, stats, and shutdown.
- `src/virtual_controller/VirtualControllerHost.hpp`: public virtual controller host and target-sink contracts.
- `src/virtual_controller/VirtualControllerHostRegistry.*`: source-linked virtual controller host registration and factory lookup.
- `src/virtual_controller/IdnVirtualControllerHost.*`: built-in IDN virtual controller host.
- `src/virtual_controller/LiberaProtocolVirtualControllerHost.*`: built-in
  Libera protocol virtual controller host with UDP discovery and TCP sessions.
- `src/virtual_controller/EtherDreamVirtualControllerHost.*`: experimental
  Ether Dream virtual controller host, including TCP command emulation, UDP
  discovery beacons, and local IP alias allocation. It is compiled and tested,
  but the app does not currently load its registration while the input protocol
  settings model is being designed.
- `src/third_party/openidn`: vendored IDN subset used by the IDN virtual controller host.
- `src/main.cpp`: CLI entry point.
- `src/gui_main.cpp`: native GUI entry point.

## Runtime Model

Libera Link is split into two sides:

```text
external protocol
    -> virtual controller host
    -> TargetSink
    -> LinkRuntime queue
    -> Libera point callback
    -> controller backend
    -> hardware
```

The virtual controller host owns the software-facing controller surface. It
does not discover or connect laser hardware. Instead, it receives a list of
targets from `LinkRuntime` and submits point data to those targets.

`LinkRuntime` owns the hardware side. It:

- discovers controllers through `libera::System`
- passes disabled controller types into `libera::System` before manager construction
- filters controllers that should not be linked
- connects selected controllers
- wraps each controller in a `TargetSink`
- creates and starts the selected virtual controller host
- records endpoint stats
- stops the virtual controller host and releases controllers on shutdown

Controller discovery settings are applied before Libera managers are
constructed. This matters for managers such as Ether Dream, LaserCube Net, and
plugins, because construction may bind sockets, create backend state, or start
discovery threads.

`LinkOptions::disabledControllerTypes` disables the `IDN` controller manager by
default so Libera Link does not rediscover already-IDN controllers as physical
outputs unless the user explicitly enables that manager.

`LinkOptions::virtualControllerRoutes` can assign different selected
controllers to different host IDs or option sets. During a running update,
`LinkRuntime` keeps compatible active hosts when they support dynamic target
changes and starts/stops host instances as needed.

## Target Queueing

`LinkRuntime` wraps each connected controller in a target sink. That target sink
accepts either continuous point slices or frame replacements from a virtual
controller host.

For continuous input, the target appends points to a local queue. For frame
input, the target replaces the current local queue with the new frame contents.

The target installs a Libera point callback on the controller. When the
controller backend asks for more points, the callback drains the local queue.
If there are not enough points ready, the callback emits blank fallback points
and records an underrun.

The runtime can increase target latency after repeated underruns, unless
`--no-auto-latency` is used.

## Built-In IDN Virtual Controller Host

The IDN virtual controller host creates one IDN service per linked target. Each service
uses the target label in its advertised name and receives a service ID starting
at `1`.

IDN packets enter through the vendored `SockIDNServer`. The IDN session
decodes incoming chunks into `ISPDB25Point` data, converts that into Libera
`LaserPoint` values, then submits either continuous slices or frame
replacements to the target sink.

The host publishes service endpoints on UDP port `7255` and assigns service
IDs starting at `1`.

## Built-In Libera Protocol Virtual Controller Host

The Libera protocol virtual controller host creates one TCP session endpoint
per linked target and advertises those endpoints over UDP discovery. It accepts
Libera protocol frame and stream data, maps point samples into Libera
`LaserPoint` values, and submits frames or continuous slices to the target
sink.

Important host options include:

- `listen_address`: local TCP bind address, default `0.0.0.0`
- `advertised_address`: optional address reported to senders
- `tcp_port`: base TCP session port
- `discovery_port`: UDP discovery advertisement port
- `broadcast_addresses`: comma-separated UDP advertisement destinations
- `broadcast_interval_ms`: discovery advertisement interval
- `max_frame_points`: maximum accepted points in one counted frame
- `max_user_channels`: accepted user channel count; Libera Link maps `u1` and
  `u2`

This host supports adding and removing targets while running, which lets the
GUI update compatible Libera protocol routes without restarting the whole link.

## Built-In Ether Dream Virtual Controller Host

The Ether Dream virtual controller host creates one virtual DAC per linked
target. Each DAC sends Ether Dream UDP discovery beacons and accepts the
standard TCP command stream, including prepare, data, begin, stop, clear,
ping, point-rate queueing, and the common `v`/`u` firmware commands seen in
the older tools.

Because Ether Dream clients expect every DAC to listen on TCP port `7765`, the
host gives each virtual DAC its own local IPv4 address. With multiple targets,
the default `ip_mode=auto` path allocates aliases on the active LAN interface;
`addresses=ip1,ip2,...` can be used when the addresses are preconfigured.

This host is currently not loaded by the main app. The implementation and tests
remain in-tree so it can be re-enabled once input protocol settings are
explicit in the UI.

## Public API Direction

The current virtual controller host API is source-linked and lives in `src/virtual_controller`. It is a
good seed for a reusable library, but it is not yet a stable external ABI.

Before treating it as a standalone library, the main areas to formalize are:

- lifecycle guarantees for `start()`, `stop()`, and `endpoints()`
- validation and UI rendering for structured virtual controller host options
- endpoint state updates after startup
- error and event reporting
- a stable binary/plugin ABI for third-party host implementations
- OS-specific host lifecycles such as virtual audio devices

See [Writing a virtual controller host](virtual-controller-hosts.md) for the current shape.
