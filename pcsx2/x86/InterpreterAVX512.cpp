// SPDX-License-Identifier: GPL-3.0+
#include "InterpreterAVX512.h"
#include <immintrin.h>
#include <cstring>

#if defined(__GNUC__) || defined(__clang__)
#define ICELAKE_TARGET __attribute__((target("avx512f,avx512vl,avx512bw,avx512dq,avx512cd,avx512vbmi,avx512vbmi2,avx512vnni")))
#else
#define ICELAKE_TARGET
#endif

#if defined(__GNUC__) || defined(__clang__)
#define AVX2_TARGET __attribute__((target("avx2")))
#else
#define AVX2_TARGET
#endif

// Per-row 4-pixel dither matrices of IPU RGB32 -> RGB16 (same as the SSE2 version), shared by the AVX2 and AVX-512 kernels.
alignas(16) static const u32 s_ipu_dither_add[4][4] = {
	{0x00000000, 0x00000000, 0x00000000, 0x00010101},
	{0x00020202, 0x00000000, 0x00030303, 0x00000000},
	{0x00000000, 0x00010101, 0x00000000, 0x00000000},
	{0x00030303, 0x00000000, 0x00020202, 0x00000000},
};
alignas(16) static const u32 s_ipu_dither_sub[4][4] = {
	{0x00040404, 0x00000000, 0x00030303, 0x00000000},
	{0x00000000, 0x00020202, 0x00000000, 0x00010101},
	{0x00030303, 0x00000000, 0x00040404, 0x00000000},
	{0x00000000, 0x00010101, 0x00000000, 0x00020202},
};

namespace InterpreterAVX512
{
	ICELAKE_TARGET void PLZCW(void* dst, const void* source)
	{
		const __m128i value = _mm_loadu_si128(static_cast<const __m128i*>(source));
		const __m128i normalized = _mm_xor_si128(value, _mm_srai_epi32(value, 31));
		const __m128i result = _mm_sub_epi32(_mm_lzcnt_epi32(normalized), _mm_set1_epi32(1));
		// PLZCW writes only the low two words; the upper half of Rd is preserved.
		_mm_storel_epi64(static_cast<__m128i*>(dst), result);
	}

	ICELAKE_TARGET void PNOR(void* dst, const void* source, const void* operand)
	{
		const __m128i a = _mm_loadu_si128(static_cast<const __m128i*>(source));
		const __m128i b = _mm_loadu_si128(static_cast<const __m128i*>(operand));
		_mm_storeu_si128(static_cast<__m128i*>(dst), _mm_ternarylogic_epi32(a, b, b, 0x03));
	}

	ICELAKE_TARGET void QFSRV(void* dst, const void* source, const void* operand, u32 byte_shift)
	{
		const __m128i a = _mm_loadu_si128(static_cast<const __m128i*>(source));
		const __m128i b = _mm_loadu_si128(static_cast<const __m128i*>(operand));
		const __m128i offsets = _mm_setr_epi8(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15);
		const __m128i index = _mm_add_epi8(offsets, _mm_set1_epi8(static_cast<char>(byte_shift)));
		_mm_storeu_si128(static_cast<__m128i*>(dst), _mm_permutex2var_epi8(b, index, a));
	}

	ICELAKE_TARGET void PEXT5(void* dst, const void* operand)
	{
		const __m128i value = _mm_loadu_si128(static_cast<const __m128i*>(operand));
		// Same windows as the JIT (emitPEXT5AVX512): one multishift, then mask.
		const __m128i ctrl = _mm_setr_epi8(61, 2, 7, 8, 29, 34, 39, 40, 61, 2, 7, 8, 29, 34, 39, 40);
		const __m128i mask = _mm_setr_epi8(-8, -8, -8, -128, -8, -8, -8, -128, -8, -8, -8, -128, -8, -8, -8, -128);
		_mm_storeu_si128(static_cast<__m128i*>(dst), _mm_and_si128(_mm_multishift_epi64_epi8(ctrl, value), mask));
	}

	ICELAKE_TARGET void PPAC5(void* dst, const void* operand)
	{
		const __m128i value = _mm_loadu_si128(static_cast<const __m128i*>(operand));
		// Same algorithm as the JIT (emitPPAC5AVX512): multishift, mask, PMADDUBSW, PMADDWD.
		const __m128i ctrl = _mm_setr_epi8(3, 11, 19, 31, 35, 43, 51, 63, 3, 11, 19, 31, 35, 43, 51, 63);
		const __m128i mask = _mm_setr_epi8(0x1f, 0x1f, 0x1f, 1, 0x1f, 0x1f, 0x1f, 1, 0x1f, 0x1f, 0x1f, 1, 0x1f, 0x1f, 0x1f, 1);
		const __m128i fields = _mm_and_si128(_mm_multishift_epi64_epi8(ctrl, value), mask);
		const __m128i pairs = _mm_maddubs_epi16(fields, _mm_set1_epi16(0x2001)); // bytes {1, 32}
		_mm_storeu_si128(static_cast<__m128i*>(dst), _mm_madd_epi16(pairs, _mm_set1_epi32(0x04000001))); // words {1, 1024}
	}

