// SPDX-License-Identifier: GPL-3.0+
#include "common/emitter/x86emitter.h"
#include "common/FPControl.h"
#include "common/Console.h"
#include "pcsx2/x86/SoftFloatEmitter.h"
#include "pcsx2/x86/InterpreterAVX512.h"
#include "pcsx2/x86/microVU_SoftFloatTables.h"
#include <array>
#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <random>
#include <vector>
#include <sys/mman.h>
#include "oracle.h"

using namespace x86Emitter;
void pxOnAssertFail(const char* file, int line, const char*, const char* message)
{
	std::fprintf(stderr, "%s:%d: %s\n", file, line, message);
	std::abort();
}
namespace Log
{
	void Writev(LOGLEVEL, ConsoleColors, const char* format, va_list args) { std::vfprintf(stderr, format, args); }
} // namespace Log
ConsoleLogWriter<LOGLEVEL_INFO> Console;
struct
{
	struct
	{
		FPControlRegister FPUFPCR{0x7f80}, VU0FPCR{0xffc0}, VU1FPCR{0xffc0};
	} Cpu;
} EmuConfig;
struct
{
	alignas(16) u32 absclip[4] = {0x7fffffff, 0x7fffffff, 0x7fffffff, 0x7fffffff};
} mVUglob;
namespace ProcessorFeatures
{
	enum class VectorISA { None, SSE4, AVX, AVX2 };
}
struct
{
	bool hasFastPext = true;
	ProcessorFeatures::VectorISA vectorISA = ProcessorFeatures::VectorISA::AVX2;
} g_cpu;
#define CHECK_FPU_SOFT true
#define CHECK_VU_SOFT(index) true

extern "C" size_t ZSTD_decompress(void*, size_t, const void*, size_t);
namespace MicroVUSoftFloatTables
{
	const std::uint64_t* sqrt_correction_lookup;
	const std::uint8_t* first_one_correction_lookup;
	const std::uint64_t* reciprocal_correction_lookup;
	void InitializeCorrectionTables()
	{
		static SqrtCorrectionTable sqrt;
		static FirstOneCorrectionTable first;
		static ReciprocalCorrectionTable reciprocal;
		static bool initialized = false;
		if (!initialized)
		{
			if (ZSTD_decompress(sqrt.data(), sizeof(sqrt), compressed_sqrt_correction_table, compressed_sqrt_correction_table_size) != sizeof(sqrt) ||
				ZSTD_decompress(first.data(), sizeof(first), compressed_first_one_correction_table, compressed_first_one_correction_table_size) != sizeof(first) ||
				ZSTD_decompress(reciprocal.data(), sizeof(reciprocal), compressed_reciprocal_correction_table, compressed_reciprocal_correction_table_size) != sizeof(reciprocal))
				std::abort();
			initialized = true;
		}
		sqrt_correction_lookup = sqrt.data();
		first_one_correction_lookup = first.data();
		reciprocal_correction_lookup = reciprocal.data();
	}
} // namespace MicroVUSoftFloatTables
#include "generators.h"
#include "pcsx2/x86/MMIAVX512.inl"
#include "pcsx2/x86/VUMinMaxAVX512.h"

template <typename F>
static std::array<double, 2> benchmarkPair(F fn)
{
	// Warm up and alternate order so short kernels do not measure clock ramping.
	for (int i = 0; i < 1000000; i++)
	{
		fn(0);
		fn(1);
	}
	std::array<std::array<double, 9>, 2> samples;
	for (int round = 0; round < 9; round++)
		for (int order = 0; order < 2; order++)
		{
			const int backend = (round + order) & 1;
			auto start = std::chrono::steady_clock::now();
			for (int i = 0; i < 2000000; i++)
				fn(backend);
			samples[backend][round] = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count() / 2000000;
		}
	for (auto& sample : samples)
		std::sort(sample.begin(), sample.end());
	return {samples[0][4], samples[1][4]};
}

// 3 normally (SSE4 / AVX2 / AVX-512); 2 with AVX512_VALIDATION_AVX2_ONLY=1 so the AVX2 round can run on a non-AVX-512 host.
static int g_backends = 3;

namespace MMIGenerator
{
	static int EEREC_D, EEREC_S, EEREC_T, EEREC_LO, EEREC_HI, temporary, Rs_value, Rt_value, Rd_value;
#define _Rd_ Rd_value
#define _Rs_ Rs_value
#define _Rt_ Rt_value
	struct XmmRegStub { int inuse; };
	static XmmRegStub xmmregs[32];
	static bool CanUse3Arg(u32 d, u32 s, u32 t) { return d == s || d != t || x86Emitter::use_avx; }
	static int _allocTempXMMreg(int) { return temporary--; }
	static void _freeXMMreg(int) {}
	static void _clearNeededXMMregs() {}
	static constexpr int XMMT_INT = 0;
	template <typename Op>
	static void ThreeArg(const Op& op, u32 d, u32 s, u32 t)
	{
		if (CanUse3Arg(d, s, t))
			op(xRegisterSSE(d), xRegisterSSE(s), xRegisterSSE(t));
		else
		{
			int t0reg = _allocTempXMMreg(XMMT_INT);
			xMOVDQA(xRegisterSSE(t0reg), xRegisterSSE(t));
			xMOVDQA(xRegisterSSE(d), xRegisterSSE(s));
			op(xRegisterSSE(d), xRegisterSSE(t0reg));
			_freeXMMreg(t0reg);
		}
	}
#include "mmi-generators.h"
#undef _Rd_
#undef _Rs_
#undef _Rt_
} // namespace MMIGenerator

