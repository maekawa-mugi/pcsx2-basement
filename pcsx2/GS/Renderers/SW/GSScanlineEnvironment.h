// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GS/GSLocalMemory.h"
#include "GS/GSVector.h"

#include <cstdio>
#include <string>

union GSScanlineSelector
{
	struct
	{
		u32 fpsm  : 2; // 0
		u32 zpsm  : 2; // 2
		u32 ztst  : 2; // 4 (0: off, 1: write, 2: test (ge), 3: test (g))
		u32 atst  : 3; // 6
		u32 afail : 2; // 9
		u32 iip   : 1; // 11
		u32 tfx   : 3; // 12
		u32 tcc   : 1; // 15
		u32 fst   : 1; // 16
		u32 ltf   : 1; // 17
		u32 tlu   : 1; // 18
		u32 fge   : 1; // 19
		u32 date  : 1; // 20
		u32 abe   : 1; // 21
		u32 aba   : 2; // 22
		u32 abb   : 2; // 24
		u32 abc   : 2; // 26
		u32 abd   : 2; // 28
		u32 pabe  : 1; // 30
		u32 aa1   : 1; // 31

		u32 fwrite    : 1; // 32
		u32 ftest     : 1; // 33
		u32 rfb       : 1; // 34
		u32 zwrite    : 1; // 35
		u32 ztest     : 1; // 36
		u32 zoverflow : 1; // 37 (z max >= 0x80000000)
		u32 zclamp    : 1; // 38
		u32 wms       : 2; // 39
		u32 wmt       : 2; // 41
		u32 datm      : 1; // 43
		u32 colclamp  : 1; // 44
		u32 fba       : 1; // 45
		u32 dthe      : 1; // 46
		u32 prim      : 2; // 47

		u32 edge   : 1; // 49
		u32 tw     : 3; // 50 (encodes values between 3 -> 10, texture cache makes sure it is at least 3)
		u32 lcm    : 1; // 53
		u32 mmin   : 2; // 54
		u32 notest : 1; // 55 (no ztest, no atest, no date, no scissor test, and horizontally aligned to 4 pixels)
		// TODO: 1D texture flag? could save 2 texture reads and 4 lerps with bilinear, and also the texture coordinate clamp/wrap code in one direction
		u32 zequal : 1; // 56
		u32 breakpoint : 1; // Insert a trap to stop the program, helpful to stop debugger on a program
	};

	struct
	{
		u32 _pad1  : 22;
		u32 ababcd :  8;
		u32 _pad2  :  2;

		u32 fb    : 2;
		u32 _pad3 : 1;
		u32 zb    : 2;
	};

	struct
	{
		u32 lo;
		u32 hi;
	};

	u64 key;

	GSScanlineSelector() = default;
	GSScanlineSelector(u64 k)
		: key(k)
	{
	}

	operator u32() const { return lo; }
	operator u64() const { return key; }

	bool IsSolidRect() const
	{
		return prim == GS_SPRITE_CLASS && iip == 0 && tfx == TFX_NONE && abe == 0 && ztst <= 1 && atst <= 1 && date == 0 && fge == 0;
	}

	std::string to_string() const
	{
		char str[1024];
		std::snprintf(str, std::size(str),
			"fpsm:%d zpsm:%d ztst:%d ztest:%d atst:%d afail:%d iip:%d rfb:%d fb:%d zb:%d zw:%d "
			"tfx:%d tcc:%d fst:%d ltf:%d tlu:%d wms:%d wmt:%d mmin:%d lcm:%d tw:%d "
			"fba:%d cclamp:%d date:%d datm:%d "
			"prim:%d abe:%d %d%d%d%d fge:%d dthe:%d notest:%d pabe:%d aa1:%d "
			"fwrite:%d ftest:%d zoverflow:%d zequal:%d zclamp:%d edge:%d",
			fpsm, zpsm, ztst, ztest, atst, afail, iip, rfb, fb, zb, zwrite,
			tfx, tcc, fst, ltf, tlu, wms, wmt, mmin, lcm, tw,
			fba, colclamp, date, datm,
			prim, abe, aba, abb, abc, abd, fge, dthe, notest, pabe, aa1,
			fwrite, ftest, zoverflow, zequal, zclamp, edge);
		return str;
	}

