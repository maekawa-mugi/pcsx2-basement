# pcsx2/x86/microVU_Misc.inl

**Source:** [`pcsx2/x86/microVU_Misc.inl`](https://github.com/maekawa-mugi/pcsx2-basement/blob/main/pcsx2/x86/microVU_Misc.inl)

> Source-specific instructions only. `P1`/`P2`/`P3` are proposals; `EXISTING` items are already implemented. Match the current source, preserve CPU feature gates and emulation semantics, and verify generated code and performance before accepting changes.

- **[P2 · MISC01 · L45–108](https://github.com/maekawa-mugi/pcsx2-basement/blob/main/pcsx2/x86/microVU_Misc.inl#L45-L108)** — Evaluate `VMOVDQU32` masked partial VF stores instead of multiple scalar lane stores/extracts. Confirm XYZW mask ordering, alignment/fault behavior and nonwritten-lane preservation; a single-lane `MOVSS` may already be optimal.
- **[P3 · MISC02 · L110–145](https://github.com/maekawa-mugi/pcsx2-basement/blob/main/pcsx2/x86/microVU_Misc.inl#L110-L145)** — Examine remaining `mVUmergeRegs()` partial-lane blends for vector-register-pressure reductions. `INSERTPS` one-lane and `BLENDPS` cases are already specialized.
