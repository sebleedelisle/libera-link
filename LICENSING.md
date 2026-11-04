# Licensing

Unless otherwise noted, project-authored files in this repository are licensed
under the GNU General Public License, version 3.

See:
- [LICENSE](LICENSE)

This is an OSI-approved open source license and allows commercial use, but if
someone distributes a modified version they must also provide the corresponding
source code under GPLv3.

## Third-party code and assets

Third-party components keep their own licenses and are not relicensed by the
repository root `LICENSE` file.

Important examples in this repository:
- `extern/libera-laser/`:
  `extern/libera-laser/LICENSE`
- `src/third_party/openidn/`:
  the University of Bonn BSD-style licence in
  [`src/third_party/openidn/LICENSE.openidn.md`](src/third_party/openidn/LICENSE.openidn.md),
  plus DexLogic / Dirk Apitz MIT notices in the server and protocol source files.
  Both full licence texts and copyright notices are reproduced in
  [`THIRD_PARTY_NOTICES.txt`](THIRD_PARTY_NOTICES.txt).
- IDN client code in `extern/libera-laser/libs/helios_dac/sdk/cpp/idn/`:
  includes DexLogic / Dirk Apitz MIT-licensed code. Its MIT notices are also
  reproduced in [`THIRD_PARTY_NOTICES.txt`](THIRD_PARTY_NOTICES.txt).
- `src/fonts/RobotoMedium.cpp` and `src/fonts/RobotoBold.cpp`:
  generated compressed C arrays from Roboto TTF files. Roboto is an upstream
  Google font distributed under the Apache License 2.0.
- `src/fonts/ForkAwesome.cpp` and `src/fonts/IconsForkAwesome.h`:
  generated from Fork Awesome font/icon metadata. Fork Awesome font files are
  distributed under the SIL Open Font License 1.1; other Fork Awesome project
  files carry their upstream licenses.

If a file or directory includes its own license text or upstream copyright
notice, follow that more specific notice for that material.

When redistributing generated font blobs, keep the corresponding upstream
license and attribution information with the source or release artifact.

## OpenIDN attribution and distribution

Libera Link's IDN support is based on OpenIDN, including work by DexLogic /
Dirk Apitz and the University of Bonn OpenIDN Laser DAC Framework team
(Matthias Frank, Leander Schickhaus and Christian Windeck), with HeliosPRO
adaptations by Gitle Mikkelsen. The vendored subset includes local modifications;
its provenance is recorded in
[`README.vendored.md`](src/third_party/openidn/README.vendored.md).

Retain the original file notices and the Bonn licence when distributing source.
When distributing binaries, include `THIRD_PARTY_NOTICES.txt` with the full Bonn
and MIT notices. An acknowledgement alone does not replace these licence texts.

CMake stages `LICENSE`, `LICENSING.md` and `THIRD_PARTY_NOTICES.txt` in
`build/licenses/`. Release packaging includes these documents in the macOS app
bundle, Linux AppImage, Windows portable ZIP and Windows installer. The GUI's
**Settings > Credits** window and the CLI's `--third-party-notices` option also
show the complete OpenIDN/IDN notice without needing an external file.