	void Print() const
	{
		fprintf(stderr, "%s\n", to_string().c_str());
	}
};

struct alignas(32) GSScanlineGlobalData // per batch variables, this is like a pixel shader constant buffer
{
	GSScanlineSelector sel;

	// - the data of vm, tex may change, multi-threaded drawing must be finished before that happens, clut and dimx are copies
	// - tex is a cached texture, it may be recycled to free up memory, its absolute address cannot be compiled into code
	// - row and column pointers are allocated once and never change or freed, thier address can be used directly

	void* vm;
	const void* tex[7];
	u32* clut;
	GSVector4i* dimx;

	GSOffset fbo;
	GSOffset zbo;
	const GSVector2i* fzbr;
	const GSVector2i* fzbc;

	GSVector4i aref;
	GSVector4i afix;
	struct { GSVector4i min, max, minmax, mask, invmask; } t; // [u] x 4 [v] x 4

#if _M_SSE >= 0x501

	u32 fm, zm;
	u32 frb, fga;
	GSVector8 mxl;
	GSVector8 k; // TEX1.K * 0x10000
	GSVector8 l; // TEX1.L * -0x10000
	struct { GSVector8i i, f; } lod; // lcm == 1

#else

	GSVector4i fm, zm;
	GSVector4i frb, fga;
	GSVector4 mxl;
	GSVector4 k; // TEX1.K * 0x10000
	GSVector4 l; // TEX1.L * -0x10000
	struct { GSVector4i i, f; } lod; // lcm == 1

#endif

#ifdef ARCH_ARM64
	// Mini version of constant data for ARM64, we don't need all of it
	alignas(16) u32 const_test_128b[8][4] = {
		{0x00000000, 0x00000000, 0x00000000, 0x00000000},
		{0xffffffff, 0x00000000, 0x00000000, 0x00000000},
		{0xffffffff, 0xffffffff, 0x00000000, 0x00000000},
		{0xffffffff, 0xffffffff, 0xffffffff, 0x00000000},
		{0x00000000, 0xffffffff, 0xffffffff, 0xffffffff},
		{0x00000000, 0x00000000, 0xffffffff, 0xffffffff},
		{0x00000000, 0x00000000, 0x00000000, 0xffffffff},
		{0x00000000, 0x00000000, 0x00000000, 0x00000000},
	};
	alignas(16) u16 const_movemaskw_mask[8] = {0x3, 0xc, 0x30, 0xc0, 0x300, 0xc00, 0x3000, 0xc000};
	alignas(16) float const_log2_coef[4] = {
		0.204446009836232697516f,
		-1.04913055217340124191f,
		2.28330284476918490682f,
		1.0f};
#endif
};

struct alignas(32) GSScanlineLocalData // per prim variables, each thread has its own
{
#if _M_SSE >= 0x501

	struct skip { GSVector8 z, s, t, q; GSVector8i rb, ga, f, _pad; } d[8];
	struct step { GSVector4 stq; struct { u32 rb, ga; } c; struct { u64 z; u32 f; } p; } d8;
	struct { u32 z, f; } p;
	struct { GSVector8i rb, ga; } c;

	// these should be stored on stack as normal local variables (no free regs to use, esp cannot be saved to anywhere, and we need an aligned stack)

	struct
	{
		GSVector8 z0, z1;
		GSVector8i f;
		GSVector8 s, t, q;
		GSVector8i rb, ga;
		GSVector8i zs, zd;
		GSVector8i uf, vf;
		GSVector8i cov;

		// mipmapping

		struct { GSVector8i i, f; } lod;
		GSVector8i uv[2];
		GSVector8i uv_minmax[2];
		GSVector8i trb, tga;
		GSVector8i test;
	} temp;

#else

	struct skip { GSVector4 z, s, t, q; GSVector4i rb, ga, f, _pad; } d[4];
	struct step { GSVector4 z, stq; GSVector4i c, f; } d4;
	struct { GSVector4i rb, ga; } c;
	struct { GSVector4i z, f; } p;

	// these should be stored on stack as normal local variables (no free regs to use, esp cannot be saved to anywhere, and we need an aligned stack)