static int testProductionMMIJit()
{
	using Kernel = void (*)(const u32*, const u32*, u32*);
	const std::array generators = {MMIGenerator::recPEXT5, MMIGenerator::recPPAC5,
		MMIGenerator::recPADDSW, MMIGenerator::recPSUBSW, MMIGenerator::recPABSW, MMIGenerator::recPABSH,
		MMIGenerator::recPPACW, MMIGenerator::recPPACH, MMIGenerator::recPPACB,
		MMIGenerator::recPADDUW, MMIGenerator::recPSUBUW, MMIGenerator::recPSLLVW, MMIGenerator::recPSRLVW, MMIGenerator::recPSRAVW,
		MMIGenerator::recPEXEH, MMIGenerator::recPREVH, MMIGenerator::recPEXCH, MMIGenerator::recPCPYH};
	const char* names[] = {"PEXT5", "PPAC5", "PADDSW", "PSUBSW", "PABSW", "PABSH", "PPACW", "PPACH", "PPACB",
		"PADDUW", "PSUBUW", "PSLLVW", "PSRLVW", "PSRAVW", "PEXEH", "PREVH", "PEXCH", "PCPYH"};
	std::mt19937 rng(0x5900512);
	const u32 edges[] = {0, 1, 0xffffffffu, 0x7fffffffu, 0x80000000u, 0x80008000u, 0x7fff7fffu};
	for (size_t op = 0; op < generators.size(); op++)
	for (int rsZero = (op == 6 ? 1 : 0); rsZero < (op >= 6 && op <= 8 ? 2 : 1); rsZero++) // PPACW general form swaps registers instead of writing Rd
		for (auto ids : {std::array{0, 1, 2}, std::array{0, 1, 0}, std::array{0, 1, 1}, std::array{0, 0, 2}, std::array{0, 0, 0}})
		{
			MMIGenerator::Rs_value = rsZero ? 0 : 1;
			MMIGenerator::Rt_value = 1;
			std::array<Kernel, 3> kernels; // 0: SSE4 (no VEX), 1: AVX2, 2: AVX-512
			for (int backend = 0; backend < g_backends; backend++)
			{
				const auto features = avx512;
				const bool saved_avx = use_avx;
				if (backend < 2)
					avx512 = {};
				use_avx = backend != 0;
				xAlignPtr(64);
				kernels[backend] = reinterpret_cast<Kernel>(xGetAlignedCallTarget());
				xMOVDQU(xRegisterSSE(ids[0]), ptr[rdi]);
				if (ids[1] != ids[0])
					xMOVDQU(xRegisterSSE(ids[1]), ptr[rsi]);
				MMIGenerator::EEREC_S = ids[0];
				MMIGenerator::EEREC_T = ids[1];
				MMIGenerator::EEREC_D = ids[2];
				MMIGenerator::temporary = 15;
				generators[op]();
				xMOVDQU(ptr[rdx], xRegisterSSE(ids[2]));
				xRET();
				avx512 = features;
				use_avx = saved_avx;
			}
			for (u32 i = 0; i < 100000; i++)
			{
				alignas(16) u32 a[4], b[4], expected[4], actual[4];
				for (int lane = 0; lane < 4; lane++)
				{
					a[lane] = i < std::size(edges) * std::size(edges) ? edges[i % std::size(edges)] : rng();
					b[lane] = i < std::size(edges) * std::size(edges) ? edges[i / std::size(edges)] : rng();
					if (ids[0] == ids[1])
						b[lane] = a[lane];
					const u32 value = b[lane];
					if (op == 0)
						expected[lane] = ((value & 31) << 3) | ((value & 0x3e0) << 6) | ((value & 0x7c00) << 9) | ((value & 0x8000) << 16);
					else if (op == 1)
						expected[lane] = ((value >> 3) & 31) | ((value >> 6) & 0x3e0) | ((value >> 9) & 0x7c00) | ((value >> 16) & 0x8000);
					else if (op == 2 || op == 3)
					{
						const s64 x = static_cast<s32>(a[lane]), y = static_cast<s32>(b[lane]);
						expected[lane] = static_cast<u32>(std::clamp<s64>(op == 2 ? x + y : x - y, -2147483648LL, 2147483647LL));
					}
					else if (op == 4)
					{
						const s64 x = static_cast<s32>(value);
						expected[lane] = static_cast<u32>(std::min<s64>(x < 0 ? -x : x, 2147483647LL));
					}
					else if (op == 5)
					{
						expected[lane] = 0;
						for (int half = 0; half < 2; half++)
						{
							const s32 x = static_cast<s16>(value >> (half * 16));
							expected[lane] |= static_cast<u32>(std::min(x < 0 ? -x : x, 32767)) << (half * 16);
						}
					}
				}
				if (op >= 14)
				{
					// Fixed halfword permutation (PSHUFLW/PSHUFHW with the same imm8) of Rt.
					const u8 imm = op == 14 ? 0xc6 : op == 15 ? 0x1b : op == 16 ? 0xd8 : 0x00;
					u16 src[8], dest[8];
					std::memcpy(src, b, 16);
					for (int q = 0; q < 2; q++)
						for (int h = 0; h < 4; h++)
							dest[q * 4 + h] = src[q * 4 + ((imm >> (h * 2)) & 3)];
					std::memcpy(expected, dest, 16);
				}
				else if (op >= 9)
				{
					for (int lane = 0; lane < 4; lane++)
						expected[lane] = 0;
					if (op == 9 || op == 10)
						for (int lane = 0; lane < 4; lane++)
						{
							const u64 x = a[lane], y = b[lane];
							expected[lane] = op == 9 ? static_cast<u32>(std::min<u64>(x + y, 0xffffffffull)) : static_cast<u32>(x > y ? x - y : 0);
						}
					else
						for (int q = 0; q < 2; q++)
						{
							const u32 count = a[q * 2] & 31, v = b[q * 2];
							const u32 r = op == 11 ? v << count : op == 12 ? v >> count : static_cast<u32>(static_cast<s32>(v) >> count);
							const s64 ext = static_cast<s32>(r);
							std::memcpy(expected + q * 2, &ext, 8);
						}
				}
				else if (op >= 6)
				{
					// PPACW/H/B: even elements of Rt fill the low 64 bits, even elements of Rs the high 64.
					// S = a (zero if $0), T = b.
					u32 s[4], t[4];
					for (int lane = 0; lane < 4; lane++)
					{
						s[lane] = rsZero ? 0 : a[lane];
						t[lane] = b[lane];
					}
					std::memset(expected, 0, 16);
					const int elemBytes = op == 6 ? 4 : (op == 7 ? 2 : 1);
					const int count = 8 / elemBytes; // elements per half
					for (int half = 0; half < 2; half++)
					{
						const u8* src = reinterpret_cast<const u8*>(half ? s : t);
						u8* dest = reinterpret_cast<u8*>(expected) + half * 8;
						for (int e = 0; e < count; e++)
							std::memcpy(dest + e * elemBytes, src + e * 2 * elemBytes, elemBytes);
					}
				}
				for (int backend = 0; backend < g_backends; backend++)
				{
					kernels[backend](a, b, actual);
					if (std::memcmp(actual, expected, 16))
					{
						std::printf("FAIL production MMI op=%zu ids=%d/%d/%d iteration=%u\n", op, ids[0], ids[1], ids[2], i);
						return 1;
					}
				}
			}
			if (g_backends == 3 && ids == std::array{0, 1, 2})
			{
				alignas(16) u32 a[4] = {0x7fffffffu, 0x80000000u, 0x80008000u, 0xffffffffu};
				alignas(16) u32 b[4] = {1, 0xffffffffu, 0x7fff7fffu, 1}, output[4];
				const auto times = benchmarkPair([&](int backend) { kernels[backend + 1](a, b, output); });
				std::printf("BENCH JIT %s AVX2 %.3f AVX512 %.3f ns/call (paired median 9) speedup %.3f\n", names[op], times[0], times[1], times[0] / times[1]);
			}
		}
	std::puts("PASS production MMI: PEXT5/PPAC5/PADDSW/PSUBSW/PABSW/PABSH/PPACW/PPACH/PPACB/PEXEH/PREVH/PEXCH/PCPYH, 100000 inputs, all aliases, AVX2/AVX512 vs scalar oracle");
	return 0;
}

// PMULTH / PMADDH / PMSUBH: S, T halfwords -> 32-bit products; LO = {p0,p1,p4,p5}, HI = {p2,p3,p6,p7},
// Rd = {p0,p2,p4,p6} (after accumulation for MADD/MSUB). Tested on SSE4 / AVX2 / AVX-512, with and without Rd.
static int testHalfwordMultiply()
{
	using Kernel = void (*)(const u32*, const u32*, const u32*, const u32*, u32*);
	const std::array generators = {MMIGenerator::recPMULTH, MMIGenerator::recPMADDH, MMIGenerator::recPMSUBH, MMIGenerator::recPHMSBH};
	const char* names[] = {"PMULTH", "PMADDH", "PMSUBH", "PHMSBH"};
	std::mt19937 rng(0x48414c46);
	for (size_t op = 0; op < generators.size(); op++)
		for (int withRd = 0; withRd < 2; withRd++)
			for (int sameST = 0; sameST < 2; sameST++)
			{
				std::array<Kernel, 3> kernels;
				for (int backend = 0; backend < g_backends; backend++)
				{
					const auto features = avx512;
					const bool saved_avx = use_avx;
					if (backend < 2)
						avx512 = {};
					use_avx = backend != 0;
					xAlignPtr(64);
					kernels[backend] = reinterpret_cast<Kernel>(xGetAlignedCallTarget());
					xMOVDQU(xRegisterSSE(1), ptr[rdi]);
					if (sameST)
						xMOVDQA(xRegisterSSE(2), xRegisterSSE(1));
					else
						xMOVDQU(xRegisterSSE(2), ptr[rsi]);
					xMOVDQU(xRegisterSSE(3), ptr[rdx]);
					xMOVDQU(xRegisterSSE(4), ptr[rcx]);
					MMIGenerator::EEREC_S = 1;
					MMIGenerator::EEREC_T = sameST ? 1 : 2;
					MMIGenerator::EEREC_LO = 3;
					MMIGenerator::EEREC_HI = 4;
					MMIGenerator::EEREC_D = 5;
					MMIGenerator::Rd_value = withRd;
					MMIGenerator::temporary = 15;
					generators[op]();
					xMOVDQU(ptr[r8], xRegisterSSE(5));
					xMOVDQU(ptr[r8 + 16], xRegisterSSE(3));
					xMOVDQU(ptr[r8 + 32], xRegisterSSE(4));
					xRET();
					avx512 = features;
					use_avx = saved_avx;
				}
				for (u32 i = 0; i < 100000; i++)
				{
					alignas(16) u32 s[4], t[4], lo[4], hi[4], actual[12], expected[12] = {};
					for (int k = 0; k < 4; k++)
					{
						s[k] = i < 16 ? (i & 1 ? 0x80008000u : 0x7fff7fffu) : rng();
						t[k] = i < 16 ? (i & 2 ? 0x80008000u : 0x7fff7fffu) : rng();
						lo[k] = rng();
						hi[k] = rng();
					}
					if (sameST)
						std::memcpy(t, s, 16);
					s32 p[8];
					for (int h = 0; h < 8; h++)
						p[h] = static_cast<s32>(static_cast<s16>(reinterpret_cast<const u16*>(s)[h])) * static_cast<s32>(static_cast<s16>(reinterpret_cast<const u16*>(t)[h]));
					const int loIdx[4] = {0, 1, 4, 5}, hiIdx[4] = {2, 3, 6, 7};
					u32 rlo[4], rhi[4];
					for (int k = 0; k < 4; k++)
					{
						const u32 a = static_cast<u32>(p[loIdx[k]]), b = static_cast<u32>(p[hiIdx[k]]);
						rlo[k] = op == 0 ? a : op == 1 ? lo[k] + a : lo[k] - a;
						rhi[k] = op == 0 ? b : op == 1 ? hi[k] + b : hi[k] - b;
					}
					std::memcpy(expected + 4, rlo, 16);
					std::memcpy(expected + 8, rhi, 16);
					if (withRd)
					{
						expected[0] = rlo[0];
						expected[1] = rhi[0];
						expected[2] = rlo[2];
						expected[3] = rhi[2];
					}
						if (op == 3)
						{
							// PHMSBH: per dword i, diff_i = p[2i+1] - p[2i]; the undocumented upper word is ~p[2i+1].
							// LO = {d0,n0,d2,n2}, HI = {d1,n1,d3,n3}, Rd = {d0,d1,d2,d3}.
							u32 d[4], n[4];
							for (int k = 0; k < 4; k++)
							{
								d[k] = static_cast<u32>(p[2 * k + 1]) - static_cast<u32>(p[2 * k]);
								n[k] = ~static_cast<u32>(p[2 * k + 1]);
							}
							const u32 elo[4] = {d[0], n[0], d[2], n[2]}, ehi[4] = {d[1], n[1], d[3], n[3]};
							std::memcpy(expected + 4, elo, 16);
							std::memcpy(expected + 8, ehi, 16);
							if (withRd)
								std::memcpy(expected, d, 16);
						}
					for (int backend = 0; backend < g_backends; backend++)
					{
						std::memset(actual, 0, sizeof(actual));
						kernels[backend](s, t, lo, hi, actual);
						if (std::memcmp(actual + 4, expected + 4, 32) || (withRd && std::memcmp(actual, expected, 16)))
						{
							std::printf("FAIL %s withRd=%d sameST=%d backend=%d iteration=%u\n", names[op], withRd, sameST, backend, i);
							return 1;
						}
					}
				}
				if (g_backends == 3 && withRd && !sameST)
				{
					alignas(16) u32 s[4] = {0x7fff8001u, 0x12345678u, 0x80008000u, 0xffff0001u}, t[4] = {0x00027fffu, 0x9abcdef0u, 0x7fff7fffu, 0x00010001u}, lo[4] = {1, 2, 3, 4}, hi[4] = {5, 6, 7, 8}, out[12];
					const auto sse_avx2 = benchmarkPair([&](int backend) { kernels[backend](s, t, lo, hi, out); });
					const auto avx2_512 = benchmarkPair([&](int backend) { kernels[backend + 1](s, t, lo, hi, out); });
					std::printf("BENCH10 %s withRd SSE4 %.3f AVX2 %.3f AVX512 %.3f ns/call (SSE4/AVX2 %.3f, AVX2/AVX512 %.3f)\n", names[op], sse_avx2[0], sse_avx2[1], avx2_512[1], sse_avx2[0] / sse_avx2[1], avx2_512[0] / avx2_512[1]);
				}
			}
	std::puts("PASS PMULTH/PMADDH/PMSUBH/PHMSBH: SSE4 / AVX2 / AVX-512 vs scalar reference, with and without Rd, 100000 inputs");
	return 0;
}

