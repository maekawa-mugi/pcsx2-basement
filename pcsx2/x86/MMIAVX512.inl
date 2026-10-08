// SPDX-License-Identifier: GPL-3.0+
// Register-only kernels shared with the standalone executable validation.
// PLZCW scratch must differ from both source and destination.
alignas(16) static const u32 s_plzcwOne[4] = {1, 1, 1, 1};
static void emitPLZCWAVX512(const xRegisterSSE& dst, const xRegisterSSE& src,
	const xRegisterSSE& scratch, const xRegisterSSE& ones)
{
	// clz((x ^ (x << 1)) | 1): the first sign change is the first set bit, and `| 1` maps 0 and -1 to 31.
	xVPSLLDImm(scratch, src, 1);
	xVPTERNLOGD(scratch, src, ptr128[s_plzcwOne], 0xbe); // (scratch ^ src) | mem; needs proper testing
	xVPLZCNTD(scratch, scratch);
	if (scratch.IsEVEXHigh())
	{
		// Only this temporary opts into the high bank. Guest operands and the
		// lane-preserving blend stay low until guest residency is enabled.
		xVMOVDQA32(ones, scratch);
		xPBLEND.W(dst, ones, 0x0f);
		return;
	}
	// The upper 64 bits of Rd survive, including when Rd aliases Rs.
	xPBLEND.W(dst, scratch, 0x0f);
}

// PPACW/PPACH/PPACB with Rs != 0: concatenate Rs above Rt, then narrow once. dst may alias s or t; no TEMP.
// width: 0 = word (VPMOVQD), 1 = halfword (VPMOVDW), 2 = byte (VPMOVWB). Needs proper testing on hardware.
static void emitPPACGeneralAVX512(const xRegisterSSE& dst, const xRegisterSSE& s, const xRegisterSSE& t, int width)
{
	xVexINSERTI128Y(dst, t, s, 1);
	if (width == 0)
		xVPMOVQD(dst, dst, true);
	else if (width == 1)
		xVPMOVDW(dst, dst, true);
	else
		xVPMOVWB(dst, dst, true);
}

static void emitPNORAVX512(const xRegisterSSE& dst, const xRegisterSSE& src,
	const xRegisterSSE& operand)
{
	// Ignore old dst: truth table is ~(src | operand), for every alias.
	xVPTERNLOGD(dst, src, operand, 0x11);
}

// PADDSW / PSUBSW: exact 64-bit sum, then VPMOVSQD's signed saturation to 32 bits.
// tmp0/tmp1 are used as YMM; dst may alias either source. Needs testing on hardware.
static void emitPADDSWSubSWAVX512(const xRegisterSSE& dst, const xRegisterSSE& s,
	const xRegisterSSE& t, const xRegisterSSE& tmp0, const xRegisterSSE& tmp1, bool sub)
{
	xVPMOVSXDQY(tmp0, s);
	if (s.GetId() == t.GetId())
	{
		if (sub)
			xVPSUBQY(tmp0, tmp0, tmp0);
		else
			xVPADDQY(tmp0, tmp0, tmp0);
	}
	else
	{
		xVPMOVSXDQY(tmp1, t);
		if (sub)
			xVPSUBQY(tmp0, tmp0, tmp1);
		else
			xVPADDQY(tmp0, tmp0, tmp1);
	}
	xVPMOVSQD(dst, tmp0, true);
}

// PMFHL.SLW: Rd = { sat32(HI0:LO0), sat32(HI2:LO2) } sign-extended to 64 bits each.
// tmp must differ from lo/hi; dst may alias neither lo nor hi in practice, but only tmp is written first.
static void emitPMFHLSLWAVX512(const xRegisterSSE& dst, const xRegisterSSE& lo,
	const xRegisterSSE& hi, const xRegisterSSE& tmp)
{
	xPSLL.Q(tmp, hi, 32); // {0,H0,0,H2}
	xPBLEND.W(tmp, lo, 0x33); // {L0,H0,L2,H2}: two signed qwords
	xVPMOVSQD(dst, tmp, false);
	xPMOVSX.DQ(dst, dst);
}

// PSLLVW / PSRLVW / PSRAVW (general form): 5-bit count from dword 0 and 2 of Rs, shift
// dwords 0 and 2 of Rt, sign-extend each 32-bit result to 64 bits. kind: 0 = SLL, 1 = SRL, 2 = SRA.
// tmp must differ from s and t; dst may alias s or t. Needs proper testing.
alignas(16) static const u32 s_psxxvwMask[4] = {31, 31, 31, 31};
static void emitPSxxVWAVX512(const xRegisterSSE& dst, const xRegisterSSE& s,
	const xRegisterSSE& t, const xRegisterSSE& tmp, int kind)
{
	xPAND(tmp, s, ptr128[s_psxxvwMask]);
	if (kind == 0)
		xVPSLLVD(dst, t, tmp);
	else if (kind == 1)
		xVPSRLVD(dst, t, tmp);
	else
		xVPSRAVD(dst, t, tmp);
	xPSLL.Q(dst, dst, 32);
	xVPSRAQImm(dst, dst, 32);
}

