# pcsx2/x86/Vif_Dynarec.cpp

**Source:** [`pcsx2/x86/Vif_Dynarec.cpp`](https://github.com/maekawa-mugi/pcsx2-basement/blob/main/pcsx2/x86/Vif_Dynarec.cpp)

> Source-specific instructions only. `P1`/`P2`/`P3` are proposals; `EXISTING` items are already implemented. Match the current source, preserve CPU feature gates and emulation semantics, and verify generated code and performance before accepting changes.

- **[P2 · VIF01 · L75–143](https://github.com/maekawa-mugi/pcsx2-basement/blob/main/pcsx2/x86/Vif_Dynarec.cpp#L75-L143)** — For UNPACK write-protect (`m4`), test a dword masked `VMOVDQU32 [dst]{k},src` to avoid `mVUsaveReg()`'s partial lane stores. Retain row/column/mode blends (`m2`, `m3`, `m5`); only protect the final write and remap the nibble correctly.
