# Libera Link

This app is part of the Libera family of apps all with a single aim : Run any laser with any control software. This is a key part of that
aim. 

`Libera Link` finds laser controllers on the network and presents them in a way that is more compatible with your software. Initially it provides an IDN entry point, and makes it compatible with as many open protocols as I can find. 

In future, more entry points will be implemented, and there is a plugin system so more compatibility can be added by third parties. 


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
./build/libera_link --idn-port 7255 --discovery-timeout-ms 8000 --max-dacs 4
```

## Notes

- Streaming to hardware uses a Libera callback-backed queue.
- Point data and effective point-rate changes coming from IDN chunks are translated to Libera point streams.
- Already-IDN Helios network DACs are skipped automatically (only non-IDN DACs are bridged).

## Licensing

Unless otherwise noted, project-authored files in this repository are licensed
under the GNU General Public License v3.0 (`LICENSE`).

Third-party code keeps its own upstream licenses; see `LICENSING.md`.
