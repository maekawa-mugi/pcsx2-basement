// SPDX-License-Identifier: GPL-3.0+
#include "pcsx2/x86/InterpreterAVX512.h"
#include <immintrin.h>
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <random>
#include "oracle.h"

namespace Reference
{
	struct macroblock_8
	{
		alignas(16) u8 Y[16][16];
		u8 Cb[8][8];
		u8 Cr[8][8];
	};
	struct pixel
	{
		u8 r, g, b, a;
	};
	struct macroblock_rgb32
	{
		alignas(16) pixel c[16][16];
	};
	static struct
	{
		macroblock_8 mb8;
		macroblock_rgb32 rgb32;
	} decoder;
#include "yuv-generators.h"
} // namespace Reference
static __attribute__((noinline)) void scalarPLZCW(void* dst, const void* source)
{
	auto* output = static_cast<u32*>(dst);
	const auto* input = static_cast<const u32*>(source);
	for (int lane = 0; lane < 2; lane++)
		output[lane] = std::countl_zero(input[lane] ^ (static_cast<s32>(input[lane]) >> 31)) - 1;
}
static __attribute__((noinline)) void scalarPNOR(void* dst, const void* source, const void* operand)
{
	auto* output = static_cast<u64*>(dst);
	const auto* a = static_cast<const u64*>(source);
	const auto* b = static_cast<const u64*>(operand);
	output[0] = ~(a[0] | b[0]);
	output[1] = ~(a[1] | b[1]);
}
static __attribute__((noinline)) void scalarQFSRV(void* dst, const void* source, const void* operand, u32 shift)
{
	const auto* a = static_cast<const u64*>(source);
	const auto* b = static_cast<const u64*>(operand);
	u64 temp[2];
	const u32 bits = shift * 8;
	if (!bits)
	{
		temp[0] = b[0];
		temp[1] = b[1];
	}
	else if (bits < 64)
	{
		temp[0] = (b[0] >> bits) | (b[1] << (64 - bits));
		temp[1] = (b[1] >> bits) | (a[0] << (64 - bits));
	}
	else if (bits == 64)
	{
		temp[0] = b[1];
		temp[1] = a[0];
	}
	else
	{
		temp[0] = (b[1] >> (bits - 64)) | (a[0] << (128 - bits));
		temp[1] = (a[0] >> (bits - 64)) | (a[1] << (128 - bits));
	}
	std::memcpy(dst, temp, 16);
}
static __attribute__((noinline)) void baselineYUV() { Reference::yuv2rgb_sse2(); }
static __attribute__((target("avx512f,avx512vl,avx512dq"))) u32 rangeClamp(u32 bits)
{
	const __m128 value = _mm_castsi128_ps(_mm_set1_epi32(bits));
	const __m128 limit = _mm_castsi128_ps(_mm_set1_epi32(0x7f7fffff));
	return _mm_cvtsi128_si32(_mm_castps_si128(_mm_range_ps(value, limit, 0x02)));
}
static __attribute__((noinline)) void scalarPEXT5(void* dst, const void* source)
{
	auto* output = static_cast<u32*>(dst);
	const auto* input = static_cast<const u32*>(source);
	for (int lane = 0; lane < 4; lane++)
	{
		u32 x = input[lane];
		output[lane] = ((x & 31) << 3) | ((x & 0x3e0) << 6) | ((x & 0x7c00) << 9) | ((x & 0x8000) << 16);
	}
}
static __attribute__((noinline)) void scalarPPAC5(void* dst, const void* source)
{
	auto* output = static_cast<u32*>(dst);
	const auto* input = static_cast<const u32*>(source);
	for (int lane = 0; lane < 4; lane++)
	{
		u32 x = input[lane];
		output[lane] = ((x >> 3) & 31) | ((x >> 6) & 0x3e0) | ((x >> 9) & 0x7c00) | ((x >> 16) & 0x8000);
	}
}
template <typename F>
static double measure(F fn, int count = 200000)
{
	std::array<double, 5> samples;
	for (auto& sample : samples)
	{
		auto start = std::chrono::steady_clock::now();
		for (int i = 0; i < count; i++)
			fn(i);
		sample = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count() / count;
	}
	std::sort(samples.begin(), samples.end());
	return samples[2];
}
int TestInterpreterLeaves()
{
	std::mt19937 rng(0x5900);
	for (u32 iteration = 0; iteration < 100000; iteration++)
	{
		alignas(16) u32 a[4], b[4], x[4], y[4];
		for (int lane = 0; lane < 4; lane++)
		{
			a[lane] = rng();
			b[lane] = rng();
		}
		if (iteration < 32)
			a[0] = 1u << iteration;
		if (iteration < 2)
			a[1] = iteration ? 0xffffffff : 0;
		for (auto funcs : {std::pair{&scalarPLZCW, &InterpreterAVX512::PLZCW}, std::pair{&scalarPEXT5, &InterpreterAVX512::PEXT5}, std::pair{&scalarPPAC5, &InterpreterAVX512::PPAC5}})
		{
			std::memcpy(x, a, 16);
			std::memcpy(y, a, 16);
			funcs.first(x, a);
			funcs.second(y, a);
			if (std::memcmp(x, y, 16))
				return 1;
			std::memcpy(y, a, 16);
			funcs.second(y, y);
			if (std::memcmp(x, y, 16))
				return 1;
		}
		scalarPNOR(x, a, b);
		InterpreterAVX512::PNOR(y, a, b);
		if (std::memcmp(x, y, 16))
		{
			std::puts("FAIL PNOR");
			return 1;
		}
		for (u32 shift = 0; shift < 16; shift++)
		{
			scalarQFSRV(x, a, b, shift);
			InterpreterAVX512::QFSRV(y, a, b, shift);
			if (std::memcmp(x, y, 16))
			{
				std::puts("FAIL QFSRV");
				return 1;
			}
			std::memcpy(y, a, 16);
			InterpreterAVX512::QFSRV(y, y, b, shift);
			if (std::memcmp(x, y, 16))
				return 1;
			std::memcpy(y, b, 16);
			InterpreterAVX512::QFSRV(y, a, y, shift);
			if (std::memcmp(x, y, 16))
				return 1;
		}
	}
	std::puts("PASS MMI: 100000 vectors, PLZCW high-half preservation, aliases, all QFSRV shifts");
	for (u32 bits : {1u, 0x80000001u, 0x7f800001u, 0x7fc00000u, 0xff800001u})
	{
		u32 expected = static_cast<s32>(bits) > 0x7f7fffff ? 0x7f7fffff : bits;
		expected = std::min(expected, 0xff7fffffu);
		u32 actual = rangeClamp(bits);
		const PS2Float add = PS2Float(bits).Add(PS2Float(0u));
		const PS2Float mul = PS2Float(bits).Mul(PS2Float::One());
		std::printf("VRANGE input=%08x legacy-clamp=%08x range=%08x PS2Float-add-zero=%08x(flags=%02x) PS2Float-mul-one=%08x(flags=%02x)\n",
			bits, expected, actual, add.raw, add.flags, mul.raw, mul.flags);
		// These are identity operations for PS2's extended finite exponent.
		// IEEE NaN rules cannot be used to decide their PS2 result.
		if ((bits & 0x7f800000u) == 0x7f800000u && (add.raw != bits || mul.raw != bits || add.flags || mul.flags))
			return 1;
	}
	for (u32 format : {0xcu, 0xdu, 0xeu, 0xfu})
		for (bool usn : {false, true})
			for (u32 count : {0u, 1u, 3u, 4u, 15u, 256u})
			{
				std::array<u8, 4096> input;
				std::array<u32, 1024> output{}, expected{};
				for (auto& byte : input)
					byte = rng();
				InterpreterAVX512::UnpackV4(output.data(), input.data(), count, format, usn);
				for (u32 i = 0; i < count * 4; i++)
				{
					if (format == 0xc)
						std::memcpy(&expected[i], input.data() + i * 4, 4);
					else if (format == 0xd)
					{
						u16 value;
						std::memcpy(&value, input.data() + i * 2, 2);
						expected[i] = usn ? value : static_cast<s16>(value);
					}
					else if (format == 0xe)
						expected[i] = usn ? input[i] : static_cast<s8>(input[i]);
					else
					{
						u16 value;
						std::memcpy(&value, input.data() + (i / 4) * 2, 2);
						expected[i] = (i % 4 == 3) ? ((value >> 15) << 7) : (((value >> ((i % 4) * 5)) & 31) << 3);
					}
				}
				if (output != expected)
				{
					std::puts("FAIL VIF");
					return 1;
				}
			}
	std::puts("PASS VIF: V4-32/V4-8/V4-16/V4-5, signed/unsigned, tails and zero count");
	alignas(64) Reference::macroblock_rgb32 pixels;
	for (int i = 0; i < 10000; i++)
	{
		auto& src = Reference::decoder.mb8;
		for (auto& row : src.Y)
			for (auto& byte : row)
				byte = rng();
		for (auto& row : src.Cb)
			for (auto& byte : row)
				byte = rng();
		for (auto& row : src.Cr)
			for (auto& byte : row)
				byte = rng();
		InterpreterAVX512::YUVToRGB(&src.Y[0][0], &src.Cb[0][0], &src.Cr[0][0], &pixels);
		Reference::yuv2rgb_reference();
		if (std::memcmp(&pixels, &Reference::decoder.rgb32, sizeof(pixels)))
		{
			std::puts("FAIL YUV/reference");
			return 1;
		}
		Reference::yuv2rgb_sse2();
		if (std::memcmp(&pixels, &Reference::decoder.rgb32, sizeof(pixels)))
		{
			std::puts("FAIL YUV/SSE2");
			return 1;
		}
	}
	std::puts("PASS IPU YUV: 10000 macroblocks against production scalar and SSE2");
	{
		// CSCThresholdSign against the scalar loops of ipu_csc, thresholds 0..511 (edges included), both sgn.
		alignas(64) u8 input[1024], got[1024], want[1024];
		for (int i = 0; i < 200000; i++)
		{
			const u32 edge[] = {0, 1, 2, 127, 128, 254, 255, 256, 257, 511};
			u32 th0 = (i & 1) ? rng() % 512 : edge[rng() % 10];
			u32 th1 = (i & 2) ? rng() % 512 : edge[rng() % 10];
			const bool sgn = i & 4;
			for (auto& byte : input)
				byte = (rng() % 3 == 0) ? static_cast<u8>(rng() % 3 + (th0 ? th0 - 1 : 0) + (rng() & 1) * (th1 ? th1 - 1 : 0)) : static_cast<u8>(rng());
			for (int px = 0; px < 256; px++)
				input[px * 4 + 3] = (rng() & 1) ? 0x80 : rng();
			std::memcpy(want, input, 1024);
			u8* p = want;
			for (int px = 0; px < 256; px++, p += 4)
			{
				if (th0 > 0)
				{
					if (p[0] < th0 && p[1] < th0 && p[2] < th0)
						*reinterpret_cast<u32*>(p) = 0;
					else if (p[0] < th1 && p[1] < th1 && p[2] < th1)
						p[3] = 0x40;
				}
				else if (th1 > 0 && p[0] < th1 && p[1] < th1 && p[2] < th1)
					p[3] = 0x40;
				if (sgn)
					*reinterpret_cast<u32*>(p) ^= 0x808080;
			}
			std::memcpy(got, input, 1024);
			InterpreterAVX512::CSCThresholdSign(got, th0, th1, sgn);
			if (std::memcmp(got, want, 1024))
			{
				std::printf("FAIL IPU CSC threshold th0=%u th1=%u sgn=%d\n", th0, th1, sgn);
				return 1;
			}
		}
		std::puts("PASS IPU CSC threshold/sign: 200000 macroblocks, thresholds 0..511, both sgn");
	}
	{
		// IPUDither against the scalar reference of ipu_dither_reference, and the intra u8->u16 expansion.
		static const int coeff[4][4] = {{-4, 0, -3, 1}, {2, -2, 3, -1}, {-3, 1, -4, 0}, {3, -1, 2, -2}};
		alignas(64) u8 in[1024];
		alignas(64) u16 got[256], want[256];
		for (int i = 0; i < 100000; i++)
		{
			for (auto& byte : in)
				byte = (rng() % 4 == 0) ? (rng() & 1 ? 255 - rng() % 6 : rng() % 6) : static_cast<u8>(rng());
			for (int px = 0; px < 256; px++)
				in[px * 4 + 3] = (rng() & 1) ? 0x40 : static_cast<u8>(rng());
			const bool dte = i & 1;
			for (int y = 0; y < 16; y++)
				for (int x = 0; x < 16; x++)
				{
					const u8* p = in + (y * 16 + x) * 4;
					const int d = dte ? coeff[y & 3][x & 3] : 0;
					const int r = std::max(0, std::min(p[0] + d, 255)) >> 3;
					const int g = std::max(0, std::min(p[1] + d, 255)) >> 3;
					const int b = std::max(0, std::min(p[2] + d, 255)) >> 3;
					want[y * 16 + x] = static_cast<u16>(r | g << 5 | b << 10 | (p[3] == 0x40) << 15);
				}
			InterpreterAVX512::IPUDither(in, got, dte);
			if (std::memcmp(got, want, sizeof(got)))
			{
				std::printf("FAIL IPU dither dte=%d\n", dte);
				return 1;
			}
		}
		std::puts("PASS IPU dither: 100000 macroblocks against scalar reference, dte on/off");
		alignas(64) u16 expanded[384];
		for (int i = 0; i < 1000; i++)
		{
			for (int k = 0; k < 384; k++)
				in[k] = static_cast<u8>(rng());
			InterpreterAVX512::IPUExpandU8ToU16(in, expanded, 384);
			for (int k = 0; k < 384; k++)
				if (expanded[k] != in[k])
				{
					std::puts("FAIL IPU intra expand");
					return 1;
				}
		}
		std::puts("PASS IPU intra u8->u16 expand");
	}
	alignas(16) u32 a[4] = {0x7fffabcd, 0xffffffff, 0x11223344, 0x12345678}, b[4] = {1, 2, 3, 4}, dst[4];
	for (auto spec : {std::pair{"PLZCW", std::pair{&scalarPLZCW, &InterpreterAVX512::PLZCW}}, std::pair{"PEXT5", std::pair{&scalarPEXT5, &InterpreterAVX512::PEXT5}}, std::pair{"PPAC5", std::pair{&scalarPPAC5, &InterpreterAVX512::PPAC5}}})
	{
		double base = measure([&](int) { spec.second.first(dst, a); });
		double fast = measure([&](int) { spec.second.second(dst, a); });
		std::printf("BENCH %s AVX2 %.3f AVX512 %.3f ns/call speedup %.3f\n", spec.first, base, fast, base / fast);
	}
	double base = measure([&](int) { scalarPNOR(dst, a, b); });
	double fast = measure([&](int) { InterpreterAVX512::PNOR(dst, a, b); });
	std::printf("BENCH PNOR AVX2 %.3f AVX512 %.3f ns/call speedup %.3f\n", base, fast, base / fast);
	base = measure([&](int i) { scalarQFSRV(dst, a, b, i & 15); });
	fast = measure([&](int i) { InterpreterAVX512::QFSRV(dst, a, b, i & 15); });
	std::printf("BENCH QFSRV AVX2 %.3f AVX512 %.3f ns/call speedup %.3f\n", base, fast, base / fast);
	base = measure([&](int) { baselineYUV(); }, 50000);
	auto& src = Reference::decoder.mb8;
	fast = measure([&](int) { InterpreterAVX512::YUVToRGB(&src.Y[0][0], &src.Cb[0][0], &src.Cr[0][0], &pixels); }, 50000);
	std::printf("BENCH IPU YUV SSE2 %.3f AVX512 %.3f ns/macroblock speedup %.3f\n", base, fast, base / fast);
	return 0;
}

