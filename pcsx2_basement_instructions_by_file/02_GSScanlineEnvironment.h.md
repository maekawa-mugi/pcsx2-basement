# pcsx2/GS/Renderers/SW/GSScanlineEnvironment.h

**Source:** [`pcsx2/GS/Renderers/SW/GSScanlineEnvironment.h`](https://github.com/maekawa-mugi/pcsx2-basement/blob/main/pcsx2/GS/Renderers/SW/GSScanlineEnvironment.h)

> Source-specific instructions only. `P1`/`P2`/`P3` are proposals; `EXISTING` items are already implemented. Match the current source, preserve CPU feature gates and emulation semantics, and verify generated code and performance before accepting changes.

**Related change:** Add constants only if actually used by the proposed GS scanline transformations in [`GSDrawScanlineCodeGenerator.all.cpp`](01_GSDrawScanlineCodeGenerator.all.cpp.md).

- **[P2 · ENV01 · L265–332](https://github.com/maekawa-mugi/pcsx2-basement/blob/main/pcsx2/GS/Renderers/SW/GSScanlineEnvironment.h#L265-L332)** — If testing GS01–GS11, add **shared, correctly aligned** constants for `0x0001` words, `0x0080` words, `0x80000000` dwords, `0x00ffffff`/`0x007fffff`, and `0xff000000` as needed; reuse current `g_const` and avoid duplicate tables.
- **[EXISTING · ENV02 · L275–332](https://github.com/maekawa-mugi/pcsx2-basement/blob/main/pcsx2/GS/Renderers/SW/GSScanlineEnvironment.h#L275-L332)** — Existing AVX-512 `WritePixel` lane mapping and GS texture 5551 conversion constants are already defined. Preserve their format mappings.
