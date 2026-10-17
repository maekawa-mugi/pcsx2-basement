# pcsx2/x86/microVU_IR.h

**Source:** [`pcsx2/x86/microVU_IR.h`](https://github.com/maekawa-mugi/pcsx2-basement/blob/main/pcsx2/x86/microVU_IR.h)

> Source-specific instructions only. `P1`/`P2`/`P3` are proposals; `EXISTING` items are already implemented. Match the current source, preserve CPU feature gates and emulation semantics, and verify generated code and performance before accepting changes.

- **[P3 · IR01 · L234–355, L463–705](https://github.com/maekawa-mugi/pcsx2-basement/blob/main/pcsx2/x86/microVU_IR.h#L234-L355)** — Inspect XMM allocator/high-register homes for redundant VF spills. High XMM16–31 scratch/cached copies are partially implemented; preserve invalidation and `CHECK_VU_SOFT` restrictions.
