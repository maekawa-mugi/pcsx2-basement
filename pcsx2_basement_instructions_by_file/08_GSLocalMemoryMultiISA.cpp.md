# pcsx2/GS/GSLocalMemoryMultiISA.cpp

**Source:** [`pcsx2/GS/GSLocalMemoryMultiISA.cpp`](https://github.com/maekawa-mugi/pcsx2-basement/blob/main/pcsx2/GS/GSLocalMemoryMultiISA.cpp)

> Source-specific instructions only. `P1`/`P2`/`P3` are proposals; `EXISTING` items are already implemented. Match the current source, preserve CPU feature gates and emulation semantics, and verify generated code and performance before accepting changes.

- **[P2 · LM01 · L588–628, L630–665](https://github.com/maekawa-mugi/pcsx2-basement/blob/main/pcsx2/GS/GSLocalMemoryMultiISA.cpp#L588-L665)** — Benchmark BLK01/BLK02 via aligned `WriteImage24` and `WriteImage8H` callers, separately from the unaligned `WriteImageX` fallback.
- **[P3 · LM02 · L597–615](https://github.com/maekawa-mugi/pcsx2-basement/blob/main/pcsx2/GS/GSLocalMemoryMultiISA.cpp#L597-L615)** — Profile whether unaligned 24-bit image uploads warrant specialized block/tail handling. Current fallback is generic; this is a larger rewrite, not a proven simple instruction reduction.