	// VIF V4-5: qword k of the 512-bit result belongs to pixel k/2 and holds {R,G} (even k) or {B,A}
	// (odd k) in its two low bytes-of-dword. Window offsets are PEXT5's {61,2,7,8} plus 16 per pixel.
	// Needs proper testing against the scalar unpack.
	struct UnpackV45Constants
	{
		alignas(64) u64 control[8];
		alignas(64) u64 maskBits[8];
		constexpr UnpackV45Constants() : control(), maskBits()
		{
			constexpr int offsets[4] = {61, 2, 7, 8};
			for (int k = 0; k < 8; k++)
			{
				const int c0 = (k & 1) ? 2 : 0;
				const int c1 = c0 + 1;
				const int base = (k / 2) * 16;
				control[k] = static_cast<u64>((offsets[c0] + base) & 63) | (static_cast<u64>((offsets[c1] + base) & 63) << 32);
				maskBits[k] = 0xf8ull | ((c1 == 3 ? 0x80ull : 0xf8ull) << 32);
			}
		}
	};

	ICELAKE_TARGET void UnpackV4(void* dst, const void* source, u32 count, u32 format, bool is_unsigned)
	{
		static constexpr UnpackV45Constants v45;
		auto* output = static_cast<__m128i*>(dst);
		const auto* input = static_cast<const u8*>(source);
		while (count >= 4)
		{
			__m512i value;
			if (format == 0xc)
			{
				// V4-32: the data is already four VU vectors.
				value = _mm512_loadu_si512(input);
				input += 64;
			}
			else if (format == 0xd)
			{
				const __m256i packed = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(input));
				value = is_unsigned ? _mm512_cvtepu16_epi32(packed) : _mm512_cvtepi16_epi32(packed);
				input += 32;
			}
			else if (format == 0xe)
			{
				const __m128i packed = _mm_loadu_si128(reinterpret_cast<const __m128i*>(input));
				value = is_unsigned ? _mm512_cvtepu8_epi32(packed) : _mm512_cvtepi8_epi32(packed);
				input += 16;
			}
			else
			{
				// 4 pixels in one qword broadcast to every lane; VPMULTISHIFTQB picks each component's
				// window (PEXT5 offsets, +16 per pixel) and the mask zero-extends to dwords.
				u64 pixels;
				std::memcpy(&pixels, input, sizeof(pixels));
				value = _mm512_and_si512(_mm512_multishift_epi64_epi8(_mm512_load_si512(v45.control),
											 _mm512_set1_epi64(static_cast<long long>(pixels))),
					_mm512_load_si512(v45.maskBits));
				input += 8;
			}
			_mm512_storeu_si512(output, value);
			output += 4;
			count -= 4;
		}
		for (u32 vector = 0; vector < count; vector++)
		{
			__m128i value;
			if (format == 0xc)
			{
				value = _mm_loadu_si128(reinterpret_cast<const __m128i*>(input));
				input += 16;
			}
			else if (format == 0xd)
			{
				value = _mm_loadl_epi64(reinterpret_cast<const __m128i*>(input));
				value = is_unsigned ? _mm_cvtepu16_epi32(value) : _mm_cvtepi16_epi32(value);
				input += 8;
			}
			else if (format == 0xe)
			{
				value = _mm_loadu_si32(input);
				value = is_unsigned ? _mm_cvtepu8_epi32(value) : _mm_cvtepi8_epi32(value);
				input += 4;
			}
			else
			{
				// V4-5 ignores USN and MODE and uses the same 1:5:5:5 expansion as PEXT5.
				value = _mm_set1_epi32(static_cast<u32>(input[0]) | (static_cast<u32>(input[1]) << 8));
				value = _mm_srlv_epi32(value, _mm_setr_epi32(0, 5, 10, 15));
				value = _mm_and_si128(value, _mm_setr_epi32(31, 31, 31, 1));
				value = _mm_sllv_epi32(value, _mm_setr_epi32(3, 3, 3, 7));
				input += 2;
			}
			_mm_storeu_si128(output++, value);
		}
	}

	ICELAKE_TARGET void YUVToRGB(const u8* y, const u8* cb, const u8* cr, void* rgba)
	{
		auto* output = static_cast<u8*>(rgba);
		const __m512i duplicate = _mm512_setr_epi32(0x00000000, 0x00010001, 0x00020002, 0x00030003,
			0x00040004, 0x00050005, 0x00060006, 0x00070007, 0x00000000, 0x00010001, 0x00020002, 0x00030003,
			0x00040004, 0x00050005, 0x00060006, 0x00070007);
		// Byte i of output 0 / 1 picks R[p], G[p], B[p], alpha[p] from the 128-byte table rg|ba.
		// A constexpr table keeps Clang from constant-folding the VPERMT2B into a long unpack/insert chain.
		struct PackIndex
		{
			alignas(64) u8 value[2][64];
			constexpr PackIndex() : value()
			{
				for (u32 half = 0; half < 2; half++)
					for (u32 i = 0; i < 64; i++)
						value[half][i] = static_cast<u8>(((i & 3) * 32) + half * 16 + (i >> 2));
			}
		};
		static constexpr PackIndex pack_index;
		const __m512i pack0 = _mm512_load_si512(pack_index.value[0]);
		const __m512i pack1 = _mm512_load_si512(pack_index.value[1]);
		const __m256i alpha = _mm256_set1_epi8(static_cast<char>(0x80));
		for (u32 row = 0; row < 16; row += 2)
		{
			const __m256i y8 = _mm256_subs_epu8(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(y + row * 16)), _mm256_set1_epi8(16));
			const __m128i cb8 = _mm_loadl_epi64(reinterpret_cast<const __m128i*>(cb + (row / 2) * 8));
			const __m128i cr8 = _mm_loadl_epi64(reinterpret_cast<const __m128i*>(cr + (row / 2) * 8));
			const __m512i cb16 = _mm512_slli_epi16(_mm512_sub_epi16(_mm512_permutexvar_epi16(duplicate, _mm512_cvtepu8_epi16(_mm256_castsi128_si256(cb8))), _mm512_set1_epi16(128)), 8);
			const __m512i cr16 = _mm512_slli_epi16(_mm512_sub_epi16(_mm512_permutexvar_epi16(duplicate, _mm512_cvtepu8_epi16(_mm256_castsi128_si256(cr8))), _mm512_set1_epi16(128)), 8);
			const __m512i lum = _mm512_mulhi_epu16(_mm512_slli_epi16(_mm512_cvtepu8_epi16(y8), 8), _mm512_set1_epi16(149 * 4));
			// Keep each product's arithmetic floor before adding the green terms.
			// A VNNI dot product would discard that separate rounding boundary.
			const __m512i rcr = _mm512_mulhi_epi16(cr16, _mm512_set1_epi16(204 * 4));
			const __m512i gcr = _mm512_mulhi_epi16(cr16, _mm512_set1_epi16(-104 * 4));
			const __m512i gcb = _mm512_mulhi_epi16(cb16, _mm512_set1_epi16(-50 * 4));
			const __m512i bcb = _mm512_mulhi_epi16(cb16, _mm512_set1_epi16(258 * 4));
			// max(0, (x + 1) >> 1) as avg_epu16(max(x, 0), 0); needs proper testing at the range edges.
			const __m512i zero = _mm512_setzero_si512();
			const __m256i r = _mm512_cvtusepi16_epi8(_mm512_avg_epu16(_mm512_max_epi16(_mm512_add_epi16(lum, rcr), zero), zero));
			const __m256i g = _mm512_cvtusepi16_epi8(_mm512_avg_epu16(_mm512_max_epi16(_mm512_add_epi16(lum, _mm512_add_epi16(gcr, gcb)), zero), zero));
			const __m256i b = _mm512_cvtusepi16_epi8(_mm512_avg_epu16(_mm512_max_epi16(_mm512_add_epi16(lum, bcb), zero), zero));
			// R|G and B|alpha as two ZMM tables; two VBMI VPERMT2B build the 32 RGBA pixels
			// (16 per output register). Needs proper testing in games.
			const __m512i rg = _mm512_inserti64x4(_mm512_castsi256_si512(r), g, 1);
			const __m512i ba = _mm512_inserti64x4(_mm512_castsi256_si512(b), alpha, 1);
			_mm512_storeu_si512(output + row * 64, _mm512_permutex2var_epi8(rg, pack0, ba));
			_mm512_storeu_si512(output + (row + 1) * 64, _mm512_permutex2var_epi8(rg, pack1, ba));
		}
	}
	// "R, G and B all below th" without building max(R,G,B): per byte, px -| (th - 1) is zero exactly
	// when px <= th - 1, and the alpha byte is masked out by the test. th is 9 bits, so th == 0 can
	// never match and th >= 256 always matches (modes 0 / 1 / 2).
	template <int Mode>
	ICELAKE_TARGET static inline __mmask16 CSCBelow(__m512i px, __m512i sat_th, __m512i rgb_mask)
	{
		if constexpr (Mode == 0)
			return 0;
		else if constexpr (Mode == 2)
			return 0xffff;
		else
			return _mm512_testn_epi32_mask(_mm512_subs_epu8(px, sat_th), rgb_mask);
	}

	template <int Mode0, int Mode1>
	ICELAKE_TARGET static void CSCThresholdSignLoop(u8* pixels, u32 th0, u32 th1, bool sgn)
	{
		const __m512i sat0 = _mm512_set1_epi8(static_cast<char>(th0 - 1));
		const __m512i sat1 = _mm512_set1_epi8(static_cast<char>(th1 - 1));
		const __m512i rgb_mask = _mm512_set1_epi32(0x00ffffff);
		const __m512i alpha40 = _mm512_set1_epi32(0x40000000);
		const __m512i sign = _mm512_set1_epi32(sgn ? 0x808080 : 0);
		for (u32 i = 0; i < 256 * 4; i += 64)
		{
			__m512i px = _mm512_loadu_si512(pixels + i);
			const __mmask16 below0 = CSCBelow<Mode0>(px, sat0, rgb_mask);
			const __mmask16 below1 = CSCBelow<Mode1>(px, sat1, rgb_mask) & static_cast<__mmask16>(~below0);
			px = _mm512_mask_ternarylogic_epi32(px, below1, rgb_mask, alpha40, 0xea); // (px & rgb) | 0x40 << 24
			px = _mm512_maskz_mov_epi32(static_cast<__mmask16>(~below0), px);
			_mm512_storeu_si512(pixels + i, _mm512_xor_si512(px, sign));
		}
	}

	// Thresholds and sign of IPU CSC on 256 RGBA32 pixels, matching the scalar loops
	// in ipu_csc: below th0 -> 0, else below th1 -> alpha 0x40, then optional sign XOR.
	// Compared against the scalar code on 200000 random macroblocks; needs proper testing in games.
	ICELAKE_TARGET void CSCThresholdSign(void* rgba, u32 th0, u32 th1, bool sgn)
	{
		auto* pixels = static_cast<u8*>(rgba);
		const int m0 = th0 == 0 ? 0 : (th0 >= 256 ? 2 : 1);
		const int m1 = th1 == 0 ? 0 : (th1 >= 256 ? 2 : 1);
		switch (m0 * 3 + m1)
		{
#define CSC_CASE(a, b) case (a) * 3 + (b): CSCThresholdSignLoop<a, b>(pixels, th0, th1, sgn); break;
			CSC_CASE(0, 0) CSC_CASE(0, 1) CSC_CASE(0, 2)
			CSC_CASE(1, 0) CSC_CASE(1, 1) CSC_CASE(1, 2)
			CSC_CASE(2, 0) CSC_CASE(2, 1) CSC_CASE(2, 2)
#undef CSC_CASE
		}
	}

	// IPU RGB32 -> RGB16 (optional 4x4 dither), 16 pixels per ZMM. Pixel bytes are {R,G,B,A}.
	// Dither: saturating add then saturating subtract of the same per-row 4-pixel matrices as the
	// SSE2 version. Then VPMULTISHIFTQB takes each 5-bit field (bits 3.. of R/G/B) as a byte,
	// PMADDUBSW {1,32} and PMADDWD {1,1024} pack R|G<<5|B<<10, and alpha == 0x40 sets bit 15
	// through a k mask. Needs proper testing against the scalar reference on real FMV/dither content.
	ICELAKE_TARGET void IPUDither(const void* rgb32, void* rgb16, bool dte)
	{
		const __m512i control = _mm512_set1_epi64(0x00332B2300130B03ll); // {3,11,19,0} + 32 for the odd pixel
		const __m512i mask = _mm512_set1_epi64(0x001F1F1F001F1F1Fll);
		const __m512i weight1 = _mm512_set1_epi16(0x2001);
		const __m512i weight2 = _mm512_set1_epi32(0x04000001);
		const __m512i alpha_mask = _mm512_set1_epi32(static_cast<int>(0xff000000u));
		const __m512i alpha_value = _mm512_set1_epi32(0x40000000);
		const __m512i alpha_bit = _mm512_set1_epi32(0x8000);
		const auto* src = static_cast<const u8*>(rgb32);
		auto* dst = static_cast<u8*>(rgb16);
		for (u32 row = 0; row < 16; row++)
		{
			__m512i px = _mm512_loadu_si512(src + row * 64);
			if (dte)
			{
				px = _mm512_adds_epu8(px, _mm512_broadcast_i32x4(_mm_loadu_si128(reinterpret_cast<const __m128i*>(s_ipu_dither_add[row & 3]))));
				px = _mm512_subs_epu8(px, _mm512_broadcast_i32x4(_mm_loadu_si128(reinterpret_cast<const __m128i*>(s_ipu_dither_sub[row & 3]))));
			}
			const __mmask16 alpha_hit = _mm512_cmpeq_epi32_mask(_mm512_and_si512(px, alpha_mask), alpha_value);
			const __m512i fields = _mm512_and_si512(_mm512_multishift_epi64_epi8(control, px), mask);
			__m512i rgba16 = _mm512_madd_epi16(_mm512_maddubs_epi16(fields, weight1), weight2);
			rgba16 = _mm512_mask_or_epi32(rgba16, alpha_hit, rgba16, alpha_bit);
			_mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + row * 32), _mm512_cvtepi32_epi16(rgba16));
		}
	}

	// IPU intra: zero-extend 384 bytes (Y, Cb, Cr) to 16 bit, 32 bytes per VPMOVZXBW.
	ICELAKE_TARGET void IPUExpandU8ToU16(const u8* src, u16* dst, u32 bytes)
	{
		for (u32 i = 0; i < bytes; i += 32)
			_mm512_storeu_si512(dst + i, _mm512_cvtepu8_epi16(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(src + i))));
	}

	// IPU VQ: nearest of 16 CLUT colours for each RGB16 pixel, lowest index on ties.
	// Two pixels per ZMM (16 words each). Key = distance << 4 | index, so an unsigned
	// minimum gives the first closest entry; max distance 3 * 31^2 = 2883 fits.
	ICELAKE_TARGET void VQ(const void* rgb16, const void* clut, u8* indx4)
	{
		const auto* src = static_cast<const u16*>(rgb16);
		const __m256i clut16 = _mm256_loadu_si256(static_cast<const __m256i*>(clut));
		const __m512i c = _mm512_inserti64x4(_mm512_castsi256_si512(clut16), clut16, 1);
		const __m512i five = _mm512_set1_epi16(31);
		const __m512i cr = _mm512_and_si512(c, five);
		const __m512i cg = _mm512_and_si512(_mm512_srli_epi16(c, 5), five);
		const __m512i cb = _mm512_and_si512(_mm512_srli_epi16(c, 10), five);
		const __m512i index = _mm512_set_epi16(15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0,
			15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0);
		const auto closest = [](__m256i keys) ICELAKE_TARGET {
			const __m128i m = _mm_min_epu16(_mm256_castsi256_si128(keys), _mm256_extracti128_si256(keys, 1));
			return static_cast<u32>(_mm_cvtsi128_si32(_mm_minpos_epu16(m))) & 15;
		};
		for (u32 n = 0; n < 128; n++)
		{
			const __m512i p = _mm512_inserti64x4(_mm512_set1_epi16(static_cast<short>(src[n * 2])),
				_mm256_set1_epi16(static_cast<short>(src[n * 2 + 1])), 1);
			const __m512i dr = _mm512_sub_epi16(_mm512_and_si512(p, five), cr);
			const __m512i dg = _mm512_sub_epi16(_mm512_and_si512(_mm512_srli_epi16(p, 5), five), cg);
			const __m512i db = _mm512_sub_epi16(_mm512_and_si512(_mm512_srli_epi16(p, 10), five), cb);
			const __m512i dist = _mm512_add_epi16(_mm512_add_epi16(_mm512_mullo_epi16(dr, dr), _mm512_mullo_epi16(dg, dg)),
				_mm512_mullo_epi16(db, db));
			const __m512i keys = _mm512_or_si512(_mm512_slli_epi16(dist, 4), index);
			indx4[n] = static_cast<u8>(closest(_mm512_extracti64x4_epi64(keys, 1)) << 4 | closest(_mm512_castsi512_si256(keys)));
		}
	}
} // namespace InterpreterAVX512

