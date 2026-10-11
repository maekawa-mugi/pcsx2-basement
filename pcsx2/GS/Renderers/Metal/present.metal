// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "GSMTLShaderCommon.h"

using namespace metal;

// use vs_convert from convert.metal

static float4 ps_crt(float4 color, int i)
{
	constexpr float4 mask[4] =
	{
		float4(1, 0, 0, 0),
		float4(0, 1, 0, 0),
		float4(0, 0, 1, 0),
		float4(1, 1, 1, 0),
	};

	return color * saturate(mask[i] + 0.5f);
}

static float4 ps_scanlines(float4 color, int i)
{
	constexpr float4 mask[2] =
	{
		float4(1, 1, 1, 0),
		float4(0, 0, 0, 0)
	};

	return color * saturate(mask[i] + 0.5f);
}

// Area resampling integrates the source texels covered by each output pixel.
static float4 present_sample(ConvertPSRes res, float2 uv, constant GSMTLPresentPSUniform& cb)
{
	if (cb.area_filter < 0.5f)
		return res.sample(uv);
	float2 footprint = abs(cb.source_size / max(abs(cb.target_size), float2(1.0f)));
	float2 lo = uv * cb.source_resolution - 0.5f * footprint;
	float2 hi = lo + footprint;
	int2 begin = int2(floor(lo));
	int2 end = int2(ceil(hi));
	int2 max_texel = int2(cb.source_resolution) - int2(1);
	float4 sum = float4(0.0f);
	for (int y = begin.y; y < end.y; y++)
	{
		for (int x = begin.x; x < end.x; x++)
		{
			float2 overlap = max(float2(0.0f), min(hi, float2(x + 1, y + 1)) - max(lo, float2(x, y)));
			sum += res.texture.read(uint2(clamp(int2(x, y), int2(0), max_texel))) * (overlap.x * overlap.y);
		}
	}
	return sum / max(footprint.x * footprint.y, 1e-8f);
}

fragment float4 ps_present_copy(ConvertShaderData data [[stage_in]], ConvertPSRes res,
	constant GSMTLPresentPSUniform& cb [[buffer(GSMTLBufferIndexUniforms)]])
{
	return present_sample(res, data.t, cb);
}


fragment float4 ps_filter_scanlines(ConvertShaderData data [[stage_in]], ConvertPSRes res,
	constant GSMTLPresentPSUniform& cb [[buffer(GSMTLBufferIndexUniforms)]])
{
	return ps_scanlines(present_sample(res, data.t, cb), uint(data.p.y) % 2);
}

fragment float4 ps_filter_diagonal(ConvertShaderData data [[stage_in]], ConvertPSRes res,
	constant GSMTLPresentPSUniform& cb [[buffer(GSMTLBufferIndexUniforms)]])
{
	uint4 p = uint4(data.p);
	return ps_crt(present_sample(res, data.t, cb), (p.x + (p.y % 3)) % 3);
}

fragment float4 ps_filter_triangular(ConvertShaderData data [[stage_in]], ConvertPSRes res,
	constant GSMTLPresentPSUniform& cb [[buffer(GSMTLBufferIndexUniforms)]])
{
	uint4 p = uint4(data.p);
	uint val = ((p.x + ((p.y >> 1) & 1) * 3) >> 1) % 3;
	return ps_crt(present_sample(res, data.t, cb), val);
}

fragment float4 ps_filter_complex(ConvertShaderData data [[stage_in]], ConvertPSRes res,
	constant GSMTLPresentPSUniform& cb [[buffer(GSMTLBufferIndexUniforms)]])
{
	float2 texdim = float2(res.texture.get_width(), res.texture.get_height());
	float factor = (0.9f - 0.4f * cos(2.f * M_PI_F * data.t.y * texdim.y));
	float ycoord = (floor(data.t.y * texdim.y) + 0.5f) / texdim.y;

	return factor * present_sample(res, float2(data.t.x, ycoord), cb);
}

