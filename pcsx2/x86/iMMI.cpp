// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Common.h"
#include "R5900OpcodeTables.h"
#include "iR5900.h"
#include "common/BitUtils.h"
#include "GS/MultiISA.h"
#include "cpuinfo.h"

using namespace x86Emitter;

namespace Interp = R5900::Interpreter::OpcodeImpl::MMI;

namespace R5900 {
namespace Dynarec {
namespace OpcodeImpl {
namespace MMI {

#include "MMIAVX512.inl"

// Fixed halfword permutation applied to both qwords (PSHUFLW/PSHUFHW with the same imm8) as one
// byte shuffle. imm8 is a template parameter so each use gets its own constant mask.
template <u8 imm8>
struct HalfwordShuffleMask
{
	alignas(16) u8 bytes[16];
	constexpr HalfwordShuffleMask() : bytes{}
	{
		for (int q = 0; q < 2; q++)
			for (int i = 0; i < 4; i++)
			{
				const int sel = (imm8 >> (i * 2)) & 3;
				bytes[q * 8 + i * 2] = static_cast<u8>(q * 8 + sel * 2);
				bytes[q * 8 + i * 2 + 1] = static_cast<u8>(q * 8 + sel * 2 + 1);
			}
	}
};

// AVX: one VPSHUFB with a memory mask (2 -> 1). Non-AVX PSHUFB is destructive on the data
// operand, so the legacy-SSE form keeps PSHUFLW+PSHUFHW. Needs proper testing.
template <u8 imm8>
static void emitHalfwordShuffle(const xRegisterSSE& dst, const xRegisterSSE& src)
{
	alignas(16) static constexpr HalfwordShuffleMask<imm8> mask;
	if (x86Emitter::use_avx)
	{
		xPSHUF.B(dst, src, ptr128[mask.bytes]);
	}
	else
	{
		xPSHUF.LW(dst, src, imm8);
		xPSHUF.HW(dst, dst, imm8);
	}
}

// AVX VPSHUFB masks that gather the even halfwords / even bytes of a source into the low 8 bytes (high 8 zeroed).
// Used by PPACH/PPACB/PMFHL.LH: one VPSHUFB per source plus an unpack, instead of 5-7 legacy shuffles/shifts.
// Needs proper testing against the interpreter.
alignas(16) static constexpr u8 s_packEvenHalfMask[16] = {0, 1, 4, 5, 8, 9, 12, 13, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80};
alignas(16) static constexpr u8 s_packEvenByteMask[16] = {0, 2, 4, 6, 8, 10, 12, 14, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80};

// dst = {pack(t) | pack(s) << 64}. Rs == 0 form is a single VPSHUFB (pass s = nullptr). tmp must differ from s/t.
static void emitPackEvenAVX(const xRegisterSSE& dst, const xRegisterSSE* s, const xRegisterSSE& t,
	const xRegisterSSE& tmp, const u8* mask)
{
	if (!s)
	{
		xPSHUF.B(dst, t, ptr128[mask]);
		return;
	}
	xPSHUF.B(tmp, *s, ptr128[mask]);
	xPSHUF.B(dst, t, ptr128[mask]);
	xPUNPCK.LQDQ(dst, dst, tmp);
}

// Splits per-qword 64-bit products into LO/HI sign-extended halves. AVX-512: VPSLLQ+VPSRAQ for LO and
// VPSRAQ for HI (3 instructions, no dword shuffles); otherwise PSHUFD x2 + PMOVSXDQ x2. dst may equal HI.
static void emitSplitLoHi(int lo, int hi, int dst)
{
	if (avx512.HasCore())
	{
		xPSLL.Q(xRegisterSSE(lo), xRegisterSSE(dst), 32);
		xVPSRAQImm(xRegisterSSE(lo), xRegisterSSE(lo), 32);
		xVPSRAQImm(xRegisterSSE(hi), xRegisterSSE(dst), 32);
		return;
	}
	xPSHUF.D(xRegisterSSE(lo), xRegisterSSE(dst), 0x88);
	xPSHUF.D(xRegisterSSE(hi), xRegisterSSE(dst), 0xdd);
	xPMOVSX.DQ(xRegisterSSE(lo), xRegisterSSE(lo));
	xPMOVSX.DQ(xRegisterSSE(hi), xRegisterSSE(hi));
}

// AVX-512 halfword multiply core (PMULTH/PMADDH/PMSUBH): sign-extend both inputs to YMM dwords, VPMULLD,
// then VPERMQ so that lo = {p0,p1,p4,p5} and hi = {p2,p3,p6,p7} (the LO/HI word order). hi is used as a temp
// and must differ from s/t/lo. Leaves the upper YMM halves of lo dirty. Needs proper testing.
static void emitHalfwordProductsAVX512(int lo, int hi, int s, int t)
{
	xVPMOVSXWDY(xRegisterSSE(lo), xRegisterSSE(s));
	xVPMOVSXWDY(xRegisterSSE(hi), xRegisterSSE(t));
	xVPMULLDY(xRegisterSSE(lo), xRegisterSSE(lo), xRegisterSSE(hi));
	xVPERMQY(xRegisterSSE(lo), xRegisterSSE(lo), 0xd8);
	xVEXTRACTI128Y(xRegisterSSE(hi), xRegisterSSE(lo), 1);
}

// AVX2 flavour of the same core (VEX.256, xmm0-15 only): 5 instructions for PMULTH where the SSE4 form needs 6-9, and
// PMADDH/PMSUBH drop from 11-18 to 7-9. VPMULLD ymm is 2 uops on Intel, so this is an instruction-count win, not
// a measured speed win. Leaves the upper YMM half of lo dirty. Needs proper testing.
static void emitHalfwordProductsAVX2(int lo, int hi, int s, int t)
{
	xVexPMOVSXWDY(xRegisterSSE(lo), xRegisterSSE(s));
	xVexPMOVSXWDY(xRegisterSSE(hi), xRegisterSSE(t));
	xVexPMULLDY(xRegisterSSE(lo), xRegisterSSE(lo), xRegisterSSE(hi));
	xVexPERMQY(xRegisterSSE(lo), xRegisterSSE(lo), 0xd8);
	xVexEXTRACTI128Y(xRegisterSSE(hi), xRegisterSSE(lo), 1);
}

static bool useWideHalfwordMultiply()
{
	return avx512.HasCore() || x86Emitter::use_avx;
}

static void emitHalfwordProducts(int lo, int hi, int s, int t)
{
	if (avx512.HasCore())
		emitHalfwordProductsAVX512(lo, hi, s, t);
	else
		emitHalfwordProductsAVX2(lo, hi, s, t);
}

// Rd = {LO0, HI0, LO2, HI2} (= {p0,p2,p4,p6} before accumulation): SHUFPS then PSHUFD 0xd8.
static void emitHalfwordRd(int rd, int lo, int hi)
{
	xSHUF.PS(xRegisterSSE(rd), xRegisterSSE(lo), xRegisterSSE(hi), 0x88);
	xPSHUF.D(xRegisterSSE(rd), xRegisterSSE(rd), 0xd8);
}

#ifndef MMI_RECOMPILE

REC_FUNC_DEL(PLZCW, _Rd_);

REC_FUNC_DEL(PMFHL, _Rd_);
REC_FUNC_DEL(PMTHL, _Rd_);

REC_FUNC_DEL(PSRLW, _Rd_);
REC_FUNC_DEL(PSRLH, _Rd_);

REC_FUNC_DEL(PSRAH, _Rd_);
REC_FUNC_DEL(PSRAW, _Rd_);

REC_FUNC_DEL(PSLLH, _Rd_);
REC_FUNC_DEL(PSLLW, _Rd_);

#else

static bool CanUse3Arg(u32 d, u32 s, u32 t)
{
	if (d == s)
		return true;
	if (d != t)
		return true;
	return x86Emitter::use_avx;
}

template <typename Op>
static void ThreeArg(const Op& op, u32 d, u32 s, u32 t)
{
	if (CanUse3Arg(d, s, t))
	{
		op(xRegisterSSE(d), xRegisterSSE(s), xRegisterSSE(t));
	}
	else
	{
		int t0reg = _allocTempXMMreg(XMMT_INT);
		xMOVDQA(xRegisterSSE(t0reg), xRegisterSSE(t));
		xMOVDQA(xRegisterSSE(d), xRegisterSSE(s));
		op(xRegisterSSE(d), xRegisterSSE(t0reg));
		_freeXMMreg(t0reg);
	}
}

void recPLZCW()
{
	int x86regs = -1;
	int xmmregs = -1;

	if (!_Rd_)
		return;

	// TODO(Stenzek): Don't flush to memory at the end here. Careful of Rs == Rd.

	EE::Profiler.EmitOp(eeOpcode::PLZCW);

	if (GPR_IS_CONST1(_Rs_))
	{
		_eeOnWriteReg(_Rd_, 0);
		_deleteEEreg(_Rd_, 0);
		GPR_SET_CONST(_Rd_);

		// Return the leading sign bits, excluding the original bit
		g_cpuConstRegs[_Rd_].UL[0] = Common::CountLeadingSignBits(g_cpuConstRegs[_Rs_].SL[0]) - 1;
		g_cpuConstRegs[_Rd_].UL[1] = Common::CountLeadingSignBits(g_cpuConstRegs[_Rs_].SL[1]) - 1;

		return;
	}

	if (avx512.HasCore())
	{
		EERecompileInfo info = eeRecompileCodeXMM(XMMINFO_READS | XMMINFO_READD | XMMINFO_WRITED);
		const int scratch = _allocTempXMMreg(XMMT_INT);
		// The low-bank kernel needs no second TEMP; a high-bank scratch still needs a low copy register.
		const bool high_scratch = xRegisterSSE(scratch).IsEVEXHigh();
		const int ones = high_scratch ? _allocTempXMMreg(XMMT_INT) : scratch;
		emitPLZCWAVX512(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_S),
			xRegisterSSE(scratch), xRegisterSSE(ones));
		_freeXMMreg(scratch);
		if (high_scratch)
			_freeXMMreg(ones);
		_clearNeededXMMregs();
		return;
	}

	_eeOnWriteReg(_Rd_, 0);

	// Branchless LZCNT form: count(x ^ (x >> 31)) - 1, where LZCNT(0) = 32 gives the same 31 as the BSR path.
	// Gated on cpuinfo (LZCNT decodes as BSR on old CPUs). Needs proper testing.
	static const bool s_has_lzcnt = cpuinfo_initialize() && cpuinfo_has_x86_lzcnt();
	const auto emitCount = [&](u32 word) {
		xMOV(ecx, eax);
		xSAR(ecx, 31);
		xXOR(eax, ecx);
		xLZCNT(eax, eax);
		xDEC(eax);
		xMOV(ptr[&cpuRegs.GPR.r[_Rd_].UL[word]], eax);
	};

	if ((xmmregs = _checkXMMreg(XMMTYPE_GPRREG, _Rs_, MODE_READ)) >= 0)
	{
		xMOVD(eax, xRegisterSSE(xmmregs));
	}
	else if ((x86regs = _checkX86reg(X86TYPE_GPR, _Rs_, MODE_READ)) >= 0)
	{
		xMOV(eax, xRegister32(x86regs));
	}
	else
	{
		xMOV(eax, ptr[&cpuRegs.GPR.r[_Rs_].UL[0]]);
	}

	_deleteEEreg(_Rd_, DELETE_REG_FREE_NO_WRITEBACK);

	// Count the number of leading bits (MSB) that match the sign bit, excluding the sign
	// bit itself.

	// Strategy: If the sign bit is set, then negate the value.  And that way the same
	// bitcompare can be used for either bit status.  but be warned!  BSR returns undefined
	// results if the EAX is zero, so we need to have special checks for zeros before
	// using it.

	// --- first word ---

	u8* label_notSigned = nullptr;
	u8* label_Zeroed = nullptr;
	if (s_has_lzcnt)
	{
		emitCount(0);
	}
	else
	{
		xMOV(ecx, 31);
		xTEST(eax, eax); // TEST sets the sign flag accordingly.
		label_notSigned = JNS8(0);
		xNOT(eax);
		x86SetJ8(label_notSigned);

		xBSR(eax, eax);
		label_Zeroed = JZ8(0); // If BSR sets the ZF, eax is "trash"
		xSUB(ecx, eax);
		xDEC(ecx); // PS2 doesn't count the first bit

		x86SetJ8(label_Zeroed);
		xMOV(ptr[&cpuRegs.GPR.r[_Rd_].UL[0]], ecx);
	}

	// second word

	if (xmmregs >= 0)
	{
		xPEXTR.D(eax, xRegisterSSE(xmmregs), 1);
	}
	else if (x86regs >= 0)
	{
		xMOV(rax, xRegister64(x86regs));
		xSHR(rax, 32);
	}
	else
	{
		xMOV(eax, ptr[&cpuRegs.GPR.r[_Rs_].UL[1]]);
	}

	if (s_has_lzcnt)
	{
		emitCount(1);
		GPR_DEL_CONST(_Rd_);
		return;
	}

	xMOV(ecx, 31);
	xTEST(eax, eax); // TEST sets the sign flag accordingly.
	label_notSigned = JNS8(0);
	xNOT(eax);
	x86SetJ8(label_notSigned);

	xBSR(eax, eax);
	label_Zeroed = JZ8(0); // If BSR sets the ZF, eax is "trash"
	xSUB(ecx, eax);
	xDEC(ecx); // PS2 doesn't count the first bit

	x86SetJ8(label_Zeroed);
	xMOV(ptr[&cpuRegs.GPR.r[_Rd_].UL[1]], ecx);

	GPR_DEL_CONST(_Rd_);
}

void recPMFHL()
{
	if (!_Rd_)
		return;

	EE::Profiler.EmitOp(eeOpcode::PMFHL);

	EERecompileInfo info = eeRecompileCodeXMM(XMMINFO_WRITED | XMMINFO_READLO | XMMINFO_READHI);

	int t0reg;

	switch (_Sa_)
	{
		case 0x00: // LW
			// SHUFPS + PSHUFD straight into Rd. Only Rd == HI without AVX needs a TEMP.
			if (EEREC_D == EEREC_HI && !x86Emitter::use_avx)
			{
				t0reg = _allocTempXMMreg(XMMT_INT);
				emitPMFHLAVX512(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_LO), xRegisterSSE(EEREC_HI), xRegisterSSE(t0reg), 0);
				_freeXMMreg(t0reg);
			}
			else
				emitPMFHLAVX512(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_LO), xRegisterSSE(EEREC_HI), xRegisterSSE(EEREC_D), 0);
			break;

