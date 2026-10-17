# pcsx2/x86/Vif_UnpackSSE.cpp

**Source:** [`pcsx2/x86/Vif_UnpackSSE.cpp`](https://github.com/maekawa-mugi/pcsx2-basement/blob/main/pcsx2/x86/Vif_UnpackSSE.cpp)

> Source-specific instructions only. `P1`/`P2`/`P3` are proposals; `EXISTING` items are already implemented. Match the current source, preserve CPU feature gates and emulation semantics, and verify generated code and performance before accepting changes.

- **[P2 · VIF02 · L330–355](https://github.com/maekawa-mugi/pcsx2-basement/blob/main/pcsx2/x86/Vif_UnpackSSE.cpp#L330-L355)** — For simple UNPACK masking, compare a correct masked store against the existing old-destination load/mask/OR/store. Do not discard row/column semantics or assume the entire computed mask is pure write-protect.
- **[EXISTING · VIF03 · L330–355](https://github.com/maekawa-mugi/pcsx2-basement/blob/main/pcsx2/x86/Vif_UnpackSSE.cpp#L330-L355)** — `VPTERNLOGD` already shortens the simple mask-merge path under AVX-512; do not add the same optimization again.
