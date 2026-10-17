# pcsx2/x86/iFPU.cpp

**Source:** [`pcsx2/x86/iFPU.cpp`](https://github.com/maekawa-mugi/pcsx2-basement/blob/main/pcsx2/x86/iFPU.cpp)

> Source-specific instructions only. `P1`/`P2`/`P3` are proposals; `EXISTING` items are already implemented. Match the current source, preserve CPU feature gates and emulation semantics, and verify generated code and performance before accepting changes.

**Related change:** Equivalent VU-side transformations are in [`microVU_Lower.inl`](14_microVU_Lower.inl.md).

- **[P1 · FPU01 · L780–796](https://github.com/maekawa-mugi/pcsx2-basement/blob/main/pcsx2/x86/iFPU.cpp#L780-L796)** — Apply the same 32-bit `LEA ebp,[rbp*2+r8]` quotient recurrence reduction as LOW02 to the EE exact DIV fallback (2→1 per iteration).
- **[P1 · FPU02 · L960–985](https://github.com/maekawa-mugi/pcsx2-basement/blob/main/pcsx2/x86/iFPU.cpp#L960-L985)** — Apply SQRT tail `LEA` foldings as LOW03 to EE exact SQRT (4→1 and 3→1).
- **[P3 · FPU03 · L1400–1515](https://github.com/maekawa-mugi/pcsx2-basement/blob/main/pcsx2/x86/iFPU.cpp#L1400-L1515)** — Review remaining exact SoftFloat scalar sign/exponent/normalization sequences for redundant masks; maintain PS2 FP exception and rounding semantics. No additional independently proven fold was identified.