	struct
	{
		GSVector4 z0, z1;
		GSVector4i f;
		GSVector4 s, t, q;
		GSVector4i rb, ga;
		GSVector4i zs, zd;
		GSVector4i uf, vf;
		GSVector4i cov;

		// mipmapping

		struct { GSVector4i i, f; } lod;
		GSVector4i uv[2];
		GSVector4i uv_minmax[2];
		GSVector4i trb, tga;
		GSVector4i test;
	} temp;

#endif

	//

	const GSScanlineGlobalData* gd;
};

namespace GSScanlineConstantData
{
	static constexpr float log2_coef[] = {
		0.204446009836232697516f,
		-1.04913055217340124191f,
		2.28330284476918490682f,
		1.0f
	};
};

// Constant shared by all threads (to reduce cache miss)
struct alignas(64) GSScanlineConstantData256B
{
	// All AVX processors support unaligned access with little to no penalty as long as you don't cross a cache line.
	// Take advantage of that to store single vectors that we index with single-element alignment
	alignas(32) u8 m_test[24] = {
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	};
	float m_log2_coef[4] = {};
	alignas(64) float m_shift[16] = {
		8.0f, -7.0f, -6.0f, -5.0f, -4.0f, -3.0f, -2.0f, -1.0f,
		0.0f,  1.0f,  2.0f,  3.0f,  4.0f,  5.0f,  6.0f,  7.0f,
	};
	// AVX-512 masked WritePixel. The 8 pixels of a 32-bit write sit at dwords 0,1,4,5,8,9,12,13
	// of a 64-byte span. m_wp_dwords moves them there; m_wp_bytes[fz][psm] maps each byte of
	// the span to the fzm mask bit (from VPMOVM2B) of its pixel, or to byte 63 (always 0) for
	// bytes not written: other dwords, and the top byte of 24-bit pixels.
	alignas(64) u8 m_wp_bytes[2][2][64] = {};
	alignas(64) u32 m_wp_dwords[16] = {};
	// PSHUFB controls (identical in both 128-bit lanes). m_rgba_shuf orders PACKUSWB(c0, c1)
	// (R0 B0 R1 B1 .. | G0 A0 G1 A1 ..) as RGBA pixels; m_clamp16_shuf zero-extends the low 8 bytes to words.
	alignas(32) u8 m_rgba_shuf[32] = {};
	alignas(32) u8 m_clamp16_shuf[32] = {};
	// 0x00ff in every word (colclamp-less RGBA masking, low-byte split).
	alignas(32) u16 m_word_lo_mask[16] = {};
	// VBMI VPMULTISHIFTQB controls for the 5551 <-> 8888 conversions (same windows as MMI PEXT5/PPAC5).
	// m_c5_expand_ctrl: RGBA5551 per dword -> bytes [R<<3, G<<3, B<<3, A<<7]; the rb/ga masks
	// are applied after the 16-bit split ([R,0,B,0] / [G,0,A,0]).
	// m_c5_pack_ctrl: bytes [R,G,B,A] per dword -> [R>>3, G>>3, B>>3, A>>7], then the
	// mask / PMADDUBSW {1,32} / PMADDWD {1,1024} constants pack them into 5551.
	alignas(32) u8 m_c5_expand_ctrl[32] = {};
	alignas(32) u8 m_c5_pack_ctrl[32] = {};
	alignas(32) u32 m_c5_rb_mask[8] = {};
	alignas(32) u32 m_c5_ga_mask[8] = {};
	alignas(32) u32 m_c5_pack_mask[8] = {};
	alignas(32) u16 m_c5_pack_w8[16] = {};
	alignas(32) u32 m_c5_pack_w16[8] = {};