static int testVUMinMax()
{
	using Kernel = void (*)(const u32*, const u32*, u32*);
	std::mt19937 rng(0x6d1a);
	const u32 edges[] = {0, 0x80000000u, 1, 0x80000001u, 0x7f7fffffu, 0x7f800000u, 0x7fffffffu, 0xff7fffffu,
		0xff800000u, 0xffffffffu, 0x3f800000u, 0xbf800000u};
	for (int min = 0; min < 2; min++)
	{
		xAlignPtr(64);
		const auto kernel = reinterpret_cast<Kernel>(xGetAlignedCallTarget());
		xMOVDQU(xmm0, ptr[rdi]);
		xMOVDQU(xmm1, ptr[rsi]);
		emitVUMinMaxAVX512(xmm0, xmm1, xmm2, xmm3, min);
		xMOVDQU(ptr[rdx], xmm0);
		xRET();
		for (u32 i = 0; i < 200000; i++)
		{
			alignas(16) u32 a[4], b[4], expected[4], actual[4];
			for (int k = 0; k < 4; k++)
			{
				a[k] = i < 144 ? edges[i % 12] : (i & 1 ? rng() : edges[rng() % 12]);
				b[k] = i < 144 ? edges[i / 12] : (i & 2 ? rng() : edges[rng() % 12]);
				// Reference: PS2 compares as sign-magnitude integers (bit pattern, no NaN handling).
				const auto key = [](u32 x) { return static_cast<s32>(x & 0x80000000u ? x ^ 0x7fffffffu : x); };
				const bool pickTo = min ? key(b[k]) > key(a[k]) : key(a[k]) > key(b[k]);
				expected[k] = pickTo ? a[k] : b[k];
			}
			kernel(a, b, actual);
			if (std::memcmp(actual, expected, 16))
			{
				std::printf("FAIL VU %s iteration=%u\n", min ? "MINI" : "MAX", i);
				return 1;
			}
		}
	}
	std::puts("PASS VU MAX/MINI AVX512 order-key blend vs scalar reference, 200000 inputs each");
	return 0;
}

static int testQFSRV()
{
	using Kernel = void (*)(const u8*, const u8*, u8*, u32);
	alignas(16) static const u8 iota[16] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
	xAlignPtr(64);
	const auto kernel = reinterpret_cast<Kernel>(xGetAlignedCallTarget());
	xMOVDQU(xmm0, ptr[rdi]); // Rs
	xMOVDQU(xmm1, ptr[rsi]); // Rt
	emitQFSRVAVX512(xmm2, xmm0, xmm1, ecx, iota);
	xMOVDQU(ptr[rdx], xmm2);
	xRET();
	std::mt19937 rng(0x9f51);
	for (u32 i = 0; i < 100000; i++)
	{
		alignas(16) u8 s[16], t[16], actual[16], expected[16], both[32];
		for (int k = 0; k < 16; k++)
		{
			s[k] = rng();
			t[k] = rng();
		}
		std::memcpy(both, t, 16);
		std::memcpy(both + 16, s, 16);
		const u32 sa = i & 15;
		std::memcpy(expected, both + sa, 16);
		kernel(s, t, actual, sa);
		if (std::memcmp(actual, expected, 16))
		{
			std::printf("FAIL QFSRV sa=%u\n", sa);
			return 1;
		}
	}
	std::puts("PASS QFSRV VPERMI2B vs memory-concatenation reference, 100000 inputs");
	return 0;
}

// Instruction sequences added in the AVX2 round: PLZCW via LZCNT, PMFHL.LH via VPSHUFB + unpack.
static int testPhase15()
{
	{
		using Kernel = void (*)(const u32*, u32*);
		xAlignPtr(64);
		const auto kernel = reinterpret_cast<Kernel>(xGetAlignedCallTarget());
		// Same sequence as recPLZCW's LZCNT path: ecx = x >> 31 (arith); eax = lzcnt(x ^ ecx) - 1.
		xMOV(eax, ptr32[rdi]);
		xMOV(ecx, eax);
		xSAR(ecx, 31);
		xXOR(eax, ecx);
		xLZCNT(eax, eax);
		xDEC(eax);
		xMOV(ptr32[rsi], eax);
		xRET();
		std::mt19937 rng(0x1c47);
		for (u32 i = 0; i < 200000; i++)
		{
			u32 value = i < 32 ? (1u << i) : i < 64 ? ~(1u << (i - 32)) : i == 64 ? 0 : i == 65 ? 0xffffffffu : rng() >> (rng() % 32);
			if (i >= 66 && (rng() & 1))
				value = ~value;
			// Reference: the BSR form of recPLZCW.
			const u32 n = static_cast<s32>(value) < 0 ? ~value : value;
			u32 expected = 31;
			if (n)
				expected = 31 - (31 - std::countl_zero(n)) - 1;
			u32 actual = 0;
			kernel(&value, &actual);
			if (actual != expected)
			{
				std::printf("FAIL PLZCW LZCNT value=%08x got=%u want=%u\n", value, actual, expected);
				return 1;
			}
		}
		std::puts("PASS PLZCW LZCNT sequence vs BSR semantics");
	}
	{
		using Kernel = void (*)(const u32*, const u32*, u32*);
		xAlignPtr(64);
		const auto kernel = reinterpret_cast<Kernel>(xGetAlignedCallTarget());
		xMOVDQU(xmm0, ptr[rdi]); // LO
		xMOVDQU(xmm1, ptr[rsi]); // HI
		const bool saved_avx = use_avx;
		use_avx = true;
		// Same sequence as recPMFHL case LH on AVX: tmp = pack(LO), D = pack(HI), D = punpckldq(tmp, D).
		xPSHUF.B(xmm3, xmm0, ptr128[MMIGenerator::s_packEvenHalfMask]);
		xPSHUF.B(xmm2, xmm1, ptr128[MMIGenerator::s_packEvenHalfMask]);
		xPUNPCK.LDQ(xmm2, xmm3, xmm2);
		use_avx = saved_avx;
		xMOVDQU(ptr[rdx], xmm2);
		xRET();
		std::mt19937 rng(0x77aa);
		for (u32 i = 0; i < 100000; i++)
		{
			alignas(16) u32 lo[4], hi[4], actual[4], expected[4] = {};
			for (int k = 0; k < 4; k++)
			{
				lo[k] = rng();
				hi[k] = rng();
			}
			const u16* l16 = reinterpret_cast<const u16*>(lo);
			const u16* h16 = reinterpret_cast<const u16*>(hi);
			const u16 v[8] = {l16[0], l16[2], h16[0], h16[2], l16[4], l16[6], h16[4], h16[6]};
			std::memcpy(expected, v, 16);
			kernel(lo, hi, actual);
			if (std::memcmp(actual, expected, 16))
			{
				std::puts("FAIL PMFHL.LH AVX2");
				return 1;
			}
		}
		std::puts("PASS PMFHL.LH VPSHUFB+unpack (AVX) vs interpreter semantics");
	}
	return 0;
}

static int testPMFHLWords()
{
	using Kernel = void (*)(const u32*, const u32*, u32*);
	std::mt19937 rng(0x2fb1);
	for (int kind : {0, 1, 3})
	{
		xAlignPtr(64);
		const auto kernel = reinterpret_cast<Kernel>(xGetAlignedCallTarget());
		xMOVDQU(xmm0, ptr[rdi]); // LO
		xMOVDQU(xmm1, ptr[rsi]); // HI
		emitPMFHLAVX512(xmm2, xmm0, xmm1, kind == 3 ? xmm3 : xmm2, kind);
		xMOVDQU(ptr[rdx], xmm2);
		xRET();
		for (u32 i = 0; i < 50000; i++)
		{
			alignas(16) u32 lo[4], hi[4], actual[4], expected[4] = {};
			for (int k = 0; k < 4; k++)
			{
				lo[k] = rng();
				hi[k] = rng();
			}
			const u16* l16 = reinterpret_cast<const u16*>(lo);
			const u16* h16 = reinterpret_cast<const u16*>(hi);
			u16* e16 = reinterpret_cast<u16*>(expected);
			if (kind == 0)
				expected[0] = lo[0], expected[1] = hi[0], expected[2] = lo[2], expected[3] = hi[2];
			else if (kind == 1)
				expected[0] = lo[1], expected[1] = hi[1], expected[2] = lo[3], expected[3] = hi[3];
			else
			{
				const u16 v[8] = {l16[0], l16[2], h16[0], h16[2], l16[4], l16[6], h16[4], h16[6]};
				std::memcpy(e16, v, 16);
			}
			kernel(lo, hi, actual);
			if (std::memcmp(actual, expected, 16))
			{
				std::printf("FAIL PMFHL kind=%d iteration=%u\n", kind, i);
				return 1;
			}
		}
	}
	std::puts("PASS PMFHL LW/UW/LH AVX512 vs interpreter semantics");
	return 0;
}

