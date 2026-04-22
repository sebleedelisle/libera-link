# Libera Link

## The Universal Translator for Lasers. 

This app is part of the Libera family of apps all with a single aim : Run any laser with any control software. This is a key part of that
aim. 

`Libera Link` finds laser controllers (either on the network, connected via USB or sound interfaces) and presents them in a way that is more compatible with your software. Initially it provides an IDN entry point, and makes it compatible with many open protocols including Ether Dream, Helios USB, LaserCube, AVB and more. 

In future, more entry points will be implemented, and the in-built plugin system allows more compatibility to be added by third parties in the future. 


## Build

```bash
cmake -S . -B build
cmake --build build -j
```

Preferred preset-based builds:

```bash
cmake --preset release
cmake --build --preset release --parallel
```

`LIBERA_LINK_OPENIDN_BRIDGE_MODE` controls bridge-specific OpenIDN behavior:

- `ON` (default): enables macOS/bridge behavior patches.
- `OFF`: keeps original OpenIDN behavior paths.

## CI / Release

Cross-platform GitHub Actions CI, signing, and release packaging are configured in
[`.github/workflows/build.yml`](.github/workflows/build.yml).

Release setup and required GitHub secrets/variables are documented in
[`docs/ci-release.md`](docs/ci-release.md).

## Run

```bash
./build/libera_link
```

Useful options:

```bash
./build/libera_link --ingester idn --discovery-timeout-ms 8000 --max-dacs 4
```

Pass custom ingester settings through repeated `--ingester-opt key=value` flags.

## Notes

- Streaming to hardware uses a Libera callback-backed queue.
- The built-in `idn` ingester is registered by default and exposes controllers as OpenIDN / IDN services.
- Already-IDN Helios network DACs are skipped automatically (only non-IDN DACs are bridged).

## Custom Ingesters

Custom ingesters can be linked into `libera_link_core` by implementing
`libera_link::ingest::Ingester` and registering a factory:

```cpp
#include "ingest/IngesterRegistry.hpp"

using namespace libera_link::ingest;

static IngesterRegistrar gCustomIngester({
    {
        "custom",
        "Custom",
        "Expose Libera Link through a custom ingress protocol.",
        false,
    },
    [](const FactoryConfig& config) -> std::unique_ptr<Ingester> {
        return std::make_unique<MyCustomIngester>(config);
    },
});
```

Once linked in, the ingester appears in the GUI selector and can be chosen from
the CLI with `--ingester custom`.

## Licensing

Unless otherwise noted, project-authored files in this repository are licensed
under the GNU General Public License v3.0 (`LICENSE`).

Third-party code keeps its own upstream licenses; see `LICENSING.md`.