#define MaskingType 4                      //[1|2|3|4] The type of CRT shadow masking used. 1: compressed TV style, 2: Aperture-grille, 3: Stretched VGA style, 4: VGA style.
#define ScanBrightness -8.00               //[-16.0 to 1.0] The overall brightness of the scanline effect. Lower for darker, higher for brighter.
#define FilterCRTAmount -3.00              //[-4.0 to 1.0] The amount of filtering used, to replicate the TV CRT look. Lower for less, higher for more.
#define HorizontalWarp 0.00                //[0.0 to 0.1] The distortion warping effect for the horizontal (x) axis of the screen. Use small increments.
#define VerticalWarp 0.00                  //[0.0 to 0.1] The distortion warping effect for the verticle (y) axis of the screen. Use small increments.
#define MaskAmountDark 0.50                //[0.0 to 1.0] The value of the dark masking line effect used. Lower for darker lower end masking, higher for brighter.
#define MaskAmountLight 1.50               //[0.0 to 2.0] The value of the light masking line effect used. Lower for darker higher end masking, higher for brighter.
#define BloomPixel -1.50                   //[-2.0 -0.5] Pixel bloom radius. Higher for increased softness of bloom.
#define BloomScanLine -2.0                 //[-4.0 -1.0] Scanline bloom radius. Higher for increased softness of bloom.
#define BloomAmount 0.15                   //[0.0 1.0] Bloom intensity. Higher for brighter.
#define Shape 2.0                          //[0.0 10.0] Kernal filter shape. Lower values will darken image and introduce moire patterns if used with curvature.
#define UseShadowMask 1                    //[0 or 1] Enables, or disables the use of the CRT shadow mask. 0 is disabled, 1 is enabled.

struct LottesCRTPass
{
	thread ConvertPSRes& res;
	constant GSMTLPresentPSUniform& uniform;
	LottesCRTPass(thread ConvertPSRes& res, constant GSMTLPresentPSUniform& uniform): res(res), uniform(uniform) {}

	float ToLinear1(float c)
	{
		return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
	}

	float3 ToLinear(float3 c)
	{
		return float3(ToLinear1(c.r), ToLinear1(c.g), ToLinear1(c.b));
	}

	float ToSrgb1(float c)
	{
		return c < 0.0031308 ? c * 12.92 : 1.055 * pow(c, 0.41666) - 0.055;
	}

	float3 ToSrgb(float3 c)
	{
		return float3(ToSrgb1(c.r), ToSrgb1(c.g), ToSrgb1(c.b));
	}

	float3 Fetch(float2 pos, float2 off)
	{
		pos = (floor(pos * uniform.target_size + off) + float2(0.5, 0.5)) / uniform.target_size;
		if (max(abs(pos.x - 0.5), abs(pos.y - 0.5)) > 0.5)
		{
			return float3(0.0, 0.0, 0.0);
		}
		else
		{
			return ToLinear(present_sample(res, pos.xy, uniform).rgb);
		}
	}

	float2 Dist(float2 pos)
	{
		pos = pos * float2(640, 480);

		return -((pos - floor(pos)) - float2(0.5, 0.5));
	}

	float Gaus(float pos, float scale)
	{
		return exp2(scale * pow(abs(pos), Shape));
	}

	float3 Horz3(float2 pos, float off)
	{
		float3 b = Fetch(pos, float2(-1.0, off));
		float3 c = Fetch(pos, float2(0.0, off));
		float3 d = Fetch(pos, float2(1.0, off));
		float dst = Dist(pos).x;

		// Convert distance to weight.
		float scale = FilterCRTAmount;
		float wb = Gaus(dst - 1.0, scale);
		float wc = Gaus(dst + 0.0, scale);
		float wd = Gaus(dst + 1.0, scale);

		return (b * wb + c * wc + d * wd) / (wb + wc + wd);
	}

