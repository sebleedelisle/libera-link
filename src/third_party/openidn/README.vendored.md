This directory vendors the OpenIDN Link subset used by `libera-link`.

Source:
- Upstream repo previously tracked at `extern/helios_openidn`
- Base submodule commit: `72b7463e22f6d01e2f8a48779f86dbe2aa49d2b0`
- Imported from the local working tree on 2026-04-18 to preserve Link-specific edits present in that checkout

Scope:
- Only the source and header files required by the `openidn-core` target are copied here
- Standalone server entrypoints and unused hardware backends are intentionally excluded

Licensing:
- The upstream server license is copied as `LICENSE.openidn.md`
