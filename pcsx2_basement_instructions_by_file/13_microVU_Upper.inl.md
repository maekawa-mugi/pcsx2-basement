# pcsx2/x86/microVU_Upper.inl

**Source:** [`pcsx2/x86/microVU_Upper.inl`](https://github.com/maekawa-mugi/pcsx2-basement/blob/main/pcsx2/x86/microVU_Upper.inl)

> Source-specific instructions only. `P1`/`P2`/`P3` are proposals; `EXISTING` items are already implemented. Match the current source, preserve CPU feature gates and emulation semantics, and verify generated code and performance before accepting changes.

- **[P2 · UPP01 · L754–804](https://github.com/maekawa-mugi/pcsx2-basement/blob/main/pcsx2/x86/microVU_Upper.inl#L754-L804)** — Evaluate constructing CLIP's six compare-result bits with k-mask operations instead of `PCMPGTD → PBLENDW → PACKSSWB → PMOVMSKB`. Preserve exact bit order, flag-history shifting, sign/denormal handling and `CHECK_VU_SOFT` gating.
- **[EXISTING · UPP02 · L760–782](https://github.com/maekawa-mugi/pcsx2-basement/blob/main/pcsx2/x86/microVU_Upper.inl#L760-L782)** — CLIP denormal suppression already has an AVX-512 `VPTESTMD` zeroing-mask path; retain it as-is unless hardware tests find a regression.
