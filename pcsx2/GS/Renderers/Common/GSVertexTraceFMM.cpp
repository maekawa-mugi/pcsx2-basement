// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "GSVertexTrace.h"
#include "GS/GSState.h"
#include "GS/GSUtil.h"
#include <cfloat>

class CURRENT_ISA::GSVertexTraceFMM
{
	static constexpr GSVector4 s_minmax = GSVector4::cxpr(FLT_MAX, -FLT_MAX, 0.f, 0.f);

	template <GS_PRIM_CLASS primclass, u32 iip, u32 tme, u32 fst, u32 color>
	static void FindMinMax(GSVertexTrace& vt, const void* vertex, const u16* index, int count);

	template <GS_PRIM_CLASS primclass, u32 iip, u32 tme, u32 fst, u32 color>
	static constexpr GSVertexTrace::FindMinMaxPtr GetFMM();

public:
	static void Populate(GSVertexTrace& vt);
};

MULTI_ISA_UNSHARED_IMPL;

void CURRENT_ISA::GSVertexTracePopulateFunctions(GSVertexTrace& vt)
{
	GSVertexTraceFMM::Populate(vt);
}

template <GS_PRIM_CLASS primclass, u32 iip, u32 tme, u32 fst, u32 color>
constexpr GSVertexTrace::FindMinMaxPtr GSVertexTraceFMM::GetFMM()
{
	constexpr bool real_iip = primclass == GS_SPRITE_CLASS ? false : iip;
	constexpr bool real_fst = tme ? fst : false;

	return FindMinMax<primclass, real_iip, tme, real_fst, color>;
}

void GSVertexTraceFMM::Populate(GSVertexTrace& vt)
{
	#define InitUpdate3(P, IIP, TME, FST, COLOR) \
		vt.m_fmm[COLOR][FST][TME][IIP][P] = GetFMM<P, IIP, TME, FST, COLOR>();

	#define InitUpdate2(P, IIP, TME) \
		InitUpdate3(P, IIP, TME, 0, 0) \
		InitUpdate3(P, IIP, TME, 0, 1) \
		InitUpdate3(P, IIP, TME, 1, 0) \
		InitUpdate3(P, IIP, TME, 1, 1) \

	#define InitUpdate(P) \
		InitUpdate2(P, 0, 0) \
		InitUpdate2(P, 0, 1) \
		InitUpdate2(P, 1, 0) \
		InitUpdate2(P, 1, 1) \

	InitUpdate(GS_POINT_CLASS);
	InitUpdate(GS_LINE_CLASS);
	InitUpdate(GS_TRIANGLE_CLASS);
	InitUpdate(GS_SPRITE_CLASS);
}

