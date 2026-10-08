// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/emitter/x86emitter.h"

namespace X86SoftFloatEmitter
{
	using namespace x86Emitter;

	// Fixed-register primitives shared by the EE and microVU soft-float kernel generators.
	// Callers establish their stack layouts and register state before emitting these sequences.
	alignas(16) inline constexpr u32 ScalarBoothDecode[8] = {0, 1, 1, 2, 0x102, 0x101, 0x101, 0};
	alignas(16) inline constexpr u32 ScalarBoothBit11[4] = {0x800, 0x800, 0x800, 0x800};
	alignas(16) inline constexpr u32 ScalarBoothBit10[4] = {0x400, 0x400, 0x400, 0x400};

	inline void EmitCarrySaveAdd(int a_offset, int b_offset, int c_offset,
		int sum_offset, int carry_offset)
	{
		xMOV(eax, ptr32[rsp + a_offset]);
		xXOR(eax, ptr32[rsp + b_offset]);
		xMOV(ecx, eax);
		xAND(ecx, ptr32[rsp + c_offset]);
		xXOR(eax, ptr32[rsp + c_offset]);
		xMOV(ptr32[rsp + sum_offset], eax);
		xMOV(edx, ptr32[rsp + a_offset]);
		xAND(edx, ptr32[rsp + b_offset]);
		xOR(edx, ecx);
		xSHL(edx, 1);
		xMOV(ptr32[rsp + carry_offset], edx);
	}

	// AVX-512VL form of the same three-input carry-save primitive.  The sum
	// register contains operand A on entry and is overwritten with A^B^C.  The
	// separate carry register receives majority(A,B,C)<<1.  Keeping the tree in
	// XMM16+ avoids the scalar soft-float Booth path's temporary stack traffic.
	inline void EmitCarrySaveAddEVEX(const xRegisterSSE& sum, const xRegisterSSE& b,
		const xRegisterSSE& c, const xRegisterSSE& carry)
	{
		xVMOVDQA32(carry, sum);
		xVPTERNLOGD(carry, b, c, 0xe8);
		xVPSLLDImm(carry, carry, 1);
		xVPTERNLOGD(sum, b, c, 0x96);
	}

