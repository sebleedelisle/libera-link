# Libera Link Internals

This note is for people working on Libera Link itself. If you only want to run
the app, start with the top-level [README](../README.md).

## Build

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

- `LIBERA_LINK_BUILD_GUI`: builds the native GUI target when ImGui and GLFW are available.
- `LIBERA_LINK_USE_BUNDLED_LIBUSB`: uses the bundled libusb from the `libera-laser` dependency tree.
- `LIBERA_LINK_OPENIDN_LINK_MODE`: enables the Libera Link OpenIDN behavior paths. The vendored OpenIDN subset currently requires this to stay `ON`.
- `LIBERA_LINK_BUILD_HARDWARE_TESTS`: enables tests that require hardware.

## Source Layout

- `src/LinkRuntime.*`: runtime orchestration, discovery, linking, queueing, stats, and shutdown.
- `src/virtual_controller/VirtualControllerHost.hpp`: public virtual controller host and target-sink contracts.
- `src/virtual_controller/VirtualControllerHostRegistry.*`: source-linked virtual controller host registration and factory lookup.
- `src/virtual_controller/IdnVirtualControllerHost.*`: built-in OpenIDN / IDN virtual controller host.
- `src/third_party/openidn`: vendored OpenIDN subset used by the IDN virtual controller host.
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

The IDN virtual controller host creates one OpenIDN service per linked target. Each service
uses the target label in its advertised name and receives a service ID starting
at `1`.

OpenIDN packets enter through the vendored `SockIDNServer`. The IDN session
decodes incoming chunks into `ISPDB25Point` data, converts that into Libera
`LaserPoint` values, then submits either continuous slices or frame
replacements to the target sink.

## Public API Direction

The current virtual controller host API is source-linked and lives in `src/virtual_controller`. It is a
good seed for a reusable library, but it is not yet a stable external ABI.

Before treating it as a standalone library, the main areas to formalize are:

- lifecycle guarantees for `start()`, `stop()`, and `endpoints()`
- validation and UI rendering for structured virtual controller host options
- endpoint state updates after startup
- error and event reporting
- OS-specific host lifecycles such as virtual audio devices

See [Writing a virtual controller host](virtual-controller-hosts.md) for the current shape.
