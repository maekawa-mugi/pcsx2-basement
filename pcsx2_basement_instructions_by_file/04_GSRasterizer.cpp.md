# pcsx2/GS/Renderers/SW/GSRasterizer.cpp

**Source:** [`pcsx2/GS/Renderers/SW/GSRasterizer.cpp`](https://github.com/maekawa-mugi/pcsx2-basement/blob/main/pcsx2/GS/Renderers/SW/GSRasterizer.cpp)

> Source-specific instructions only. `P1`/`P2`/`P3` are proposals; `EXISTING` items are already implemented. Match the current source, preserve CPU feature gates and emulation semantics, and verify generated code and performance before accepting changes.

- **[P2 · RAS01 · L1401–1435](https://github.com/maekawa-mugi/pcsx2-basement/blob/main/pcsx2/GS/Renderers/SW/GSRasterizer.cpp#L1401-L1435)** — Implement/test the source TODO: hoist Windows x64 XMM6–15 save/restore from each JIT `DrawScanline` invocation to `Flush()` or a dedicated trampoline. Requires an ABI-safe private calling convention; per-call saves total 10 XMM stores and 10 loads.
