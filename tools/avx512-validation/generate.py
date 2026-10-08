#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0+
"""Extract the production generators verbatim for a dependency-light Linux test.

No emulation implementation is duplicated. Only the surrounding VM/allocator
state is replaced, since these internal kernels use pointers and raw operands.
"""
from pathlib import Path
import re
import sys

root = Path(__file__).resolve().parents[2]


def function(source, name):
    match = re.search(r"^(?:(?:static|__ri) )?(?:void|bool|mVUSoftDivCapTailPatch) " + name + r"\([^;]*?\)\s*\{", source, re.M)
    if not match:
        raise RuntimeError(f"Cannot find production generator: {name}")
    depth = 1
    end = match.end()
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end] + "\n"


upper = (root / "pcsx2/x86/microVU_Upper.inl").read_text()
soft = (root / "pcsx2/x86/microVU_UpperSoft.inl").read_text()
lower = (root / "pcsx2/x86/microVU_Lower.inl").read_text()
vu = (root / "pcsx2/x86/microVU.h").read_text()
ee = (root / "pcsx2/x86/iFPU.cpp").read_text()
output = [upper[upper.index("struct VuSoftFmacJitResult"):upper.index("//------------------------------------------------------------------")]]
output += [upper[upper.index("alignas(4) static constexpr u32 s_vu_soft_truncate_mxcsr"):upper.index("// Note: If modXYZW")]]
output += [vu[vu.index("struct alignas(16) microVUSoftBoothCacheEntry"):vu.index("struct microVU\n")]]
fields = vu[vu.index("\tconst void* softMulExact;"):vu.index("\tu8* resumePtrXG;")]
output += ["struct microVU { u32 index = 0;\n" + fields + "};\n"]
output += ["static constexpr sptr VU_SOFT_RESULT_VALUE_OFFSET = offsetof(VuSoftFmacJitResult, value);\n"]
output += ["template <typename MacFlags>\n" + function(soft, "mVUemitUpperStatusFromMacFlags")]
output += [function(soft, "mVUupperSoftNeedsTruncateMxcsr")]
for name in ["mVUGenerateSoftMulExactKernel", "mVUGenerateSoftMaddIntegratedLaneKernel",
             "mVUGenerateSoftAddExactLaneKernel", "mVUGenerateSoftMulExactVectorKernel",
             "mVUGenerateSoftMulBoothPackedKernel", "mVUGenerateSoftMaddPackedKernels",
             "mVUGenerateSoftMaddExactVectorKernels"]:
    output += [function(soft, name)]
output += [lower[lower.index("struct mVUSoftDivCapTailPatch"):lower.index("static void mVUemitLowerSoftQAndStatusWriteback")]]
for name in ["mVUGenerateLowerSrtReciprocalSoftExactKernel", "mVUGenerateLowerDivSoftExactKernel",
             "mVUGenerateLowerDivSoftCapTail", "mVUGenerateLowerSqrtSoftExactKernel",
             "mVUGenerateLowerRsqrtSoftExactKernel"]:
    output += [function(lower, name)]
output += ["alignas(16) static const u32 s_pos[4] = {0x7fffffff, 0xffffffff, 0xffffffff, 0xffffffff};\n"]
output += [ee[ee.index("static const void* s_fpuSoftAddSubExact"):ee.index("void GenerateSoftFloatKernels()")]]
output += [function(ee, "GenerateSoftFloatKernels")]
Path(sys.argv[1]).write_text("\n".join(output))
# Extract the complete production MMI instruction sequences. Only register
# allocation and profiling are replaced; the emitted arithmetic is unchanged.
mmi = (root / "pcsx2/x86/iMMI.cpp").read_text()
mmi_output = [mmi[mmi.index("// Fixed halfword permutation"):mmi.index("#ifndef MMI_RECOMPILE")]]
for name in ["recPEXT5", "recPPAC5", "recPADDSW", "recPSUBSW", "recPABSW", "recPABSH",
             "recPPACW", "recPPACH", "recPPACB",
             "recPADDUW", "recPSUBUW", "recPSLLVW", "recPSRLVW", "recPSRAVW",
             "recPEXEH", "recPREVH", "recPEXCH", "recPCPYH",
             "recPMULTH", "recPMADDH", "recPMSUBH", "recPHMSBH",
             "recPEXTLW", "recPEXTLB", "recPEXTLH", "recPCGTH", "recPCGTW", "recPCPYUD"]:
    body = function(mmi.replace("() //needs clamping", "()"), name)
    body = body[body.index("\tEERecompileInfo info = eeRecompileCodeXMM"):]
    body = re.sub(r"\tEERecompileInfo info = eeRecompileCodeXMM\([^;]*\);", "", body, count=1)
    mmi_output.append("static void " + name + "() {\n" + body)
Path(sys.argv[1]).with_name("mmi-generators.h").write_text("\n".join(mmi_output))
if len(sys.argv) > 2:
    yuv = (root / "pcsx2/IPU/yuv2rgb.cpp").read_text()
    defines = yuv[yuv.index("#define IPU_Y_BIAS"):yuv.index("MULTI_ISA_UNSHARED_START")]
    Path(sys.argv[2]).write_text(defines + function(yuv, "yuv2rgb_reference") + function(yuv, "yuv2rgb_sse2"))
if len(sys.argv) > 4:
    # Keep the production PS2Float arithmetic verbatim, replacing only its
    # application-wide Common.h include with the dependencies used here.
    header = (root / "pcsx2/PS2Float.h").read_text()
    inline = (root / "pcsx2/PS2Float.inl").read_text().replace('#include "Common.h"', '')
    prefix = '#include "common/Pcsx2Defs.h"\n#include "common/Pcsx2Types.h"\n#include "common/Console.h"\n#include <bit>\n#include <string>\n'
    Path(sys.argv[3]).write_text(prefix + header.replace('#include "PS2Float.inl"', inline))
    source = (root / "pcsx2/PS2Float.cpp").read_text()
    Path(sys.argv[4]).write_text(source.replace('#include "Common.h"', '').replace('#include "PS2Float.h"', '#include "oracle.h"'))
