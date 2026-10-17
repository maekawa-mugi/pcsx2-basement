# pcsx2/GS/Renderers/SW/GSSetupPrimCodeGenerator.all.cpp

**Source:** [`pcsx2/GS/Renderers/SW/GSSetupPrimCodeGenerator.all.cpp`](https://github.com/maekawa-mugi/pcsx2-basement/blob/main/pcsx2/GS/Renderers/SW/GSSetupPrimCodeGenerator.all.cpp)

> Source-specific instructions only. `P1`/`P2`/`P3` are proposals; `EXISTING` items are already implemented. Match the current source, preserve CPU feature gates and emulation semantics, and verify generated code and performance before accepting changes.

- **[P2 · SET01 · L408–409](https://github.com/maekawa-mugi/pcsx2-basement/blob/main/pcsx2/GS/Renderers/SW/GSSetupPrimCodeGenerator.all.cpp#L408-L409)** — Replace `PCMPEQD + PSRLD(16)` used to build the `0x0000ffff` mask with one aligned constant-vector load (2→1); compare with zero-dependency register generation.
- **[P2 · SET02 · L85–145](https://github.com/maekawa-mugi/pcsx2-basement/blob/main/pcsx2/GS/Renderers/SW/GSSetupPrimCodeGenerator.all.cpp#L85-L145)** — On Windows x64, refine the existing conditional XMM6–15 save count if generated register use allows a smaller exact set. Never clobber ABI-preserved registers.
- **[P3 · SET03 · L404–510](https://github.com/maekawa-mugi/pcsx2-basement/blob/main/pcsx2/GS/Renderers/SW/GSSetupPrimCodeGenerator.all.cpp#L404-L510)** — Review repeated `CVTTPS2DQ + PAND(mask16) + PACKUSDW` and repeated source-colour reloads in interpolated setup. Only remove masks/reloads when value ranges or register liveness prove them redundant; saturation semantics matter.