// AVX2 versions of the IPU leaves. Needs proper testing against the scalar code in games (FMV, VQ).
namespace InterpreterAVX2
{
	// "R, G and B all below th" as a per-dword all-ones mask; see InterpreterAVX512::CSCBelow.
	template <int Mode>
	AVX2_TARGET static inline __m256i CSCBelow(__m256i px, __m256i sat_th, __m256i rgb_mask)
	{
		if constexpr (Mode == 0)
			return _mm256_setzero_si256();
		else if constexpr (Mode == 2)
			return _mm256_set1_epi32(-1);
		else
			return _mm256_cmpeq_epi32(_mm256_and_si256(_mm256_subs_epu8(px, sat_th), rgb_mask), _mm256_setzero_si256());
	}

	template <int Mode0, int Mode1>
	AVX2_TARGET static void CSCThresholdSignLoop(u8* pixels, u32 th0, u32 th1, bool sgn)
	{
		const __m256i sat0 = _mm256_set1_epi8(static_cast<char>(th0 - 1));
		const __m256i sat1 = _mm256_set1_epi8(static_cast<char>(th1 - 1));
		const __m256i rgb_mask = _mm256_set1_epi32(0x00ffffff);
		const __m256i alpha40 = _mm256_set1_epi32(0x40000000);
		const __m256i sign = _mm256_set1_epi32(sgn ? 0x808080 : 0);
		for (u32 i = 0; i < 256 * 4; i += 32)
		{
			__m256i px = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(pixels + i));
			const __m256i below0 = CSCBelow<Mode0>(px, sat0, rgb_mask);
			const __m256i below1 = _mm256_andnot_si256(below0, CSCBelow<Mode1>(px, sat1, rgb_mask));
			px = _mm256_blendv_epi8(px, _mm256_or_si256(_mm256_and_si256(px, rgb_mask), alpha40), below1);
			px = _mm256_andnot_si256(below0, px);
			_mm256_storeu_si256(reinterpret_cast<__m256i*>(pixels + i), _mm256_xor_si256(px, sign));
		}
	}

	AVX2_TARGET void CSCThresholdSign(void* rgba, u32 th0, u32 th1, bool sgn)
	{
		auto* pixels = static_cast<u8*>(rgba);
		const int m0 = th0 == 0 ? 0 : (th0 >= 256 ? 2 : 1);
		const int m1 = th1 == 0 ? 0 : (th1 >= 256 ? 2 : 1);
		switch (m0 * 3 + m1)
		{
#define CSC_CASE(a, b) case (a) * 3 + (b): CSCThresholdSignLoop<a, b>(pixels, th0, th1, sgn); break;
			CSC_CASE(0, 0) CSC_CASE(0, 1) CSC_CASE(0, 2)
			CSC_CASE(1, 0) CSC_CASE(1, 1) CSC_CASE(1, 2)
			CSC_CASE(2, 0) CSC_CASE(2, 1) CSC_CASE(2, 2)
#undef CSC_CASE
		}
	}

	// 16 bytes -> 16 words per VPMOVZXBW ymm. bytes must be a multiple of 16.
	AVX2_TARGET void IPUExpandU8ToU16(const u8* src, u16* dst, u32 bytes)
	{
		for (u32 i = 0; i < bytes; i += 16)
			_mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + i), _mm256_cvtepu8_epi16(_mm_loadu_si128(reinterpret_cast<const __m128i*>(src + i))));
	}

	// One pixel per YMM against the 16 CLUT entries; key = distance << 4 | index (max 2883 << 4 fits in u16),
	// so the unsigned minimum is the lowest index among the closest entries.
	AVX2_TARGET void VQ(const void* rgb16, const void* clut, u8* indx4)
	{
		const auto* src = static_cast<const u16*>(rgb16);
		const __m256i c = _mm256_loadu_si256(static_cast<const __m256i*>(clut));
		const __m256i five = _mm256_set1_epi16(31);
		const __m256i cr = _mm256_and_si256(c, five);
		const __m256i cg = _mm256_and_si256(_mm256_srli_epi16(c, 5), five);
		const __m256i cb = _mm256_and_si256(_mm256_srli_epi16(c, 10), five);
		const __m256i index = _mm256_set_epi16(15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0);
		const auto closest = [&](u16 pixel) AVX2_TARGET {
			const __m256i p = _mm256_set1_epi16(static_cast<short>(pixel));
			const __m256i dr = _mm256_sub_epi16(_mm256_and_si256(p, five), cr);
			const __m256i dg = _mm256_sub_epi16(_mm256_and_si256(_mm256_srli_epi16(p, 5), five), cg);
			const __m256i db = _mm256_sub_epi16(_mm256_and_si256(_mm256_srli_epi16(p, 10), five), cb);
			const __m256i dist = _mm256_add_epi16(_mm256_add_epi16(_mm256_mullo_epi16(dr, dr), _mm256_mullo_epi16(dg, dg)),
				_mm256_mullo_epi16(db, db));
			const __m256i keys = _mm256_or_si256(_mm256_slli_epi16(dist, 4), index);
			const __m128i m = _mm_min_epu16(_mm256_castsi256_si128(keys), _mm256_extracti128_si256(keys, 1));
			return static_cast<u32>(_mm_cvtsi128_si32(_mm_minpos_epu16(m))) & 15;
		};
		for (u32 n = 0; n < 128; n++)
			indx4[n] = static_cast<u8>(closest(src[n * 2 + 1]) << 4 | closest(src[n * 2]));
	}

	// IPU RGB32 -> RGB16 with optional dither, 16 pixels per row as two YMM. Fields are taken the
	// AVX-512 way (>>3, mask, PMADDUBSW {1,32}, PMADDWD {1,1024}) but without VPMULTISHIFTQB; alpha == 0x40
	// is a dword compare. Needs proper testing against the scalar reference on real FMV/dither content.
	AVX2_TARGET void IPUDither(const void* rgb32, void* rgb16, bool dte)
	{
		const __m256i mask = _mm256_set1_epi32(0x001f1f1f);
		const __m256i weight1 = _mm256_set1_epi16(0x2001);
		const __m256i weight2 = _mm256_set1_epi32(0x04000001);
		const __m256i alpha_value = _mm256_set1_epi32(0x40);
		const __m256i alpha_bit = _mm256_set1_epi32(0x8000);
		const auto* src = static_cast<const u8*>(rgb32);
		auto* dst = static_cast<u8*>(rgb16);
		const auto convert = [&](__m256i px) AVX2_TARGET {
			const __m256i fields = _mm256_and_si256(_mm256_srli_epi32(px, 3), mask);
			const __m256i rgb = _mm256_madd_epi16(_mm256_maddubs_epi16(fields, weight1), weight2);
			const __m256i alpha = _mm256_and_si256(_mm256_cmpeq_epi32(_mm256_srli_epi32(px, 24), alpha_value), alpha_bit);
			return _mm256_or_si256(rgb, alpha);
		};
		for (u32 row = 0; row < 16; row++)
		{
			__m256i p0 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(src + row * 64));
			__m256i p1 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(src + row * 64 + 32));
			if (dte)
			{
				const __m256i add = _mm256_broadcastsi128_si256(_mm_load_si128(reinterpret_cast<const __m128i*>(s_ipu_dither_add[row & 3])));
				const __m256i sub = _mm256_broadcastsi128_si256(_mm_load_si128(reinterpret_cast<const __m128i*>(s_ipu_dither_sub[row & 3])));
				p0 = _mm256_subs_epu8(_mm256_adds_epu8(p0, add), sub);
				p1 = _mm256_subs_epu8(_mm256_adds_epu8(p1, add), sub);
			}
			// Values are <= 0xffff, so the unsigned-saturating pack is exact; it interleaves the 128-bit lanes.
			const __m256i packed = _mm256_packus_epi32(convert(p0), convert(p1));
			_mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + row * 32), _mm256_permute4x64_epi64(packed, 0xd8));
		}
	}

	// Same arithmetic as yuv2rgb_sse2 with the two luma rows of a chroma row in one YMM (one row per 128-bit
	// lane; both rows share Cb/Cr, which are broadcast to both lanes). Needs proper testing in FMV games.
	AVX2_TARGET void YUVToRGB(const u8* y, const u8* cb, const u8* cr, void* rgba)
	{
		const __m256i c_bias = _mm256_set1_epi8(static_cast<char>(128));
		const __m256i y_bias = _mm256_set1_epi8(16);
		const __m256i y_mask = _mm256_set1_epi16(static_cast<short>(0xFF00));
		const __m256i round_1bit = _mm256_set1_epi16(1);
		const __m256i zero = _mm256_setzero_si256();
		const __m256i y_coefficient = _mm256_set1_epi16(0x95 << 2);
		const __m256i gcr_coefficient = _mm256_set1_epi16(static_cast<short>(static_cast<u16>(-0x68) << 2));
		const __m256i gcb_coefficient = _mm256_set1_epi16(static_cast<short>(static_cast<u16>(-0x32) << 2));
		const __m256i rcr_coefficient = _mm256_set1_epi16(0xcc << 2);
		const __m256i bcb_coefficient = _mm256_set1_epi16(0x102 << 2);
		auto* output = static_cast<u8*>(rgba);
		for (u32 n = 0; n < 8; n++)
		{
			// (Cb - 128) << 8 and (Cr - 128) << 8 for chroma 0..7, in both lanes.
			__m256i cbv = _mm256_xor_si256(_mm256_broadcastq_epi64(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(cb + n * 8))), c_bias);
			__m256i crv = _mm256_xor_si256(_mm256_broadcastq_epi64(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(cr + n * 8))), c_bias);
			cbv = _mm256_unpacklo_epi8(zero, cbv);
			crv = _mm256_unpacklo_epi8(zero, crv);
			const __m256i rc = _mm256_mulhi_epi16(crv, rcr_coefficient);
			const __m256i gc = _mm256_adds_epi16(_mm256_mulhi_epi16(crv, gcr_coefficient), _mm256_mulhi_epi16(cbv, gcb_coefficient));
			const __m256i bc = _mm256_mulhi_epi16(cbv, bcb_coefficient);

			// Rows 2n (lane 0) and 2n + 1 (lane 1) are contiguous in Y.
			__m256i yv = _mm256_subs_epu8(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(y + n * 32)), y_bias);
			const __m256i y_even = _mm256_mulhi_epu16(_mm256_slli_epi16(yv, 8), y_coefficient);
			const __m256i y_odd = _mm256_mulhi_epu16(_mm256_and_si256(yv, y_mask), y_coefficient);

			const auto channel = [&](__m256i c) AVX2_TARGET {
				const __m256i even = _mm256_srai_epi16(_mm256_add_epi16(_mm256_adds_epi16(c, y_even), round_1bit), 1);
				const __m256i odd = _mm256_srai_epi16(_mm256_add_epi16(_mm256_adds_epi16(c, y_odd), round_1bit), 1);
				const __m256i packed = _mm256_packus_epi16(even, odd);
				return _mm256_unpacklo_epi8(packed, _mm256_shuffle_epi32(packed, _MM_SHUFFLE(3, 2, 3, 2)));
			};
			const __m256i r = channel(rc);
			const __m256i g = channel(gc);
			const __m256i b = channel(bc);

			const __m256i rg_l = _mm256_unpacklo_epi8(r, g);
			const __m256i ba_l = _mm256_unpacklo_epi8(b, c_bias);
			const __m256i rg_h = _mm256_unpackhi_epi8(r, g);
			const __m256i ba_h = _mm256_unpackhi_epi8(b, c_bias);
			const __m256i out[4] = {
				_mm256_unpacklo_epi16(rg_l, ba_l), _mm256_unpackhi_epi16(rg_l, ba_l),
				_mm256_unpacklo_epi16(rg_h, ba_h), _mm256_unpackhi_epi16(rg_h, ba_h)};
			u8* row0 = output + n * 128;
			u8* row1 = row0 + 64;
			for (int k = 0; k < 4; k++)
			{
				_mm_storeu_si128(reinterpret_cast<__m128i*>(row0 + k * 16), _mm256_castsi256_si128(out[k]));
				_mm_storeu_si128(reinterpret_cast<__m128i*>(row1 + k * 16), _mm256_extracti128_si256(out[k], 1));
			}
		}
	}

	AVX2_TARGET void UnpackV4(void* dst, const void* source, u32 count, u32 format, bool is_unsigned)
	{
		auto* output = static_cast<u8*>(dst);
		const auto* input = static_cast<const u8*>(source);
		while (count >= 2)
		{
			__m256i value;
			if (format == 0xc)
			{
				value = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(input));
				input += 32;
			}
			else if (format == 0xd)
			{
				const __m128i packed = _mm_loadu_si128(reinterpret_cast<const __m128i*>(input));
				value = is_unsigned ? _mm256_cvtepu16_epi32(packed) : _mm256_cvtepi16_epi32(packed);
				input += 16;
			}
			else
			{
				const __m128i packed = _mm_loadl_epi64(reinterpret_cast<const __m128i*>(input));
				value = is_unsigned ? _mm256_cvtepu8_epi32(packed) : _mm256_cvtepi8_epi32(packed);
				input += 8;
			}
			_mm256_storeu_si256(reinterpret_cast<__m256i*>(output), value);
			output += 32;
			count -= 2;
		}
		if (count)
		{
			__m128i value;
			if (format == 0xc)
			{
				value = _mm_loadu_si128(reinterpret_cast<const __m128i*>(input));
			}
			else if (format == 0xd)
			{
				value = _mm_loadl_epi64(reinterpret_cast<const __m128i*>(input));
				value = is_unsigned ? _mm_cvtepu16_epi32(value) : _mm_cvtepi16_epi32(value);
			}
			else
			{
				value = _mm_loadu_si32(input);
				value = is_unsigned ? _mm_cvtepu8_epi32(value) : _mm_cvtepi8_epi32(value);
			}
			_mm_storeu_si128(reinterpret_cast<__m128i*>(output), value);
		}
	}
	// VIF unpack formats S-32/16/8 (0..2), V2-32/16/8 (4..6) and V3-32/16/8 (8..10) for the plain interpreter
	// path (no mask, no mode, cl == wl), one 16-byte vector at a time without the call/ret into the SSE
	// unpackers. Mirrors VifUnpackSSE_Base::xUPK_* with IsAligned == true and UnpkLoopIteration = min(cl, 3):
	//  - S and V2 take every vector straight from the stream. The generic code reaches the same result only
	//    for wl <= 4 (S) / wl <= 2 (V2) and a cycle starting at cl == 0; the caller guarantees that.
	//  - V3 keeps W (the next element, over-read exactly like the generic code) only on some iterations:
	//    V3-32/V3-8 when iteration == 1, V3-16 when iteration is odd; otherwise W = 0. `cl` is the cycle
	//    position of the first vector and `wl` the cycle length (>= 1).
	// Needs proper testing on VIF-heavy titles.
	template <u32 Format, bool Unsigned>
	AVX2_TARGET static void UnpackSVLoop(u8* output, const u8* input, u32 count, u32 cl, u32 wl)
	{
		const __m128i zero = _mm_setzero_si128();
		for (u32 i = 0; i < count; i++)
		{
			const u32 iteration = cl < 3 ? cl : 3;
			__m128i value;
			if constexpr (Format == 0)
			{
				value = _mm_castps_si128(_mm_broadcast_ss(reinterpret_cast<const float*>(input)));
				input += 4;
			}
			else if constexpr (Format == 1)
			{
				u16 raw;
				std::memcpy(&raw, input, sizeof(raw));
				value = _mm_set1_epi32(Unsigned ? static_cast<s32>(raw) : static_cast<s16>(raw));
				input += 2;
			}
			else if constexpr (Format == 2)
			{
				s32 element = Unsigned ? *input : static_cast<s8>(*input);
				value = _mm_set1_epi32(element);
				input += 1;
			}
			else if constexpr (Format == 4)
			{
				const __m128i pair = _mm_loadl_epi64(reinterpret_cast<const __m128i*>(input));
				value = _mm_blend_epi32(_mm_shuffle_epi32(pair, 0x44), zero, 8);
				input += 8;
			}
			else if constexpr (Format == 5)
			{
				const __m128i pair = _mm_loadu_si32(input);
				value = _mm_shuffle_epi32(Unsigned ? _mm_cvtepu16_epi32(pair) : _mm_cvtepi16_epi32(pair), 0x44);
				input += 4;
			}
			else if constexpr (Format == 6)
			{
				u16 raw;
				std::memcpy(&raw, input, sizeof(raw));
				const __m128i pair = _mm_cvtsi32_si128(raw);
				value = _mm_shuffle_epi32(Unsigned ? _mm_cvtepu8_epi32(pair) : _mm_cvtepi8_epi32(pair), 0x44);
				input += 2;
			}
			else if constexpr (Format == 8)
			{
				value = _mm_loadu_si128(reinterpret_cast<const __m128i*>(input));
				if (iteration != 1)
					value = _mm_blend_epi32(value, zero, 8);
				input += 12;
			}
			else if constexpr (Format == 9)
			{
				const __m128i packed = _mm_loadl_epi64(reinterpret_cast<const __m128i*>(input));
				value = Unsigned ? _mm_cvtepu16_epi32(packed) : _mm_cvtepi16_epi32(packed);
				if (!(iteration & 1))
					value = _mm_blend_epi32(value, zero, 8);
				input += 6;
			}
			else
			{
				const __m128i packed = _mm_loadu_si32(input);
				value = Unsigned ? _mm_cvtepu8_epi32(packed) : _mm_cvtepi8_epi32(packed);
				if (iteration != 1)
					value = _mm_blend_epi32(value, zero, 8);
				input += 3;
			}
			_mm_storeu_si128(reinterpret_cast<__m128i*>(output), value);
			output += 16;
			if (++cl >= wl)
				cl = 0;
		}
	}

	// format is the low four bits of the UNPACK command (0..2, 4..6, 8..10).
	AVX2_TARGET void UnpackSV(void* dst, const void* source, u32 count, u32 format, bool is_unsigned, u32 cl, u32 wl)
	{
		auto* output = static_cast<u8*>(dst);
		const auto* input = static_cast<const u8*>(source);
#define SV_CASE(f) case f: if (is_unsigned) UnpackSVLoop<f, true>(output, input, count, cl, wl); else UnpackSVLoop<f, false>(output, input, count, cl, wl); break;
		switch (format)
		{
			SV_CASE(0) SV_CASE(1) SV_CASE(2)
			SV_CASE(4) SV_CASE(5) SV_CASE(6)
			SV_CASE(8) SV_CASE(9) SV_CASE(10)
		}
#undef SV_CASE
	}
} // namespace InterpreterAVX2
