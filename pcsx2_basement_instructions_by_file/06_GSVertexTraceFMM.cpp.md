# pcsx2/GS/Renderers/Common/GSVertexTraceFMM.cpp

**Source:** [`pcsx2/GS/Renderers/Common/GSVertexTraceFMM.cpp`](https://github.com/maekawa-mugi/pcsx2-basement/blob/main/pcsx2/GS/Renderers/Common/GSVertexTraceFMM.cpp)

> Source-specific instructions only. `P1`/`P2`/`P3` are proposals; `EXISTING` items are already implemented. Match the current source, preserve CPU feature gates and emulation semantics, and verify generated code and performance before accepting changes.

- **[P2 · VTX01 · L225–260](https://github.com/maekawa-mugi/pcsx2-basement/blob/main/pcsx2/GS/Renderers/Common/GSVertexTraceFMM.cpp#L225-L260)** — Remove six ZMM stores to aligned stack arrays followed by XMM reloads when reducing vertex minima/maxima. Use lane extraction or register-only reduction, **preserving the original reduction order** for signed zero/NaNs.
- **[EXISTING · VTX02 · L170–230](https://github.com/maekawa-mugi/pcsx2-basement/blob/main/pcsx2/GS/Renderers/Common/GSVertexTraceFMM.cpp#L170-L230)** — ZMM four-vertex batching and `_mm512_cmp_ps_mask` NaN handling already exist; do not implement them again.