// AVX2 flavours of the IPU / VIF leaves, checked against the same scalar references as the AVX-512 ones.
int TestAVX2Leaves()
{
	std::mt19937 rng(0xa2);
	for (u32 format : {0xcu, 0xdu, 0xeu})
		for (bool usn : {false, true})
			for (u32 count : {0u, 1u, 2u, 3u, 4u, 5u, 15u, 256u})
			{
				std::array<u8, 4096> input;
				std::array<u32, 1024> output{}, expected{};
				for (auto& byte : input)
					byte = rng();
				InterpreterAVX2::UnpackV4(output.data(), input.data(), count, format, usn);
				for (u32 i = 0; i < count * 4; i++)
				{
					if (format == 0xc)
						std::memcpy(&expected[i], input.data() + i * 4, 4);
					else if (format == 0xd)
					{
						u16 value;
						std::memcpy(&value, input.data() + i * 2, 2);
						expected[i] = usn ? value : static_cast<s16>(value);
					}
					else
						expected[i] = usn ? input[i] : static_cast<s8>(input[i]);
				}
				if (output != expected)
				{
					std::printf("FAIL VIF AVX2 format=%x usn=%d count=%u\n", format, usn, count);
					return 1;
				}
			}
	std::puts("PASS VIF AVX2: V4-32/V4-8/V4-16, signed/unsigned, odd counts and tails");
	{
		alignas(64) u8 input[1024], got[1024], want[1024];
		for (int i = 0; i < 200000; i++)
		{
			const u32 edge[] = {0, 1, 2, 127, 128, 254, 255, 256, 257, 511};
			u32 th0 = (i & 1) ? rng() % 512 : edge[rng() % 10];
			u32 th1 = (i & 2) ? rng() % 512 : edge[rng() % 10];
			const bool sgn = i & 4;
			for (auto& byte : input)
				byte = (rng() % 3 == 0) ? static_cast<u8>(rng() % 3 + (th0 ? th0 - 1 : 0) + (rng() & 1) * (th1 ? th1 - 1 : 0)) : static_cast<u8>(rng());
			for (int px = 0; px < 256; px++)
				input[px * 4 + 3] = (rng() & 1) ? 0x80 : rng();
			std::memcpy(want, input, 1024);
			u8* p = want;
			for (int px = 0; px < 256; px++, p += 4)
			{
				if (th0 > 0)
				{
					if (p[0] < th0 && p[1] < th0 && p[2] < th0)
						*reinterpret_cast<u32*>(p) = 0;
					else if (p[0] < th1 && p[1] < th1 && p[2] < th1)
						p[3] = 0x40;
				}
				else if (th1 > 0 && p[0] < th1 && p[1] < th1 && p[2] < th1)
					p[3] = 0x40;
				if (sgn)
					*reinterpret_cast<u32*>(p) ^= 0x808080;
			}
			std::memcpy(got, input, 1024);
			InterpreterAVX2::CSCThresholdSign(got, th0, th1, sgn);
			if (std::memcmp(got, want, 1024))
			{
				std::printf("FAIL IPU CSC AVX2 th0=%u th1=%u sgn=%d\n", th0, th1, sgn);
				return 1;
			}
		}
		std::puts("PASS IPU CSC threshold/sign AVX2: 200000 macroblocks, thresholds 0..511, both sgn");
	}
	{
		alignas(64) u8 in[384];
		alignas(64) u16 expanded[384];
		for (int i = 0; i < 1000; i++)
		{
			for (auto& byte : in)
				byte = static_cast<u8>(rng());
			InterpreterAVX2::IPUExpandU8ToU16(in, expanded, 384);
			for (int k = 0; k < 384; k++)
				if (expanded[k] != in[k])
				{
					std::puts("FAIL IPU intra expand AVX2");
					return 1;
				}
		}
		std::puts("PASS IPU intra u8->u16 expand AVX2");
	}
	{
		// VQ against the scalar nearest-colour search (lowest index on ties). rgb16 = r | g << 5 | b << 10 | a << 15.
		alignas(32) u16 clut[16];
		alignas(32) u16 pixels[256];
		u8 got[128], want[128];
		for (int iter = 0; iter < 20000; iter++)
		{
			for (auto& c : clut)
				c = static_cast<u16>(rng());
			if (iter & 1) // force duplicate entries so tie-breaking matters
				clut[rng() % 16] = clut[rng() % 16];
			for (auto& px : pixels)
				px = (rng() & 3) ? static_cast<u16>(rng()) : clut[rng() % 16];
			const auto closest = [&](u16 px) {
				u8 index = 0;
				int best = 0x7fffffff;
				for (u8 k = 0; k < 16; k++)
				{
					const int dr = (px & 31) - (clut[k] & 31);
					const int dg = ((px >> 5) & 31) - ((clut[k] >> 5) & 31);
					const int db = ((px >> 10) & 31) - ((clut[k] >> 10) & 31);
					const int d = dr * dr + dg * dg + db * db;
					if (best > d)
					{
						best = d;
						index = k;
					}
				}
				return index;
			};
			for (int n = 0; n < 128; n++)
				want[n] = static_cast<u8>(closest(pixels[n * 2 + 1]) << 4 | closest(pixels[n * 2]));
			InterpreterAVX2::VQ(pixels, clut, got);
			if (std::memcmp(got, want, 128))
			{
				std::puts("FAIL IPU VQ AVX2");
				return 1;
			}
		}
		std::puts("PASS IPU VQ AVX2: 20000 blocks incl. duplicate CLUT entries");
	}
	{
		// AVX2 YUV->RGB against the production reference and SSE2 kernels, AVX2 dither against the scalar reference.
		alignas(64) Reference::macroblock_rgb32 pixels;
		auto& src = Reference::decoder.mb8;
		for (int i = 0; i < 10000; i++)
		{
			for (auto& row : src.Y)
				for (auto& byte : row)
					byte = rng();
			for (auto& row : src.Cb)
				for (auto& byte : row)
					byte = rng();
			for (auto& row : src.Cr)
				for (auto& byte : row)
					byte = rng();
			InterpreterAVX2::YUVToRGB(&src.Y[0][0], &src.Cb[0][0], &src.Cr[0][0], &pixels);
			Reference::yuv2rgb_reference();
			if (std::memcmp(&pixels, &Reference::decoder.rgb32, sizeof(pixels)))
			{
				std::puts("FAIL YUV AVX2/reference");
				return 1;
			}
		}
		std::puts("PASS IPU YUV AVX2: 10000 macroblocks against production scalar");
		double base = measure([&](int) { baselineYUV(); }, 50000);
		double fast = measure([&](int) { InterpreterAVX2::YUVToRGB(&src.Y[0][0], &src.Cb[0][0], &src.Cr[0][0], &pixels); }, 50000);
		std::printf("BENCH IPU YUV SSE2 %.3f AVX2 %.3f ns/macroblock speedup %.3f\n", base, fast, base / fast);

		static const int coeff[4][4] = {{-4, 0, -3, 1}, {2, -2, 3, -1}, {-3, 1, -4, 0}, {3, -1, 2, -2}};
		alignas(64) u8 in[1024];
		alignas(64) u16 got[256], want[256];
		for (int i = 0; i < 100000; i++)
		{
			for (auto& byte : in)
				byte = (rng() % 4 == 0) ? (rng() & 1 ? 255 - rng() % 6 : rng() % 6) : static_cast<u8>(rng());
			for (int px = 0; px < 256; px++)
				in[px * 4 + 3] = (rng() & 1) ? 0x40 : static_cast<u8>(rng());
			const bool dte = i & 1;
			for (int y = 0; y < 16; y++)
				for (int x = 0; x < 16; x++)
				{
					const u8* p = in + (y * 16 + x) * 4;
					const int d = dte ? coeff[y & 3][x & 3] : 0;
					const int r = std::max(0, std::min(p[0] + d, 255)) >> 3;
					const int g = std::max(0, std::min(p[1] + d, 255)) >> 3;
					const int b = std::max(0, std::min(p[2] + d, 255)) >> 3;
					want[y * 16 + x] = static_cast<u16>(r | g << 5 | b << 10 | (p[3] == 0x40) << 15);
				}
			InterpreterAVX2::IPUDither(in, got, dte);
			if (std::memcmp(got, want, sizeof(got)))
			{
				std::printf("FAIL IPU dither AVX2 dte=%d\n", dte);
				return 1;
			}
		}
		std::puts("PASS IPU dither AVX2: 100000 macroblocks against scalar reference, dte on/off");
	}
	{
		// UnpackSV against a model of the generic SSE unpackers (work register loaded at cl == 0, IsAligned == true,
		// iteration = min(cl, 3)). S needs wl <= 4 and V2 wl <= 2 from cl == 0 over whole cycles; V3 takes any phase.
		const auto ext = [](const u8* p, int bytes, bool usn) -> u32 {
			if (bytes == 4)
			{
				u32 v;
				std::memcpy(&v, p, 4);
				return v;
			}
			if (bytes == 2)
			{
				u16 v;
				std::memcpy(&v, p, 2);
				return usn ? v : static_cast<u32>(static_cast<s32>(static_cast<s16>(v)));
			}
			return usn ? *p : static_cast<u32>(static_cast<s32>(static_cast<s8>(*p)));
		};
		static const u32 vsize[11] = {4, 2, 1, 0, 8, 4, 2, 0, 12, 6, 3};
		u64 checked = 0;
		for (u32 format : {0u, 1u, 2u, 4u, 5u, 6u, 8u, 9u, 10u})
			for (bool usn : {false, true})
				for (u32 wl = 1; wl <= 6; wl++)
					for (u32 cl0 = 0; cl0 < wl; cl0++)
					{
						const bool v3 = format >= 8;
						if (!v3 && (cl0 != 0 || wl > (format < 4 ? 4u : 2u)))
							continue;
						for (int iter = 0; iter < 400; iter++)
						{
							u32 count = 1 + rng() % 40;
							if (!v3)
								count = (1 + rng() % 12) * wl;
							std::array<u8, 1024> input;
							for (auto& byte : input)
								byte = rng();
							std::array<u32, 40 * 4> got{}, want{};
							InterpreterAVX2::UnpackSV(got.data(), input.data(), count, format, usn, cl0, wl);
							const u8* p = input.data();
							u32 work[4] = {};
							u32 cl = cl0;
							for (u32 k = 0; k < count; k++)
							{
								const u32 it = std::min(cl, 3u);
								u32* out = &want[k * 4];
								const int elem = format % 4 == 0 ? 4 : (format % 4 == 1 ? 2 : 1);
								if (format < 8 && it == 0)
									for (int i = 0; i < 4; i++)
										work[i] = ext(p + i * elem, elem, usn);
								if (format < 4)
									out[0] = out[1] = out[2] = out[3] = work[it];
								else if (format < 8)
								{
									const u32 base = it == 0 ? 0 : 2;
									out[0] = work[base];
									out[1] = work[base + 1];
									out[2] = work[base];
									out[3] = elem == 4 ? 0 : work[base + 1];
								}
								else
								{
									for (int i = 0; i < 4; i++)
										out[i] = ext(p + i * elem, elem, usn);
									const bool keep = format == 9 ? (it & 1) : it == 1;
									if (!keep)
										out[3] = 0;
								}
								p += vsize[format];
								if (++cl >= wl)
									cl = 0;
							}
							if (got != want)
							{
								std::printf("FAIL VIF UnpackSV format=%u usn=%d wl=%u cl0=%u count=%u\n", format, usn, wl, cl0, count);
								return 1;
							}
							checked++;
						}
					}
		std::printf("PASS VIF UnpackSV AVX2: S/V2/V3 x 8/16/32, signed/unsigned, wl 1..6, %llu runs against the generic-unpacker model\n", static_cast<unsigned long long>(checked));
	}
	return 0;
}
