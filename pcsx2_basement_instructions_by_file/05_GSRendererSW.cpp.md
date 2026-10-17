# pcsx2/GS/Renderers/SW/GSRendererSW.cpp

**Source:** [`pcsx2/GS/Renderers/SW/GSRendererSW.cpp`](https://github.com/maekawa-mugi/pcsx2-basement/blob/main/pcsx2/GS/Renderers/SW/GSRendererSW.cpp)

> Source-specific instructions only. `P1`/`P2`/`P3` are proposals; `EXISTING` items are already implemented. Match the current source, preserve CPU feature gates and emulation semantics, and verify generated code and performance before accepting changes.

- **[EXISTING · REN01 · L225–280](https://github.com/maekawa-mugi/pcsx2-basement/blob/main/pcsx2/GS/Renderers/SW/GSRendererSW.cpp#L225-L280)** — Four-vertex ZMM conversion and masked stores preserving `GSVertexSW::_pad` are already implemented. Use as a precedent for byte/word masked VRAM stores; validate against sprite and scalar paths.
- **[P2 · REN02 · L1150–1210](https://github.com/maekawa-mugi/pcsx2-basement/blob/main/pcsx2/GS/Renderers/SW/GSRendererSW.cpp#L1150-L1210)** — Expose exactly integral constant LOD to the specialized sampler (GS14); avoid recomputing a condition already resolved per draw.
