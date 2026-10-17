# pcsx2/GS/GSClut.cpp

**Source:** [`pcsx2/GS/GSClut.cpp`](https://github.com/maekawa-mugi/pcsx2-basement/blob/main/pcsx2/GS/GSClut.cpp)

> Source-specific instructions only. `P1`/`P2`/`P3` are proposals; `EXISTING` items are already implemented. Match the current source, preserve CPU feature gates and emulation semantics, and verify generated code and performance before accepting changes.

- **[P2 · CLU01 · L866–900](https://github.com/maekawa-mugi/pcsx2-basement/blob/main/pcsx2/GS/GSClut.cpp#L866-L900)** — Test AVX-512 comparison masks plus alpha selection for `GSClut::Expand16` (TA0/TA1, AEM) to reduce temporary boolean vectors. Check transparent-zero semantics and output equality.