	constexpr GSScanlineConstantData256B()
	{
		using namespace GSScanlineConstantData;
		{
			const u8 expand[8] = {61, 2, 7, 8, 29, 34, 39, 40};
			const u8 pack[8] = {3, 11, 19, 31, 35, 43, 51, 63};
			for (int i = 0; i < 32; i++)
			{
				m_c5_expand_ctrl[i] = expand[i % 8];
				m_c5_pack_ctrl[i] = pack[i % 8];
			}
			for (int i = 0; i < 8; i++)
			{
				m_c5_rb_mask[i] = 0x00f800f8;
				m_c5_ga_mask[i] = 0x008000f8;
				m_c5_pack_mask[i] = 0x011f1f1f;
				m_c5_pack_w16[i] = 0x04000001;
			}
			for (int i = 0; i < 16; i++)
				m_c5_pack_w8[i] = 0x2001;
		}
		for (size_t i = 0; i < std::size(m_word_lo_mask); i++)
			m_word_lo_mask[i] = 0x00ff;
		for (int lane = 0; lane < 2; lane++)
		{
			for (int i = 0; i < 4; i++)
			{
				m_rgba_shuf[lane * 16 + i * 4 + 0] = static_cast<u8>(2 * i);
				m_rgba_shuf[lane * 16 + i * 4 + 1] = static_cast<u8>(8 + 2 * i);
				m_rgba_shuf[lane * 16 + i * 4 + 2] = static_cast<u8>(2 * i + 1);
				m_rgba_shuf[lane * 16 + i * 4 + 3] = static_cast<u8>(9 + 2 * i);
			}
			for (int i = 0; i < 8; i++)
			{
				m_clamp16_shuf[lane * 16 + i * 2] = static_cast<u8>(i);
				m_clamp16_shuf[lane * 16 + i * 2 + 1] = 0x80;
			}
		}
		for (size_t n = 0; n < std::size(log2_coef); ++n)
		{
			m_log2_coef[n] = log2_coef[n];
		}
		for (int fz = 0; fz < 2; fz++)
		{
			for (int psm = 0; psm < 2; psm++)
			{
				for (int j = 0; j < 64; j++)
				{
					const int d = j / 4;
					const int pixel = (d / 4) * 2 + (d % 4);
					const bool written = (d % 4) < 2 && !(psm == 1 && (j % 4) == 3);
					m_wp_bytes[fz][psm][j] = written ? static_cast<u8>((pixel < 4 ? 2 * pixel : 16 + 2 * (pixel - 4)) + 8 * fz) : 63;
				}
			}
		}
		for (int d = 0; d < 16; d++)
			m_wp_dwords[d] = (d % 4) < 2 ? (d / 4) * 2 + (d % 4) : 0;
	}
};

struct alignas(64) GSScanlineConstantData128B
{
	alignas(16) u32 m_test[8][4] = {
		{0x00000000, 0x00000000, 0x00000000, 0x00000000},
		{0xffffffff, 0x00000000, 0x00000000, 0x00000000},
		{0xffffffff, 0xffffffff, 0x00000000, 0x00000000},
		{0xffffffff, 0xffffffff, 0xffffffff, 0x00000000},
		{0x00000000, 0xffffffff, 0xffffffff, 0xffffffff},
		{0x00000000, 0x00000000, 0xffffffff, 0xffffffff},
		{0x00000000, 0x00000000, 0x00000000, 0xffffffff},
		{0x00000000, 0x00000000, 0x00000000, 0x00000000},
	};
	alignas(16) float m_shift[5][4] = {
		{ 4.0f  , 4.0f  , 4.0f  , 4.0f},
		{ 0.0f  , 1.0f  , 2.0f  , 3.0f},
		{ -1.0f , 0.0f  , 1.0f  , 2.0f},
		{ -2.0f , -1.0f , 0.0f  , 1.0f},
		{ -3.0f , -2.0f , -1.0f , 0.0f},
	};
	alignas(16) float m_log2_coef[4][4] = {};
	alignas(16) u16 m_word_lo_mask[8] = {};

	constexpr GSScanlineConstantData128B()
	{
		using namespace GSScanlineConstantData;
		for (size_t i = 0; i < std::size(m_word_lo_mask); i++)
			m_word_lo_mask[i] = 0x00ff;
		for (size_t n = 0; n < std::size(log2_coef); ++n)
		{
			for (size_t i = 0; i < 4; ++i)
				m_log2_coef[n][i] = log2_coef[n];
		}
	}
};

extern const GSScanlineConstantData256B g_const_256b;
extern const GSScanlineConstantData128B g_const_128b;
