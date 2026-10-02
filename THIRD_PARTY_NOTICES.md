# paraLLEl-GS integration notices

This project includes paraLLEl-GS, a Vulkan implementation of the PlayStation 2
Graphics Synthesizer, and integration code selectively ported from
pcsx2-reliquary.

## paraLLEl-GS renderer integration

- Files: `pcsx2/GS/Renderers/parallel-gs/GSRendererPGS.cpp` and
  `pcsx2/GS/Renderers/parallel-gs/GSRendererPGS.h`
- Copyright: 2024 Hans-Kristian Arntzen
- License: LGPL-3.0-or-later (the original headers use `LGPL-3.0+`)
- Integration source:
  <https://github.com/DiscoStarslayer/pcsx2-reliquary/commit/fd5438a0c172481e6b519dee33dd688884b1ea78>

## Vulkan platform definitions

- File: `pcsx2/GS/Renderers/Vulkan/VKLoaderPlatformDefines.h`
- Copyright: 2002-2024 PCSX2 Dev Team
- License: LGPL-3.0-or-later
- Source: the same pcsx2-reliquary integration commit linked above

## paraLLEl-GS library

- Path: `pcsx2/GS/parallel-gs` (Git submodule)
- Upstream: <https://github.com/Arntzen-Software/parallel-gs>
- Imported revision: `609993aa471e4bd36490628be09dcbbc149b5046`
- Copyright holders and contributors include Arntzen Software AS,
  Hans-Kristian Arntzen, and Runar Heyer; individual source headers retain
  their original notices.
- License: LGPL-3.0-or-later

The submodule's Granite framework and its dependencies retain their own
copyright and license notices. This document supplements the existing
`bin/docs/ThirdPartyLicenses.html` and does not replace those notices.

## License texts and rebuilding

The root `COPYING.LGPLv3` is a verbatim copy of paraLLEl-GS's LGPLv3 text.
`COPYING.GPLv3` supplies the GPLv3 text referenced by the LGPLv3. Both files,
along with this notice, are copied into binary distribution documentation.

The LGPL notices above apply to the identified library and integration files;
the existing PCSX2 source license notices are retained.

To obtain the library sources needed for rebuilding, run
`git submodule update --init --recursive` in a source checkout. The parent
repository's CMake and Visual Studio build definitions compile these sources,
including locally modified paraLLEl-GS sources. Follow the
[PCSX2 build guide](https://pcsx2.net/docs/advanced/building/) for the platform's
compiler and dependency setup, and use an out-of-tree directory for CMake.
