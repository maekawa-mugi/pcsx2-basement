#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0+
"""Extract real EE allocator/template/MMI code, without replacing allocation.

The standalone host supplies memory, liveness inputs and non-vector VM hooks.
Algorithms and state layouts below come from the production sources verbatim.
"""
from pathlib import Path
import re
import sys

root = Path(__file__).resolve().parents[2]


def function(source, name):
    match = re.search(r"^(?:(?:static|__ri) )?(?:void|bool|int|EERecompileInfo) " + name + r"\([^;]*?\)\s*\{", source, re.M)
    if not match:
        raise RuntimeError(f"Cannot find production function: {name}")
    depth, end = 1, match.end()
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end] + "\n"


header = (root / "pcsx2/x86/iCore.h").read_text()
header = header[header.index("#define MODE_READ"):]
header = header.replace('#include "EERecompileInfo.h"', '#include "pcsx2/x86/EERecompileInfo.h"')
Path(sys.argv[1]).write_text(header)
core = (root / "pcsx2/x86/iCore.cpp").read_text()
templates = (root / "pcsx2/x86/ix86-32/iR5900Templates.cpp").read_text()
gpr = (root / "pcsx2/x86/ix86-32/iCore.cpp").read_text()
mmi = (root / "pcsx2/x86/iMMI.cpp").read_text()
output = []
for name in ["_initXMMregs", "_getFreeXMMreg", "_allocTempXMMreg", "_allocTempXMMregEVEX",
             "_eeCallWithHighXMM", "_checkXMMreg", "_hasXMMreg", "_allocGPRtoXMMreg",
             "_reallocateXMMreg", "_addNeededGPRtoXMMreg", "_clearNeededXMMregs",
             "_deleteGPRtoX86reg", "_deleteGPRtoXMMreg", "_writebackXMMreg",
             "_freeXMMreg", "_freeXMMregWithoutWriteback", "_flushXMMreg", "_flushXMMregs"]:
    output.append(function(core, name))
output.append(function(gpr, "_validateRegs"))
output.append(function(templates, "_deleteEEreg"))
output.append(function(templates, "_deleteEEreg128"))
output.append(function(templates, "eeRecompileCodeXMM"))
output.append(mmi[mmi.index("static bool CanUse3Arg"):mmi.index("void recPLZCW()")])
output.append(function(mmi, "recPLZCW"))
# Same production body with the Phase 1 allocation policy. Low operands select
# the unchanged low branch; no arithmetic or liveness implementation is copied.
output.append(function(mmi, "recPLZCW").replace("recPLZCW()", "recPLZCWPhase1()"))
# Experimental consumer stays in validation until real hot-block measurements
# justify its extra EVEX bytes. Production retains the Phase 1 policy.
output.append(function(mmi, "recPLZCW").replace("recPLZCW()", "recPLZCWPhase2()").replace("_allocTempXMMreg(XMMT_INT)", "_allocTempXMMregEVEX(XMMT_INT)", 1))
output.append(function(core, "_eeCallWithHighXMM").replace("_eeCallWithHighXMM", "eeCallWithWindowsHighXMM").replace("SHADOW_STACK_SIZE", "32"))
# These no-refactor MMI operations also stress LO/HI and operand renaming.
for name in ["recPADDW", "recPSUBW", "recPXOR", "recPOR", "recPNOR", "recPMFLO", "recPMFHI", "recPMTLO", "recPMTHI"]:
    output.append(function(mmi, name))
Path(sys.argv[2]).write_text("\n".join(output))