static int testBroadcastD()
{
	using Kernel = void (*)(u32, u32*);
	xAlignPtr(64);
	const auto kernel = reinterpret_cast<Kernel>(xGetAlignedCallTarget());
	xVPBROADCASTD(xmm1, edi);
	xMOVDQU(ptr[rsi], xmm1);
	xRET();
	std::mt19937 rng(0x7c1);
	for (int i = 0; i < 1000; i++)
	{
		const u32 v = rng();
		alignas(16) u32 out[4] = {};
		kernel(v, out);
		if (out[0] != v || out[1] != v || out[2] != v || out[3] != v)
		{
			std::puts("FAIL VPBROADCASTD");
			return 1;
		}
	}
	std::puts("PASS VPBROADCASTD r32 (CTC2 I / STATUS broadcast)");
	return 0;
}

// BMI2 SHLX/SHRX/SARX (EE variable shifts), BLTZ sign PTEST, and the iFPUd underflow VPMULTISHIFTQB repack.
static int testPhase10()
{
	std::mt19937_64 rng(0x5a17);
	// Variable shifts: register and memory sources, 32 and 64-bit, counts above the operand width (masked).
	using ShiftKernel = u64 (*)(u64, u64, const u64*);
	for (int kind = 0; kind < 3; kind++)
		for (int wide = 0; wide < 2; wide++)
			for (int mem = 0; mem < 2; mem++)
			{
				const xImplBMI_RVM& op = kind == 0 ? xSHLX : kind == 1 ? xSHRX : xSARX;
				xAlignPtr(64);
				const auto kernel = reinterpret_cast<ShiftKernel>(xGetAlignedCallTarget());
				if (wide)
				{
					if (mem)
						op(rax, rsi, ptr64[rdx]);
					else
						op(rax, rsi, rdi);
				}
				else
				{
					if (mem)
						op(eax, esi, ptr32[rdx]);
					else
						op(eax, esi, edi);
					xMOVSX(rax, eax);
				}
				xRET();
				for (int i = 0; i < 100000; i++)
				{
					const u64 t = rng(), c = i < 130 ? i : rng();
					const u64 got = kernel(t, c, &t);
					u64 want;
					if (wide)
					{
						const u32 n = c & 63;
						want = kind == 0 ? t << n : kind == 1 ? t >> n : static_cast<u64>(static_cast<s64>(t) >> n);
					}
					else
					{
						const u32 n = c & 31, v = static_cast<u32>(t);
						const u32 r = kind == 0 ? v << n : kind == 1 ? v >> n : static_cast<u32>(static_cast<s32>(v) >> n);
						want = static_cast<u64>(static_cast<s64>(static_cast<s32>(r)));
					}
					if (got != want)
					{
						std::printf("FAIL BMI2 shift kind=%d wide=%d mem=%d i=%d\n", kind, wide, mem, i);
						return 1;
					}
				}
			}
	// BLTZ sign test: ZF is set when bit 63 of the low qword is clear.
	{
		using Kernel = u32 (*)(const u64*);
		alignas(16) static const u32 mask[4] = {0, 0x80000000u, 0, 0};
		xAlignPtr(64);
		const auto kernel = reinterpret_cast<Kernel>(xGetAlignedCallTarget());
		xMOVDQU(xmm1, ptr[rdi]);
		xPTEST(xmm1, ptr128[mask]);
		xSETZ(al);
		xMOVZX(eax, al);
		xRET();
		for (int i = 0; i < 100000; i++)
		{
			alignas(16) u64 v[2] = {i < 4 ? (i & 1 ? 0x8000000000000000ull : 0x7fffffffffffffffull) : static_cast<u64>(rng()), rng()};
			if (kernel(v) != ((v[0] >> 63) == 0))
			{
				std::puts("FAIL BLTZ PTEST");
				return 1;
			}
		}
	}
	// iFPUd underflow repack: (x<<12>>41) | (x>>63<<31) per qword vs multishift + mask.
	if (avx512.HasCore())
	{
		using Kernel = void (*)(const u64*, u64*);
		alignas(16) static const u8 ctrl[16] = {29, 37, 45, 56, 0, 0, 0, 0, 29, 37, 45, 56, 0, 0, 0, 0};
		alignas(16) static const u8 mask[16] = {0xff, 0xff, 0x7f, 0x80, 0, 0, 0, 0, 0xff, 0xff, 0x7f, 0x80, 0, 0, 0, 0};
		xAlignPtr(64);
		const auto kernel = reinterpret_cast<Kernel>(xGetAlignedCallTarget());
		xMOVDQU(xmm1, ptr[rdi]);
		xMOVAPS(xmm2, ptr128[ctrl]);
		xVPMULTISHIFTQB(xmm1, xmm2, xmm1);
		xPAND(xmm1, ptr128[mask]);
		xMOVDQU(ptr[rsi], xmm1);
		xRET();
		for (int i = 0; i < 200000; i++)
		{
			alignas(16) u64 in[2] = {rng(), rng()}, out[2];
			kernel(in, out);
			for (int k = 0; k < 2; k++)
			{
				const u64 want = ((in[k] << 12) >> 41) | (((in[k] >> 63) << 31));
				if (out[k] != want)
				{
					std::puts("FAIL iFPUd repack");
					return 1;
				}
			}
		}
	}
	// Paired micro-benchmarks (call overhead included; both sides pay it equally).
	{
		using K = u64 (*)(u64, u64, const u64*);
		xAlignPtr(64);
		const auto oldShift = reinterpret_cast<K>(xGetAlignedCallTarget());
		xMOV(rcx, rsi);
		xMOV(rax, rdi);
		xSHL(rax, cl);
		xRET();
		xAlignPtr(64);
		const auto newShift = reinterpret_cast<K>(xGetAlignedCallTarget());
		xSHLX(rax, rsi, rdi);
		xRET();
		const std::array<K, 2> k = {oldShift, newShift};
		volatile u64 sink = 0;
		const auto t = benchmarkPair([&](int b) { sink = k[b](0x123456789abcdefull, 13, nullptr); });
		std::printf("BENCH10 DSLLV mov rcx+shl vs shlx: %.3f vs %.3f ns/call (speedup %.3f)\n", t[0], t[1], t[0] / t[1]);

		using P = u32 (*)(const u64*);
		alignas(16) static const u32 mask[4] = {0, 0x80000000u, 0, 0};
		xAlignPtr(64);
		const auto oldSign = reinterpret_cast<P>(xGetAlignedCallTarget());
		xMOVDQU(xmm1, ptr[rdi]);
		xMOVMSKPS(eax, xmm1);
		xTEST(al, 2);
		xSETZ(al);
		xMOVZX(eax, al);
		xRET();
		xAlignPtr(64);
		const auto newSign = reinterpret_cast<P>(xGetAlignedCallTarget());
		xMOVDQU(xmm1, ptr[rdi]);
		xPTEST(xmm1, ptr128[mask]);
		xSETZ(al);
		xMOVZX(eax, al);
		xRET();
		const std::array<P, 2> kp = {oldSign, newSign};
		alignas(16) u64 v[2] = {0x8000000000000001ull, 0};
		const auto tp = benchmarkPair([&](int b) { sink = kp[b](v); });
		std::printf("BENCH10 BLTZ sign MOVMSKPS+TEST vs PTEST: %.3f vs %.3f ns/call (speedup %.3f)\n", tp[0], tp[1], tp[0] / tp[1]);
	}
	std::puts("PASS SHLX/SHRX/SARX, BLTZ sign PTEST, iFPUd underflow multishift repack");
	return 0;
}