	float3 Horz5(float2 pos, float off)
	{
		float3 a = Fetch(pos, float2(-2.0, off));
		float3 b = Fetch(pos, float2(-1.0, off));
		float3 c = Fetch(pos, float2(0.0, off));
		float3 d = Fetch(pos, float2(1.0, off));
		float3 e = Fetch(pos, float2(2.0, off));
		float dst = Dist(pos).x;

		// Convert distance to weight.
		float scale = FilterCRTAmount;

		float wa = Gaus(dst - 2.0, scale);
		float wb = Gaus(dst - 1.0, scale);
		float wc = Gaus(dst + 0.0, scale);
		float wd = Gaus(dst + 1.0, scale);
		float we = Gaus(dst + 2.0, scale);

		return (a * wa + b * wb + c * wc + d * wd + e * we) / (wa + wb + wc + wd + we);
	}

	float3 Horz7(float2 pos, float off)
	{
		float3 a = Fetch(pos, float2(-3.0, off));
		float3 b = Fetch(pos, float2(-2.0, off));
		float3 c = Fetch(pos, float2(-1.0, off));
		float3 d = Fetch(pos, float2( 0.0, off));
		float3 e = Fetch(pos, float2( 1.0, off));
		float3 f = Fetch(pos, float2( 2.0, off));
		float3 g = Fetch(pos, float2( 3.0, off));

		float dst = Dist(pos).x;
		// Convert distance to weight.
		float scale = BloomPixel;
		float wa = Gaus(dst - 3.0, scale);
		float wb = Gaus(dst - 2.0, scale);
		float wc = Gaus(dst - 1.0, scale);
		float wd = Gaus(dst + 0.0, scale);
		float we = Gaus(dst + 1.0, scale);
		float wf = Gaus(dst + 2.0, scale);
		float wg = Gaus(dst + 3.0, scale);

		// Return filtered sample.
		return (a * wa + b * wb + c * wc + d * wd + e * we + f * wf + g * wg) / (wa + wb + wc + wd + we + wf + wg);
	}

	// Return scanline weight.
	float Scan(float2 pos, float off)
	{
		float dst = Dist(pos).y;
		return Gaus(dst + off, ScanBrightness);
	}

	float BloomScan(float2 pos, float off)
	{
		float dst = Dist(pos).y;
		
		return Gaus(dst + off, BloomScanLine);
	}

	float3 Tri(float2 pos)
	{
		float3 a = Horz3(pos, -1.0);
		float3 b = Horz5(pos, 0.0);
		float3 c = Horz3(pos, 1.0);

		float wa = Scan(pos, -1.0);
		float wb = Scan(pos, 0.0);
		float wc = Scan(pos, 1.0);

		return (a * wa) + (b * wb) + (c * wc);
	}

	float3 Bloom(float2 pos)
	{
		float3 a = Horz5(pos,-2.0);
		float3 b = Horz7(pos,-1.0);
		float3 c = Horz7(pos, 0.0);
		float3 d = Horz7(pos, 1.0);
		float3 e = Horz5(pos, 2.0);

		float wa = BloomScan(pos,-2.0);
		float wb = BloomScan(pos,-1.0); 
		float wc = BloomScan(pos, 0.0);
		float wd = BloomScan(pos, 1.0);
		float we = BloomScan(pos, 2.0);

		return a * wa + b * wb + c * wc + d * wd + e * we;
	}

	float2 Warp(float2 pos)
	{
		pos = pos * 2.0 - 1.0;
		pos *= float2(1.0 + (pos.y * pos.y) * HorizontalWarp, 1.0 + (pos.x * pos.x) * VerticalWarp);
		return pos * 0.5 + 0.5;
	}