// AVX2 form of PSLLVW/PSRLVW/PSRAVW: variable dword shift (VPSxxVD), then gather dwords 0 and 2 and
// sign-extend them (4 instructions, 1 TEMP). tmp must differ from s and t. Needs proper testing.
static void emitPSxxVWAVX2(const xRegisterSSE& dst, const xRegisterSSE& s,
	const xRegisterSSE& t, const xRegisterSSE& tmp, int kind)
{
	xPAND(tmp, s, ptr128[s_psxxvwMask]);
	if (kind == 0)
		xVPSLLVD(dst, t, tmp);
	else if (kind == 1)
		xVPSRLVD(dst, t, tmp);
	else
		xVPSRAVD(dst, t, tmp);
	xPSHUF.D(dst, dst, 0x08);
	xPMOVSX.DQ(dst, dst);
}

// PABSW: |x| with 0x80000000 -> 0x7fffffff, i.e. PABS then unsigned min with INT_MAX (only INT_MIN
// exceeds it). Same for halfwords with 16-bit ops. No temp needed. Needs proper testing.
alignas(16) static const u32 s_pabsMaxW[4] = {0x7fffffffu, 0x7fffffffu, 0x7fffffffu, 0x7fffffffu};
alignas(16) static const u16 s_pabsMaxH[8] = {0x7fff, 0x7fff, 0x7fff, 0x7fff, 0x7fff, 0x7fff, 0x7fff, 0x7fff};
static void emitPABSAVX512(const xRegisterSSE& dst, const xRegisterSSE& src, bool halfword)
{
	if (halfword)
	{
		xPABS.W(dst, src);
		xPMIN.UW(dst, ptr128[s_pabsMaxH]);
	}
	else
	{
		xPABS.D(dst, src);
		xPMIN.UD(dst, ptr128[s_pabsMaxW]);
	}
}

// PADDUW: S + min(T, ~S) saturates to 0xffffffff on carry. tmp must differ from s and t (it may be dst
// when dst aliases neither). SSE4.1 only; ~S is one VPTERNLOGD under AVX-512, else PCMPEQD + PXOR.
static void emitPADDUWAVX512(const xRegisterSSE& dst, const xRegisterSSE& s,
	const xRegisterSSE& t, const xRegisterSSE& tmp)
{
	if (avx512.HasCore())
		xVPTERNLOGD(tmp, s, s, 0x33); // ~s
	else
	{
		xPCMP.EQD(tmp, tmp);
		xPXOR(tmp, s);
	}
	xPMIN.UD(tmp, tmp, t);
	xPADD.D(dst, s, tmp);
}

// PSUBUW: unsigned saturating difference. dst != t: max(S,T) - T (no TEMP). dst == t: S - min(S,T)
// through tmp, since the 2-operand SSE form cannot subtract from a clobbered source. SSE4.1 only.
static void emitPSUBUWAVX512(const xRegisterSSE& dst, const xRegisterSSE& s,
	const xRegisterSSE& t, const xRegisterSSE& tmp)
{
	if (dst.GetId() != t.GetId())
	{
		xPMAX.UD(dst, s, t);
		xPSUB.D(dst, t);
	}
	else
	{
		xPMIN.UD(tmp, s, t);
		xPSUB.D(dst, s, tmp);
	}
}

// QFSRV: bytes [sa, sa+16) of {Rt, Rs} (Rt low). Replaces two stores + an unaligned
// reload (store-forwarding stall). idx receives the result; sa is a byte count 0..15 in a GPR.
static void emitQFSRVAVX512(const xRegisterSSE& idx, const xRegisterSSE& s, const xRegisterSSE& t,
	const xRegister32& sa, const void* iota16)
{
	xVPBROADCASTB(idx, sa);
	xPADD.B(idx, ptr128[iota16]);
	xVPERMI2B(idx, t, s);
}

// PMFHL LW/UW/LH. kind: 0 = LW, 1 = UW, 3 = LH (the Sa field). tmp must differ from lo/hi/dst.
static void emitPMFHLAVX512(const xRegisterSSE& dst, const xRegisterSSE& lo,
	const xRegisterSSE& hi, const xRegisterSSE& tmp, int kind)
{
	if (kind == 3)
	{
		// {L0,L1,H0,H1,L2,L3,H2,H3} halfwords: truncate each, then interleave dwords.
		xVPMOVDW(tmp, lo);
		xVPMOVDW(dst, hi);
		xPUNPCK.LDQ(dst, tmp, dst);
	}
	else
	{
		if (dst.GetId() == hi.GetId() && !x86Emitter::use_avx)
		{
			// Non-AVX SHUFPS overwrites its first source, so copy LO to tmp first.
			xMOVAPS(tmp, lo);
			xSHUF.PS(tmp, hi, kind ? 0xdd : 0x88);
			xPSHUF.D(dst, tmp, 0xd8);
			return;
		}
		xSHUF.PS(dst, lo, hi, kind ? 0xdd : 0x88);
		xPSHUF.D(dst, dst, 0xd8);
	}
}