// PEXTLW/B/H, PCGTH/W, PCPYUD with $0 operands (zero-extension, sign mask, byte shift) on SSE4 / AVX2 / AVX-512.
static int testPhase11()
{
	using Kernel = void (*)(const u32*, const u32*, u32*);
	const std::array generators = {MMIGenerator::recPEXTLW, MMIGenerator::recPEXTLB, MMIGenerator::recPEXTLH,
		MMIGenerator::recPCGTH, MMIGenerator::recPCGTW, MMIGenerator::recPCPYUD};
	const char* names[] = {"PEXTLW", "PEXTLB", "PEXTLH", "PCGTH", "PCGTW", "PCPYUD"};
	std::mt19937 rng(0x11110011);
	for (size_t op = 0; op < generators.size(); op++)
		for (int zero = 0; zero < 2; zero++) // Rs==0 (PCPYUD: Rt==0)
			for (auto ids : {std::array{0, 1, 2}, std::array{0, 1, 0}, std::array{0, 1, 1}, std::array{0, 0, 2}, std::array{0, 0, 0}})
			{
				MMIGenerator::Rs_value = (op != 5 && zero) ? 0 : 1;
				MMIGenerator::Rt_value = (op == 5 && zero) ? 0 : 1;
				std::array<Kernel, 3> kernels;
				for (int backend = 0; backend < g_backends; backend++)
				{
					const auto features = avx512;
					const bool saved_avx = use_avx;
					if (backend < 2)
						avx512 = {};
					use_avx = backend != 0;
					xAlignPtr(64);
					kernels[backend] = reinterpret_cast<Kernel>(xGetAlignedCallTarget());
					xMOVDQU(xRegisterSSE(ids[0]), ptr[rdi]);
					if (ids[1] != ids[0])
						xMOVDQU(xRegisterSSE(ids[1]), ptr[rsi]);
					MMIGenerator::EEREC_S = ids[0];
					MMIGenerator::EEREC_T = ids[1];
					MMIGenerator::EEREC_D = ids[2];
					MMIGenerator::temporary = 15;
					generators[op]();
					xMOVDQU(ptr[rdx], xRegisterSSE(ids[2]));
					xRET();
					avx512 = features;
					use_avx = saved_avx;
				}
				for (u32 i = 0; i < 100000; i++)
				{
					alignas(16) u32 a[4], b[4], actual[4];
					for (int lane = 0; lane < 4; lane++)
					{
						a[lane] = i < 16 ? ((i >> lane) & 1 ? 0x80008080u : 0x7fff7f7fu) : rng();
						b[lane] = i < 16 ? ((i >> (3 - lane)) & 1 ? 0xffff8000u : 0x00017f01u) : rng();
						if (ids[0] == ids[1])
							b[lane] = a[lane];
					}
					u8 s[16], t[16], e[16] = {};
					std::memcpy(s, a, 16);
					std::memcpy(t, b, 16);
					// A zero source register is not read by the recompiler; the oracle uses architectural $0.
					if (op != 5 && zero)
						std::memset(s, 0, 16);
					if (op == 5 && zero)
						std::memset(t, 0, 16);
					if (op <= 2)
					{
						const int w = op == 0 ? 4 : (op == 1 ? 1 : 2);
						for (int k = 0; k < 8 / w; k++)
						{
							std::memcpy(e + (2 * k) * w, t + k * w, w);
							std::memcpy(e + (2 * k + 1) * w, s + k * w, w);
						}
					}
					else if (op <= 4)
					{
						const int w = op == 3 ? 2 : 4;
						for (int k = 0; k < 16 / w; k++)
						{
							s64 x = 0, y = 0;
							if (w == 2) { s16 xs, ys; std::memcpy(&xs, s + k * 2, 2); std::memcpy(&ys, t + k * 2, 2); x = xs; y = ys; }
							else { s32 xs, ys; std::memcpy(&xs, s + k * 4, 4); std::memcpy(&ys, t + k * 4, 4); x = xs; y = ys; }
							std::memset(e + k * w, x > y ? 0xff : 0, w);
						}
					}
					else
					{
						std::memcpy(e, s + 8, 8);
						std::memcpy(e + 8, t + 8, 8);
					}
					for (int backend = 0; backend < g_backends; backend++)
					{
						kernels[backend](a, b, actual);
						if (std::memcmp(actual, e, 16))
						{
							std::printf("FAIL phase11 %s zero=%d ids=%d/%d/%d backend=%d iteration=%u\n", names[op], zero, ids[0], ids[1], ids[2], backend, i);
							return 1;
						}
					}
				}
			}
	std::puts("PASS phase11: PEXTLW/B/H, PCGTH/W, PCPYUD with $0 operands, 100000 inputs, all aliases, SSE4/AVX2/AVX512 vs scalar oracle");
	return 0;
}

static int testClipDenormal()
{
	using Kernel = void (*)(const u32*, u32*, u32*);
	alignas(16) static const u32 exponent[4] = {0x7f800000, 0x7f800000, 0x7f800000, 0x7f800000};
	// Legacy: zero the lanes whose exponent is 0. New: VPTESTMD + zeroing VPTERNLOGD.
	xAlignPtr(64);
	const auto legacy = reinterpret_cast<Kernel>(xGetAlignedCallTarget());
	xMOVDQU(xmm0, ptr[rdi]);
	xPAND(xmm1, xmm0, ptr128[exponent]);
	xPXOR(xmm2, xmm2);
	xPCMP.EQD(xmm1, xmm2);
	xPANDN(xmm1, xmm0);
	xMOVDQU(ptr[rsi], xmm1);
	xRET();
	xAlignPtr(64);
	const auto fresh = reinterpret_cast<Kernel>(xGetAlignedCallTarget());
	xMOVDQU(xmm0, ptr[rdi]);
	xVPTESTMD(k1, xmm0, ptr128[exponent]);
	xVPTERNLOGD(xmm1, xmm0, xmm0, 0xcc, k1, true);
	xMOVDQU(ptr[rsi], xmm1);
	xRET();
	std::mt19937 rng(0xc11b);
	for (int i = 0; i < 200000; i++)
	{
		alignas(16) u32 in[4], a[4], b[4];
		for (u32& v : in)
		{
			v = rng();
			if (rng() & 1)
				v &= 0x807fffffu; // denormal / zero
			if (!(rng() & 7))
				v |= 0x7f800000u;
		}
		legacy(in, a, nullptr);
		fresh(in, b, nullptr);
		if (std::memcmp(a, b, 16))
		{
			std::puts("FAIL CLIP denormal zeroing");
			return 1;
		}
	}
	std::puts("PASS CLIP denormal zeroing: memory VPTESTMD + zeroing VPTERNLOGD vs PAND/PCMPEQD/PANDN, 200000 vectors");
	return 0;
}

static int testVPCMPDMemory()
{
	// VPCMPD k, xmm, m128, imm8 (rip-relative absolute and [rdi+disp8]) against the register form.
	using Kernel = void (*)(const u32*, u32*);
	alignas(16) static u32 constant[4];
	std::mt19937 rng(0xc0de);
	for (int variant = 0; variant < 3; variant++)
		for (u8 imm = 0; imm < 8; imm++)
			for (bool masked : {false, true})
			{
				const auto emit = [&](bool memory) {
					xAlignPtr(64);
					const auto kernel = reinterpret_cast<Kernel>(xGetAlignedCallTarget());
					xMOVDQU(xmm0, ptr[rdi]);
					xMOVDQU(xmm1, ptr[rdi + 16]);
					xMOV(eax, 0x0b);
					xKMOVW(k3, eax);
					const xRegisterK mask = masked ? k3 : k0;
					if (!memory)
						xVPCMPD(k1, xmm0, xmm1, imm, mask);
					else if (variant == 0)
						xVPCMPD(k1, xmm0, ptr128[rdi + 16], imm, mask);
					else if (variant == 1)
						xVPCMPD(k1, xmm0, ptr128[constant], imm, mask);
					else
						xVPCMPD(k1, xmm0, ptr128[rdi + 64 + 16], imm, mask); // disp8 limit and beyond
					xKMOVW(eax, k1);
					xMOV(ptr32[rsi], eax);
					xRET();
					return kernel;
				};
				const Kernel legacy = emit(false);
				const Kernel fresh = emit(true);
				for (int i = 0; i < 20000; i++)
				{
					alignas(16) u32 in[40] = {};
					for (int k = 0; k < 8; k++)
						in[k] = (rng() & 3) ? static_cast<u32>(rng()) : 0x80000000u + (rng() & 3);
					if (rng() & 1)
						for (int k = 0; k < 4; k++)
							if (rng() & 1)
								in[4 + k] = in[k];
					std::memcpy(in + 20, in + 4, 16); // [rdi + 80]
					std::memcpy(constant, in + 4, 16);
					u32 a = 0xdead, b = 0xbeef;
					legacy(in, &a);
					fresh(in, &b);
					if (a != b)
					{
						std::printf("FAIL VPCMPD memory variant=%d imm=%u masked=%d\n", variant, imm, masked);
						return 1;
					}
				}
			}
	std::puts("PASS VPCMPD k,xmm,m128: 3 addressing forms x 8 predicates x writemask, vs register form");
	return 0;
}

static int testCSCThresholdSign()
{
	std::mt19937 rng(0xc5c512);
	alignas(64) std::array<u32, 256> pixels{};
	alignas(64) std::array<u32, 256> expected{};

	auto run = [&](u32 th0, u32 th1, bool sgn) {
		for (u32& p : pixels)
			p = rng();
		expected = pixels;
		for (u32& p : expected)
		{
			const u32 r = p & 0xff;
			const u32 g = (p >> 8) & 0xff;
			const u32 b = (p >> 16) & 0xff;
			const u32 maximum = std::max({r, g, b});
			if (maximum < th0)
				p = 0;
			else if (maximum < th1)
				p = (p & 0x00ffffffu) | 0x40000000u;
			if (sgn)
				p ^= 0x00808080u;
		}
		InterpreterAVX512::CSCThresholdSign(pixels.data(), th0, th1, sgn);
		return std::memcmp(pixels.data(), expected.data(), sizeof(pixels)) == 0;
	};

	constexpr std::array<u32, 8> edges = {0, 1, 2, 254, 255, 256, 257, 511};
	for (const u32 th0 : edges)
		for (const u32 th1 : edges)
			for (const bool sgn : {false, true})
				if (!run(th0, th1, sgn))
			{
				std::printf("FAIL CSCThresholdSign edge th0=%u th1=%u sgn=%d\n", th0, th1, sgn);
				return 1;
			}
	for (u32 i = 0; i < 20000; i++)
	{
		const u32 th0 = rng() & 0x1ff;
		const u32 th1 = rng() & 0x1ff;
		const bool sgn = (rng() & 1) != 0;
		if (!run(th0, th1, sgn))
		{
			std::printf("FAIL CSCThresholdSign random th0=%u th1=%u sgn=%d iteration=%u\n", th0, th1, sgn, i);
			return 1;
		}
	}
	std::puts("PASS CSCThresholdSign: edge modes and 20000 random macroblocks vs scalar oracle");
	return 0;
}

