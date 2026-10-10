# Hardware texture interpolation (experimental)

The hardware renderer now has two independent controls:

- **Texture Filtering Mode** (the existing `filter` option): Nearest, Forced, Follow PS2, and Forced Except Sprites. The stored values are unchanged and the setting is shared with the software renderer.
- **Interpolation Method** (`TextureInterpolation`): Bilinear (0, default), Bicubic/Catmull-Rom (1), and windowed Jinc (2). This option only affects the classic hardware renderers.

The kernel is selected **after** the original PS2/forced/nearest decision. Nearest always wins. For ordinary colour textures, bicubic and Jinc use a 4x4 nearest-tap reconstruction in the TFX pixel shader; texture wrap and clamp still come from the original sampler state. The existing bilinear path is unchanged by default.

To avoid altering accuracy-sensitive emulation, non-bilinear interpolation falls back to the original sampling path for palette/depth formats, texture shuffle, special wrap/region handling, framebuffer reads, hardware mipmapping/trilinear filtering, and anisotropic filtering. This initial implementation therefore does not apply the new kernels to all 3D surfaces. They require 16 taps per sampled pixel and can be substantially slower, especially at high internal resolutions.

The implementation covers Vulkan, OpenGL, Direct3D 11/12, and Metal. Software and parallel-GS renderers are not changed.

## Validation checklist

- Compare Bilinear/default mode with the base branch across representative games and GS dumps.
- Test nearest, follow-PS2, forced, and forced-except-sprites modes independently from the interpolation selector.
- Compare sharp/soft edges, alpha textures and repeat/clamp borders for bicubic and Jinc.
- Verify correct fallbacks for palette, depth, render-target sampling, mipmapped 3D textures and anisotropy.
- Check shader compilation on Vulkan, OpenGL, D3D11, D3D12 and Metal and compare performance.
- Check global and per-game configuration persistence.

The source changes and numerical kernel weights have been inspected, but shader compilation and real-game/GS-dump validation are still required.