// PEXT5: each dword RGBA5551 -> bytes {R<<3, G<<3, B<<3, A<<7}. One VPMULTISHIFTQB picks
// the 8-bit windows (control = bit offsets per byte, per qword), then a mask drops stray bits.
alignas(16) static const u8 s_pext5Ctrl[16] = {61, 2, 7, 8, 29, 34, 39, 40, 61, 2, 7, 8, 29, 34, 39, 40};
alignas(16) static const u8 s_pext5Mask[16] = {0xf8, 0xf8, 0xf8, 0x80, 0xf8, 0xf8, 0xf8, 0x80, 0xf8, 0xf8, 0xf8, 0x80, 0xf8, 0xf8, 0xf8, 0x80};
static void emitPEXT5AVX512(const xRegisterSSE& dst, const xRegisterSSE& t, const xRegisterSSE& tmp)
{
	xMOVDQA(tmp, ptr128[s_pext5Ctrl]);
	xVPMULTISHIFTQB(dst, tmp, t);
	xPAND(dst, ptr128[s_pext5Mask]);
}

// PPAC5: each dword RGBA8888 -> R5 | G5<<5 | B5<<10 | A1<<15 (upper 16 bits zero).
// VPMULTISHIFTQB picks R>>3, G>>3, B>>3, A>>7 as bytes (windows start at bits 3,11,19,31 per pixel;
// the mask drops spill from the next field), then PMADDUBSW (R+32G, B+32A) and PMADDWD (lo+1024*hi)
// merge them. 5 instructions, 1 TEMP only when Rd aliases Rt.
alignas(16) static const u8 s_ppac5Ctrl[16] = {3, 11, 19, 31, 35, 43, 51, 63, 3, 11, 19, 31, 35, 43, 51, 63};
alignas(16) static const u8 s_ppac5Mask[16] = {0x1f, 0x1f, 0x1f, 0x01, 0x1f, 0x1f, 0x1f, 0x01, 0x1f, 0x1f, 0x1f, 0x01, 0x1f, 0x1f, 0x1f, 0x01};
alignas(16) static const s8 s_ppac5W8[16] = {1, 32, 1, 32, 1, 32, 1, 32, 1, 32, 1, 32, 1, 32, 1, 32};
alignas(16) static const s16 s_ppac5W16[8] = {1, 1024, 1, 1024, 1, 1024, 1, 1024};
static void emitPPAC5AVX512(const xRegisterSSE& dst, const xRegisterSSE& t, const xRegisterSSE& tmp)
{
	xMOVDQA(tmp, ptr128[s_ppac5Ctrl]);
	xVPMULTISHIFTQB(dst, tmp, t);
	xPAND(dst, ptr128[s_ppac5Mask]);
	xPMADD.UBSW(dst, ptr128[s_ppac5W8]); // dst is the unsigned operand; do not use a 3-operand swap
	xPMADD.WD(dst, ptr128[s_ppac5W16]);
}

alignas(16) static const u32 s_ppac5Masks[4][4] = {
	{0x1f, 0x1f, 0x1f, 0x1f}, {0x3e0, 0x3e0, 0x3e0, 0x3e0},
	{0x7c00, 0x7c00, 0x7c00, 0x7c00}, {0x8000, 0x8000, 0x8000, 0x8000}};

// AVX (3-operand) form of PPAC5 without VPTERNLOGD: acc |= shifted & mask as PAND + POR. 11 instructions.
static void emitPPAC5AVX(const xRegisterSSE& dst, const xRegisterSSE& t,
	const xRegisterSSE& acc, const xRegisterSSE& tmp)
{
	xPSRL.D(acc, t, 3);
	xPAND(acc, ptr128[s_ppac5Masks[0]]);
	xPSRL.D(tmp, t, 6);
	xPAND(tmp, ptr128[s_ppac5Masks[1]]);
	xPOR(acc, tmp);
	xPSRL.D(tmp, t, 9);
	xPAND(tmp, ptr128[s_ppac5Masks[2]]);
	xPOR(acc, tmp);
	xPSRL.D(dst, t, 16);
	xPAND(dst, ptr128[s_ppac5Masks[3]]);
	xPOR(dst, acc);
}

static void emitMMIBitSelect(const xRegisterSSE& dst, const xRegisterSSE& mask,
	const xRegisterSSE& replacement)
{
	// (old dst & mask) | (replacement & ~mask). No mask register or temp.
	xVPTERNLOGD(dst, mask, replacement, 0xe2);
}
