# Related implementation unit: IPU AVX-512 implementation and dispatch

**Sources:** [`pcsx2/x86/InterpreterAVX512.cpp`](https://github.com/maekawa-mugi/pcsx2-basement/blob/main/pcsx2/x86/InterpreterAVX512.cpp), [`pcsx2/IPU/IPU_MultiISA.cpp`](https://github.com/maekawa-mugi/pcsx2-basement/blob/main/pcsx2/IPU/IPU_MultiISA.cpp)

> Source-specific instructions only. `P1`/`P2`/`P3` are proposals; `EXISTING` items are already implemented. Match the current source, preserve CPU feature gates and emulation semantics, and verify generated code and performance before accepting changes.

- **[EXISTING · IPU01 · L174–354](https://github.com/maekawa-mugi/pcsx2-basement/blob/main/pcsx2/x86/InterpreterAVX512.cpp#L174-L354)** — AVX-512 threshold/sign `VPTESTN`/ternary logic, alpha mask comparisons, YUV→RGB VBMI permutation, palette VQ lookup/tie-breaking, and dithering paths are already implemented. Prioritize correctness/benchmark validation, not duplicate k-mask rewrites.
- **[EXISTING · IPU02 · L1398–1400, L1915–1930, L1972–1985](https://github.com/maekawa-mugi/pcsx2-basement/blob/main/pcsx2/IPU/IPU_MultiISA.cpp#L1915-L1930)** — Confirm CPU feature dispatch actually selects the existing AVX-512 IPU implementation and preserves fallback behavior.