	float3 Mask(float2 pos)
	{
#if MaskingType == 1
		// Very compressed TV style shadow mask.
		float lines = MaskAmountLight;
		float odd = 0.0;

		if (fract(pos.x / 6.0) < 0.5)
		{
			odd = 1.0;
		}
		if (fract((pos.y + odd) / 2.0) < 0.5)
		{
			lines = MaskAmountDark;
		}
		pos.x = fract(pos.x / 3.0);
		float3 mask = float3(MaskAmountDark, MaskAmountDark, MaskAmountDark);

		if (pos.x < 0.333)
		{
			mask.r = MaskAmountLight;
		}
		else if (pos.x < 0.666)
		{
			mask.g = MaskAmountLight;
		}
		else
		{
			mask.b = MaskAmountLight;
		}

		mask *= lines;

		return mask;

#elif MaskingType == 2
		// Aperture-grille.
		pos.x = fract(pos.x / 3.0);
		float3 mask = float3(MaskAmountDark, MaskAmountDark, MaskAmountDark);

		if (pos.x < 0.333)
		{
			mask.r = MaskAmountLight;
		}
		else if (pos.x < 0.666)
		{
			mask.g = MaskAmountLight;
		}
		else
		{
			mask.b = MaskAmountLight;
		}

		return mask;

#elif MaskingType == 3
		// Stretched VGA style shadow mask (same as prior shaders).
		pos.x += pos.y * 3.0;
		float3 mask = float3(MaskAmountDark, MaskAmountDark, MaskAmountDark);
		pos.x = fract(pos.x / 6.0);

		if (pos.x < 0.333)
		{
			mask.r = MaskAmountLight;
		}
		else if (pos.x < 0.666)
		{
			mask.g = MaskAmountLight;
		}
		else
		{
			mask.b = MaskAmountLight;
		}

		return mask;

#else
		// VGA style shadow mask.
		pos.xy = floor(pos.xy * float2(1.0, 0.5));
		pos.x += pos.y * 3.0;

		float3 mask = float3(MaskAmountDark, MaskAmountDark, MaskAmountDark);
		pos.x = fract(pos.x / 6.0);

		if (pos.x < 0.333)
		{
			mask.r = MaskAmountLight;
		}
		else if (pos.x < 0.666)
		{
			mask.g = MaskAmountLight;
		}
		else
		{
			mask.b = MaskAmountLight;
		}
		return mask;
#endif
	}

	float4 Run(float4 fragcoord)
	{
		float4 color;
		fragcoord -= uniform.target_rect;
		float2 inSize = uniform.target_resolution - (2 * uniform.target_rect.xy);

		float2 pos = Warp(fragcoord.xy / inSize);
		color.rgb = Tri(pos);
		color.rgb += Bloom(pos) * BloomAmount;
	#if UseShadowMask
		color.rgb *= Mask(fragcoord.xy);
	#endif
		color.rgb = ToSrgb(color.rgb);

		return color;
	}
};

fragment float4 ps_filter_lottes(ConvertShaderData data [[stage_in]], ConvertPSRes res,
	constant GSMTLPresentPSUniform& uniform [[buffer(GSMTLBufferIndexUniforms)]])
{
	return LottesCRTPass(res, uniform).Run(data.p);
}

fragment float4 ps_4x_rgss(ConvertShaderData data [[stage_in]], ConvertPSRes res,
	constant GSMTLPresentPSUniform& cb [[buffer(GSMTLBufferIndexUniforms)]])
{
	float2 dxy = float2(dfdx(data.t.x), dfdy(data.t.y));
	float3 color = 0;

	float s = 1.0/8.0;
	float l = 3.0/8.0;

	color += present_sample(res, data.t + float2( s, l) * dxy, cb).rgb;
	color += present_sample(res, data.t + float2( l,-s) * dxy, cb).rgb;
	color += present_sample(res, data.t + float2(-s,-l) * dxy, cb).rgb;
	color += present_sample(res, data.t + float2(-l, s) * dxy, cb).rgb;

	return float4(color * 0.25,1);
}

fragment float4 ps_automagical_supersampling(ConvertShaderData data [[stage_in]], ConvertPSRes res,
	constant GSMTLPresentPSUniform& cb [[buffer(GSMTLBufferIndexUniforms)]])
{
	float2 ratio = (cb.source_size / cb.target_size) * 0.5;
	float2 steps = floor(ratio);
	float3 col = present_sample(res, data.t, cb).rgb;
	float div = 1;

	for (float y = 0; y < steps.y; y++)
	{
		for (float x = 0; x < steps.x; x++)
		{
			float2 offset = float2(x,y) - ratio * 0.5;
			col += present_sample(res, data.t + offset * cb.rcp_source_resolution * 2.0, cb).rgb;
			div++;
		}
	}

	return float4(col / div, 1);
}