		case 0x01: // UW
			// SHUFPS + PSHUFD straight into Rd. Only Rd == HI without AVX needs a TEMP.
			if (EEREC_D == EEREC_HI && !x86Emitter::use_avx)
			{
				t0reg = _allocTempXMMreg(XMMT_INT);
				emitPMFHLAVX512(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_LO), xRegisterSSE(EEREC_HI), xRegisterSSE(t0reg), 1);
				_freeXMMreg(t0reg);
			}
			else
				emitPMFHLAVX512(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_LO), xRegisterSSE(EEREC_HI), xRegisterSSE(EEREC_D), 1);
			break;

		case 0x02: // SLW
			if (avx512.HasCore())
			{
				// {LO0,HI0} and {LO2,HI2} as signed qwords, saturate to s32 (VPMOVSQD),
				// then sign-extend back. Needs proper testing against the interpreter.
				// Rd can hold the intermediate unless it aliases LO (HI is read first).
				if (EEREC_D != EEREC_LO)
				{
					emitPMFHLSLWAVX512(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_LO), xRegisterSSE(EEREC_HI),
						xRegisterSSE(EEREC_D));
					break;
				}
				t0reg = _allocTempXMMreg(XMMT_INT);
				emitPMFHLSLWAVX512(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_LO), xRegisterSSE(EEREC_HI),
					xRegisterSSE(t0reg));
				_freeXMMreg(t0reg);
				break;
			}
			// fall to interp
			_deleteEEreg(_Rd_, 0);
			iFlushCall(FLUSH_INTERPRETER); // since calling CALLFunc
			xFastCall((void*)(uptr)R5900::Interpreter::OpcodeImpl::MMI::PMFHL);
			break;

		case 0x03: // LH
			if (avx512.HasCore())
			{
				t0reg = _allocTempXMMreg(XMMT_INT);
				emitPMFHLAVX512(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_LO), xRegisterSSE(EEREC_HI), xRegisterSSE(t0reg), 3);
				_freeXMMreg(t0reg);
				break;
			}
			t0reg = _allocTempXMMreg(XMMT_INT);
			if (x86Emitter::use_avx)
			{
				// {L0,L1,H0,H1,L2,L3,H2,H3}: gather even halfwords of each, then interleave dwords (7 -> 3).
				xPSHUF.B(xRegisterSSE(t0reg), xRegisterSSE(EEREC_LO), ptr128[s_packEvenHalfMask]);
				xPSHUF.B(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_HI), ptr128[s_packEvenHalfMask]);
				xPUNPCK.LDQ(xRegisterSSE(EEREC_D), xRegisterSSE(t0reg), xRegisterSSE(EEREC_D));
				_freeXMMreg(t0reg);
				break;
			}
			xPSHUF.LW(xRegisterSSE(t0reg), xRegisterSSE(EEREC_HI), 0x88);
			xPSHUF.LW(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_LO), 0x88);
			xPSHUF.HW(xRegisterSSE(t0reg), xRegisterSSE(t0reg), 0x88);
			xPSHUF.HW(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_D), 0x88);
			xPSRL.DQ(xRegisterSSE(t0reg), 4);
			xPSRL.DQ(xRegisterSSE(EEREC_D), 4);
			xPUNPCK.LDQ(xRegisterSSE(EEREC_D), xRegisterSSE(t0reg));
			_freeXMMreg(t0reg);
			break;

		case 0x04: // SH
			if (EEREC_D == EEREC_HI)
			{
				xPACK.SSDW(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_LO));
				xPSHUF.D(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_D), 0x72);
			}
			else
			{
				xPACK.SSDW(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_LO), xRegisterSSE(EEREC_HI));

				// shuffle so a1a0b1b0->a1b1a0b0
				xPSHUF.D(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_D), 0xd8);
			}
			break;
		default:
			Console.Error("PMFHL??  *pcsx2 head esplode!*");
			pxFail("PMFHL??  *pcsx2 head esplode!*");
	}

	_clearNeededXMMregs();
}

void recPMTHL()
{
	if (_Sa_ != 0)
		return;

	EE::Profiler.EmitOp(eeOpcode::PMTHL);

	EERecompileInfo info = eeRecompileCodeXMM(XMMINFO_READS | XMMINFO_READLO | XMMINFO_READHI | XMMINFO_WRITELO | XMMINFO_WRITEHI);

	xBLEND.PS(xRegisterSSE(EEREC_LO), xRegisterSSE(EEREC_S), 0x5);
	xSHUF.PS(xRegisterSSE(EEREC_HI), xRegisterSSE(EEREC_S), 0xdd);
	xSHUF.PS(xRegisterSSE(EEREC_HI), xRegisterSSE(EEREC_HI), 0x72);

	_clearNeededXMMregs();
}

////////////////////////////////////////////////////
void recPSRLH()
{
	if (!_Rd_)
		return;

	EE::Profiler.EmitOp(eeOpcode::PSRLH);

	EERecompileInfo info = eeRecompileCodeXMM(XMMINFO_READT | XMMINFO_WRITED);
	if ((_Sa_ & 0xf) == 0)
	{
		xMOVDQA(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_T));
	}
	else
	{
		xPSRL.W(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_T), _Sa_ & 0xf);
	}
	_clearNeededXMMregs();
}

////////////////////////////////////////////////////
void recPSRLW()
{
	if (!_Rd_)
		return;

	EE::Profiler.EmitOp(eeOpcode::PSRLW);

	EERecompileInfo info = eeRecompileCodeXMM(XMMINFO_READT | XMMINFO_WRITED);
	if (_Sa_ == 0)
	{
		xMOVDQA(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_T));
	}
	else
	{
		xPSRL.D(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_T), _Sa_);
	}
	_clearNeededXMMregs();
}

////////////////////////////////////////////////////
void recPSRAH()
{
	if (!_Rd_)
		return;

	EE::Profiler.EmitOp(eeOpcode::PSRAH);

	EERecompileInfo info = eeRecompileCodeXMM(XMMINFO_READT | XMMINFO_WRITED);
	if ((_Sa_ & 0xf) == 0)
	{
		xMOVDQA(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_T));
	}
	else
	{
		xPSRA.W(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_T), _Sa_ & 0xf);
	}
	_clearNeededXMMregs();
}

////////////////////////////////////////////////////
void recPSRAW()
{
	if (!_Rd_)
		return;

	EE::Profiler.EmitOp(eeOpcode::PSRAW);

	EERecompileInfo info = eeRecompileCodeXMM(XMMINFO_READT | XMMINFO_WRITED);
	if (_Sa_ == 0)
	{
		xMOVDQA(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_T));
	}
	else
	{
		xPSRA.D(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_T), _Sa_);
	}
	_clearNeededXMMregs();
}

////////////////////////////////////////////////////
void recPSLLH()
{
	if (!_Rd_)
		return;

	EE::Profiler.EmitOp(eeOpcode::PSLLH);

	EERecompileInfo info = eeRecompileCodeXMM(XMMINFO_READT | XMMINFO_WRITED);
	if ((_Sa_ & 0xf) == 0)
	{
		xMOVDQA(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_T));
	}
	else
	{
		xPSLL.W(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_T), _Sa_ & 0xf);
	}
	_clearNeededXMMregs();
}

////////////////////////////////////////////////////
void recPSLLW()
{
	if (!_Rd_)
		return;

	EE::Profiler.EmitOp(eeOpcode::PSLLW);

	EERecompileInfo info = eeRecompileCodeXMM(XMMINFO_READT | XMMINFO_WRITED);
	if (_Sa_ == 0)
	{
		xMOVDQA(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_T));
	}
	else
	{
		xPSLL.D(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_T), _Sa_);
	}
	_clearNeededXMMregs();
}

/*
void recMADD()
{
}

void recMADDU()
{
}

void recPLZCW()
{
}
*/

#endif

/*********************************************************
*   MMI0 opcodes                                         *
*                                                        *
*********************************************************/
#ifndef MMI0_RECOMPILE

REC_FUNC_DEL(PADDB,  _Rd_);
REC_FUNC_DEL(PADDH,  _Rd_);
REC_FUNC_DEL(PADDW,  _Rd_);
REC_FUNC_DEL(PADDSB, _Rd_);
REC_FUNC_DEL(PADDSH, _Rd_);
REC_FUNC_DEL(PADDSW, _Rd_);
REC_FUNC_DEL(PSUBB,  _Rd_);
REC_FUNC_DEL(PSUBH,  _Rd_);
REC_FUNC_DEL(PSUBW,  _Rd_);
REC_FUNC_DEL(PSUBSB, _Rd_);
REC_FUNC_DEL(PSUBSH, _Rd_);
REC_FUNC_DEL(PSUBSW, _Rd_);

REC_FUNC_DEL(PMAXW,  _Rd_);
REC_FUNC_DEL(PMAXH,  _Rd_);

REC_FUNC_DEL(PCGTW,  _Rd_);
REC_FUNC_DEL(PCGTH,  _Rd_);
REC_FUNC_DEL(PCGTB,  _Rd_);

REC_FUNC_DEL(PEXTLW, _Rd_);

REC_FUNC_DEL(PPACW,  _Rd_);
REC_FUNC_DEL(PEXTLH, _Rd_);
REC_FUNC_DEL(PPACH,  _Rd_);
REC_FUNC_DEL(PEXTLB, _Rd_);
REC_FUNC_DEL(PPACB,  _Rd_);
REC_FUNC_DEL(PEXT5,  _Rd_);
REC_FUNC_DEL(PPAC5,  _Rd_);

#else

////////////////////////////////////////////////////
void recPMAXW()
{
	if (!_Rd_)
		return;

	EE::Profiler.EmitOp(eeOpcode::PMAXW);

	EERecompileInfo info = eeRecompileCodeXMM(XMMINFO_READS | XMMINFO_READT | XMMINFO_WRITED);
	if (EEREC_S == EEREC_T)
		xMOVDQA(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_S));
	else
		xPMAX.SD(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_S), xRegisterSSE(EEREC_T));
	_clearNeededXMMregs();
}

////////////////////////////////////////////////////
void recPPACW()
{
	if (!_Rd_)
		return;

	EE::Profiler.EmitOp(eeOpcode::PPACW);

	EERecompileInfo info = eeRecompileCodeXMM(((_Rs_ != 0) ? XMMINFO_READS : 0) | XMMINFO_READT | XMMINFO_WRITED);

	if (_Rs_ == 0)
	{
		if (avx512.HasCore())
		{
			// [T0, T2, 0, 0] is a plain qword->dword truncation.
			xVPMOVQD(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_T));
		}
		else
		{
			xPSHUF.D(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_T), 0x88);
			xPSRL.DQ(xRegisterSSE(EEREC_D), 8);
		}
	}
	else if (avx512.HasCore())
	{
		emitPPACGeneralAVX512(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_S), xRegisterSSE(EEREC_T), 0);
	}
	else
	{
		int t0reg = _allocTempXMMreg(XMMT_INT);
		if (EEREC_D == EEREC_T)
		{
			xPSHUF.D(xRegisterSSE(t0reg), xRegisterSSE(EEREC_S), 0x88);
			xPSHUF.D(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_T), 0x88);
			xPUNPCK.LQDQ(xRegisterSSE(EEREC_D), xRegisterSSE(t0reg));
			_freeXMMreg(t0reg);
		}
		else
		{
			xPSHUF.D(xRegisterSSE(t0reg), xRegisterSSE(EEREC_T), 0x88);
			xPSHUF.D(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_S), 0x88);
			xPUNPCK.LQDQ(xRegisterSSE(t0reg), xRegisterSSE(EEREC_D));

			// swap mmx regs.. don't ask
			xmmregs[t0reg] = xmmregs[EEREC_D];
			xmmregs[EEREC_D].inuse = 0;
		}
	}

	_clearNeededXMMregs();
}

void recPPACH()
{
	if (!_Rd_)
		return;

	EE::Profiler.EmitOp(eeOpcode::PPACH);

	EERecompileInfo info = eeRecompileCodeXMM((_Rs_ != 0 ? XMMINFO_READS : 0) | XMMINFO_READT | XMMINFO_WRITED);
	if (avx512.HasCore())
	{
		// Even halfwords of each source: dword->word truncation, no shuffle constants.
		if (_Rs_ == 0)
		{
			xVPMOVDW(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_T));
		}
		else
		{
			emitPPACGeneralAVX512(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_S), xRegisterSSE(EEREC_T), 1);
		}
	}
	else if (x86Emitter::use_avx)
	{
		if (_Rs_ == 0)
		{
			emitPackEvenAVX(xRegisterSSE(EEREC_D), nullptr, xRegisterSSE(EEREC_T), xRegisterSSE(EEREC_D), s_packEvenHalfMask);
		}
		else
		{
			const int t0reg = _allocTempXMMreg(XMMT_INT);
			const xRegisterSSE s(EEREC_S);
			emitPackEvenAVX(xRegisterSSE(EEREC_D), &s, xRegisterSSE(EEREC_T), xRegisterSSE(t0reg), s_packEvenHalfMask);
			_freeXMMreg(t0reg);
		}
	}
	else if (_Rs_ == 0)
	{
		xPSHUF.LW(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_T), 0x88);
		xPSHUF.HW(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_D), 0x88);
		xPSLL.DQ(xRegisterSSE(EEREC_D), 4);
		xPSRL.DQ(xRegisterSSE(EEREC_D), 8);
	}
	else
	{
		int t0reg = _allocTempXMMreg(XMMT_INT);
		xPSHUF.LW(xRegisterSSE(t0reg), xRegisterSSE(EEREC_S), 0x88);
		xPSHUF.LW(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_T), 0x88);
		xPSHUF.HW(xRegisterSSE(t0reg), xRegisterSSE(t0reg), 0x88);
		xPSHUF.HW(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_D), 0x88);

		xPSRL.DQ(xRegisterSSE(t0reg), 4);
		xPSRL.DQ(xRegisterSSE(EEREC_D), 4);
		xPUNPCK.LQDQ(xRegisterSSE(EEREC_D), xRegisterSSE(t0reg));

		_freeXMMreg(t0reg);
	}
	_clearNeededXMMregs();
}

