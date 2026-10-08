// SPDX-License-Identifier: GPL-3.0+
#pragma once
#include "common/Pcsx2Defs.h"

// Callers must select this backend with avx512.HasCore(). These leaf functions
// contain no CPU detection, shared state or guest register decoding.
namespace InterpreterAVX512
{
	void PLZCW(void* dst, const void* source);
	void PNOR(void* dst, const void* source, const void* operand);
	void QFSRV(void* dst, const void* source, const void* operand, u32 byte_shift);
	void PEXT5(void* dst, const void* operand);
	void PPAC5(void* dst, const void* operand);
	void UnpackV4(void* dst, const void* source, u32 count, u32 format, bool is_unsigned);
	void YUVToRGB(const u8* y, const u8* cb, const u8* cr, void* rgba);
	void CSCThresholdSign(void* rgba, u32 th0, u32 th1, bool sgn);
	void IPUDither(const void* rgb32, void* rgb16, bool dte);
	void IPUExpandU8ToU16(const u8* src, u16* dst, u32 bytes);
	void VQ(const void* rgb16, const void* clut, u8* indx4);
} // namespace InterpreterAVX512

// AVX2 flavours of the IPU leaves above, for CPUs without the Ice Lake feature set. Select with
// cpuinfo_has_x86_avx2(). Same contracts as the AVX-512 functions of the same name.
namespace InterpreterAVX2
{
	void CSCThresholdSign(void* rgba, u32 th0, u32 th1, bool sgn);
	void IPUExpandU8ToU16(const u8* src, u16* dst, u32 bytes);
	void VQ(const void* rgb16, const void* clut, u8* indx4);
	void YUVToRGB(const u8* y, const u8* cb, const u8* cr, void* rgba);
	void IPUDither(const void* rgb32, void* rgb16, bool dte);
	// V4-32/16/8 only (format 0xc..0xe); two VU vectors per iteration. V4-5 stays on the generic path.
	void UnpackV4(void* dst, const void* source, u32 count, u32 format, bool is_unsigned);
	// S/V2/V3 (formats 0..2, 4..6, 8..10); see the comment at the definition for what the caller must guarantee.
	void UnpackSV(void* dst, const void* source, u32 count, u32 format, bool is_unsigned, u32 cl, u32 wl);
} // namespace InterpreterAVX2

namespace R5900::Interpreter::OpcodeImpl::MMI
{
	void PLZCW_AVX512();
	void PNOR_AVX512();
	void QFSRV_AVX512();
	void PEXT5_AVX512();
	void PPAC5_AVX512();
} // namespace R5900::Interpreter::OpcodeImpl::MMI