static int testPADSBHAndPHMADHReductions()
{
	using PADSBHKernel = void (*)(const u16*, const u16*, u16*);
	alignas(16) static const s16 sign[8] = {-1, -1, -1, -1, 1, 1, 1, 1};
	struct AliasLayout { int d, s, t; };
	constexpr AliasLayout layouts[] = {
		{0, 1, 2}, {0, 0, 1}, {0, 1, 0}, {0, 1, 1}, {0, 0, 0},
	};
	std::array<PADSBHKernel, std::size(layouts)> padsbh{};
	const bool old_use_avx = x86Emitter::use_avx;
	x86Emitter::use_avx = true;
	for (size_t n = 0; n < std::size(layouts); n++)
	{
		const auto [d, s, t] = layouts[n];
		xAlignPtr(64);
		padsbh[n] = reinterpret_cast<PADSBHKernel>(xGetAlignedCallTarget());
		if (s == t)
		{
			xMOVDQU(xRegisterSSE(s), ptr[rdi]);
		}
		else
		{
			xMOVDQU(xRegisterSSE(s), ptr[rdi]);
			xMOVDQU(xRegisterSSE(t), ptr[rsi]);
		}
		if (d == s)
		{
			xPSIGN.W(xmm15, xRegisterSSE(t), ptr128[sign]);
			xPADD.W(xRegisterSSE(d), xRegisterSSE(s), xmm15);
		}
		else
		{
			xPSIGN.W(xRegisterSSE(d), xRegisterSSE(t), ptr128[sign]);
			xPADD.W(xRegisterSSE(d), xRegisterSSE(s), xRegisterSSE(d));
		}
		xMOVDQU(ptr[rdx], xRegisterSSE(d));
		xRET();
	}
	x86Emitter::use_avx = old_use_avx;

	using PHMADHKernel = void (*)(const s16*, const s16*, s32*);
	alignas(16) static const u32 odd_mask[4] = {0xffff0000u, 0xffff0000u, 0xffff0000u, 0xffff0000u};
	xAlignPtr(64);
	const auto phmadh_legacy = reinterpret_cast<PHMADHKernel>(xGetAlignedCallTarget());
	xMOVDQU(xmm0, ptr[rdi]);
	xMOVDQU(xmm1, ptr[rsi]);
	xPXOR(xmm2, xmm2);
	xPBLEND.W(xmm2, xmm0, 0xaa);
	xPMADD.WD(xmm2, xmm1);
	xMOVDQU(ptr[rdx], xmm2);
	xRET();
	xAlignPtr(64);
	const auto phmadh_fresh = reinterpret_cast<PHMADHKernel>(xGetAlignedCallTarget());
	xMOVDQU(xmm0, ptr[rdi]);
	xMOVDQU(xmm1, ptr[rsi]);
	xPAND(xmm2, xmm0, ptr128[odd_mask]);
	xPMADD.WD(xmm2, xmm1);
	xMOVDQU(ptr[rdx], xmm2);
	xRET();

	std::mt19937 rng(0x5bad5b);
	for (u32 i = 0; i < 200000; i++)
	{
		alignas(16) u16 a[8], b[8], actual[8], expected[8];
		for (int lane = 0; lane < 8; lane++)
		{
			a[lane] = static_cast<u16>(rng());
			b[lane] = static_cast<u16>(rng());
		}
		for (size_t n = 0; n < std::size(layouts); n++)
		{
			const u16* rhs = layouts[n].s == layouts[n].t ? a : b;
			for (int lane = 0; lane < 8; lane++)
				expected[lane] = lane < 4 ? static_cast<u16>(a[lane] - rhs[lane]) : static_cast<u16>(a[lane] + rhs[lane]);
			padsbh[n](a, rhs, actual);
			if (std::memcmp(actual, expected, sizeof(actual)))
			{
				std::printf("FAIL PADSBH reduction alias=%zu iteration=%u\n", n, i);
				return 1;
			}
		}

		alignas(16) s32 legacy[4], fresh[4];
		phmadh_legacy(reinterpret_cast<const s16*>(a), reinterpret_cast<const s16*>(b), legacy);
		phmadh_fresh(reinterpret_cast<const s16*>(a), reinterpret_cast<const s16*>(b), fresh);
		if (std::memcmp(legacy, fresh, sizeof(legacy)))
		{
			std::printf("FAIL PHMADH odd-half reduction iteration=%u\n", i);
			return 1;
		}
	}
	std::puts("PASS PADSBH 3->2 and PHMADH odd-half 3->2 reductions: 200000 vectors; PADSBH all alias classes");
	return 0;
}

static int testPMFHLSLW()
{
	using Kernel = void (*)(const u32*, const u32*, u32*);
	xAlignPtr(64);
	const auto kernel = reinterpret_cast<Kernel>(xGetAlignedCallTarget());
	xMOVDQU(xmm0, ptr[rdi]); // LO
	xMOVDQU(xmm1, ptr[rsi]); // HI
	emitPMFHLSLWAVX512(xmm1, xmm0, xmm1, xmm1); // production: Rd serves as the temp (Rd == HI worst case)
	xMOVDQU(ptr[rdx], xmm1);
	xRET();
	std::mt19937_64 rng(0x514a5);
	const u32 edges[] = {0, 1, 0x7fffffffu, 0x80000000u, 0xffffffffu, 0x80000001u};
	for (u32 i = 0; i < 200000; i++)
	{
		alignas(16) u32 lo[4], hi[4], actual[4];
		for (int k = 0; k < 4; k++)
		{
			lo[k] = i < 36 * 36 ? edges[(i + k) % 6] : static_cast<u32>(rng());
			hi[k] = i < 36 * 36 ? edges[(i / 6 + k) % 6] : static_cast<u32>(rng());
			if (i >= 36 * 36 && (i & 1) && k != 1 && k != 3)
				hi[k] = (rng() & 1) ? 0 : 0xffffffffu; // bias towards in-range values
		}
		u64 expected[2];
		for (int half = 0; half < 2; half++)
		{
			const s64 t = static_cast<s64>((static_cast<u64>(hi[half * 2]) << 32) | lo[half * 2]);
			if (t >= 0x7fffffffLL)
				expected[half] = 0x7fffffffULL;
			else if (t <= -0x80000000LL)
				expected[half] = 0xffffffff80000000ULL;
			else
				expected[half] = static_cast<u64>(static_cast<s64>(static_cast<s32>(lo[half * 2])));
		}
		kernel(lo, hi, actual);
		if (std::memcmp(actual, expected, 16))
		{
			std::printf("FAIL PMFHL.SLW iteration=%u\n", i);
			return 1;
		}
	}
	std::puts("PASS PMFHL.SLW AVX512 vs interpreter semantics, 200000 inputs");
	return 0;
}

// PPACW/H/B with Rs != 0: VINSERTI128 + one narrowing move, every alias layout.
static int testPPACGeneral()
{
	using Kernel = void (*)(const u8*, const u8*, u8*);
	std::mt19937 rng(0x99ac);
	for (int width = 0; width < 3; width++)
		for (auto ids : {std::array{0, 1, 2}, std::array{0, 1, 0}, std::array{0, 1, 1}, std::array{0, 0, 2}, std::array{0, 0, 0}})
		{
			const xRegisterSSE s(ids[0]), t(ids[1]), d(ids[2]);
			auto fn = reinterpret_cast<Kernel>(xGetAlignedCallTarget());
			xMOVDQU(s, ptr[rdi]);
			if (ids[1] != ids[0])
				xMOVDQU(t, ptr[rsi]);
			emitPPACGeneralAVX512(d, s, t, width);
			xMOVDQU(ptr[rdx], d);
			xRET();
			const int w = 4 >> width; // 4, 2, 1 bytes kept per 2*w element
			for (u32 i = 0; i < 100000; i++)
			{
				alignas(16) u8 a[16], b[16], out[16] = {}, expected[16];
				for (int k = 0; k < 16; k++)
				{
					a[k] = rng();
					b[k] = ids[0] == ids[1] ? a[k] : static_cast<u8>(rng());
				}
				// Result: even elements (w bytes each, stride 2w) of Rt then of Rs.
				for (int k = 0; k < 8 / w; k++)
				{
					std::memcpy(expected + k * w, b + k * 2 * w, w);
					std::memcpy(expected + 8 + k * w, a + k * 2 * w, w);
				}
				fn(a, b, out);
				if (std::memcmp(out, expected, 16))
				{
					std::printf("FAIL PPAC general width=%d ids=%d/%d/%d\n", width, ids[0], ids[1], ids[2]);
					return 1;
				}
			}
		}
	std::puts("PASS PPACW/H/B general form: VINSERTI128 + narrow, 100000 inputs, all aliases");
	return 0;
}