////////////////////////////////////////////////////
void recPPACB()
{
	if (!_Rd_)
		return;

	EE::Profiler.EmitOp(eeOpcode::PPACB);

	EERecompileInfo info = eeRecompileCodeXMM((_Rs_ != 0 ? XMMINFO_READS : 0) | XMMINFO_READT | XMMINFO_WRITED);
	if (avx512.HasCore())
	{
		// Low byte of each halfword: word->byte truncation (VPMOVWB), no zero temp.
		if (_Rs_ == 0)
		{
			xVPMOVWB(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_T));
		}
		else
		{
			emitPPACGeneralAVX512(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_S), xRegisterSSE(EEREC_T), 2);
		}
	}
	else if (x86Emitter::use_avx)
	{
		if (_Rs_ == 0)
		{
			emitPackEvenAVX(xRegisterSSE(EEREC_D), nullptr, xRegisterSSE(EEREC_T), xRegisterSSE(EEREC_D), s_packEvenByteMask);
		}
		else
		{
			const int t0reg = _allocTempXMMreg(XMMT_INT);
			const xRegisterSSE s(EEREC_S);
			emitPackEvenAVX(xRegisterSSE(EEREC_D), &s, xRegisterSSE(EEREC_T), xRegisterSSE(t0reg), s_packEvenByteMask);
			_freeXMMreg(t0reg);
		}
	}
	else if (_Rs_ == 0)
	{
		const int t0reg = _allocTempXMMreg(XMMT_INT);

		xPSLL.W(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_T), 8);
		xPXOR(xRegisterSSE(t0reg), xRegisterSSE(t0reg));
		xPSRL.W(xRegisterSSE(EEREC_D), 8);
		xPACK.USWB(xRegisterSSE(EEREC_D), xRegisterSSE(t0reg));

		_freeXMMreg(t0reg);
	}
	else
	{
		const int t0reg = _allocTempXMMreg(XMMT_INT);

		xPSLL.W(xRegisterSSE(t0reg),   xRegisterSSE(EEREC_S), 8);
		xPSLL.W(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_T), 8);
		xPSRL.W(xRegisterSSE(t0reg), 8);
		xPSRL.W(xRegisterSSE(EEREC_D), 8);

		xPACK.USWB(xRegisterSSE(EEREC_D), xRegisterSSE(t0reg));
		_freeXMMreg(t0reg);
	}
	_clearNeededXMMregs();
}

////////////////////////////////////////////////////
void recPEXT5()
{
	if (!_Rd_)
		return;

	EE::Profiler.EmitOp(eeOpcode::PEXT5);

	EERecompileInfo info = eeRecompileCodeXMM(XMMINFO_READT | XMMINFO_WRITED);
	if (avx512.HasCore())
	{
		// Rd holds the control constant unless it aliases Rt.
		const int a0 = EEREC_D != EEREC_T ? EEREC_D : _allocTempXMMreg(XMMT_INT);
		emitPEXT5AVX512(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_T), xRegisterSSE(a0));
		if (a0 != EEREC_D)
			_freeXMMreg(a0);
		_clearNeededXMMregs();
		return;
	}
	int t0reg = _allocTempXMMreg(XMMT_INT);
	int t1reg = _allocTempXMMreg(XMMT_INT);

	xPSLL.D(xRegisterSSE(t0reg), xRegisterSSE(EEREC_T), 22); // for bit 5..9
	xPSRL.W(xRegisterSSE(t1reg), xRegisterSSE(EEREC_T), 15); // for bit 15
	xPSRL.D(xRegisterSSE(t0reg), 27);
	xPSLL.D(xRegisterSSE(t1reg), 20);
	xPOR(xRegisterSSE(t0reg), xRegisterSSE(t1reg));

	xPSLL.D(xRegisterSSE(t1reg),   xRegisterSSE(EEREC_T), 17); // for bit 10..14
	xPSLL.D(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_T), 27); // for bit 0..4
	xPSRL.D(xRegisterSSE(EEREC_D), 27);
	xPSRL.W(xRegisterSSE(t1reg),   11);
	xPOR(xRegisterSSE(EEREC_D), xRegisterSSE(t1reg));

	xPSLL.W(xRegisterSSE(EEREC_D), 3);
	xPSLL.W(xRegisterSSE(t0reg),  11);
	xPOR(xRegisterSSE(EEREC_D), xRegisterSSE(t0reg));

	_freeXMMreg(t0reg);
	_freeXMMreg(t1reg);
	_clearNeededXMMregs();
}

////////////////////////////////////////////////////
void recPPAC5()
{
	if (!_Rd_)
		return;

	EE::Profiler.EmitOp(eeOpcode::PPAC5);

	EERecompileInfo info = eeRecompileCodeXMM(XMMINFO_READT | XMMINFO_WRITED);
	if (avx512.HasCore())
	{
		// Rd holds the control constant unless it aliases Rt. 5 instructions, needs proper testing.
		const int a0 = EEREC_D != EEREC_T ? EEREC_D : _allocTempXMMreg(XMMT_INT);
		emitPPAC5AVX512(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_T), xRegisterSSE(a0));
		if (a0 != EEREC_D)
			_freeXMMreg(a0);
		_clearNeededXMMregs();
		return;
	}

	int t0reg = _allocTempXMMreg(XMMT_INT);
	int t1reg = _allocTempXMMreg(XMMT_INT);

	if (x86Emitter::use_avx)
	{
		emitPPAC5AVX(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_T), xRegisterSSE(t0reg), xRegisterSSE(t1reg));
		_freeXMMreg(t0reg);
		_freeXMMreg(t1reg);
		_clearNeededXMMregs();
		return;
	}

	xPSLL.D(xRegisterSSE(t0reg), xRegisterSSE(EEREC_T),  8); // for bit 10..14
	xPSRL.D(xRegisterSSE(t1reg), xRegisterSSE(EEREC_T), 31); // for bit 15
	xPSRL.D(xRegisterSSE(t0reg), 17);
	xPSLL.D(xRegisterSSE(t1reg), 15);
	xPOR(xRegisterSSE(t0reg), xRegisterSSE(t1reg));

	xPSRL.D(xRegisterSSE(t1reg),   xRegisterSSE(EEREC_T), 11); // for bit 5..9
	xPSLL.D(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_T), 24); // for bit 0..4
	xPSRL.D(xRegisterSSE(EEREC_D), 27);
	xPSLL.D(xRegisterSSE(t1reg),    5);
	xPOR(xRegisterSSE(EEREC_D), xRegisterSSE(t1reg));

	xPCMP.EQD(xRegisterSSE(t1reg), xRegisterSSE(t1reg));
	xPSRL.D(xRegisterSSE(t1reg), 22);
	if (avx512.HasCore())
		emitMMIBitSelect(xRegisterSSE(EEREC_D), xRegisterSSE(t1reg), xRegisterSSE(t0reg));
	else
	{
		xPAND(xRegisterSSE(EEREC_D), xRegisterSSE(t1reg));
		xPANDN(xRegisterSSE(t1reg), xRegisterSSE(t0reg));
		xPOR(xRegisterSSE(EEREC_D), xRegisterSSE(t1reg));
	}

	_freeXMMreg(t0reg);
	_freeXMMreg(t1reg);
	_clearNeededXMMregs();
}

////////////////////////////////////////////////////
void recPMAXH()
{
	if (!_Rd_)
		return;

	EE::Profiler.EmitOp(eeOpcode::PMAXH);

	EERecompileInfo info = eeRecompileCodeXMM(XMMINFO_READS | XMMINFO_READT | XMMINFO_WRITED);
	xPMAX.SW(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_S), xRegisterSSE(EEREC_T));
	_clearNeededXMMregs();
}

////////////////////////////////////////////////////
void recPCGTB()
{
	if (!_Rd_)
		return;

	EE::Profiler.EmitOp(eeOpcode::PCGTB);

	EERecompileInfo info = eeRecompileCodeXMM(XMMINFO_READS | XMMINFO_READT | XMMINFO_WRITED);
	ThreeArg(xPCMP.GTB, EEREC_D, EEREC_S, EEREC_T);
	_clearNeededXMMregs();
}

////////////////////////////////////////////////////
void recPCGTH()
{
	if (!_Rd_)
		return;

	EE::Profiler.EmitOp(eeOpcode::PCGTH);

	EERecompileInfo info = eeRecompileCodeXMM((_Rs_ != 0 ? XMMINFO_READS : 0) | XMMINFO_READT | XMMINFO_WRITED);
	if (_Rs_ == 0)
	{
		// 0 > Rt is the sign mask of Rt (needs proper testing)
		xPSRA.W(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_T), 15);
	}
	else
	{
		ThreeArg(xPCMP.GTW, EEREC_D, EEREC_S, EEREC_T);
	}
	_clearNeededXMMregs();
}

////////////////////////////////////////////////////
void recPCGTW()
{
	if (!_Rd_)
		return;

	EE::Profiler.EmitOp(eeOpcode::PCGTW);

	EERecompileInfo info = eeRecompileCodeXMM((_Rs_ != 0 ? XMMINFO_READS : 0) | XMMINFO_READT | XMMINFO_WRITED);
	if (_Rs_ == 0)
		xPSRA.D(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_T), 31);
	else
		ThreeArg(xPCMP.GTD, EEREC_D, EEREC_S, EEREC_T);
	_clearNeededXMMregs();
}

////////////////////////////////////////////////////
void recPADDSB()
{
	if (!_Rd_)
		return;

	EE::Profiler.EmitOp(eeOpcode::PADDSB);

	EERecompileInfo info = eeRecompileCodeXMM(XMMINFO_READS | XMMINFO_READT | XMMINFO_WRITED);
	xPADD.SB(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_S), xRegisterSSE(EEREC_T));
	_clearNeededXMMregs();
}

////////////////////////////////////////////////////
void recPADDSH()
{
	if (!_Rd_)
		return;

	EE::Profiler.EmitOp(eeOpcode::PADDSH);

	EERecompileInfo info = eeRecompileCodeXMM(XMMINFO_READS | XMMINFO_READT | XMMINFO_WRITED);
	xPADD.SW(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_S), xRegisterSSE(EEREC_T));
	_clearNeededXMMregs();
}

////////////////////////////////////////////////////
//NOTE: check kh2 movies if changing this
void recPADDSW()
{
	if (!_Rd_)
		return;

	EE::Profiler.EmitOp(eeOpcode::PADDSW);

	EERecompileInfo info = eeRecompileCodeXMM(XMMINFO_READS | XMMINFO_READT | XMMINFO_WRITED);
	if (avx512.HasCore())
	{
		const int a0 = _allocTempXMMreg(XMMT_INT);
		const int a1 = _allocTempXMMreg(XMMT_INT);
		emitPADDSWSubSWAVX512(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_S), xRegisterSSE(EEREC_T),
			xRegisterSSE(a0), xRegisterSSE(a1), false);
		_freeXMMreg(a0);
		_freeXMMreg(a1);
		_clearNeededXMMregs();
		return;
	}
	int t0reg = _allocTempXMMreg(XMMT_INT);
	int t1reg = _allocTempXMMreg(XMMT_INT);
	int t2reg = _allocTempXMMreg(XMMT_INT);

	// The idea is:
	//  s = x + y; (wrap-arounded)
	//  if Sign(x) == Sign(y) && Sign(s) != Sign(x) && Sign(x) == 0 then positive overflow (clamp with 0x7fffffff)
	//  if Sign(x) == Sign(y) && Sign(s) != Sign(x) && Sign(x) == 1 then negative overflow (clamp with 0x80000000)

	if (EEREC_S == EEREC_T)
	{
		if (EEREC_D == EEREC_S)
			xMOVDQA(xRegisterSSE(t0reg), xRegisterSSE(EEREC_S));

		// normal addition
		xPADD.D(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_S), xRegisterSSE(EEREC_T));

		xPXOR(xRegisterSSE(t0reg), xRegisterSSE(EEREC_D == EEREC_S ? t0reg : EEREC_S), xRegisterSSE(EEREC_D));
	}
	else
	{
		// get sign bit
		xPXOR(xRegisterSSE(t0reg), xRegisterSSE(EEREC_S), xRegisterSSE(EEREC_T)); // Sign(Rs) != Sign(Rt)

		// normal addition
		xPADD.D(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_S), xRegisterSSE(EEREC_T));

		xPXOR(xRegisterSSE(t1reg), xRegisterSSE(EEREC_D == EEREC_S ? EEREC_T : EEREC_S), xRegisterSSE(EEREC_D)); // Sign(Rs) != Sign(Rd)
		xPANDN(xRegisterSSE(t0reg), xRegisterSSE(t1reg)); // (Sign(Rs) == Sign(Rt)) & (Sign(Rs) != Sign(Rd))
	}

	xPSRA.D(xRegisterSSE(t0reg), 31);

	xPCMP.EQD(xRegisterSSE(t1reg), xRegisterSSE(t1reg));
	xPXOR(xRegisterSSE(t0reg), xRegisterSSE(t1reg)); // could've been avoided if Intel wasn't too prudish for a PORN instruction
	xPSLL.D(xRegisterSSE(t1reg), 31); // 0x80000000

	xPSRA.D(xRegisterSSE(t2reg), xRegisterSSE(EEREC_D), 31);
	xPXOR(xRegisterSSE(t1reg), xRegisterSSE(t2reg)); // t2reg = (Rd < 0) ? 0x7fffffff : 0x80000000

	if (avx512.HasCore())
		emitMMIBitSelect(xRegisterSSE(EEREC_D), xRegisterSSE(t0reg), xRegisterSSE(t1reg));
	else
	{
		xPAND(xRegisterSSE(EEREC_D), xRegisterSSE(t0reg));
		xPANDN(xRegisterSSE(t0reg), xRegisterSSE(t1reg));
		xPOR(xRegisterSSE(EEREC_D), xRegisterSSE(t0reg));
	}

	_freeXMMreg(t0reg);
	_freeXMMreg(t1reg);
	_freeXMMreg(t2reg);
	_clearNeededXMMregs();
}

////////////////////////////////////////////////////
void recPSUBSB()
{
	if (!_Rd_)
		return;

	EE::Profiler.EmitOp(eeOpcode::PSUBSB);

	EERecompileInfo info = eeRecompileCodeXMM(XMMINFO_READS | XMMINFO_READT | XMMINFO_WRITED);
	ThreeArg(xPSUB.SB, EEREC_D, EEREC_S, EEREC_T);
	_clearNeededXMMregs();
}

////////////////////////////////////////////////////
void recPSUBSH()
{
	if (!_Rd_)
		return;

	EE::Profiler.EmitOp(eeOpcode::PSUBSH);

	EERecompileInfo info = eeRecompileCodeXMM(XMMINFO_READS | XMMINFO_READT | XMMINFO_WRITED);
	ThreeArg(xPSUB.SW, EEREC_D, EEREC_S, EEREC_T);
	_clearNeededXMMregs();
}

