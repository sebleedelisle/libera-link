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
  `src/third_party/openidn/LICENSE.openidn.md`
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
