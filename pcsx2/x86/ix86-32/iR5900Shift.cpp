// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Common.h"
#include "R5900OpcodeTables.h"
#include "x86/iR5900.h"

#include "cpuinfo.h"

using namespace x86Emitter;

static bool hasBMI2()
{
	static const bool s_has_bmi2 = cpuinfo_initialize() && cpuinfo_has_x86_bmi2();
	return s_has_bmi2;
}

namespace R5900::Dynarec::OpcodeImpl
{

/*********************************************************
* Shift arithmetic with constant shift                   *
* Format:  OP rd, rt, sa                                 *
*********************************************************/
#ifndef SHIFT_RECOMPILE

namespace Interp = R5900::Interpreter::OpcodeImpl;

REC_FUNC_DEL(SLL, _Rd_);
REC_FUNC_DEL(SRL, _Rd_);
REC_FUNC_DEL(SRA, _Rd_);
REC_FUNC_DEL(DSLL, _Rd_);
REC_FUNC_DEL(DSRL, _Rd_);
REC_FUNC_DEL(DSRA, _Rd_);
REC_FUNC_DEL(DSLL32, _Rd_);
REC_FUNC_DEL(DSRL32, _Rd_);
REC_FUNC_DEL(DSRA32, _Rd_);

REC_FUNC_DEL(SLLV, _Rd_);
REC_FUNC_DEL(SRLV, _Rd_);
REC_FUNC_DEL(SRAV, _Rd_);
REC_FUNC_DEL(DSLLV, _Rd_);
REC_FUNC_DEL(DSRLV, _Rd_);
REC_FUNC_DEL(DSRAV, _Rd_);

#else

static void recMoveTtoD(EERecompileInfo info)
{
	if (info & PROCESS_EE_T)
		xMOV(xRegister32(EEREC_D), xRegister32(EEREC_T));
	else
		xMOV(xRegister32(EEREC_D), ptr32[&cpuRegs.GPR.r[_Rt_].UL[0]]);
}

static void recMoveTtoD64(EERecompileInfo info)
{
	if (info & PROCESS_EE_T)
		xMOV(xRegister64(EEREC_D), xRegister64(EEREC_T));
	else
		xMOV(xRegister64(EEREC_D), ptr64[&cpuRegs.GPR.r[_Rt_].UD[0]]);
}

static void recMoveSToRCX(EERecompileInfo info)
{
	// load full 64-bits for store->load forwarding, since we always store >=64.
	if (info & PROCESS_EE_S)
		xMOV(rcx, xRegister64(EEREC_S));
	else
		xMOV(rcx, ptr64[&cpuRegs.GPR.r[_Rs_].UL[0]]);
}

//// SLL
static void recSLL_const()
{
	g_cpuConstRegs[_Rd_].SD[0] = (s32)(g_cpuConstRegs[_Rt_].UL[0] << _Sa_);
}

static void recSLLs_(EERecompileInfo info, int sa)
{
	// TODO: Use BMI
	pxAssert(!(info & PROCESS_EE_XMM));

	recMoveTtoD(info);
	if (sa != 0)
		xSHL(xRegister32(EEREC_D), sa);
	xMOVSX(xRegister64(EEREC_D), xRegister32(EEREC_D));
}

static void recSLL_(EERecompileInfo info)
{
	recSLLs_(info, _Sa_);
}

EERECOMPILE_CODEX(eeRecompileCodeRC2, SLL, XMMINFO_WRITED | XMMINFO_READT);

//// SRL
static void recSRL_const()
{
	g_cpuConstRegs[_Rd_].SD[0] = (s32)(g_cpuConstRegs[_Rt_].UL[0] >> _Sa_);
}

static void recSRLs_(EERecompileInfo info, int sa)
{
	pxAssert(!(info & PROCESS_EE_XMM));

	recMoveTtoD(info);
	if (sa != 0)
		xSHR(xRegister32(EEREC_D), sa);
	xMOVSX(xRegister64(EEREC_D), xRegister32(EEREC_D));
}

static void recSRL_(EERecompileInfo info)
{
	recSRLs_(info, _Sa_);
}

EERECOMPILE_CODEX(eeRecompileCodeRC2, SRL, XMMINFO_WRITED | XMMINFO_READT);

//// SRA
static void recSRA_const()
{
	g_cpuConstRegs[_Rd_].SD[0] = (s32)(g_cpuConstRegs[_Rt_].SL[0] >> _Sa_);
}

static void recSRAs_(EERecompileInfo info, int sa)
{
	pxAssert(!(info & PROCESS_EE_XMM));

	recMoveTtoD(info);
	if (sa != 0)
		xSAR(xRegister32(EEREC_D), sa);
	xMOVSX(xRegister64(EEREC_D), xRegister32(EEREC_D));
}

static void recSRA_(EERecompileInfo info)
{
	recSRAs_(info, _Sa_);
}

EERECOMPILE_CODEX(eeRecompileCodeRC2, SRA, XMMINFO_WRITED | XMMINFO_READT);

////////////////////////////////////////////////////
static void recDSLL_const()
{
	g_cpuConstRegs[_Rd_].UD[0] = (u64)(g_cpuConstRegs[_Rt_].UD[0] << _Sa_);
}

static void recDSLLs_(EERecompileInfo info, int sa)
{
	pxAssert(!(info & PROCESS_EE_XMM));

	recMoveTtoD64(info);
	if (sa != 0)
		xSHL(xRegister64(EEREC_D), sa);
}

static void recDSLL_(EERecompileInfo info)
{
	recDSLLs_(info, _Sa_);
}

EERECOMPILE_CODEX(eeRecompileCodeRC2, DSLL, XMMINFO_WRITED | XMMINFO_READT | XMMINFO_64BITOP);

////////////////////////////////////////////////////
static void recDSRL_const()
{
	g_cpuConstRegs[_Rd_].UD[0] = (u64)(g_cpuConstRegs[_Rt_].UD[0] >> _Sa_);
}

static void recDSRLs_(EERecompileInfo info, int sa)
{
	pxAssert(!(info & PROCESS_EE_XMM));

	recMoveTtoD64(info);
	if (sa != 0)
		xSHR(xRegister64(EEREC_D), sa);
}

static void recDSRL_(EERecompileInfo info)
{
	recDSRLs_(info, _Sa_);
}

EERECOMPILE_CODEX(eeRecompileCodeRC2, DSRL, XMMINFO_WRITED | XMMINFO_READT | XMMINFO_64BITOP);

//// DSRA
static void recDSRA_const()
{
	g_cpuConstRegs[_Rd_].SD[0] = (u64)(g_cpuConstRegs[_Rt_].SD[0] >> _Sa_);
}

static void recDSRAs_(EERecompileInfo info, int sa)
{
	pxAssert(!(info & PROCESS_EE_XMM));

	recMoveTtoD64(info);
	if (sa != 0)
		xSAR(xRegister64(EEREC_D), sa);
}

static void recDSRA_(EERecompileInfo info)
{
	recDSRAs_(info, _Sa_);
}

EERECOMPILE_CODEX(eeRecompileCodeRC2, DSRA, XMMINFO_WRITED | XMMINFO_READT | XMMINFO_64BITOP);

///// DSLL32
static void recDSLL32_const()
{
	g_cpuConstRegs[_Rd_].UD[0] = (u64)(g_cpuConstRegs[_Rt_].UD[0] << (_Sa_ + 32));
}

static void recDSLL32_(EERecompileInfo info)
{
	recDSLLs_(info, _Sa_ + 32);
}

EERECOMPILE_CODEX(eeRecompileCodeRC2, DSLL32, XMMINFO_WRITED | XMMINFO_READT | XMMINFO_64BITOP);

//// DSRL32
static void recDSRL32_const()
{
	g_cpuConstRegs[_Rd_].UD[0] = (u64)(g_cpuConstRegs[_Rt_].UD[0] >> (_Sa_ + 32));
}

static void recDSRL32_(EERecompileInfo info)
{
	recDSRLs_(info, _Sa_ + 32);
}

EERECOMPILE_CODEX(eeRecompileCodeRC2, DSRL32, XMMINFO_WRITED | XMMINFO_READT);

//// DSRA32
static void recDSRA32_const()
{
	g_cpuConstRegs[_Rd_].SD[0] = (u64)(g_cpuConstRegs[_Rt_].SD[0] >> (_Sa_ + 32));
}

static void recDSRA32_(EERecompileInfo info)
{
	recDSRAs_(info, _Sa_ + 32);
}

EERECOMPILE_CODEX(eeRecompileCodeRC2, DSRA32, XMMINFO_WRITED | XMMINFO_READT | XMMINFO_64BITOP);

/*********************************************************
* Shift arithmetic with variant register shift           *
* Format:  OP rd, rt, rs                                 *
*********************************************************/

// BMI2 SHLX/SHRX/SARX take the count from any register (no CL constraint) and the source from a register or
// memory, so the RCX move and the Rt->Rd copy disappear. The count is masked to 5/6 bits just like SHL/SHR/SAR.
// Returns the register holding the count (EEREC_S if allocated, otherwise eax loaded from memory).
// Needs proper testing on real BMI2 hardware.
static xRegister64 recShiftCountBMI2(EERecompileInfo info, bool dest_written_first = false)
{
	// When Rd is written (loaded with the constant Rt) before the shift and aliases Rs, the count must be copied.
	if ((info & PROCESS_EE_S) && !(dest_written_first && EEREC_S == EEREC_D))
		return xRegister64(EEREC_S);
	if (info & PROCESS_EE_S)
	{
		xMOV(rax, xRegister64(EEREC_S));
		return rax;
	}
	xMOV(rax, ptr64[&cpuRegs.GPR.r[_Rs_].UL[0]]);
	return rax;
}

static void recShiftV_constt(EERecompileInfo info, const xImpl_Group2& shift, const xImplBMI_RVM& shiftx)
{
	pxAssert(_Rs_ != 0);
	if (hasBMI2())
	{
		const xRegister64 count = recShiftCountBMI2(info, true);
		xMOV(xRegister32(EEREC_D), g_cpuConstRegs[_Rt_].UL[0]);
		shiftx(xRegister32(EEREC_D), xRegister32(count.GetId()), xRegister32(EEREC_D));
		xMOVSX(xRegister64(EEREC_D), xRegister32(EEREC_D));
		return;
	}
	recMoveSToRCX(info);
	xMOV(xRegister32(EEREC_D), g_cpuConstRegs[_Rt_].UL[0]);
	shift(xRegister32(EEREC_D), cl);
	xMOVSX(xRegister64(EEREC_D), xRegister32(EEREC_D));
}

static void recShiftV(EERecompileInfo info, const xImpl_Group2& shift, const xImplBMI_RVM& shiftx)
{
	pxAssert(_Rs_ != 0);

	if (hasBMI2())
	{
		const xRegister64 count = recShiftCountBMI2(info);
		if (info & PROCESS_EE_T)
			shiftx(xRegister32(EEREC_D), xRegister32(count.GetId()), xRegister32(EEREC_T));
		else
			shiftx(xRegister32(EEREC_D), xRegister32(count.GetId()), ptr32[&cpuRegs.GPR.r[_Rt_].UL[0]]);
		xMOVSX(xRegister64(EEREC_D), xRegister32(EEREC_D));
		return;
	}

	recMoveSToRCX(info);
	recMoveTtoD(info);
	shift(xRegister32(EEREC_D), cl);
	xMOVSX(xRegister64(EEREC_D), xRegister32(EEREC_D));
}

static void recDShiftV_constt(EERecompileInfo info, const xImpl_Group2& shift, const xImplBMI_RVM& shiftx)
{
	pxAssert(_Rs_ != 0);
	if (hasBMI2())
	{
		const xRegister64 count = recShiftCountBMI2(info, true);
		xMOV64(xRegister64(EEREC_D), g_cpuConstRegs[_Rt_].SD[0]);
		shiftx(xRegister64(EEREC_D), count, xRegister64(EEREC_D));
		return;
	}
	recMoveSToRCX(info);
	xMOV64(xRegister64(EEREC_D), g_cpuConstRegs[_Rt_].SD[0]);
	shift(xRegister64(EEREC_D), cl);
}

static void recDShiftV(EERecompileInfo info, const xImpl_Group2& shift, const xImplBMI_RVM& shiftx)
{
	pxAssert(_Rs_ != 0);
	if (hasBMI2())
	{
		const xRegister64 count = recShiftCountBMI2(info);
		if (info & PROCESS_EE_T)
			shiftx(xRegister64(EEREC_D), count, xRegister64(EEREC_T));
		else
			shiftx(xRegister64(EEREC_D), count, ptr64[&cpuRegs.GPR.r[_Rt_].UD[0]]);
		return;
	}
	recMoveSToRCX(info);
	recMoveTtoD64(info);
	shift(xRegister64(EEREC_D), cl);
}

//// SLLV
static void recSLLV_const()
{
	g_cpuConstRegs[_Rd_].SD[0] = (s32)(g_cpuConstRegs[_Rt_].UL[0] << (g_cpuConstRegs[_Rs_].UL[0] & 0x1f));
}

static void recSLLV_consts(EERecompileInfo info)
{
	recSLLs_(info, g_cpuConstRegs[_Rs_].UL[0] & 0x1f);
}

static void recSLLV_constt(EERecompileInfo info)
{
	recShiftV_constt(info, xSHL, xSHLX);
}

static void recSLLV_(EERecompileInfo info)
{
	recShiftV(info, xSHL, xSHLX);
}

EERECOMPILE_CODERC0(SLLV, XMMINFO_READS | XMMINFO_READT | XMMINFO_WRITED);

//// SRLV
static void recSRLV_const()
{
	g_cpuConstRegs[_Rd_].SD[0] = (s32)(g_cpuConstRegs[_Rt_].UL[0] >> (g_cpuConstRegs[_Rs_].UL[0] & 0x1f));
}

static void recSRLV_consts(EERecompileInfo info)
{
	recSRLs_(info, g_cpuConstRegs[_Rs_].UL[0] & 0x1f);
}

static void recSRLV_constt(EERecompileInfo info)
{
	recShiftV_constt(info, xSHR, xSHRX);
}

static void recSRLV_(EERecompileInfo info)
{
	recShiftV(info, xSHR, xSHRX);
}

EERECOMPILE_CODERC0(SRLV, XMMINFO_READS | XMMINFO_READT | XMMINFO_WRITED);

//// SRAV
static void recSRAV_const()
{
	g_cpuConstRegs[_Rd_].SD[0] = (s32)(g_cpuConstRegs[_Rt_].SL[0] >> (g_cpuConstRegs[_Rs_].UL[0] & 0x1f));
}

static void recSRAV_consts(EERecompileInfo info)
{
	recSRAs_(info, g_cpuConstRegs[_Rs_].UL[0] & 0x1f);
}

static void recSRAV_constt(EERecompileInfo info)
{
	recShiftV_constt(info, xSAR, xSARX);
}

static void recSRAV_(EERecompileInfo info)
{
	recShiftV(info, xSAR, xSARX);
}

EERECOMPILE_CODERC0(SRAV, XMMINFO_READS | XMMINFO_READT | XMMINFO_WRITED);

//// DSLLV
static void recDSLLV_const()
{
	g_cpuConstRegs[_Rd_].UD[0] = (u64)(g_cpuConstRegs[_Rt_].UD[0] << (g_cpuConstRegs[_Rs_].UL[0] & 0x3f));
}

static void recDSLLV_consts(EERecompileInfo info)
{
	int sa = g_cpuConstRegs[_Rs_].UL[0] & 0x3f;
	recDSLLs_(info, sa);
}

static void recDSLLV_constt(EERecompileInfo info)
{
	recDShiftV_constt(info, xSHL, xSHLX);
}

static void recDSLLV_(EERecompileInfo info)
{
	recDShiftV(info, xSHL, xSHLX);
}

EERECOMPILE_CODERC0(DSLLV, XMMINFO_READS | XMMINFO_READT | XMMINFO_WRITED | XMMINFO_64BITOP);

//// DSRLV
static void recDSRLV_const()
{
	g_cpuConstRegs[_Rd_].UD[0] = (u64)(g_cpuConstRegs[_Rt_].UD[0] >> (g_cpuConstRegs[_Rs_].UL[0] & 0x3f));
}

static void recDSRLV_consts(EERecompileInfo info)
{
	int sa = g_cpuConstRegs[_Rs_].UL[0] & 0x3f;
	recDSRLs_(info, sa);
}

static void recDSRLV_constt(EERecompileInfo info)
{
	recDShiftV_constt(info, xSHR, xSHRX);
}

static void recDSRLV_(EERecompileInfo info)
{
	recDShiftV(info, xSHR, xSHRX);
}

EERECOMPILE_CODERC0(DSRLV, XMMINFO_READS | XMMINFO_READT | XMMINFO_WRITED | XMMINFO_64BITOP);

//// DSRAV
static void recDSRAV_const()
{
	g_cpuConstRegs[_Rd_].SD[0] = (s64)(g_cpuConstRegs[_Rt_].SD[0] >> (g_cpuConstRegs[_Rs_].UL[0] & 0x3f));
}

static void recDSRAV_consts(EERecompileInfo info)
{
	int sa = g_cpuConstRegs[_Rs_].UL[0] & 0x3f;
	recDSRAs_(info, sa);
}

static void recDSRAV_constt(EERecompileInfo info)
{
	recDShiftV_constt(info, xSAR, xSARX);
}

static void recDSRAV_(EERecompileInfo info)
{
	recDShiftV(info, xSAR, xSARX);
}

EERECOMPILE_CODERC0(DSRAV, XMMINFO_READS | XMMINFO_READT | XMMINFO_WRITED | XMMINFO_64BITOP);

#endif

} // namespace R5900::Dynarec::OpcodeImpl