////////////////////////////////////////////////////
//NOTE: check kh2 movies if changing this
void recPSUBSW()
{
	if (!_Rd_)
		return;

	EE::Profiler.EmitOp(eeOpcode::PSUBSW);

	EERecompileInfo info = eeRecompileCodeXMM(XMMINFO_READS | XMMINFO_READT | XMMINFO_WRITED);
	if (avx512.HasCore())
	{
		const int a0 = _allocTempXMMreg(XMMT_INT);
		const int a1 = _allocTempXMMreg(XMMT_INT);
		emitPADDSWSubSWAVX512(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_S), xRegisterSSE(EEREC_T),
			xRegisterSSE(a0), xRegisterSSE(a1), true);
		_freeXMMreg(a0);
		_freeXMMreg(a1);
		_clearNeededXMMregs();
		return;
	}
	int t0reg = _allocTempXMMreg(XMMT_INT);
	int t1reg = _allocTempXMMreg(XMMT_INT);
	int t2reg = _allocTempXMMreg(XMMT_INT);

	// The idea is:
	//  s = x - y; (wrap-arounded)
	//  if Sign(x) != Sign(y) && Sign(s) != Sign(x) && Sign(x) == 0 then positive overflow (clamp with 0x7fffffff)
	//  if Sign(x) != Sign(y) && Sign(s) != Sign(x) && Sign(x) == 1 then negative overflow (clamp with 0x80000000)

	// get sign bit
	xPSRL.D(xRegisterSSE(t0reg), xRegisterSSE(EEREC_S), 31);
	xPSRL.D(xRegisterSSE(t1reg), xRegisterSSE(EEREC_T), 31);

	// normal subtraction
	if (EEREC_D != EEREC_T || x86Emitter::use_avx)
		xPSUB.D(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_S), xRegisterSSE(EEREC_T));
	else
	{
		xMOVDQA(xRegisterSSE(t2reg), xRegisterSSE(EEREC_T));
		xMOVDQA(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_S));
		xPSUB.D(xRegisterSSE(EEREC_D), xRegisterSSE(t2reg));
	}

	// overflow check
	// t2reg = 0xffffffff if NOT overflow, else 0
	xPSRL.D(xRegisterSSE(t2reg), xRegisterSSE(EEREC_D), 31);
	xPCMP.EQD(xRegisterSSE(t1reg), xRegisterSSE(t0reg)); // Sign(Rs) == Sign(Rt)
	xPCMP.EQD(xRegisterSSE(t2reg), xRegisterSSE(t0reg)); // Sign(Rs) == Sign(Rd)
	xPOR(xRegisterSSE(t2reg), xRegisterSSE(t1reg)); // (Sign(Rs) == Sign(Rt)) | (Sign(Rs) == Sign(Rd))
	xPCMP.EQD(xRegisterSSE(t1reg), xRegisterSSE(t1reg));
	xPSRL.D(xRegisterSSE(t1reg), 1); // 0x7fffffff
	xPADD.D(xRegisterSSE(t1reg), xRegisterSSE(t0reg)); // t1reg = (Rs < 0) ? 0x80000000 : 0x7fffffff

	// saturation
	if (avx512.HasCore())
		emitMMIBitSelect(xRegisterSSE(EEREC_D), xRegisterSSE(t2reg), xRegisterSSE(t1reg));
	else
	{
		xPAND(xRegisterSSE(EEREC_D), xRegisterSSE(t2reg));
		xPANDN(xRegisterSSE(t2reg), xRegisterSSE(t1reg));
		xPOR(xRegisterSSE(EEREC_D), xRegisterSSE(t2reg));
	}

	_freeXMMreg(t0reg);
	_freeXMMreg(t1reg);
	_freeXMMreg(t2reg);
	_clearNeededXMMregs();
}

////////////////////////////////////////////////////
void recPADDB()
{
	if (!_Rd_)
		return;

	EE::Profiler.EmitOp(eeOpcode::PADDB);

	EERecompileInfo info = eeRecompileCodeXMM(XMMINFO_READS | XMMINFO_READT | XMMINFO_WRITED);
	xPADD.B(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_S), xRegisterSSE(EEREC_T));
	_clearNeededXMMregs();
}

////////////////////////////////////////////////////
void recPADDH()
{
	if (!_Rd_)
		return;

	EE::Profiler.EmitOp(eeOpcode::PADDH);

	EERecompileInfo info = eeRecompileCodeXMM((_Rs_ != 0 ? XMMINFO_READS : 0) | (_Rt_ != 0 ? XMMINFO_READT : 0) | XMMINFO_WRITED);
	if (_Rs_ == 0)
	{
		if (_Rt_ == 0)
			xPXOR(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_D));
		else
			xMOVDQA(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_T));
	}
	else if (_Rt_ == 0)
	{
		xMOVDQA(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_S));
	}
	else
	{
		xPADD.W(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_S), xRegisterSSE(EEREC_T));
	}
	_clearNeededXMMregs();
}

////////////////////////////////////////////////////
void recPADDW()
{
	if (!_Rd_)
		return;

	EE::Profiler.EmitOp(eeOpcode::PADDW);

	EERecompileInfo info = eeRecompileCodeXMM((_Rs_ != 0 ? XMMINFO_READS : 0) | (_Rt_ != 0 ? XMMINFO_READT : 0) | XMMINFO_WRITED);
	if (_Rs_ == 0)
	{
		if (_Rt_ == 0)
			xPXOR(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_D));
		else
			xMOVDQA(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_T));
	}
	else if (_Rt_ == 0)
	{
		xMOVDQA(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_S));
	}
	else
	{
		xPADD.D(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_S), xRegisterSSE(EEREC_T));
	}
	_clearNeededXMMregs();
}

////////////////////////////////////////////////////
void recPSUBB()
{
	if (!_Rd_)
		return;

	EE::Profiler.EmitOp(eeOpcode::PSUBB);

	EERecompileInfo info = eeRecompileCodeXMM(XMMINFO_READS | XMMINFO_READT | XMMINFO_WRITED);
	ThreeArg(xPSUB.B, EEREC_D, EEREC_S, EEREC_T);
	_clearNeededXMMregs();
}

////////////////////////////////////////////////////
void recPSUBH()
{
	if (!_Rd_)
		return;

	EE::Profiler.EmitOp(eeOpcode::PSUBH);

	EERecompileInfo info = eeRecompileCodeXMM(XMMINFO_READS | XMMINFO_READT | XMMINFO_WRITED);
	if (EEREC_D != EEREC_T || x86Emitter::use_avx)
		xPSUB.W(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_S), xRegisterSSE(EEREC_T));
	else
	{
		int t0reg = _allocTempXMMreg(XMMT_INT);
		xMOVDQA(xRegisterSSE(t0reg), xRegisterSSE(EEREC_T));
		xMOVDQA(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_S));
		xPSUB.W(xRegisterSSE(EEREC_D), xRegisterSSE(t0reg));
		_freeXMMreg(t0reg);
	}
	_clearNeededXMMregs();
}

////////////////////////////////////////////////////
void recPSUBW()
{
	if (!_Rd_)
		return;

	EE::Profiler.EmitOp(eeOpcode::PSUBW);

	EERecompileInfo info = eeRecompileCodeXMM(XMMINFO_READS | XMMINFO_READT | XMMINFO_WRITED);
	ThreeArg(xPSUB.D, EEREC_D, EEREC_S, EEREC_T);
	_clearNeededXMMregs();
}

////////////////////////////////////////////////////
void recPEXTLW()
{
	if (!_Rd_)
		return;

	EE::Profiler.EmitOp(eeOpcode::PEXTLW);

	EERecompileInfo info = eeRecompileCodeXMM((_Rs_ != 0 ? XMMINFO_READS : 0) | XMMINFO_READT | XMMINFO_WRITED);
	if (_Rs_ == 0)
	{
		xPMOVZX.DQ(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_T));
	}
	else
	{
		ThreeArg(xPUNPCK.LDQ, EEREC_D, EEREC_T, EEREC_S);
	}
	_clearNeededXMMregs();
}

void recPEXTLB()
{
	if (!_Rd_)
		return;

	EE::Profiler.EmitOp(eeOpcode::PEXTLB);

	EERecompileInfo info = eeRecompileCodeXMM((_Rs_ != 0 ? XMMINFO_READS : 0) | XMMINFO_READT | XMMINFO_WRITED);
	if (_Rs_ == 0)
	{
		xPMOVZX.BW(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_T));
	}
	else
	{
		ThreeArg(xPUNPCK.LBW, EEREC_D, EEREC_T, EEREC_S);
	}
	_clearNeededXMMregs();
}

void recPEXTLH()
{
	if (!_Rd_)
		return;

	EE::Profiler.EmitOp(eeOpcode::PEXTLH);

	EERecompileInfo info = eeRecompileCodeXMM((_Rs_ != 0 ? XMMINFO_READS : 0) | XMMINFO_READT | XMMINFO_WRITED);
	if (_Rs_ == 0)
	{
		xPMOVZX.WD(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_T));
	}
	else
	{
		ThreeArg(xPUNPCK.LWD, EEREC_D, EEREC_T, EEREC_S);
	}
	_clearNeededXMMregs();
}

#endif

/*********************************************************
*   MMI1 opcodes                                         *
*                                                        *
*********************************************************/
#ifndef MMI1_RECOMPILE

REC_FUNC_DEL(PABSW,  _Rd_);
REC_FUNC_DEL(PABSH,  _Rd_);

REC_FUNC_DEL(PMINW,  _Rd_);
REC_FUNC_DEL(PADSBH, _Rd_);
REC_FUNC_DEL(PMINH,  _Rd_);
REC_FUNC_DEL(PCEQB,  _Rd_);
REC_FUNC_DEL(PCEQH,  _Rd_);
REC_FUNC_DEL(PCEQW,  _Rd_);

REC_FUNC_DEL(PADDUB, _Rd_);
REC_FUNC_DEL(PADDUH, _Rd_);
REC_FUNC_DEL(PADDUW, _Rd_);

REC_FUNC_DEL(PSUBUB, _Rd_);
REC_FUNC_DEL(PSUBUH, _Rd_);
REC_FUNC_DEL(PSUBUW, _Rd_);

REC_FUNC_DEL(PEXTUW, _Rd_);
REC_FUNC_DEL(PEXTUH, _Rd_);
REC_FUNC_DEL(PEXTUB, _Rd_);
REC_FUNC_DEL(QFSRV,  _Rd_);

#else

////////////////////////////////////////////////////

void recPABSW() //needs clamping
{
	if (!_Rd_)
		return;

	EE::Profiler.EmitOp(eeOpcode::PABSW);

	EERecompileInfo info = eeRecompileCodeXMM(XMMINFO_READT | XMMINFO_WRITED);
	// PABS + unsigned min with INT_MAX: no TEMP, SSE4.1 only.
	emitPABSAVX512(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_T), false);
	_clearNeededXMMregs();
}

////////////////////////////////////////////////////
void recPABSH()
{
	if (!_Rd_)
		return;

	EE::Profiler.EmitOp(eeOpcode::PABSH);

	EERecompileInfo info = eeRecompileCodeXMM(XMMINFO_READT | XMMINFO_WRITED);
	// PABS + unsigned min with INT_MAX: no TEMP, SSE4.1 only.
	emitPABSAVX512(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_T), true);
	_clearNeededXMMregs();
}

////////////////////////////////////////////////////
void recPMINW()
{
	if (!_Rd_)
		return;

	EE::Profiler.EmitOp(eeOpcode::PMINW);

	EERecompileInfo info = eeRecompileCodeXMM(XMMINFO_READS | XMMINFO_READT | XMMINFO_WRITED);
	xPMIN.SD(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_S), xRegisterSSE(EEREC_T));
	_clearNeededXMMregs();
}

////////////////////////////////////////////////////
alignas(16) static const s16 s_padsbhSign[8] = {-1, -1, -1, -1, 1, 1, 1, 1};

void recPADSBH()
{
	if (!_Rd_)
		return;

	EE::Profiler.EmitOp(eeOpcode::PADSBH);

	const EERecompileInfo info = eeRecompileCodeXMM(XMMINFO_READS | XMMINFO_READT | XMMINFO_WRITED);

	if (avx512.HasCore() || x86Emitter::use_avx)
	{
		// Only 128-bit VEX forms are used, so plain AVX is enough. Negate the low four Rt halfwords and preserve the high four, then add Rs.
		// When Rd aliases Rs, keep the signed Rt in a TEMP so the old Rs survives.
		const int signed_t = (EEREC_D == EEREC_S) ? _allocTempXMMreg(XMMT_INT) : EEREC_D;
		xPSIGN.W(xRegisterSSE(signed_t), xRegisterSSE(EEREC_T), ptr128[s_padsbhSign]);
		xPADD.W(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_S), xRegisterSSE(signed_t));
		if (signed_t != EEREC_D)
			_freeXMMreg(signed_t);
	}
	else if (EEREC_S == EEREC_T)
	{
		xPADD.W(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_S), xRegisterSSE(EEREC_T));
		// reset lower bits to 0s
		xPSRL.DQ(xRegisterSSE(EEREC_D), 8);
		xPSLL.DQ(xRegisterSSE(EEREC_D), 8);
	}
	else
	{
		const int t0reg = _allocTempXMMreg(XMMT_INT);

		xPSUB.W(xRegisterSSE(t0reg),   xRegisterSSE(EEREC_S), xRegisterSSE(EEREC_T));
		xPADD.W(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_S), xRegisterSSE(EEREC_T));

		// t0reg - subs, EEREC_D - adds
		xPBLEND.W(xRegisterSSE(EEREC_D), xRegisterSSE(t0reg), 0x0f);
		_freeXMMreg(t0reg);
	}

	_clearNeededXMMregs();
}

////////////////////////////////////////////////////
void recPADDUW()
{
	if (!_Rd_)
		return;

	EE::Profiler.EmitOp(eeOpcode::PADDUW);

	EERecompileInfo info = eeRecompileCodeXMM((_Rs_ ? XMMINFO_READS : 0) | (_Rt_ ? XMMINFO_READT : 0) | XMMINFO_WRITED);

	if (_Rt_ == 0)
	{
		if (_Rs_ == 0)
		{
			xPXOR(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_D));
		}
		else
			xMOVDQA(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_S));
	}
	else if (_Rs_ == 0)
	{
		xMOVDQA(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_T));
	}
	else
	{
		// ~S + min, SSE4.1 only (ternlog form under AVX-512). Rd serves as TEMP when it aliases neither input.
		const bool own = EEREC_D != EEREC_S && EEREC_D != EEREC_T;
		const int a0 = own ? EEREC_D : _allocTempXMMreg(XMMT_INT);
		emitPADDUWAVX512(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_S), xRegisterSSE(EEREC_T), xRegisterSSE(a0));
		if (!own)
			_freeXMMreg(a0);
	}
	_clearNeededXMMregs();
}

////////////////////////////////////////////////////
void recPSUBUB()
{
	if (!_Rd_)
		return;

	EE::Profiler.EmitOp(eeOpcode::PSUBUB);

	EERecompileInfo info = eeRecompileCodeXMM(XMMINFO_READS | XMMINFO_READT | XMMINFO_WRITED);
	ThreeArg(xPSUB.USB, EEREC_D, EEREC_S, EEREC_T);
	_clearNeededXMMregs();
}