template <GS_PRIM_CLASS primclass, u32 iip, u32 tme, u32 fst, u32 color>
void GSVertexTraceFMM::FindMinMax(GSVertexTrace& vt, const void* vertex, const u16* index, int count)
{
	const GSDrawingContext* context = vt.m_state->m_context;

	constexpr int n = GSUtil::GetClassVertexCount(primclass);

	GSVector4 tmin = s_minmax.xxxx();
	GSVector4 tmax = s_minmax.yyyy();
	GSVector4i tnan = GSVector4i::zero();
	GSVector4i cmin = GSVector4i::xffffffff();
	GSVector4i cmax = GSVector4i::zero();

	GSVector4i pmin = GSVector4i::xffffffff();
	GSVector4i pmax = GSVector4i::zero();

	const GSVertex* RESTRICT v = (GSVertex*)vertex;

	// Process 2 vertices at a time for increased efficiency
	auto processVertices = [&tmin, &tmax, &cmin, &cmax, &pmin, &pmax, &tnan](const GSVertex& v0, const GSVertex& v1, bool finalVertex)
	{
		if (color)
		{
			GSVector4i c0 = GSVector4i::load(v0.RGBAQ.U32[0]);
			GSVector4i c1 = GSVector4i::load(v1.RGBAQ.U32[0]);
			if (iip || finalVertex)
			{
				cmin = cmin.min_u8(c0.min_u8(c1));
				cmax = cmax.max_u8(c0.max_u8(c1));
			}
			else if (n == 2)
			{
				// For even n, we process v1 and v2 of the same prim
				// (For odd n, we process one vertex from each of two prims)
				GSVector4i c = c1; // second color is provoking in flat-shaded primitives
				cmin = cmin.min_u8(c);
				cmax = cmax.max_u8(c);
			}
		}

		if (tme)
		{
			if (!fst)
			{
				GSVector4 stq0 = GSVector4::cast(GSVector4i(v0.m[0]));
				GSVector4 stq1 = GSVector4::cast(GSVector4i(v1.m[0]));

				GSVector4 q;
				// Sprites always have indices == vertices, so we don't have to look at the index table here
				if (primclass == GS_SPRITE_CLASS)
					q = stq1.wwww();
				else
					q = stq0.wwww(stq1);

				// Note: If in the future this is changed in a way that causes parts of calculations to go unused,
				//       make sure to remove the z (rgba) field as it's often denormal.
				//       Then, use GSVector4::noopt() to prevent clang from optimizing out your "useless" shuffle
				//       e.g. stq = (stq.xyww() / stq.wwww()).noopt().xyww(stq);
				GSVector4 st = stq0.xyxy(stq1) / q;

				stq0 = st.xyww(primclass == GS_SPRITE_CLASS ? stq1 : stq0);
				stq1 = st.zwww(stq1);

				const GSVector4i nan0 = GSVector4i::cast(stq0 != stq0);
				const GSVector4i nan1 = GSVector4i::cast(stq1 != stq1);

				// Only update entries that are not NaN.
				tmin = tmin.blend32(tmin.min(stq0), GSVector4::cast(~nan0));
				tmin = tmin.blend32(tmin.min(stq1), GSVector4::cast(~nan1));
				tmax = tmax.blend32(tmax.max(stq0), GSVector4::cast(~nan0));
				tmax = tmax.blend32(tmax.max(stq1), GSVector4::cast(~nan1));

				tnan |= nan0 | nan1;
			}
			else
			{
				GSVector4i uv0(v0.m[1]);
				GSVector4i uv1(v1.m[1]);

				GSVector4 st0 = GSVector4(uv0.uph16()).xyxy();
				GSVector4 st1 = GSVector4(uv1.uph16()).xyxy();

				tmin = tmin.min(st0.min(st1));
				tmax = tmax.max(st0.max(st1));
			}
		}

		GSVector4i xyzf0(v0.m[1]);
		GSVector4i xyzf1(v1.m[1]);

		GSVector4i xy0 = xyzf0.upl16();
		GSVector4i zf0 = xyzf0.ywyw();
		GSVector4i xy1 = xyzf1.upl16();
		GSVector4i zf1 = xyzf1.ywyw();

		GSVector4i p0 = xy0.blend32<0xc>(primclass == GS_SPRITE_CLASS ? zf1 : zf0);
		GSVector4i p1 = xy1.blend32<0xc>(zf1);

		pmin = pmin.min_u32(p0.min_u32(p1));
		pmax = pmax.max_u32(p0.max_u32(p1));
	};

	if (n == 2)
	{
		for (int i = 0; i < count; i += 2)
		{
			processVertices(v[index[i + 0]], v[index[i + 1]], false);
		}
	}
	else if (iip || n == 1) // iip means final and non-final vertexes are treated the same
	{
		int i = 0;
#if defined(__AVX512F__) && defined(__AVX512BW__) && defined(__AVX512VL__) && defined(__AVX512DQ__)
		// Four vertices per ZMM, one per 128-bit lane, with the same lane operations as
		// processVertices. NaN lanes are masked out with a k-mask instead of a blend.
		// Needs proper testing: reducing the lanes at the end changes the order of the
		// min/max, which can flip the sign of a zero in tmin/tmax.
		if (count >= 8)
		{
			const auto load4 = [&](int base, int m) {
				__m512i r = _mm512_castsi128_si512(_mm_load_si128(reinterpret_cast<const __m128i*>(&v[index[base + 0]].m[m])));
				r = _mm512_inserti32x4(r, _mm_load_si128(reinterpret_cast<const __m128i*>(&v[index[base + 1]].m[m])), 1);
				r = _mm512_inserti32x4(r, _mm_load_si128(reinterpret_cast<const __m128i*>(&v[index[base + 2]].m[m])), 2);
				return _mm512_inserti32x4(r, _mm_load_si128(reinterpret_cast<const __m128i*>(&v[index[base + 3]].m[m])), 3);
			};

			__m512 ztmin = _mm512_broadcast_f32x4(tmin.m);
			__m512 ztmax = _mm512_broadcast_f32x4(tmax.m);
			__m512i zcmin = _mm512_broadcast_i32x4(cmin.m);
			__m512i zcmax = _mm512_broadcast_i32x4(cmax.m);
			__m512i zpmin = _mm512_broadcast_i32x4(pmin.m);
			__m512i zpmax = _mm512_broadcast_i32x4(pmax.m);
			u32 knan = 0;

			for (; i + 4 <= count; i += 4)
			{
				const __m512i m0 = load4(i, 0);
				const __m512i m1 = load4(i, 1);

				if (color)
				{
					// GSVector4i::load(RGBAQ.U32[0]): the colour in dword 0, zero elsewhere.
					const __m512i c = _mm512_maskz_shuffle_epi32(0x1111, m0, _MM_PERM_CCCC);
					zcmin = _mm512_min_epu8(zcmin, c);
					zcmax = _mm512_max_epu8(zcmax, c);
				}

				if (tme)
				{
					if (!fst)
					{
						const __m512 stq = _mm512_castsi512_ps(m0);
						const __m512 q = _mm512_permute_ps(stq, _MM_SHUFFLE(3, 3, 3, 3));
						// (s / q, t / q, q, q)
						const __m512 t = _mm512_mask_div_ps(q, 0x3333, stq, q);
						const __mmask16 nan = _mm512_cmp_ps_mask(t, t, _CMP_UNORD_Q);
						ztmin = _mm512_mask_min_ps(ztmin, static_cast<__mmask16>(~nan), ztmin, t);
						ztmax = _mm512_mask_max_ps(ztmax, static_cast<__mmask16>(~nan), ztmax, t);
						knan |= nan;
					}
					else
					{
						const __m512 st = _mm512_permute_ps(
							_mm512_cvtepi32_ps(_mm512_unpackhi_epi16(m1, _mm512_setzero_si512())), _MM_SHUFFLE(1, 0, 1, 0));
						ztmin = _mm512_min_ps(ztmin, st);
						ztmax = _mm512_max_ps(ztmax, st);
					}
				}

				const __m512i xy = _mm512_unpacklo_epi16(m1, _mm512_setzero_si512());
				const __m512i zf = _mm512_shuffle_epi32(m1, _MM_PERM_DBDB);
				const __m512i p = _mm512_mask_blend_epi32(0xcccc, xy, zf);
				zpmin = _mm512_min_epu32(zpmin, p);
				zpmax = _mm512_max_epu32(zpmax, p);
			}

			alignas(64) float ft[2][16];
			alignas(64) u32 it[4][16];
			_mm512_store_ps(ft[0], ztmin);
			_mm512_store_ps(ft[1], ztmax);
			_mm512_store_si512(it[0], zcmin);
			_mm512_store_si512(it[1], zcmax);
			_mm512_store_si512(it[2], zpmin);
			_mm512_store_si512(it[3], zpmax);
			u32 nanbits = 0;
			for (int l = 0; l < 4; l++)
			{
				tmin = tmin.min(GSVector4::load<true>(&ft[0][l * 4]));
				tmax = tmax.max(GSVector4::load<true>(&ft[1][l * 4]));
				cmin = cmin.min_u8(GSVector4i::load<true>(&it[0][l * 4]));
				cmax = cmax.max_u8(GSVector4i::load<true>(&it[1][l * 4]));
				pmin = pmin.min_u32(GSVector4i::load<true>(&it[2][l * 4]));
				pmax = pmax.max_u32(GSVector4i::load<true>(&it[3][l * 4]));
				nanbits |= (knan >> (l * 4)) & 0xf;
			}
			tnan |= GSVector4i(_mm_movm_epi32(static_cast<__mmask8>(nanbits)));
		}
#endif
		for (; i < (count - 1); i += 2) // 2x loop unroll
		{
			processVertices(v[index[i + 0]], v[index[i + 1]], true);
		}
		if (count & 1)
		{
			// Compiler optimizations go!
			// (And if they don't, it's only one vertex out of many)
			processVertices(v[index[i]], v[index[i]], true);
		}
	}
	else if (n == 3)
	{
		int i = 0;
		for (; i < (count - 3); i += 6)
		{
			processVertices(v[index[i + 0]], v[index[i + 3]], false);
			processVertices(v[index[i + 1]], v[index[i + 4]], false);
			processVertices(v[index[i + 2]], v[index[i + 5]], true);
		}
		if (count & 1)
		{
			processVertices(v[index[i + 0]], v[index[i + 1]], false);
			// Compiler optimizations go!
			// (And if they don't, it's only one vertex out of many)
			processVertices(v[index[i + 2]], v[index[i + 2]], true);
		}
	}
	else
	{
		pxAssertRel(0, "Bad n value");
	}

	GSVector4 o(context->XYOFFSET);
	GSVector4 s(1.0f / 16, 1.0f / 16, 2.0f, 1.0f);

	vt.m_min.p = (GSVector4(pmin) - o) * s;
	vt.m_max.p = (GSVector4(pmax) - o) * s;

	// Fix signed int conversion
	vt.m_min.p = vt.m_min.p.insert32<0, 2>(GSVector4::load((float)(u32)pmin.extract32<2>()));
	vt.m_max.p = vt.m_max.p.insert32<0, 2>(GSVector4::load((float)(u32)pmax.extract32<2>()));

	if (tme)
	{
		if (fst)
		{
			s = GSVector4(1.0f / 16, 1.0f).xxyy();
		}
		else
		{
			s = GSVector4(1 << context->TEX0.TW, 1 << context->TEX0.TH, 1, 1);
		}

		vt.m_min.t = tmin * s;
		vt.m_max.t = tmax * s;

		if (!fst)
			vt.nan.value = tnan.mask() & ~4; // Remove pad bit.
	}
	else
	{
		vt.m_min.t = GSVector4::zero();
		vt.m_max.t = GSVector4::zero();
		vt.nan.value = 0;
	}

	if (color)
	{
		vt.m_min.c = cmin.u8to32();
		vt.m_max.c = cmax.u8to32();
	}
	else
	{
		vt.m_min.c = GSVector4i::zero();
		vt.m_max.c = GSVector4i::zero();
	}
}
