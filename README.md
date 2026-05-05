# Libera Link


Libera Link is a translator app for laser controllers. It finds controllers
through Libera, then exposes them as software-facing virtual controllers that
other laser software can talk to.

The built-in virtual controller is IDN. That means
controllers such as Ether Dream, Helios USB, LaserCube, AVB, and plugin-backed
controllers can be made visible to IDN-aware software.

It can also provide a network interface for USB devices - so connect all of your Helios
to a small computer at stage and talk to it from another computer at front of house. 

More end points will be added soon, along with a plug-in architecture for the host side as well as the output side. 


## OMNIA LIBERA INTER SE
### Any software. Any laser.

Libera Link is part of the growing Libera ecosystem - interoperable tools
designed to connect lasers and software without restriction.

The laser industry has been held back by closed systems and vendor lock-in.
Libera is built to break that cycle.

## Using The App

Start Libera Link, wait for the controller scan to finish, then enable the
controllers you want to link. The app will start the selected virtual controller
and show one active endpoint per enabled controller.

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

Already-IDN Helios network controllers are skipped automatically because they
already expose the protocol Libera Link would provide for them.

## Virtual Controllers

A virtual controller is the software-facing side of Libera Link. Its host
presents linked Libera controllers as something another laser-control program
can talk to.

The built-in virtual controller is:

- `idn`: exposes linked controllers as IDN services.

More virtual controller hosts can be added in code. See
[Writing a virtual controller host](docs/virtual-controller-hosts.md).

## Command Line

The GUI is the normal way to use Libera Link, but the CLI can run the same link
runtime:

```bash
./build/libera_link --virtual-controller idn --discovery-timeout-ms 8000 --max-dacs 4
```

Useful options:

- `--virtual-controller <id>` selects the virtual controller to expose.
- `--virtual-controller-opt key=value` passes custom options to the selected
  virtual controller.
- `--max-dacs <count>` limits how many discovered controllers are linked.
- `--disable-controller-type <type>` prevents a Libera controller manager type
  from being constructed or discovered.
- `--latency-ms <ms>` sets the starting target buffer latency.
- `--no-auto-latency` disables automatic latency increases after underruns.

Run `./build/libera_link --help` for the full list.

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