////////////////////////////////////////////////////
void recPSUBUH()
{
	if (!_Rd_)
		return;

	EE::Profiler.EmitOp(eeOpcode::PSUBUH);

	EERecompileInfo info = eeRecompileCodeXMM(XMMINFO_READS | XMMINFO_READT | XMMINFO_WRITED);
	ThreeArg(xPSUB.USW, EEREC_D, EEREC_S, EEREC_T);
	_clearNeededXMMregs();
}

////////////////////////////////////////////////////
void recPSUBUW()
{
	if (!_Rd_)
		return;

	EE::Profiler.EmitOp(eeOpcode::PSUBUW);

	EERecompileInfo info = eeRecompileCodeXMM(XMMINFO_READS | XMMINFO_READT | XMMINFO_WRITED);
	const bool own = EEREC_D != EEREC_T; // only Rd == Rt needs a TEMP
	const int a0 = own ? EEREC_D : _allocTempXMMreg(XMMT_INT);
	emitPSUBUWAVX512(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_S), xRegisterSSE(EEREC_T), xRegisterSSE(a0));
	if (!own)
		_freeXMMreg(a0);
	_clearNeededXMMregs();
}

////////////////////////////////////////////////////
void recPEXTUH()
{
	if (!_Rd_)
		return;

	EE::Profiler.EmitOp(eeOpcode::PEXTUH);

	EERecompileInfo info = eeRecompileCodeXMM((_Rs_ != 0 ? XMMINFO_READS : 0) | XMMINFO_READT | XMMINFO_WRITED);
	if (_Rs_ == 0)
	{
		xPUNPCK.HWD(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_T));
		xPSRL.D(xRegisterSSE(EEREC_D), 16);
	}
	else
	{
		ThreeArg(xPUNPCK.HWD, EEREC_D, EEREC_T, EEREC_S);
	}
	_clearNeededXMMregs();
}

alignas(16) static u32 tempqw[8];
alignas(16) static const u8 s_qfsrvIota[16] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};

void recQFSRV()
{
	if (!_Rd_)
		return;
	//Console.WriteLn("recQFSRV()");

	EE::Profiler.EmitOp(eeOpcode::QFSRV);

	if (_Rs_ == _Rt_ + 1)
	{
		_flushEEreg(_Rs_);
		_flushEEreg(_Rt_);
		EERecompileInfo info = eeRecompileCodeXMM(XMMINFO_WRITED);

		xMOV(eax, ptr32[&cpuRegs.sa]);
		xLEA(rcx, ptr[&cpuRegs.GPR.r[_Rt_]]);
		xMOVDQU(xRegisterSSE(EEREC_D), ptr32[rax + rcx]);
		return;
	}

	EERecompileInfo info = eeRecompileCodeXMM(XMMINFO_READS | XMMINFO_READT | XMMINFO_WRITED);

	if (avx512.HasCore())
	{
		xMOV(eax, ptr32[&cpuRegs.sa]);
		if (EEREC_D != EEREC_S && EEREC_D != EEREC_T)
		{
			// The index register becomes the result, so Rd can serve as it directly.
			emitQFSRVAVX512(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_S), xRegisterSSE(EEREC_T), eax, s_qfsrvIota);
		}
		else
		{
			const int a0 = _allocTempXMMreg(XMMT_INT);
			emitQFSRVAVX512(xRegisterSSE(a0), xRegisterSSE(EEREC_S), xRegisterSSE(EEREC_T), eax, s_qfsrvIota);
			// Both sources are dead now: hand the guest register over to the temp instead of copying (as PPACW does).
			xmmregs[a0] = xmmregs[EEREC_D];
			xmmregs[EEREC_D].inuse = 0;
		}
		_clearNeededXMMregs();
		return;
	}
	xMOV(eax, ptr32[&cpuRegs.sa]);
	xLEA(rcx, ptr[tempqw]);
	xMOVDQA(ptr32[rcx], xRegisterSSE(EEREC_T));
	xMOVDQA(ptr32[rcx + 16], xRegisterSSE(EEREC_S));
	xMOVDQU(xRegisterSSE(EEREC_D), ptr32[rax + rcx]);

	_clearNeededXMMregs();
}


void recPEXTUB()
{
	if (!_Rd_)
		return;

	EE::Profiler.EmitOp(eeOpcode::PEXTUB);

	EERecompileInfo info = eeRecompileCodeXMM((_Rs_ != 0 ? XMMINFO_READS : 0) | XMMINFO_READT | XMMINFO_WRITED);

	if (_Rs_ == 0)
	{
		xPUNPCK.HBW(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_T));
		xPSRL.W(xRegisterSSE(EEREC_D), 8);
	}
	else
	{
		ThreeArg(xPUNPCK.HBW, EEREC_D, EEREC_T, EEREC_S);
	}
	_clearNeededXMMregs();
}

////////////////////////////////////////////////////
void recPEXTUW()
{
	if (!_Rd_)
		return;

	EE::Profiler.EmitOp(eeOpcode::PEXTUW);

	EERecompileInfo info = eeRecompileCodeXMM((_Rs_ != 0 ? XMMINFO_READS : 0) | XMMINFO_READT | XMMINFO_WRITED);
	if (_Rs_ == 0)
	{
		xPUNPCK.HDQ(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_T));
		xPSRL.Q(xRegisterSSE(EEREC_D), 32);
	}
	else
	{
		ThreeArg(xPUNPCK.HDQ, EEREC_D, EEREC_T, EEREC_S);
	}
	_clearNeededXMMregs();
}

////////////////////////////////////////////////////
void recPMINH()
{
	if (!_Rd_)
		return;

	EE::Profiler.EmitOp(eeOpcode::PMINH);

	EERecompileInfo info = eeRecompileCodeXMM(XMMINFO_READS | XMMINFO_READT | XMMINFO_WRITED);
	xPMIN.SW(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_S), xRegisterSSE(EEREC_T));
	_clearNeededXMMregs();
}

////////////////////////////////////////////////////
void recPCEQB()
{
	if (!_Rd_)
		return;

	EE::Profiler.EmitOp(eeOpcode::PCEQB);

	EERecompileInfo info = eeRecompileCodeXMM(XMMINFO_READS | XMMINFO_READT | XMMINFO_WRITED);
	xPCMP.EQB(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_S), xRegisterSSE(EEREC_T));
	_clearNeededXMMregs();
}

////////////////////////////////////////////////////
void recPCEQH()
{
	if (!_Rd_)
		return;

	EE::Profiler.EmitOp(eeOpcode::PCEQH);

	EERecompileInfo info = eeRecompileCodeXMM(XMMINFO_READS | XMMINFO_READT | XMMINFO_WRITED);
	xPCMP.EQW(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_S), xRegisterSSE(EEREC_T));
	_clearNeededXMMregs();
}

////////////////////////////////////////////////////
void recPCEQW()
{
	if (!_Rd_)
		return;

	EE::Profiler.EmitOp(eeOpcode::PCEQW);

	EERecompileInfo info = eeRecompileCodeXMM(XMMINFO_READS | XMMINFO_READT | XMMINFO_WRITED);
	xPCMP.EQD(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_S), xRegisterSSE(EEREC_T));
	_clearNeededXMMregs();
}

////////////////////////////////////////////////////
void recPADDUB()
{
	if (!_Rd_)
		return;

	EE::Profiler.EmitOp(eeOpcode::PADDUB);

	EERecompileInfo info = eeRecompileCodeXMM(XMMINFO_READS | (_Rt_ ? XMMINFO_READT : 0) | XMMINFO_WRITED);
	if (_Rt_)
	{
		xPADD.USB(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_S), xRegisterSSE(EEREC_T));
	}
	else
		xMOVDQA(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_S));
	_clearNeededXMMregs();
}

////////////////////////////////////////////////////
void recPADDUH()
{
	if (!_Rd_)
		return;

	EE::Profiler.EmitOp(eeOpcode::PADDUH);

	EERecompileInfo info = eeRecompileCodeXMM(XMMINFO_READS | XMMINFO_READT | XMMINFO_WRITED);
	xPADD.USW(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_S), xRegisterSSE(EEREC_T));
	_clearNeededXMMregs();
}

#endif
/*********************************************************
*   MMI2 opcodes                                         *
*                                                        *
*********************************************************/
#ifndef MMI2_RECOMPILE

REC_FUNC_DEL(PMFHI,  _Rd_);
REC_FUNC_DEL(PMFLO,  _Rd_);
REC_FUNC_DEL(PCPYLD, _Rd_);
REC_FUNC_DEL(PAND,  _Rd_);
REC_FUNC_DEL(PXOR,  _Rd_);

REC_FUNC_DEL(PMADDW, _Rd_);
REC_FUNC_DEL(PSLLVW, _Rd_);
REC_FUNC_DEL(PSRLVW, _Rd_);
REC_FUNC_DEL(PMSUBW, _Rd_);
REC_FUNC_DEL(PINTH,  _Rd_);
REC_FUNC_DEL(PMULTW, _Rd_);
REC_FUNC_DEL(PDIVW,  _Rd_);
REC_FUNC_DEL(PMADDH, _Rd_);
REC_FUNC_DEL(PHMADH, _Rd_);
REC_FUNC_DEL(PMSUBH, _Rd_);
REC_FUNC_DEL(PHMSBH, _Rd_);
REC_FUNC_DEL(PEXEH,  _Rd_);
REC_FUNC_DEL(PREVH,  _Rd_);
REC_FUNC_DEL(PMULTH, _Rd_);
REC_FUNC_DEL(PDIVBW, _Rd_);
REC_FUNC_DEL(PEXEW,  _Rd_);
REC_FUNC_DEL(PROT3W, _Rd_);

#else

////////////////////////////////////////////////////
void recPMADDW()
{
	EE::Profiler.EmitOp(eeOpcode::PMADDW);

	EERecompileInfo info = eeRecompileCodeXMM((((_Rs_) && (_Rt_)) ? XMMINFO_READS : 0) | (((_Rs_) && (_Rt_)) ? XMMINFO_READT : 0) | (_Rd_ ? XMMINFO_WRITED : 0) | XMMINFO_WRITELO | XMMINFO_WRITEHI | XMMINFO_READLO | XMMINFO_READHI);
	xSHUF.PS(xRegisterSSE(EEREC_LO), xRegisterSSE(EEREC_HI), 0x88);
	xPSHUF.D(xRegisterSSE(EEREC_LO), xRegisterSSE(EEREC_LO), 0xd8); // LO = {LO[0], HI[0], LO[2], HI[2]}
	int dst = _Rd_ ? EEREC_D : EEREC_HI;

	if (!_Rs_ || !_Rt_)
		xPXOR(xRegisterSSE(dst), xRegisterSSE(dst));
	else
		xPMUL.DQ(xRegisterSSE(dst), xRegisterSSE(EEREC_S), xRegisterSSE(EEREC_T));

	// add from LO/HI
	xPADD.Q(xRegisterSSE(dst), xRegisterSSE(EEREC_LO));

	// interleave & sign extend
	emitSplitLoHi(EEREC_LO, EEREC_HI, dst);
	_clearNeededXMMregs();
}

////////////////////////////////////////////////////
void recPSLLVW()
{
	if (!_Rd_)
		return;

	EE::Profiler.EmitOp(eeOpcode::PSLLVW);

	EERecompileInfo info = eeRecompileCodeXMM((_Rs_ ? XMMINFO_READS : 0) | (_Rt_ ? XMMINFO_READT : 0) | XMMINFO_WRITED);
	if (_Rs_ == 0)
	{
		if (_Rt_ == 0)
		{
			xPXOR(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_D));
		}
		else
		{
			xPSHUF.D(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_T), 0x88);
			xPMOVSX.DQ(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_D));
		}
	}
	else if (_Rt_ == 0)
	{
		xPXOR(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_D));
	}
	else
	{
		if (avx512.HasCore() || g_cpu.vectorISA >= ProcessorFeatures::VectorISA::AVX2)
		{
			const int a0 = _allocTempXMMreg(XMMT_INT);
			(avx512.HasCore() ? emitPSxxVWAVX512 : emitPSxxVWAVX2)(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_S), xRegisterSSE(EEREC_T),
				xRegisterSSE(a0), 0);
			_freeXMMreg(a0);
			_clearNeededXMMregs();
			return;
		}
		int t0reg = _allocTempXMMreg(XMMT_INT);
		int t1reg = _allocTempXMMreg(XMMT_INT);

		// shamt is 5-bit
		xPSLL.Q(xRegisterSSE(t0reg), xRegisterSSE(EEREC_S), 27 + 32);
		xPSRL.Q(xRegisterSSE(t0reg),                        27 + 32);

		// EEREC_D[0] <- Rt[0], t1reg[0] <- Rt[2]
		xMOVHL.PS(xRegisterSSE(t1reg), xRegisterSSE(EEREC_T));

		// shift (left) Rt[0]
		xPSLL.D(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_T), xRegisterSSE(t0reg));

		// shift (left) Rt[2]
		xMOVHL.PS(xRegisterSSE(t0reg), xRegisterSSE(t0reg));
		xPSLL.D(xRegisterSSE(t1reg), xRegisterSSE(t0reg));

		// merge & sign extend
		xPUNPCK.LDQ(xRegisterSSE(EEREC_D), xRegisterSSE(t1reg));
		xPMOVSX.DQ(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_D));

		_freeXMMreg(t0reg);
		_freeXMMreg(t1reg);
	}
	_clearNeededXMMregs();
}

////////////////////////////////////////////////////
void recPSRLVW()
{
	if (!_Rd_)
		return;

	EE::Profiler.EmitOp(eeOpcode::PSRLVW);

	EERecompileInfo info = eeRecompileCodeXMM((_Rs_ ? XMMINFO_READS : 0) | (_Rt_ ? XMMINFO_READT : 0) | XMMINFO_WRITED);
	if (_Rs_ == 0)
	{
		if (_Rt_ == 0)
		{
			xPXOR(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_D));
		}
		else
		{
			xPSHUF.D(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_T), 0x88);
			xPMOVSX.DQ(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_D));
		}
	}
	else if (_Rt_ == 0)
	{
		xPXOR(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_D));
	}
	else
	{
		if (avx512.HasCore() || g_cpu.vectorISA >= ProcessorFeatures::VectorISA::AVX2)
		{
			const int a0 = _allocTempXMMreg(XMMT_INT);
			(avx512.HasCore() ? emitPSxxVWAVX512 : emitPSxxVWAVX2)(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_S), xRegisterSSE(EEREC_T),
				xRegisterSSE(a0), 1);
			_freeXMMreg(a0);
			_clearNeededXMMregs();
			return;
		}
		int t0reg = _allocTempXMMreg(XMMT_INT);
		int t1reg = _allocTempXMMreg(XMMT_INT);

		// shamt is 5-bit
		xPSLL.Q(xRegisterSSE(t0reg), xRegisterSSE(EEREC_S), 27 + 32);
		xPSRL.Q(xRegisterSSE(t0reg),                        27 + 32);

		// EEREC_D[0] <- Rt[0], t1reg[0] <- Rt[2]
		xMOVHL.PS(xRegisterSSE(t1reg), xRegisterSSE(EEREC_T));

		// shift (right logical) Rt[0]
		xPSRL.D(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_T), xRegisterSSE(t0reg));

		// shift (right logical) Rt[2]
		xMOVHL.PS(xRegisterSSE(t0reg), xRegisterSSE(t0reg));
		xPSRL.D(xRegisterSSE(t1reg), xRegisterSSE(t0reg));

		// merge & sign extend
		xPUNPCK.LDQ(xRegisterSSE(EEREC_D), xRegisterSSE(t1reg));
		xPMOVSX.DQ(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_D));

		_freeXMMreg(t0reg);
		_freeXMMreg(t1reg);
	}
	_clearNeededXMMregs();
}