static int testMMIJit()
{
	using Kernel = void (*)(const u32*, const u32*, u32*);
	std::mt19937 rng(0x5125900);
	// Distinct registers, Rd=Rs, Rd=Rt, Rs=Rt, and all equal.
	for (auto ids : {std::array{0, 1, 2}, std::array{0, 1, 0}, std::array{0, 1, 1},
			 std::array{0, 0, 2}, std::array{0, 0, 0}})
	{
		for (bool plzcw : {false, true})
		{
			const xRegisterSSE src(ids[0]), operand(ids[1]), dst(ids[2]);
			auto fn = reinterpret_cast<Kernel>(xGetAlignedCallTarget());
			xMOVDQU(src, ptr[rdi]);
			if (ids[1] != ids[0])
				xMOVDQU(operand, ptr[rsi]);
			if (ids[2] != ids[0] && ids[2] != ids[1])
				xMOVDQU(dst, ptr[rdx]);
			if (plzcw)
				emitPLZCWAVX512(dst, src, xmm14, xmm15);
			else
				emitPNORAVX512(dst, src, operand);
			xMOVDQU(ptr[rdx], dst);
			xRET();
			for (u32 i = 0; i < 100000; i++)
			{
				alignas(16) u32 a[4], b[4], out[4], expected[4];
				for (int lane = 0; lane < 4; lane++)
				{
					a[lane] = rng();
					b[lane] = rng();
					out[lane] = rng();
				}
				if (i < 32)
					a[0] = 1u << i;
				if (i < 2)
					a[1] = i ? 0xffffffffu : 0;
				const u32* old = ids[2] == ids[0] ? a : ids[2] == ids[1] ? b :
				                                                           out;
				std::memcpy(expected, old, 16);
				for (int lane = 0; lane < (plzcw ? 2 : 4); lane++)
					expected[lane] = plzcw ? std::countl_zero(a[lane] ^ (static_cast<s32>(a[lane]) >> 31)) - 1 :
					                         ~(a[lane] | (ids[0] == ids[1] ? a[lane] : b[lane]));
				fn(a, b, out);
				if (std::memcmp(out, expected, 16))
				{
					std::printf("FAIL JIT MMI plzcw=%d ids=%d/%d/%d\n", plzcw, ids[0], ids[1], ids[2]);
					return 1;
				}
			}
		}
	}
	auto select = reinterpret_cast<Kernel>(xGetAlignedCallTarget());
	xMOVDQU(xmm0, ptr[rdx]);
	xMOVDQU(xmm1, ptr[rdi]);
	xMOVDQU(xmm2, ptr[rsi]);
	emitMMIBitSelect(xmm0, xmm1, xmm2);
	xMOVDQU(ptr[rdx], xmm0);
	xRET();
	for (u32 i = 0; i < 100000; i++)
	{
		alignas(16) u32 mask[4], replacement[4], out[4], expected[4];
		for (int lane = 0; lane < 4; lane++)
		{
			mask[lane] = i < 2 ? (i ? 0xffffffffu : 0) : rng();
			replacement[lane] = rng();
			out[lane] = rng();
			expected[lane] = (out[lane] & mask[lane]) | (replacement[lane] & ~mask[lane]);
		}
		select(mask, replacement, out);
		if (std::memcmp(out, expected, 16))
		{
			std::puts("FAIL JIT MMI bit select");
			return 1;
		}
	}
	std::puts("PASS JIT PLZCW/PNOR/bit select: 100000 vectors, register aliases, upper-half preservation");
	return 0;
}

