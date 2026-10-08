#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0+
"""Extract the production microVU upper SoftFloat emitters, including the inline
paths that run inside recompiled blocks, for a standalone Linux test.

Everything from microVU_UpperSoft.inl up to the opcode dispatcher is copied
verbatim. The harness supplies only the allocator, flag-instance and VM state
that a real microVU block would provide.
"""
from pathlib import Path
import sys

root = Path(__file__).resolve().parents[2]
upper = (root / "pcsx2/x86/microVU_Upper.inl").read_text()
soft = (root / "pcsx2/x86/microVU_UpperSoft.inl").read_text()
misc = (root / "pcsx2/x86/microVU_Misc.h").read_text()
vu = (root / "pcsx2/x86/microVU.h").read_text()
alloc = (root / "pcsx2/x86/microVU_Alloc.inl").read_text()

output = []
# Instruction-field macros and fixed host registers used by microVU.
output.append(misc[misc.index("#define _Ft_ "):misc.index("// Function Params")])
output.append(upper[upper.index("struct VuSoftFmacJitResult"):upper.index("//------------------------------------------------------------------")])
output.append(upper[upper.index("alignas(4) static constexpr u32 s_vu_soft_truncate_mxcsr"):upper.index("// Note: If modXYZW")])
output.append(vu[vu.index("struct alignas(16) microVUSoftBoothCacheEntry"):vu.index("struct microVU\n")])
fields = vu[vu.index("\tconst void* softMulExact;"):vu.index("\tu8* resumePtrXG;")]
output.append("struct microVU\n{\n\talignas(16) u32 macFlag[4] = {};\n\tu32 index = 0;\n\tu32 cop2 = 0;\n"
              "\tu32 code = 0;\n\tHarnessProgram prog;\n\tHarnessRegAlloc* regAlloc = nullptr;\n" + fields +
              "\tHarnessVURegs& regs() const { return g_vuRegs; }\n};\n")
# Flag-instance helpers used by the soft flag writeback.
output.append(alloc[alloc.index("__fi static const x32& getFlagReg"):alloc.index("__fi void setBitSFLAG")])
mflag = alloc[alloc.index("__fi void mVUallocMFLAGb"):]
depth, end = 0, mflag.index("{")
while True:
    depth += (mflag[end] == "{") - (mflag[end] == "}")
    end += 1
    if depth == 0:
        break
output.append(mflag[:end] + "\n")
lane = upper[upper.index("static void mVUemitExtractLane"):upper.index('#include "microVU_UpperSoft.inl"')]
output.append(lane)
body = soft[soft.index("static void mVUemitUpperSoftStackAlloc"):soft.index("static void mVUemitUpperSoftExact(")]
output.append(body)
Path(sys.argv[1]).write_text("\n".join(output))