////////////////////////////////////////////////////
void recPMSUBW()
{
	EE::Profiler.EmitOp(eeOpcode::PMSUBW);

	EERecompileInfo info = eeRecompileCodeXMM((((_Rs_) && (_Rt_)) ? XMMINFO_READS : 0) | (((_Rs_) && (_Rt_)) ? XMMINFO_READT : 0) | (_Rd_ ? XMMINFO_WRITED : 0) | XMMINFO_WRITELO | XMMINFO_WRITEHI | XMMINFO_READLO | XMMINFO_READHI);
	xSHUF.PS(xRegisterSSE(EEREC_LO), xRegisterSSE(EEREC_HI), 0x88);
	xPSHUF.D(xRegisterSSE(EEREC_LO), xRegisterSSE(EEREC_LO), 0xd8); // LO = {LO[0], HI[0], LO[2], HI[2]}
	int dst = _Rd_ ? EEREC_D : EEREC_HI;

	if (!_Rs_ || !_Rt_)
		xPXOR(xRegisterSSE(dst), xRegisterSSE(dst));
	else
		xPMUL.DQ(xRegisterSSE(dst), xRegisterSSE(EEREC_S), xRegisterSSE(EEREC_T));

	// sub from LO/HI
	if (x86Emitter::use_avx)
	{
		xPSUB.Q(xRegisterSSE(dst), xRegisterSSE(EEREC_LO), xRegisterSSE(dst));
	}
	else
	{
		xPSUB.Q(xRegisterSSE(EEREC_LO), xRegisterSSE(dst));
		xMOVDQA(xRegisterSSE(dst), xRegisterSSE(EEREC_LO));
	}

	emitSplitLoHi(EEREC_LO, EEREC_HI, dst);
	_clearNeededXMMregs();
}

////////////////////////////////////////////////////
void recPMULTW()
{
	EE::Profiler.EmitOp(eeOpcode::PMULTW);

	EERecompileInfo info = eeRecompileCodeXMM((((_Rs_) && (_Rt_)) ? XMMINFO_READS : 0) | (((_Rs_) && (_Rt_)) ? XMMINFO_READT : 0) | (_Rd_ ? XMMINFO_WRITED : 0) | XMMINFO_WRITELO | XMMINFO_WRITEHI);
	if (!_Rs_ || !_Rt_)
	{
		if (_Rd_)
			xPXOR(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_D));
		xPXOR(xRegisterSSE(EEREC_LO), xRegisterSSE(EEREC_LO));
		xPXOR(xRegisterSSE(EEREC_HI), xRegisterSSE(EEREC_HI));
	}
	else
	{
		int dst = _Rd_ ? EEREC_D : EEREC_HI;
		xPMUL.DQ(xRegisterSSE(dst), xRegisterSSE(EEREC_S), xRegisterSSE(EEREC_T));

		// interleave & sign extend
		emitSplitLoHi(EEREC_LO, EEREC_HI, dst);
	}
	_clearNeededXMMregs();
}
////////////////////////////////////////////////////
void recPDIVW()
{
	EE::Profiler.EmitOp(eeOpcode::PDIVW);

	_deleteEEreg(_Rd_, 0);
	recCall(Interp::PDIVW);
}

////////////////////////////////////////////////////
void recPDIVBW()
{
	EE::Profiler.EmitOp(eeOpcode::PDIVBW);

	_deleteEEreg(_Rd_, 0);
	recCall(Interp::PDIVBW); //--
}

////////////////////////////////////////////////////

//upper word of each doubleword in LO and HI is undocumented/undefined
//contains the upper multiplication result (before the addition with the lower multiplication result)
void recPHMADH()
{
	EE::Profiler.EmitOp(eeOpcode::PHMADH);

	EERecompileInfo info = eeRecompileCodeXMM((_Rd_ ? XMMINFO_WRITED : 0) | XMMINFO_READS | XMMINFO_READT | XMMINFO_WRITELO | XMMINFO_WRITEHI);
	int t0reg = _allocTempXMMreg(XMMT_INT);

	if (avx512.HasCore() || x86Emitter::use_avx)
	{
		alignas(16) static const u32 s_phmadhOddHalfwords[4] = {
			0xffff0000u, 0xffff0000u, 0xffff0000u, 0xffff0000u};
		xPAND(xRegisterSSE(t0reg), xRegisterSSE(EEREC_S), ptr128[s_phmadhOddHalfwords]);
	}
	else
	{
		xPXOR(xRegisterSSE(t0reg), xRegisterSSE(t0reg));
		xPBLEND.W(xRegisterSSE(t0reg), xRegisterSSE(EEREC_S), 0xaa);
	}
	xPMADD.WD(xRegisterSSE(t0reg), xRegisterSSE(EEREC_T));

	int dst = _Rd_ ? EEREC_D : EEREC_LO;
	xPMADD.WD(xRegisterSSE(dst), xRegisterSSE(EEREC_S), xRegisterSSE(EEREC_T));

	xMOVSHDUP(xRegisterSSE(EEREC_HI), xRegisterSSE(dst));
	xBLEND.PS(xRegisterSSE(EEREC_HI), xRegisterSSE(t0reg), 0xa);
	if (_Rd_)
	{
		xMOVSLDUP(xRegisterSSE(EEREC_LO), xRegisterSSE(t0reg));
		xBLEND.PS(xRegisterSSE(EEREC_LO), xRegisterSSE(dst), 0x5);
	}
	else
	{
		xMOVSLDUP(xRegisterSSE(t0reg), xRegisterSSE(t0reg));
		xBLEND.PS(xRegisterSSE(EEREC_LO), xRegisterSSE(t0reg), 0xa);
	}

	_freeXMMreg(t0reg);
	_clearNeededXMMregs();
}

void recPMSUBH()
{
	EE::Profiler.EmitOp(eeOpcode::PMSUBH);

	EERecompileInfo info = eeRecompileCodeXMM((_Rd_ ? XMMINFO_WRITED : 0) | XMMINFO_READS | XMMINFO_READT | XMMINFO_READLO | XMMINFO_READHI | XMMINFO_WRITELO | XMMINFO_WRITEHI);
	int t0reg = _allocTempXMMreg(XMMT_INT);
	int t1reg = _allocTempXMMreg(XMMT_INT);

	if (useWideHalfwordMultiply())
	{
		emitHalfwordProducts(t0reg, t1reg, EEREC_S, EEREC_T);
		xPSUB.D(xRegisterSSE(EEREC_LO), xRegisterSSE(t0reg));
		xPSUB.D(xRegisterSSE(EEREC_HI), xRegisterSSE(t1reg));
		if (_Rd_)
			emitHalfwordRd(EEREC_D, EEREC_LO, EEREC_HI);
		_freeXMMreg(t0reg);
		_freeXMMreg(t1reg);
		_clearNeededXMMregs();
		return;
	}

	if (!_Rd_)
	{
		xPXOR(xRegisterSSE(t0reg), xRegisterSSE(t0reg));
		xPSHUF.D(xRegisterSSE(t1reg), xRegisterSSE(EEREC_S), 0xd8); //S0, S1, S4, S5, S2, S3, S6, S7
		xPUNPCK.LWD(xRegisterSSE(t1reg), xRegisterSSE(t0reg)); //S0, 0, S1, 0, S4, 0, S5, 0
		xPSHUF.D(xRegisterSSE(t0reg), xRegisterSSE(EEREC_T), 0xd8); //T0, T1, T4, T5, T2, T3, T6, T7
		xPUNPCK.LWD(xRegisterSSE(t0reg), xRegisterSSE(t0reg)); //T0, T0, T1, T1, T4, T4, T5, T5
		xPMADD.WD(xRegisterSSE(t0reg), xRegisterSSE(t1reg)); //S0*T0+0*T0, S1*T1+0*T1, S4*T4+0*T4, S5*T5+0*T5

		xPSUB.D(xRegisterSSE(EEREC_LO), xRegisterSSE(t0reg));

		xPXOR(xRegisterSSE(t0reg), xRegisterSSE(t0reg));
		xPSHUF.D(xRegisterSSE(t1reg), xRegisterSSE(EEREC_S), 0xd8); //S0, S1, S4, S5, S2, S3, S6, S7
		xPUNPCK.HWD(xRegisterSSE(t1reg), xRegisterSSE(t0reg)); //S2, 0, S3, 0, S6, 0, S7, 0
		xPSHUF.D(xRegisterSSE(t0reg), xRegisterSSE(EEREC_T), 0xd8); //T0, T1, T4, T5, T2, T3, T6, T7
		xPUNPCK.HWD(xRegisterSSE(t0reg), xRegisterSSE(t0reg)); //T2, T2, T3, T3, T6, T6, T7, T7
		xPMADD.WD(xRegisterSSE(t0reg), xRegisterSSE(t1reg)); //S2*T2+0*T2, S3*T3+0*T3, S6*T6+0*T6, S7*T7+0*T7

		xPSUB.D(xRegisterSSE(EEREC_HI), xRegisterSSE(t0reg));
	}
	else
	{
		xPMUL.LW(xRegisterSSE(t0reg), xRegisterSSE(EEREC_S), xRegisterSSE(EEREC_T));
		xPMUL.HW(xRegisterSSE(t1reg), xRegisterSSE(EEREC_S), xRegisterSSE(EEREC_T));

		// 4-7
		xPUNPCK.HWD(xRegisterSSE(EEREC_D), xRegisterSSE(t0reg), xRegisterSSE(t1reg));
		// 0-3
		xPUNPCK.LWD(xRegisterSSE(t0reg),   xRegisterSSE(t0reg), xRegisterSSE(t1reg));

		// 2,3,6,7, L->H
		xPUNPCK.HQDQ(xRegisterSSE(t1reg), xRegisterSSE(t0reg), xRegisterSSE(EEREC_D));
		// 0,1,4,5, L->H
		xPUNPCK.LQDQ(xRegisterSSE(t0reg), xRegisterSSE(t0reg), xRegisterSSE(EEREC_D));

		xPSUB.D(xRegisterSSE(EEREC_LO), xRegisterSSE(t0reg));
		xPSUB.D(xRegisterSSE(EEREC_HI), xRegisterSSE(t1reg));

		// 0,2,4,6, L->H
		xPSHUF.D(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_LO), 0x88);
		xPSHUF.D(xRegisterSSE(t0reg), xRegisterSSE(EEREC_HI), 0x88);
		xPUNPCK.LDQ(xRegisterSSE(EEREC_D), xRegisterSSE(t0reg));
	}

	_freeXMMreg(t0reg);
	_freeXMMreg(t1reg);

	_clearNeededXMMregs();
}

