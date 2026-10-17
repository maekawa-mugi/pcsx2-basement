# pcsx2/x86/iMMI.cpp

**Source:** [`pcsx2/x86/iMMI.cpp`](https://github.com/maekawa-mugi/pcsx2-basement/blob/main/pcsx2/x86/iMMI.cpp)

> Source-specific instructions only. `P1`/`P2`/`P3` are proposals; `EXISTING` items are already implemented. Match the current source, preserve CPU feature gates and emulation semantics, and verify generated code and performance before accepting changes.

- **[P1 · MMI01 · L2787–2817](https://github.com/maekawa-mugi/pcsx2-basement/blob/main/pcsx2/x86/iMMI.cpp#L2787-L2817)** — In SSE `recPCPYUD`, when destination aliases `Rt` (but not `Rs`), replace `PUNPCKHQDQ + PSHUFD(0x4e)` with `MOVHLPS(dst, Rs)` (2→1). This copies `Rs.hi64` to the low half and preserves `Rt.hi64` above.
- **[EXISTING · MMI02 · L300–420, L690–800, L880–910](https://github.com/maekawa-mugi/pcsx2-basement/blob/main/pcsx2/x86/iMMI.cpp#L690-L800)** — Existing AVX-512 implementations cover PMFHL cases, PPACH/PPACB and several ternary-logic conversions; do not repeat already-selected paths.
- **[P3 · MMI03 · L1140–1170](https://github.com/maekawa-mugi/pcsx2-basement/blob/main/pcsx2/x86/iMMI.cpp#L1140-L1170)** — Only revisit non-AVX-512 signed/subtract bit-select sequences if disassembly reveals an unoptimized active path. Many AVX-512 alternatives already use shared helpers; check destination aliasing carefully.