using Scalar = u64 (*)(u32, u32, u32, u32, u32);
using Packed = u32 (*)(const u32*, const u32*, const u32*, void*, u32, u32);
static void saveABI()
{
	for (int id : {3, 5, 12, 13, 14, 15})
		xPUSH(xRegister64(id));
	xSUB(rsp, 8);
}
static void restoreABI()
{
	xADD(rsp, 8);
	for (int id : {15, 14, 13, 12, 5, 3})
		xPOP(xRegister64(id));
	xRET();
}
static Scalar scalarWrapper(const void* entry, u32* productStatus = nullptr)
{
	auto fn = reinterpret_cast<Scalar>(xGetAlignedCallTarget());
	saveABI();
	xMOV(r9d, r8d);
	xMOV(r8d, ecx);
	xMOV(ecx, edx);
	xMOV(edx, esi);
	xMOV(eax, edi);
	xCALL(entry);
	if (productStatus)
	{
		xMOV64(r11, reinterpret_cast<uptr>(productStatus));
		xMOV(ptr32[r11], ecx);
	}
	xSHL(rdx, 32);
	xOR(rax, rdx);
	restoreABI();
	return fn;
}
static Packed packedWrapper(const void* entry, bool booth)
{
	auto fn = reinterpret_cast<Packed>(xGetAlignedCallTarget());
	saveABI();
	xMOV(r10d, r8d);
	xMOV(r8d, r9d);
	xMOV(r9, rcx);
	xMOV(rcx, rdx);
	xMOV(rdx, rsi);
	xMOV(rax, rdi);
	xMOV64(r11, reinterpret_cast<uptr>(s_vu_soft_lane_mask));
	xMOVZX(r11d, ptr8[xAddressVoid(r11, r10, 1)]);
	if (booth)
	{
		xMOV(rcx, r9);
		xMOV(r10d, r11d);
	}
	xCALL(entry);
	restoreABI();
	return fn;
}
struct Backend
{
	microVU vu;
	std::array<Scalar, 12> scalar;
	Scalar integrated;
	Packed booth;
	std::array<Packed, 2> madd;
	size_t bytes;
};
static u32 integratedStatus[2];
static Backend generate(bool avx512Enabled)
{
	avx512 = {};
	if (avx512Enabled)
		avx512 = {true, true, true, true, true, true, true, true, true, true, true, false};
	use_avx = true;
	Backend backend;
	auto& vu = backend.vu;
	vu.softBoothCache = std::make_unique<microVUSoftBoothCacheEntry[]>(mVUsoftBoothCacheSize);
	vu.softSrtReciprocalCache = std::make_unique<microVUSoftUnaryCacheEntry[]>(mVUsoftLowerCacheSize);
	vu.softDivCache = std::make_unique<microVUSoftLowerCacheEntry[]>(mVUsoftLowerCacheSize);
	vu.softSqrtCache = std::make_unique<microVUSoftUnaryCacheEntry[]>(mVUsoftLowerCacheSize);
	vu.softRsqrtCache = std::make_unique<microVUSoftLowerCacheSet[]>(mVUsoftLowerCacheSize);
	u8* start = xGetPtr();
	mVUGenerateSoftMulExactKernel(vu);
	mVUGenerateSoftAddExactLaneKernel(vu);
	mVUGenerateSoftMaddIntegratedLaneKernel(vu);
	mVUGenerateSoftMulExactVectorKernel(vu);
	mVUGenerateSoftMulBoothPackedKernel(vu);
	mVUGenerateSoftMaddPackedKernels(vu);
	mVUGenerateSoftMaddExactVectorKernels(vu);
	mVUGenerateLowerSrtReciprocalSoftExactKernel(vu);
	const auto tail = mVUGenerateLowerDivSoftExactKernel(vu);
	mVUGenerateLowerSqrtSoftExactKernel(vu);
	mVUGenerateLowerRsqrtSoftExactKernel(vu);
	mVUGenerateLowerDivSoftCapTail(tail);
	GenerateSoftFloatKernels();
	std::array<const void*, 12> entries = {vu.softAddExactLane, vu.softMulExact, vu.softDivExact, vu.softSqrtExact,
		vu.softRsqrtExact, s_fpuSoftAddSubExact[0], s_fpuSoftAddSubExact[1], s_fpuSoftMulExact,
		s_fpuSoftDivExact, s_fpuSoftDivCapExact, s_fpuSoftSqrtExact, s_fpuSoftRsqrtExact};
	for (size_t i = 0; i < entries.size(); i++)
		backend.scalar[i] = scalarWrapper(entries[i]);
	backend.integrated = scalarWrapper(vu.softMaddIntegratedLane, &integratedStatus[avx512Enabled]);
	backend.booth = packedWrapper(vu.softMulBoothPacked, true);
	for (int i = 0; i < 2; i++)
		backend.madd[i] = packedWrapper(vu.softMaddPacked[i], false);
	backend.bytes = xGetPtr() - start;
	return backend;
}
static const char* names[] = {"VU ADD", "VU MUL", "VU DIV", "VU SQRT", "VU RSQRT", "EE ADD", "EE SUB", "EE MUL", "EE DIV", "EE DIV cap", "EE SQRT", "EE RSQRT"};
static u64 arithmeticOracle(size_t op, u32 a, u32 b)
{
	const PS2Float result = [&]() {
		switch (op)
		{
			case 1:
			case 7:
				return PS2Float(a).Mul(PS2Float(b));
			case 5:
				return PS2Float(a).Add(PS2Float(b));
			case 6:
				return PS2Float(a).Sub(PS2Float(b));
			case 8:
			case 9:
				return PS2Float(a).Div(PS2Float(b));
			case 10:
				return PS2Float(a).Sqrt();
			default:
				std::abort();
		}
	}();
	// Convert the interpreter's flags to the internal generated-kernel ABI.
	const u64 flags = (result.HasOverflow() ? (op == 1 ? 2 : 1) : 0) | (result.HasUnderflow() ? (op == 1 ? 1 : 2) : 0) |
	                  (result.HasDivideByZero() ? 4 : 0) | (result.HasInvalid() ? 8 : 0);
	return result.raw | (flags << 32);
}
extern int TestInterpreterLeaves();
extern int TestAVX2Leaves();
extern int TestPhase2Allocator(const void* exact_helper);
extern int TestUpperSoftInline();
extern int TestFlagClassification();
int main()
{
	__builtin_cpu_init();
	if (std::getenv("AVX512_VALIDATION_AVX2_ONLY"))
	{
		void* avx2_code = mmap(nullptr, 4 * 1024 * 1024, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
		if (avx2_code == MAP_FAILED)
			return 1;
		xSetPtr(static_cast<u8*>(avx2_code));
		g_backends = 2;
		if (testProductionMMIJit() || testHalfwordMultiply() || testPhase11() || testPhase15() || TestAVX2Leaves())
			return 1;
		std::puts("PASS AVX2-only subset");
		return 0;
	}
	if (!__builtin_cpu_supports("avx512f") || !__builtin_cpu_supports("avx512vl") ||
		!__builtin_cpu_supports("avx512dq") || !__builtin_cpu_supports("avx512bw") ||
		!__builtin_cpu_supports("avx512cd") || !__builtin_cpu_supports("avx512vbmi") ||
		!__builtin_cpu_supports("avx512vbmi2") || !__builtin_cpu_supports("avx512vnni") ||
		!__builtin_cpu_supports("avx512bitalg") || !__builtin_cpu_supports("avx512vpopcntdq") ||
		!__builtin_cpu_supports("vpclmulqdq"))
	{
		std::puts("SKIP: AVX-512 host required");
		return 77;
	}
	void* code = mmap(nullptr, 4 * 1024 * 1024, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
	if (code == MAP_FAILED)
		return 1;
	xSetPtr(static_cast<u8*>(code));
	MicroVUSoftFloatTables::InitializeCorrectionTables();
	auto baseline = generate(false);
	auto extended = generate(true);
	FPControlRegister::SetCurrent(EmuConfig.Cpu.VU0FPCR);
	std::mt19937 rng(0x512);
	const u32 edge[] = {0, 0x80000000, 1, 0x80000001, 0x007fffff, 0x00800000, 0x00800001, 0x3f800000,
		0x3fffffff, 0x40000000, 0x7f7fffff, 0x7f800000, 0x7fffffff, 0xff800000, 0xffffffff};
	for (size_t op = 0; op < baseline.scalar.size(); op++)
	{
		for (unsigned i = 0; i < 30000; i++)
		{
			u32 a = i < std::size(edge) * std::size(edge) ? edge[i % std::size(edge)] : rng();
			u32 b = i < std::size(edge) * std::size(edge) ? edge[i / std::size(edge)] : rng();
			for (int repeat = 0; repeat < 2; repeat++)
			{
				u64 expected = baseline.scalar[op](a, b, 0, 0, 0);
				u64 actual = extended.scalar[op](a, b, 0, 0, 0);
				if (op == 1 || (op >= 5 && op <= 10))
				{
					const u64 oracle = arithmeticOracle(op, a, b);
					if (actual != oracle || expected != oracle)
					{
						std::printf("FAIL PS2Float oracle %s %08x %08x: oracle=%016llx AVX2=%016llx AVX512=%016llx\n",
							names[op], a, b, (unsigned long long)oracle, (unsigned long long)expected, (unsigned long long)actual);
						return 1;
					}
				}
				if (expected != actual)
				{
					std::printf("FAIL %s %08x %08x: %016llx != %016llx\n", names[op], a, b, (unsigned long long)expected, (unsigned long long)actual);
					return 1;
				}
			}
		}
		std::printf("PASS %s: 30000 inputs, miss/hit%s\n", names[op], op == 1 || (op >= 5 && op <= 10) ? ", independent PS2Float oracle (bits and flags)" : ", AVX2 differential");
		std::fflush(stdout);
	}
	for (unsigned i = 0; i < 100000; i++)
	{
		alignas(16) u32 a[4], b[4], c[4], x[4], y[4];
		for (int lane = 0; lane < 4; lane++)
		{
			a[lane] = i < 1000 ? edge[(i + lane) % std::size(edge)] : rng();
			b[lane] = rng();
			c[lane] = rng();
		}
		const u32 mask = i & 15;
		for (int repeat = 0; repeat < 2; repeat++)
		{
			baseline.booth(a, b, c, x, mask, 0);
			extended.booth(a, b, c, y, mask, 0);
			if (std::memcmp(x, y, sizeof(x)))
			{
				std::printf("FAIL Booth %u\n", i);
				return 1;
			}
			for (int sub = 0; sub < 2; sub++)
			{
				alignas(16) VuSoftFmacJitResult rx{}, ry{};
				u32 sx = baseline.madd[sub](a, b, c, &rx, mask, 0);
				u32 sy = extended.madd[sub](a, b, c, &ry, mask, 0);
				if (sx != sy || (sx && std::memcmp(&rx, &ry, sizeof(rx))))
				{
					std::printf("FAIL MADD %u mask=%u sub=%d success=%u/%u\n", i, mask, sub, sx, sy);
					return 1;
				}
			}
		}
	}
	std::puts("PASS Booth/MADD/MSUB: 100000 vectors, all masks, miss/hit and fallback");
	// Force the rare regular Booth correction, which random raw floats seldom reach.
	std::vector<std::array<u32, 2>> correctionInputs;
	while (correctionInputs.size() < 4096)
	{
		const u32 a = 0x3f800000 | (rng() & 0x7fffff);
		const u32 b = 0x3f800000 | (rng() & 0x7fffff);
		if ((a & 0x7fffff) && (b & 0x7fffff) &&
			(((u64(a & 0x7fffff) | 0x800000) * (u64(b & 0x7fffff) | 0x800000)) & 0x7fffff) < 0x8000)
			correctionInputs.push_back({a, b});
	}
	for (unsigned i = 0; i < 100000; i++)
	{
		const auto inputs = correctionInputs[i % correctionInputs.size()];
		const u32 a = i < 50000 ? inputs[0] ^ ((i & 1) << 31) : rng();
		const u32 b = i < 50000 ? inputs[1] ^ ((i & 2) << 30) : rng();
		const u32 c = i < std::size(edge) ? edge[i] : rng();
		for (size_t op : {1u, 7u})
		{
			const u64 expected = arithmeticOracle(op, a, b);
			if (baseline.scalar[op](a, b, 0, 0, 0) != expected || extended.scalar[op](a, b, 0, 0, 0) != expected)
			{
				std::printf("FAIL directed MUL %s %08x %08x\n", names[op], a, b);
				return 1;
			}
		}
		for (u32 sub : {0u, 1u})
			for (u32 overflow : {0u, 1u})
			{
				const u64 expected = baseline.integrated(a, b, c, overflow, sub);
				const u64 actual = extended.integrated(a, b, c, overflow, sub);
				if (expected != actual || integratedStatus[0] != integratedStatus[1])
				{
					std::printf("FAIL scalar MADD/MSUB %08x %08x %08x sub=%u overflow=%u\n", a, b, c, sub, overflow);
					return 1;
				}
			}
	}
	std::puts("PASS directed scalar Booth: 100000 MUL inputs vs PS2Float; 400000 integrated MADD/MSUB cases, result/lane/product flags vs AVX2");
	for (int workload = 0; workload < 2; workload++)
		for (int op = 0; op < 3; op++)
		{
			const std::array kernels = {op == 2 ? baseline.integrated : baseline.scalar[op == 0 ? 1 : 7],
				op == 2 ? extended.integrated : extended.scalar[op == 0 ? 1 : 7]};
			std::array<size_t, 2> cursor{};
			volatile u64 checksum = 0;
			const auto times = benchmarkPair([&](int backend) {
				const auto inputs = correctionInputs[cursor[backend]++ & 4095];
				checksum = kernels[backend](workload ? inputs[0] : 0x3fc12345, workload ? inputs[1] : 0x3fa54321, 0x3f812345, 0, 0);
			});
			std::printf("BENCH scalar %s %s AVX2 %.3f AVX512 %.3f ns/call speedup %.3f (paired median 9)\n",
				op == 0 ? "VU MUL" : op == 1 ? "EE MUL" : "VU MADD", workload ? "correction" : "ordinary",
				times[0], times[1], times[0] / times[1]);
		}
	if (testMMIJit() || testPPACGeneral() || testProductionMMIJit() || testHalfwordMultiply() || testPMFHLSLW() || testBroadcastD() || testPhase10() || testPhase11() || testClipDenormal() || testVPCMPDMemory() || testCSCThresholdSign() || testPADSBHAndPHMADHReductions() || testPMFHLWords() || testPhase15() || testQFSRV() || testVUMinMax() || TestInterpreterLeaves() || TestAVX2Leaves() || TestPhase2Allocator(s_fpuSoftMulExact) || TestUpperSoftInline() || TestFlagClassification())
		return 1;
	std::printf("generated bytes AVX2=%zu AVX512=%zu\n", baseline.bytes, extended.bytes);
	for (size_t op : {2u, 3u, 4u, 10u})
	{
		volatile u64 checksum = 0;
		const std::array kernels = {baseline.scalar[op], extended.scalar[op]};
		const auto times = benchmarkPair([&](int backend) { checksum = kernels[backend](0x40800000, 0x40000000, 0, 0, 0); });
		std::printf("BENCH %s AVX2 %.3f AVX512 %.3f ns/call (paired median 9) speedup %.3f\n", names[op], times[0], times[1], times[0] / times[1]);
	}
	munmap(code, 4 * 1024 * 1024);
}