////////////////////////////////////////////////////
//upper word of each doubleword in LO and HI is undocumented/undefined
//it contains the NOT of the upper multiplication result (before the substraction of the lower multiplication result)
void recPHMSBH()
{
	EE::Profiler.EmitOp(eeOpcode::PHMSBH);

	EERecompileInfo info = eeRecompileCodeXMM((_Rd_ ? XMMINFO_WRITED : 0) | XMMINFO_READS | XMMINFO_READT | XMMINFO_WRITELO | XMMINFO_WRITEHI);

	if (avx512.HasCore())
	{
		// P = {p0..p7} as dwords in LO's YMM. With x = P: dwords {x1-x0, ~x1, x3-x2, ~x3, ...} are the
		// interleaved (diff, NOT odd) pairs; VPERMQ 0xd8 then splits them into LO={d0,n0,d2,n2} and
		// HI={d1,n1,d3,n3}. VPTERNLOGD 0x72 = C ? ~B : A with C = odd-lane mask. Needs proper testing.
		alignas(32) static const u32 s_oddMask[8] = {0, ~0u, 0, ~0u, 0, ~0u, 0, ~0u};
		xVPMOVSXWDY(xRegisterSSE(EEREC_LO), xRegisterSSE(EEREC_S));
		xVPMOVSXWDY(xRegisterSSE(EEREC_HI), xRegisterSSE(EEREC_T));
		xVPMULLDY(xRegisterSSE(EEREC_LO), xRegisterSSE(EEREC_LO), xRegisterSSE(EEREC_HI));
		xVPSHUFDY(xRegisterSSE(EEREC_HI), xRegisterSSE(EEREC_LO), 0xb1);
		xVPSUBDY(xRegisterSSE(EEREC_HI), xRegisterSSE(EEREC_HI), xRegisterSSE(EEREC_LO));
		xVPTERNLOGDY(xRegisterSSE(EEREC_HI), xRegisterSSE(EEREC_LO), ptr128[s_oddMask], 0x72);
		xVPERMQY(xRegisterSSE(EEREC_LO), xRegisterSSE(EEREC_HI), 0xd8);
		xVEXTRACTI128Y(xRegisterSSE(EEREC_HI), xRegisterSSE(EEREC_LO), 1);
		if (_Rd_)
			emitHalfwordRd(EEREC_D, EEREC_LO, EEREC_HI);
		_clearNeededXMMregs();
		return;
	}

	if (x86Emitter::use_avx)
	{
		// Same split as the AVX-512 path, with an explicit ones register instead of VPTERNLOGD:
		// HI = {x1-x0, x0-x1, ...} then odd lanes replaced by ~x (LO ^ ones), VPERMQ + VEXTRACTI128 split.
		// Leaves the upper YMM halves of LO dirty. Needs proper testing; ~10 instructions vs ~16.
		const int ones = _allocTempXMMreg(XMMT_INT);
		xVexPMOVSXWDY(xRegisterSSE(EEREC_LO), xRegisterSSE(EEREC_S));
		xVexPMOVSXWDY(xRegisterSSE(EEREC_HI), xRegisterSSE(EEREC_T));
		xVexPMULLDY(xRegisterSSE(EEREC_LO), xRegisterSSE(EEREC_LO), xRegisterSSE(EEREC_HI));
		xVexPSHUFDY(xRegisterSSE(EEREC_HI), xRegisterSSE(EEREC_LO), 0xb1);
		xVexPSUBDY(xRegisterSSE(EEREC_HI), xRegisterSSE(EEREC_HI), xRegisterSSE(EEREC_LO));
		xVexPCMPEQDY(xRegisterSSE(ones), xRegisterSSE(ones), xRegisterSSE(ones));
		xVexPXORY(xRegisterSSE(EEREC_LO), xRegisterSSE(EEREC_LO), xRegisterSSE(ones));
		xVexPBLENDDY(xRegisterSSE(EEREC_HI), xRegisterSSE(EEREC_HI), xRegisterSSE(EEREC_LO), 0xaa);
		xVexPERMQY(xRegisterSSE(EEREC_LO), xRegisterSSE(EEREC_HI), 0xd8);
		xVexEXTRACTI128Y(xRegisterSSE(EEREC_HI), xRegisterSSE(EEREC_LO), 1);
		if (_Rd_)
			emitHalfwordRd(EEREC_D, EEREC_LO, EEREC_HI);
		_freeXMMreg(ones);
		_clearNeededXMMregs();
		return;
	}

	int t0reg = _allocTempXMMreg(XMMT_INT);

	xPCMP.EQD(xRegisterSSE(EEREC_LO), xRegisterSSE(EEREC_LO));
	xPSRL.D(xRegisterSSE(EEREC_LO), 16);
	xPAND(xRegisterSSE(EEREC_HI), xRegisterSSE(EEREC_S), xRegisterSSE(EEREC_LO));
	xPMADD.WD(xRegisterSSE(EEREC_HI), xRegisterSSE(EEREC_T));
	xPSLL.D(xRegisterSSE(EEREC_LO), 16);
	xPAND(xRegisterSSE(EEREC_LO), xRegisterSSE(EEREC_S));
	xPMADD.WD(xRegisterSSE(EEREC_LO), xRegisterSSE(EEREC_T));
	xMOVDQA(xRegisterSSE(t0reg), xRegisterSSE(EEREC_LO));
	xPSUB.D(xRegisterSSE(EEREC_LO), xRegisterSSE(EEREC_HI));
	if (_Rd_)
		xMOVDQA(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_LO));

	xPCMP.EQD(xRegisterSSE(EEREC_HI), xRegisterSSE(EEREC_HI));
	xPXOR(xRegisterSSE(t0reg), xRegisterSSE(EEREC_HI));

	xMOVDQA(xRegisterSSE(EEREC_HI), xRegisterSSE(EEREC_LO));

	xSHUF.PS(xRegisterSSE(EEREC_LO), xRegisterSSE(t0reg), 0x88);
	xSHUF.PS(xRegisterSSE(EEREC_LO), xRegisterSSE(EEREC_LO), 0xd8);

	xSHUF.PS(xRegisterSSE(EEREC_HI), xRegisterSSE(t0reg), 0xdd);
	xSHUF.PS(xRegisterSSE(EEREC_HI), xRegisterSSE(EEREC_HI), 0xd8);

	_freeXMMreg(t0reg);
	_clearNeededXMMregs();
}

////////////////////////////////////////////////////
void recPEXEH()
{
	if (!_Rd_)
		return;

	EE::Profiler.EmitOp(eeOpcode::PEXEH);

	EERecompileInfo info = eeRecompileCodeXMM(XMMINFO_READT | XMMINFO_WRITED);
	emitHalfwordShuffle<0xc6>(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_T));
	_clearNeededXMMregs();
}

////////////////////////////////////////////////////
void recPREVH()
{
	if (!_Rd_)
		return;

	EE::Profiler.EmitOp(eeOpcode::PREVH);

	EERecompileInfo info = eeRecompileCodeXMM(XMMINFO_READT | XMMINFO_WRITED);
	emitHalfwordShuffle<0x1b>(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_T));
	_clearNeededXMMregs();
}

////////////////////////////////////////////////////
void recPINTH()
{
	if (!_Rd_)
		return;

	EE::Profiler.EmitOp(eeOpcode::PINTH);

	EERecompileInfo info = eeRecompileCodeXMM(XMMINFO_READS | XMMINFO_READT | XMMINFO_WRITED);
	if (EEREC_D == EEREC_S)
	{
		int t0reg = _allocTempXMMreg(XMMT_INT);
		xMOVHL.PS(xRegisterSSE(t0reg), xRegisterSSE(EEREC_S));
		if (EEREC_D != EEREC_T)
			xMOVQZX(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_T));
		xPUNPCK.LWD(xRegisterSSE(EEREC_D), xRegisterSSE(t0reg));
		_freeXMMreg(t0reg);
	}
	else
	{
		xMOVLH.PS(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_T));
		xPUNPCK.HWD(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_S));
	}
	_clearNeededXMMregs();
}

void recPEXEW()
{
	if (!_Rd_)
		return;

	EE::Profiler.EmitOp(eeOpcode::PEXEW);

	EERecompileInfo info = eeRecompileCodeXMM(XMMINFO_READT | XMMINFO_WRITED);
	xPSHUF.D(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_T), 0xc6);
	_clearNeededXMMregs();
}

void recPROT3W()
{
	if (!_Rd_)
		return;

	EE::Profiler.EmitOp(eeOpcode::PROT3W);

	EERecompileInfo info = eeRecompileCodeXMM(XMMINFO_READT | XMMINFO_WRITED);
	xPSHUF.D(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_T), 0xc9);
	_clearNeededXMMregs();
}

void recPMULTH()
{
	EE::Profiler.EmitOp(eeOpcode::PMULTH);

	EERecompileInfo info = eeRecompileCodeXMM(XMMINFO_READS | XMMINFO_READT | (_Rd_ ? XMMINFO_WRITED : 0) | XMMINFO_WRITELO | XMMINFO_WRITEHI);
	if (useWideHalfwordMultiply())
	{
		emitHalfwordProducts(EEREC_LO, EEREC_HI, EEREC_S, EEREC_T);
		if (_Rd_)
			emitHalfwordRd(EEREC_D, EEREC_LO, EEREC_HI);
		_clearNeededXMMregs();
		return;
	}
	int t0reg = _allocTempXMMreg(XMMT_INT);

	xPMUL.LW(xRegisterSSE(EEREC_LO), xRegisterSSE(EEREC_S), xRegisterSSE(EEREC_T));
	xPMUL.HW(xRegisterSSE(EEREC_HI), xRegisterSSE(EEREC_S), xRegisterSSE(EEREC_T));

	// 4-7
	xPUNPCK.HWD(xRegisterSSE(t0reg),    xRegisterSSE(EEREC_LO), xRegisterSSE(EEREC_HI));
	// 0-3
	xPUNPCK.LWD(xRegisterSSE(EEREC_LO), xRegisterSSE(EEREC_LO), xRegisterSSE(EEREC_HI));

	if (_Rd_)
	{
		// 0,2,4,6, L->H
		xPSHUF.D(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_LO), 0x88);
		xPSHUF.D(xRegisterSSE(EEREC_HI), xRegisterSSE(t0reg), 0x88);
		xPUNPCK.LQDQ(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_HI));
	}

	// 2,3,6,7, L->H
	xPUNPCK.HQDQ(xRegisterSSE(EEREC_HI), xRegisterSSE(EEREC_LO), xRegisterSSE(t0reg));
	// 0,1,4,5, L->H
	xPUNPCK.LQDQ(xRegisterSSE(EEREC_LO), xRegisterSSE(EEREC_LO), xRegisterSSE(t0reg));

	_freeXMMreg(t0reg);
	_clearNeededXMMregs();
}

void recPMFHI()
{
	if (!_Rd_)
		return;

	EE::Profiler.EmitOp(eeOpcode::PMFHI);

	EERecompileInfo info = eeRecompileCodeXMM(XMMINFO_WRITED | XMMINFO_READHI);
	xMOVDQA(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_HI));
	_clearNeededXMMregs();
}

////////////////////////////////////////////////////
void recPMFLO()
{
	if (!_Rd_)
		return;

	EE::Profiler.EmitOp(eeOpcode::PMFLO);

	EERecompileInfo info = eeRecompileCodeXMM(XMMINFO_WRITED | XMMINFO_READLO);
	xMOVDQA(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_LO));
	_clearNeededXMMregs();
}

////////////////////////////////////////////////////
void recPAND()
{
	if (!_Rd_)
		return;

	EE::Profiler.EmitOp(eeOpcode::PAND);

	EERecompileInfo info = eeRecompileCodeXMM(XMMINFO_WRITED | XMMINFO_READS | XMMINFO_READT);
	xPAND(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_S), xRegisterSSE(EEREC_T));
	_clearNeededXMMregs();
}

////////////////////////////////////////////////////
void recPXOR()
{
	if (!_Rd_)
		return;

	EE::Profiler.EmitOp(eeOpcode::PXOR);

	EERecompileInfo info = eeRecompileCodeXMM(XMMINFO_WRITED | XMMINFO_READS | XMMINFO_READT);
	xPXOR(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_S), xRegisterSSE(EEREC_T));
	_clearNeededXMMregs();
}

////////////////////////////////////////////////////
void recPCPYLD()
{
	if (!_Rd_)
		return;

	EE::Profiler.EmitOp(eeOpcode::PCPYLD);

	EERecompileInfo info = eeRecompileCodeXMM(XMMINFO_WRITED | ((_Rs_ == 0) ? 0 : XMMINFO_READS) | XMMINFO_READT);
	if (_Rs_ == 0)
	{
		xMOVQZX(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_T));
	}
	else
	{
		if (EEREC_D == EEREC_T || x86Emitter::use_avx)
			xPUNPCK.LQDQ(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_T), xRegisterSSE(EEREC_S));
		else if (EEREC_S == EEREC_T)
			xPSHUF.D(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_S), 0x44);
		else if (EEREC_D == EEREC_S)
		{
			xPUNPCK.LQDQ(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_T));
			xPSHUF.D(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_D), 0x4e);
		}
		else
		{
			xMOVQZX(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_T));
			xPUNPCK.LQDQ(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_S));
		}
	}
	_clearNeededXMMregs();
}

void recPMADDH()
{
	EE::Profiler.EmitOp(eeOpcode::PMADDH);

	EERecompileInfo info = eeRecompileCodeXMM((_Rd_ ? XMMINFO_WRITED : 0) | XMMINFO_READS | XMMINFO_READT | XMMINFO_READLO | XMMINFO_READHI | XMMINFO_WRITELO | XMMINFO_WRITEHI);
	int t0reg = _allocTempXMMreg(XMMT_INT);
	int t1reg = _allocTempXMMreg(XMMT_INT);

	if (useWideHalfwordMultiply())
	{
		emitHalfwordProducts(t0reg, t1reg, EEREC_S, EEREC_T);
		xPADD.D(xRegisterSSE(EEREC_LO), xRegisterSSE(t0reg));
		xPADD.D(xRegisterSSE(EEREC_HI), xRegisterSSE(t1reg));
		if (_Rd_)
			emitHalfwordRd(EEREC_D, EEREC_LO, EEREC_HI);
		_freeXMMreg(t0reg);
		_freeXMMreg(t1reg);
		_clearNeededXMMregs();
		return;
	}

	if (!_Rd_)
	{
		xPXOR(xRegisterSSE(t0reg), xRegisterSSE(t0reg));
		xPSHUF.D(xRegisterSSE(t1reg), xRegisterSSE(EEREC_S), 0xd8); //S0, S1, S4, S5, S2, S3, S6, S7
		xPUNPCK.LWD(xRegisterSSE(t1reg), xRegisterSSE(t0reg)); //S0, 0, S1, 0, S4, 0, S5, 0
		xPSHUF.D(xRegisterSSE(t0reg), xRegisterSSE(EEREC_T), 0xd8); //T0, T1, T4, T5, T2, T3, T6, T7
		xPUNPCK.LWD(xRegisterSSE(t0reg), xRegisterSSE(t0reg)); //T0, T0, T1, T1, T4, T4, T5, T5
		xPMADD.WD(xRegisterSSE(t0reg), xRegisterSSE(t1reg)); //S0*T0+0*T0, S1*T1+0*T1, S4*T4+0*T4, S5*T5+0*T5

		xPADD.D(xRegisterSSE(EEREC_LO), xRegisterSSE(t0reg));

		xPXOR(xRegisterSSE(t0reg), xRegisterSSE(t0reg));
		xPSHUF.D(xRegisterSSE(t1reg), xRegisterSSE(EEREC_S), 0xd8); //S0, S1, S4, S5, S2, S3, S6, S7
		xPUNPCK.HWD(xRegisterSSE(t1reg), xRegisterSSE(t0reg)); //S2, 0, S3, 0, S6, 0, S7, 0
		xPSHUF.D(xRegisterSSE(t0reg), xRegisterSSE(EEREC_T), 0xd8); //T0, T1, T4, T5, T2, T3, T6, T7
		xPUNPCK.HWD(xRegisterSSE(t0reg), xRegisterSSE(t0reg)); //T2, T2, T3, T3, T6, T6, T7, T7
		xPMADD.WD(xRegisterSSE(t0reg), xRegisterSSE(t1reg)); //S2*T2+0*T2, S3*T3+0*T3, S6*T6+0*T6, S7*T7+0*T7

		xPADD.D(xRegisterSSE(EEREC_HI), xRegisterSSE(t0reg));
	}
	else
	{
		xPMUL.LW(xRegisterSSE(t0reg), xRegisterSSE(EEREC_S), xRegisterSSE(EEREC_T));
		xPMUL.HW(xRegisterSSE(t1reg), xRegisterSSE(EEREC_S), xRegisterSSE(EEREC_T));

		// 4-7
		xPUNPCK.HWD(xRegisterSSE(EEREC_D), xRegisterSSE(t0reg), xRegisterSSE(t1reg));
		// 0-3
		xPUNPCK.LWD(xRegisterSSE(t0reg),   xRegisterSSE(t0reg), xRegisterSSE(t1reg));

		// 2,3,6,7, L->H
		xPUNPCK.HQDQ(xRegisterSSE(t1reg), xRegisterSSE(t0reg), xRegisterSSE(EEREC_D));
		// 0,1,4,5, L->H
		xPUNPCK.LQDQ(xRegisterSSE(t0reg), xRegisterSSE(t0reg), xRegisterSSE(EEREC_D));

		xPADD.D(xRegisterSSE(EEREC_LO), xRegisterSSE(t0reg));
		xPADD.D(xRegisterSSE(EEREC_HI), xRegisterSSE(t1reg));

		// 0,2,4,6, L->H
		xPSHUF.D(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_LO), 0x88);
		xPSHUF.D(xRegisterSSE(t0reg), xRegisterSSE(EEREC_HI), 0x88);
		xPUNPCK.LDQ(xRegisterSSE(EEREC_D), xRegisterSSE(t0reg));
	}

	_freeXMMreg(t0reg);
	_freeXMMreg(t1reg);

	_clearNeededXMMregs();
}

