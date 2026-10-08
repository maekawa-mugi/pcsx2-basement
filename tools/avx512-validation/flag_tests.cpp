// SPDX-License-Identifier: GPL-3.0+
// microVU MAC/STATUS flag classification (mVUupdateFlags): the legacy
// MOVMSKPS/CMPEQPS sequence against the AVX-512 VFPCLASSPS/VPMOVD2M one,
// for every lane mask, several host registers and MXCSR.DAZ on and off.
#include "common/emitter/x86emitter.h"
#include <immintrin.h>
#include <array>
#include <cstdio>
#include <random>

using namespace x86Emitter;

namespace
{
	alignas(16) u32 g_flag_in[4];
	using FlagFn = u32 (*)();

	FlagFn EmitFlags(bool useK, int reg, u32 mask, bool daz)
	{
		const xRegisterSSE value(reg);
		const xRegisterSSE temp((reg + 1) & 15);
		xAlignPtr(16);
		const FlagFn fn = reinterpret_cast<FlagFn>(xGetAlignedCallTarget());
		xMOVAPS(value, ptr128[&g_flag_in]);
		if (useK)
		{
			xVFPCLASSPS(k1, value, static_cast<u8>(0x06 | (daz ? 0x20 : 0x00)));
			xVPMOVD2M(k2, value);
			xKMOVW(eax, k2);
			xKMOVW(ecx, k1);
			xSHL(eax, 4);
			xOR(eax, ecx);
			xAND(eax, mask | (mask << 4));
		}
		else
		{
			xMOVMSKPS(eax, value);
			xXOR.PS(temp, temp);
			xCMPEQ.PS(temp, value);
			xMOVMSKPS(ecx, temp);
			xAND(eax, mask);
			xSHL(eax, 4);
			xAND(ecx, mask);
			xOR(eax, ecx);
		}
		xRET();
		return fn;
	}
} // namespace

int TestFlagClassification()
{
	static constexpr u32 edge[] = {0, 0x80000000, 1, 0x80000001, 0x007fffff, 0x807fffff, 0x00800000,
		0x80800000, 0x3f800000, 0xbf800000, 0x7f7fffff, 0xff7fffff, 0x7f800000, 0xff800000, 0x7fc00000,
		0xffc00000, 0x7f800001, 0xff800001};
	std::mt19937 rng(0xf1a6);
	const u32 host_mxcsr = _mm_getcsr();
	u64 checks = 0;
	for (int daz = 0; daz < 2; daz++)
	{
		for (int reg : {0, 7, 8, 15})
		{
			for (u32 mask = 0; mask < 16; mask++)
			{
				const FlagFn legacy = EmitFlags(false, reg, mask, daz);
				const FlagFn k = EmitFlags(true, reg, mask, daz);
				for (u32 i = 0; i < 20000; i++)
				{
					for (int lane = 0; lane < 4; lane++)
					{
						const u32 pick = rng();
						g_flag_in[lane] = (i < 10000 || (pick & 3)) ? edge[pick % std::size(edge)] : static_cast<u32>(rng());
					}
					_mm_setcsr((host_mxcsr & ~0x0040u) | (daz ? 0x0040u : 0));
					const u32 expected = legacy();
					const u32 actual = k();
					_mm_setcsr(host_mxcsr);
					checks++;
					if (expected != actual)
					{
						std::printf("FAIL flag classification daz=%d reg=%d mask=%x in=%08x %08x %08x %08x: legacy=%02x k=%02x\n",
							daz, reg, mask, g_flag_in[0], g_flag_in[1], g_flag_in[2], g_flag_in[3], expected, actual);
						return 1;
					}
				}
			}
		}
	}
	std::printf("PASS microVU flag classification (VFPCLASSPS/VPMOVD2M vs MOVMSKPS/CMPEQPS): %llu vectors, "
				"all lane masks, DAZ on/off\n", (unsigned long long)checks);
	return 0;
}