	// Emit the regular (non-first-one) scalar PS2 Booth correction with all
	// transient Booth terms and CSA nodes held in volatile AVX-512 high XMM
	// registers.  Assumptions match the legacy scalar sequence:
	//   r9d = restored operand significand
	//   [rsp+mantissa_a_offset] = restored source significand
	//   [rsp+full_lo/full_hi] = ordinary 24x24 product
	// The helper applies only the retained bit-15 correction to full_lo/full_hi.
	// Game-level integration still needs proper testing beyond kernel differentials.
	inline void EmitScalarBoothCorrectionEVEX(int mantissa_a_offset, int full_lo_offset,
		int full_hi_offset)
	{
		const std::array<xRegisterSSE, 8> data = {
			xmm16, xmm17, xmm18, xmm19, xmm20, xmm21, xmm22, xmm23};
		const std::array<xRegisterSSE, 3> negate = {xmm24, xmm25, xmm26};
		const xRegisterSSE data5_original = xmm27;
		const xRegisterSSE csa0_carry = xmm28;
		const xRegisterSSE csa1_carry = xmm29;
		const xRegisterSSE scratch = xmm30;

		xMOV64(r11, reinterpret_cast<uptr>(ScalarBoothDecode));
		for (int bit = 0; bit < 8; bit++)
		{
			const u32 shift = bit * 2;
			xMOV(edx, ptr32[rsp + mantissa_a_offset]);
			if (shift != 0)
				xSHL(edx, shift);
			xMOV(eax, r9d);
			if (bit == 0)
				xSHL(eax, 1);
			else
				xSHR(eax, shift - 1);
			xAND(eax, 7);
			xMOV(ecx, ptr32[xAddressVoid(r11, rax, 4)]);
			xMOV(r10d, ecx);
			xAND(r10d, 3);
			xMUL(edx, r10d);
			xSHR(ecx, 8);
			if (shift != 0)
				xSHL(ecx, shift);
			if (bit >= 5)
				xVMOVD(negate[bit - 5], ecx);
			xNEG(ecx);
			xXOR(edx, ecx);
			if (bit == 4)
				xAND(edx, ~0x7ffu);
			else if (bit == 5)
			{
				xVMOVD(data5_original, edx);
				xAND(edx, ~0xfffu);
			}
			xVMOVD(data[bit], edx);
		}

		// t0 = CSA(d1,d2,d3), t1 = CSA(d4,d5,d6).
		EmitCarrySaveAddEVEX(data[1], data[2], data[3], csa0_carry);
		EmitCarrySaveAddEVEX(data[4], data[5], data[6], csa1_carry);

		// t1_hi |= (d5_original & 0x800) | negate[6]. Keep the correction in
		// vector registers instead of round-tripping through GPRs.
		xVPTERNLOGD(scratch, data5_original, ptr128[ScalarBoothBit11], 0x88);
		xVPTERNLOGD(csa1_carry, scratch, negate[1], 0xfe);

		// d7 |= (d5_original & 0x400) + negate[5].
		xVPTERNLOGD(scratch, data5_original, ptr128[ScalarBoothBit10], 0x88);
		xVPADDD(scratch, scratch, negate[0]);
		xVPTERNLOGD(data[7], scratch, scratch, 0xfe);

		// t2 = CSA(d0,t0_lo,t0_hi).  d2 is dead and becomes t2_hi.
		EmitCarrySaveAddEVEX(data[0], data[1], csa0_carry, data[2]);
		// t3 = CSA(d7,t1_lo,t1_hi).  d3 is dead and becomes t3_hi.
		EmitCarrySaveAddEVEX(data[7], data[4], csa1_carry, data[3]);
		// t4 = CSA(t2_hi,t3_lo,t3_hi).  t1_lo is dead and becomes t4_hi.
		EmitCarrySaveAddEVEX(data[2], data[7], data[3], data[4]);
		// t5 = CSA(t2_lo,t4_lo,t4_hi).  d5 is dead and becomes t5_hi.
		EmitCarrySaveAddEVEX(data[0], data[2], data[4], data[5]);

		// Only bit 15 of the final tree sum can alter the retained PS2 product.
		xVMOVD(eax, data[5]);
		xVMOVD(ecx, negate[2]);
		xADD(eax, ecx);
		xAND(eax, ~0x7fffu);
		xVMOVD(ecx, data[0]);
		xAND(ecx, ~0x7fffu);
		xADD(ecx, eax);
		xXOR(ecx, ptr32[rsp + full_lo_offset]);
		xAND(ecx, 0x8000);
		xSUB(ptr32[rsp + full_lo_offset], ecx);
		xSBB(ptr32[rsp + full_hi_offset], 0);
	}

	inline void EmitDivCarrySaveStep()
	{
		xXOR(eax, eax);
		xCMP(r8d, 0);
		xForwardJLE8 quotient_not_positive;
		xMOV(eax, r11d);
		xNOT(eax);
		xINC(r10d);
		xForwardJump8 add_ready_positive;
		quotient_not_positive.SetTarget();
		xForwardJGE8 quotient_not_negative;
		xMOV(eax, r11d);
		quotient_not_negative.SetTarget();
		add_ready_positive.SetTarget();

		xMOV(edx, r9d);
		xXOR(edx, r10d);
		xMOV(ecx, edx);
		xAND(ecx, eax);
		xXOR(edx, eax);
		xMOV(eax, r9d);
		xAND(eax, r10d);
		xOR(eax, ecx);
		xSHL(eax, 1);

		xTEST(r8d, r8d);
		xMOV(ecx, edx);
		xCMOVZ(ecx, r9d);
		xMOV(r8d, eax);
		xCMOVZ(r8d, r10d);
		xMOV(r9d, edx);
		xSHL(r9d, 1);
		xMOV(r10d, eax);
		xSHL(r10d, 1);
		xMOV(edx, ecx);
		xAND(edx, 0xff000000);
		xADD(edx, r8d);
		xAND(ecx, 0x00ffffff);
		xOR(edx, ecx);
		xXOR(r8d, r8d);
		xCMP(edx, 1 << 23);
		xForwardJL8 quotient_not_one;
		xMOV(r8d, 1);
		xForwardJump8 quotient_selected;
		quotient_not_one.SetTarget();
		xCMP(edx, static_cast<u32>(~0u << 24));
		xForwardJGE8 quotient_selected_nonnegative;
		xMOV(r8d, -1);
		quotient_selected_nonnegative.SetTarget();
		quotient_selected.SetTarget();
	}