#endif
/*********************************************************
*   MMI3 opcodes                                         *
*                                                        *
*********************************************************/
#ifndef MMI3_RECOMPILE

REC_FUNC_DEL(PMADDUW, _Rd_);
REC_FUNC_DEL(PSRAVW,  _Rd_);
REC_FUNC_DEL(PMTHI,   _Rd_);
REC_FUNC_DEL(PMTLO,   _Rd_);
REC_FUNC_DEL(PINTEH,  _Rd_);
REC_FUNC_DEL(PMULTUW, _Rd_);
REC_FUNC_DEL(PDIVUW,  _Rd_);
REC_FUNC_DEL(PCPYUD,  _Rd_);
REC_FUNC_DEL(POR,     _Rd_);
REC_FUNC_DEL(PNOR,    _Rd_);
REC_FUNC_DEL(PCPYH,   _Rd_);
REC_FUNC_DEL(PEXCW,   _Rd_);
REC_FUNC_DEL(PEXCH,   _Rd_);

#else

////////////////////////////////////////////////////
//REC_FUNC( PSRAVW, _Rd_ );

void recPSRAVW()
{
	if (!_Rd_)
		return;

	EE::Profiler.EmitOp(eeOpcode::PSRAVW);

	EERecompileInfo info = eeRecompileCodeXMM((_Rs_ ? XMMINFO_READS : 0) | (_Rt_ ? XMMINFO_READT : 0) | XMMINFO_WRITED);
	if (_Rs_ == 0)
	{
		if (_Rt_ == 0)
		{
			xPXOR(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_D));
		}
		else
		{
			xPSHUF.D(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_T), 0x88);
			xPMOVSX.DQ(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_D));
		}
	}
	else if (_Rt_ == 0)
	{
		xPXOR(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_D));
	}
	else
	{
		if (avx512.HasCore() || g_cpu.vectorISA >= ProcessorFeatures::VectorISA::AVX2)
		{
			const int a0 = _allocTempXMMreg(XMMT_INT);
			(avx512.HasCore() ? emitPSxxVWAVX512 : emitPSxxVWAVX2)(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_S), xRegisterSSE(EEREC_T),
				xRegisterSSE(a0), 2);
			_freeXMMreg(a0);
			_clearNeededXMMregs();
			return;
		}
		int t0reg = _allocTempXMMreg(XMMT_INT);
		int t1reg = _allocTempXMMreg(XMMT_INT);

		// shamt is 5-bit
		xPSLL.Q(xRegisterSSE(t0reg), xRegisterSSE(EEREC_S), 27 + 32);
		xPSRL.Q(xRegisterSSE(t0reg),                        27 + 32);

		// EEREC_D[0] <- Rt[0], t1reg[0] <- Rt[2]
		xMOVHL.PS(xRegisterSSE(t1reg), xRegisterSSE(EEREC_T));

		// shift (right arithmetic) Rt[0]
		xPSRA.D(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_T), xRegisterSSE(t0reg));

		// shift (right arithmetic) Rt[2]
		xMOVHL.PS(xRegisterSSE(t0reg), xRegisterSSE(t0reg));
		xPSRA.D(xRegisterSSE(t1reg), xRegisterSSE(t0reg));

		// merge & sign extend
		xPUNPCK.LDQ(xRegisterSSE(EEREC_D), xRegisterSSE(t1reg));
		xPMOVSX.DQ(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_D));

		_freeXMMreg(t0reg);
		_freeXMMreg(t1reg);
	}

	_clearNeededXMMregs();
}


////////////////////////////////////////////////////
alignas(16) static const u32 s_tempPINTEH[4] = {0x0000ffff, 0x0000ffff, 0x0000ffff, 0x0000ffff};

void recPINTEH()
{
	if (!_Rd_)
		return;

	EE::Profiler.EmitOp(eeOpcode::PINTEH);

	EERecompileInfo info = eeRecompileCodeXMM((_Rs_ ? XMMINFO_READS : 0) | (_Rt_ ? XMMINFO_READT : 0) | XMMINFO_WRITED);

	int t0reg = -1;

	if (_Rs_ == 0)
	{
		if (_Rt_ == 0)
		{
			xPXOR(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_D));
		}
		else
		{
			xPAND(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_T), ptr[s_tempPINTEH]);
		}
	}
	else if (_Rt_ == 0)
	{
		xPSLL.D(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_S), 16);
	}
	else
	{
		if (EEREC_S == EEREC_T)
		{
			emitHalfwordShuffle<0xa0>(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_S));
		}
		else if (EEREC_D == EEREC_T)
		{
			pxAssert(EEREC_D != EEREC_S);
			t0reg = _allocTempXMMreg(XMMT_INT);
			xPSLL.D(xRegisterSSE(t0reg), xRegisterSSE(EEREC_S), 16);
			xPBLEND.W(xRegisterSSE(EEREC_D), xRegisterSSE(t0reg), 0xaa);
		}
		else
		{
			xPSLL.D(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_S), 16);
			xPBLEND.W(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_T), 0x55);
		}
	}

	if (t0reg >= 0)
		_freeXMMreg(t0reg);
	_clearNeededXMMregs();
}

////////////////////////////////////////////////////
void recPMULTUW()
{
	EE::Profiler.EmitOp(eeOpcode::PMULTUW);

	EERecompileInfo info = eeRecompileCodeXMM((((_Rs_) && (_Rt_)) ? XMMINFO_READS : 0) | (((_Rs_) && (_Rt_)) ? XMMINFO_READT : 0) | (_Rd_ ? XMMINFO_WRITED : 0) | XMMINFO_WRITELO | XMMINFO_WRITEHI);
	if (!_Rs_ || !_Rt_)
	{
		if (_Rd_)
			xPXOR(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_D));
		xPXOR(xRegisterSSE(EEREC_LO), xRegisterSSE(EEREC_LO));
		xPXOR(xRegisterSSE(EEREC_HI), xRegisterSSE(EEREC_HI));
	}
	else
	{
		int dst = _Rd_ ? EEREC_D : EEREC_HI;
		xPMUL.UDQ(xRegisterSSE(dst), xRegisterSSE(EEREC_S), xRegisterSSE(EEREC_T));

		// interleave & sign extend
		emitSplitLoHi(EEREC_LO, EEREC_HI, dst);
	}
	_clearNeededXMMregs();
}

////////////////////////////////////////////////////
void recPMADDUW()
{
	EE::Profiler.EmitOp(eeOpcode::PMADDUW);

	EERecompileInfo info = eeRecompileCodeXMM((((_Rs_) && (_Rt_)) ? XMMINFO_READS : 0) | (((_Rs_) && (_Rt_)) ? XMMINFO_READT : 0) | (_Rd_ ? XMMINFO_WRITED : 0) | XMMINFO_WRITELO | XMMINFO_WRITEHI | XMMINFO_READLO | XMMINFO_READHI);
	xSHUF.PS(xRegisterSSE(EEREC_LO), xRegisterSSE(EEREC_HI), 0x88);
	xPSHUF.D(xRegisterSSE(EEREC_LO), xRegisterSSE(EEREC_LO), 0xd8); // LO = {LO[0], HI[0], LO[2], HI[2]}
	int dst = _Rd_ ? EEREC_D : EEREC_HI;
	xPMUL.UDQ(xRegisterSSE(dst), xRegisterSSE(EEREC_S), xRegisterSSE(EEREC_T));
	xPADD.Q(xRegisterSSE(dst), xRegisterSSE(EEREC_LO));
	// interleave & sign extend
	emitSplitLoHi(EEREC_LO, EEREC_HI, dst);

	_clearNeededXMMregs();
}

////////////////////////////////////////////////////
void recPDIVUW()
{
	EE::Profiler.EmitOp(eeOpcode::PDIVUW);

	_deleteEEreg(_Rd_, 0);
	recCall(Interp::PDIVUW);
}

////////////////////////////////////////////////////
void recPEXCW()
{
	EE::Profiler.EmitOp(eeOpcode::PEXCW);

	if (!_Rd_)
		return;

	EERecompileInfo info = eeRecompileCodeXMM(XMMINFO_READT | XMMINFO_WRITED);
	xPSHUF.D(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_T), 0xd8);
	_clearNeededXMMregs();
}

////////////////////////////////////////////////////
void recPEXCH()
{
	EE::Profiler.EmitOp(eeOpcode::PEXCH);

	if (!_Rd_)
		return;

	EERecompileInfo info = eeRecompileCodeXMM(XMMINFO_READT | XMMINFO_WRITED);
	emitHalfwordShuffle<0xd8>(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_T));
	_clearNeededXMMregs();
}

////////////////////////////////////////////////////
void recPNOR()
{
	if (!_Rd_)
		return;

	EE::Profiler.EmitOp(eeOpcode::PNOR);

	EERecompileInfo info = eeRecompileCodeXMM((_Rs_ != 0 ? XMMINFO_READS : 0) | (_Rt_ != 0 ? XMMINFO_READT : 0) | XMMINFO_WRITED);
	if (avx512.HasCore())
	{
		if (_Rs_ && _Rt_)
			emitPNORAVX512(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_S), xRegisterSSE(EEREC_T));
		else if (_Rs_ || _Rt_)
		{
			const xRegisterSSE src(_Rs_ ? EEREC_S : EEREC_T);
			emitPNORAVX512(xRegisterSSE(EEREC_D), src, src);
		}
		else
			xPCMP.EQD(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_D));
		_clearNeededXMMregs();
		return;
	}

	if (_Rs_ == 0)
	{
		if (_Rt_ == 0)
		{
			xPCMP.EQD(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_D));
		}
		else
		{
			if (EEREC_D == EEREC_T)
			{
				int t0reg = _allocTempXMMreg(XMMT_INT);
				xPCMP.EQD(xRegisterSSE(t0reg), xRegisterSSE(t0reg));
				xPXOR(xRegisterSSE(EEREC_D), xRegisterSSE(t0reg));
				_freeXMMreg(t0reg);
			}
			else
			{
				xPCMP.EQD(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_D));
				if (_Rt_ != 0)
					xPXOR(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_T));
			}
		}
	}
	else if (_Rt_ == 0)
	{
		if (EEREC_D == EEREC_S)
		{
			int t0reg = _allocTempXMMreg(XMMT_INT);
			xPCMP.EQD(xRegisterSSE(t0reg), xRegisterSSE(t0reg));
			xPXOR(xRegisterSSE(EEREC_D), xRegisterSSE(t0reg));
			_freeXMMreg(t0reg);
		}
		else
		{
			xPCMP.EQD(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_D));
			xPXOR(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_S));
		}
	}
	else
	{
		int t0reg = _allocTempXMMreg(XMMT_INT);
		xPOR(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_S), xRegisterSSE(EEREC_T));
		xPCMP.EQD(xRegisterSSE(t0reg), xRegisterSSE(t0reg));
		xPXOR(xRegisterSSE(EEREC_D), xRegisterSSE(t0reg));
		_freeXMMreg(t0reg);
	}
	_clearNeededXMMregs();
}

////////////////////////////////////////////////////
void recPMTHI()
{
	EE::Profiler.EmitOp(eeOpcode::PMTHI);

	EERecompileInfo info = eeRecompileCodeXMM(XMMINFO_READS | XMMINFO_WRITEHI);
	xMOVDQA(xRegisterSSE(EEREC_HI), xRegisterSSE(EEREC_S));
	_clearNeededXMMregs();
}

////////////////////////////////////////////////////
void recPMTLO()
{
	EE::Profiler.EmitOp(eeOpcode::PMTLO);

	EERecompileInfo info = eeRecompileCodeXMM(XMMINFO_READS | XMMINFO_WRITELO);
	xMOVDQA(xRegisterSSE(EEREC_LO), xRegisterSSE(EEREC_S));
	_clearNeededXMMregs();
}

////////////////////////////////////////////////////
void recPCPYUD()
{
	if (!_Rd_)
		return;

	EE::Profiler.EmitOp(eeOpcode::PCPYUD);

	EERecompileInfo info = eeRecompileCodeXMM(XMMINFO_READS | ((_Rt_ == 0) ? 0 : XMMINFO_READT) | XMMINFO_WRITED);

	if (_Rt_ == 0)
	{
		if (x86Emitter::use_avx || EEREC_D == EEREC_S)
		{
			// [S.hi64, 0] is a byte shift right by 8 (needs proper testing)
			xPSRL.DQ(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_S), 8);
		}
		else
		{
			xPXOR(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_D));
			xMOVHL.PS(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_S));
		}
	}
	else
	{
		if (EEREC_D == EEREC_S || x86Emitter::use_avx)
			xPUNPCK.HQDQ(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_S), xRegisterSSE(EEREC_T));
		else if (EEREC_D == EEREC_T)
		{
			//TODO
			xPUNPCK.HQDQ(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_S));
			xPSHUF.D(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_D), 0x4e);
		}
		else
		{
			if (EEREC_S == EEREC_T)
			{
				xPSHUF.D(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_S), 0xee);
			}
			else
			{
				xMOVDQA(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_S));
				xPUNPCK.HQDQ(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_T));
			}
		}
	}
	_clearNeededXMMregs();
}

////////////////////////////////////////////////////
void recPOR()
{
	if (!_Rd_)
		return;

	EE::Profiler.EmitOp(eeOpcode::POR);

	EERecompileInfo info = eeRecompileCodeXMM((_Rs_ != 0 ? XMMINFO_READS : 0) | (_Rt_ != 0 ? XMMINFO_READT : 0) | XMMINFO_WRITED);

	if (_Rs_ == 0)
	{
		if (_Rt_ == 0)
		{
			xPXOR(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_D));
		}
		else
			xMOVDQA(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_T));
	}
	else if (_Rt_ == 0)
	{
		xMOVDQA(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_S));
	}
	else
	{
		xPOR(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_S), xRegisterSSE(EEREC_T));
	}
	_clearNeededXMMregs();
}

////////////////////////////////////////////////////
void recPCPYH()
{
	if (!_Rd_)
		return;

	EE::Profiler.EmitOp(eeOpcode::PCPYH);

	EERecompileInfo info = eeRecompileCodeXMM(XMMINFO_READT | XMMINFO_WRITED);
	emitHalfwordShuffle<0x00>(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_T));
	_clearNeededXMMregs();
}

#endif // else MMI3_RECOMPILE

} // namespace MMI
} // namespace OpcodeImpl
} // namespace Dynarec
} // namespace R5900
