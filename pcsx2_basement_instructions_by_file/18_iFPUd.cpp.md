# pcsx2/x86/iFPUd.cpp

**Source:** [`pcsx2/x86/iFPUd.cpp`](https://github.com/maekawa-mugi/pcsx2-basement/blob/main/pcsx2/x86/iFPUd.cpp)

> Source-specific instructions only. `P1`/`P2`/`P3` are proposals; `EXISTING` items are already implemented. Match the current source, preserve CPU feature gates and emulation semantics, and verify generated code and performance before accepting changes.

- **[EXISTING · FPUD01 · L140–170](https://github.com/maekawa-mugi/pcsx2-basement/blob/main/pcsx2/x86/iFPUd.cpp#L140-L170)** — The underflow repacking `VPMULTISHIFTQB` fast path is already implemented; run exact-vs-old-path tests, do not treat it as a new candidate.
