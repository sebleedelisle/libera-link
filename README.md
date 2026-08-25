# Libera Link

Libera Link is a desktop and command-line bridge for laser controllers. It
discovers controllers through Libera, then exposes them as software-facing
virtual controllers that other laser software can talk to.

The default virtual controller is IDN, so controllers such as Ether Dream,
Helios USB, LaserCube, AVB, and plugin-backed Libera controllers can be made
visible to IDN-aware software. Libera Link can also expose linked controllers
through the Libera protocol for tools that speak Libera directly.

This is useful when the output hardware is not on the same machine as the
software driving it. For example, Helios USB controllers can be connected to a
small computer at stage while another computer at front of house sends control
data over the network.

## OMNIA LIBERA INTER SE

### Any software. Any laser.

Libera Link is part of the growing Libera ecosystem - interoperable tools
designed to connect lasers and software without restriction.

The laser industry has been held back by closed systems and vendor lock-in.
Libera is built to break that cycle.

## Using The App

Start Libera Link, wait for the controller scan to finish, choose a virtual
controller host, then enable the physical controllers you want to link. The app
starts the selected host and shows one active endpoint per enabled controller.

The main window shows:

- discovered controllers
- whether each controller can be linked
- the selected virtual controller
- active endpoint IDs
- input/output point rates
- buffering, underrun, and dropped-point counters
- recent runtime logs

The Settings window can disable specific Libera controller types from
discovery. Disabled types are not constructed, so their discovery sockets,
USB scans, plugin backends, and background threads are not started.

The IDN controller manager is disabled by default because already-IDN Helios
network controllers expose the same protocol that Libera Link would provide for
them. Use the CLI or settings UI to enable it when you explicitly want to scan
IDN controllers as physical outputs.

## Virtual Controllers

A virtual controller is the software-facing side of Libera Link. Its host
presents linked Libera controllers as something another laser-control program
can talk to.

The app currently exposes these built-in virtual controller hosts:

- `idn`: exposes linked controllers as IDN services. This is the default.
- `libera`: exposes linked controllers through the Libera protocol with UDP
  discovery advertisements and TCP sessions.

An Ether Dream virtual-controller host implementation also exists in-tree for
tests and future UI work, but the app currently loads the `idn` and `libera`
host registrations.

More virtual controller hosts can be added in code. See [Writing a virtual
controller host](docs/virtual-controller-hosts.md).

## Building From Source

Clone submodules before configuring:

```bash
git submodule update --init --recursive
```

Preferred local build:

```bash
cmake --preset release
cmake --build --preset release --parallel
```

Useful presets:

- `release`: macOS or other Ninja release build, with the GUI enabled.
- `debug`: macOS or other Ninja debug build, with the GUI enabled.
- `linux-release`: Linux release build using the system libusb package.
- `win-release`: Windows Visual Studio 2022 release build.
- `win-debug`: Windows Visual Studio 2022 debug build.

The GUI target fetches pinned GLFW and Dear ImGui sources through CMake
FetchContent unless `LIBERA_LINK_GUI_GLFW_SOURCE_DIR` and
`LIBERA_LINK_GUI_IMGUI_SOURCE_DIR` point at existing source trees.

## Command Line

The GUI is the normal way to use Libera Link, but the CLI can run the same link
runtime:

```bash
./build/libera_link --virtual-controller idn --discovery-timeout-ms 8000 --max-dacs 4
```

Libera protocol example:

```bash
./build/libera_link --virtual-controller libera --virtual-controller-opt tcp_port=18000
```

Useful options:

- `--virtual-controller <id>` selects the virtual controller to expose.
- `--virtual-controller-opt key=value` passes custom options to the selected
  virtual controller.
- `--max-dacs <count>` limits how many discovered controllers are linked.
- `--disable-controller-type <type>` prevents a Libera controller manager type
  from being constructed or discovered.
- `--enable-controller-type <type>` re-enables a default-disabled controller
  manager type.
- `--latency-ms <ms>` sets the starting target buffer latency.
- `--max-latency-ms <ms>` sets the upper limit for automatic latency increases.
- `--no-auto-latency` disables automatic latency increases after underruns.

Run `./build/libera_link --help` for the full list of options, virtual
controller hosts, and available controller manager types.

## Safety

Libera Link streams live laser output to real hardware. Use appropriate safety
procedures, low power during setup, and verify your projection path before
enabling output.

## Developer Docs

- [Internals](docs/internals.md): architecture, build notes, and runtime flow.
- [Writing a virtual controller host](docs/virtual-controller-hosts.md):
  pseudo-code for adding a new software-facing controller.
- [CI and release](docs/ci-release.md): packaging, signing, and release setup.

## Licensing

Unless otherwise noted, project-authored files in this repository are licensed
under the GNU General Public License v3.0 (`LICENSE`).

Third-party code keeps its own upstream licenses; see `LICENSING.md`.