	// Input: r9d = restored dividend significand, r11d = restored divisor
	// significand. Output: eax = ordinary normalized quotient T, edx = cap,
	// ecx = distance u to the next quotient, and r8d = (sma < smb). This helper
	// only constructs the one-sided predicate; it does not select T over SRT.
	inline void EmitSrtDivCapQuotient()
	{
		xMOV(eax, r9d);
		xCMP(r9d, r11d);
		xForwardJGE8 numerator_not_lt;
		xSHL(rax, 24);
		xMOV(r8d, 1);
		xForwardJump8 numerator_ready;
		numerator_not_lt.SetTarget();
		xSHL(rax, 23);
		xXOR(r8d, r8d);
		numerator_ready.SetTarget();

		xXOR(edx, edx);
		xUDIV(r11);
		xMOV(ecx, r11d);
		xSUB(ecx, edx);
		xMOV(edx, 1 << 22);
		xTEST(r8d, r8d);
		xForwardJZ8 cap_ready_not_lt;
		xMOV(edx, r11d);
		xSUB(edx, 1 << 22);
		xCMP(edx, 1 << 23);
		xForwardJGE8 cap_ready_from_max;
		xMOV(edx, 1 << 23);
		cap_ready_not_lt.SetTarget();
		cap_ready_from_max.SetTarget();
	}

	inline void EmitSqrtCarrySaveStep()
	{
		xXOR(eax, eax);
		xCMP(r8d, 0);
		xForwardJLE8 sqrt_digit_not_positive;
		xMOV(eax, edi);
		xNOT(eax);
		xINC(r10d);
		xForwardJump8 sqrt_add_ready_positive;
		sqrt_digit_not_positive.SetTarget();
		xForwardJGE8 sqrt_digit_not_negative;
		xMOV(eax, edi);
		sqrt_digit_not_negative.SetTarget();
		sqrt_add_ready_positive.SetTarget();
		xMOV(edi, eax);

		xMOV(edx, r9d);
		xXOR(edx, r10d);
		xMOV(ecx, edx);
		xAND(ecx, edi);
		xXOR(edx, edi);
		xMOV(eax, r9d);
		xAND(eax, r10d);
		xOR(eax, ecx);
		xSHL(eax, 1);

		xTEST(r8d, r8d);
		xMOV(ecx, edx);
		xCMOVZ(ecx, r9d);
		xMOV(r8d, eax);
		xCMOVZ(r8d, r10d);
		xMOV(r9d, edx);
		xSHL(r9d, 1);
		xMOV(r10d, eax);
		xSHL(r10d, 1);
		xMOV(edx, ecx);
		xAND(ecx, 0xff000000);
		xADD(ecx, r8d);
		xAND(edx, 0x00ffffff);
		xOR(ecx, edx);
		xXOR(r8d, r8d);
		xCMP(ecx, 1 << 23);
		xForwardJL8 sqrt_quotient_not_one;
		xMOV(r8d, 1);
		xForwardJump8 sqrt_quotient_selected;
		sqrt_quotient_not_one.SetTarget();
		xCMP(ecx, static_cast<u32>(~0u << 24));
		xForwardJGE8 sqrt_quotient_selected_nonnegative;
		xMOV(r8d, -1);
		sqrt_quotient_selected_nonnegative.SetTarget();
		sqrt_quotient_selected.SetTarget();
	}
} // namespace X86SoftFloatEmitter
