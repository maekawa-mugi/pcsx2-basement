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
// Final output. Modes: bilinear=1, box/area=2, Catmull-Rom=3, Jinc=4.
// Working spaces: encoded RGB=0, linear light=1, sigmoidized linear=2.
float final_srgb_decode(float x)
{
	return x <= 0.04045 ? x / 12.92 : pow((x + 0.055) / 1.055, 2.4);
}
float final_srgb_encode(float x)
{
	return x <= 0.0031308 ? 12.92 * x : 1.055 * pow(x, 1.0 / 2.4) - 0.055;
}
float final_sigmoid(float x)
{
	return (1.0 / (1.0 + exp(-8.0 * (x - 0.5))) - 0.01798620996) / 0.96402758008;
}
float final_unsigmoid(float x)
{
	float p = clamp(x, 0.0, 1.0) * 0.96402758008 + 0.01798620996;
	return clamp(0.5 + log(p / (1.0 - p)) / 8.0, 0.0, 1.0);
}
float4 final_encode_input(float4 pixel, float space)
{
	if (space >= 0.5)
	{
		pixel.r = final_srgb_decode(pixel.r);
		pixel.g = final_srgb_decode(pixel.g);
		pixel.b = final_srgb_decode(pixel.b);
		if (space >= 1.5)
		{
			pixel.r = final_sigmoid(pixel.r);
			pixel.g = final_sigmoid(pixel.g);
			pixel.b = final_sigmoid(pixel.b);
		}
	}
	return pixel;
}
float4 final_decode_output(float4 pixel, float space)
{
	pixel = clamp(pixel, float4(0.0), float4(1.0));
	if (space >= 0.5)
	{
		if (space >= 1.5)
		{
			pixel.r = final_unsigmoid(pixel.r);
			pixel.g = final_unsigmoid(pixel.g);
			pixel.b = final_unsigmoid(pixel.b);
		}
		pixel.r = final_srgb_encode(pixel.r);
		pixel.g = final_srgb_encode(pixel.g);
		pixel.b = final_srgb_encode(pixel.b);
	}
	return pixel;
}
float final_cubic(float x)
{
	x = abs(x);
	if (x < 1.0) return ((1.5 * x - 2.5) * x) * x + 1.0;
	if (x < 2.0) return (((-0.5 * x + 2.5) * x) - 4.0) * x + 2.0;
	return 0.0;
}
float final_jinc(float r)
{
	// Normalized Jinc(pi*r), approximated by the texture-filter polynomial.
	float q = 2.46740110027 * r * r;
	return 1.0 + q * (-0.5 + q * (0.0833333333333 + q * (-0.00694444444444 +
		q * (0.000347222222222 + q * (-0.0000115740740741 +
		q * (0.00000027557319224 + q * (-0.00000000492094986 +
		q * 0.0000000000683461217)))))));
}
float4 final_read(int2 coord, float space, ConvertPSRes res, constant GSMTLPresentPSUniform& cb)
{
	int2 limit = int2(cb.source_resolution) - int2(1);
	return final_encode_input(res.texture.read(uint2(clamp(coord, int2(0), limit))), space);
}
float4 final_resample(float2 uv, float mode, float space, ConvertPSRes res, constant GSMTLPresentPSUniform& cb)
{
	if (mode < 1.5 && space < 0.5)
		return res.sample(uv);

	// Area is always a true weighted average in Native RGB or Linear Light.
	if (mode >= 1.5 && mode < 2.5 && space >= 1.5)
		space = 1.0;

	float2 location = uv * cb.source_resolution - float2(0.5);
	int2 base = int2(floor(location));
	if (mode < 1.5)
	{
		// Manual bilinear is required to decode RGB before mixing.
		float2 f = fract(location);
		float4 a = final_read(base, space, res, cb);
		float4 b = final_read(base + int2(1, 0), space, res, cb);
		float4 c = final_read(base + int2(0, 1), space, res, cb);
		float4 d = final_read(base + int2(1, 1), space, res, cb);
		return final_decode_output(mix(mix(a, b, f.x), mix(c, d, f.x), f.y), space);
	}

	float2 source_per_output = abs(cb.source_size / max(abs(cb.target_size), float2(1.0)));
	if (mode < 2.5)
	{
		// Exact pixel-overlap box resampling, directly from the source.
		float2 low = uv * cb.source_resolution - 0.5 * source_per_output;
		float2 high = low + source_per_output;
		int2 first = int2(floor(low));
		int2 last = int2(ceil(high));
		float4 sum = float4(0.0);
		for (int y = first.y; y < last.y; y++)
		{
			for (int x = first.x; x < last.x; x++)
			{
				float2 overlap = max(float2(0.0), min(high, float2(x + 1, y + 1)) - max(low, float2(x, y)));
				sum += final_read(int2(x, y), space, res, cb) * (overlap.x * overlap.y);
			}
		}
		return final_decode_output(sum / max(source_per_output.x * source_per_output.y, 1e-8), space);
	}

	// Widen the kernel for minification instead of using a fixed 4x4 footprint.
	float2 scale = max(float2(1.0), source_per_output);
	int2 first = int2(ceil(location - 2.0 * scale));
	int2 last = int2(floor(location + 2.0 * scale));
	float4 sum = float4(0.0);
	float4 center_min = float4(1.0);
	float4 center_max = float4(0.0);
	float total = 0.0;
	for (int y = first.y; y <= last.y; y++)
	{
		for (int x = first.x; x <= last.x; x++)
		{
			float2 delta = (float2(x, y) - location) / scale;
			float weight = 0.0;
			if (mode < 3.5)
				weight = final_cubic(delta.x) * final_cubic(delta.y);
			else
			{
				float radius = length(delta);
				if (radius < 2.0) weight = final_jinc(radius) * final_jinc(radius * 0.5);
			}
			if (weight == 0.0) continue;
			float4 pixel = final_read(int2(x, y), space, res, cb);
			sum += pixel * weight;
			total += weight;
			if (x >= base.x && x <= base.x + 1 && y >= base.y && y <= base.y + 1)
			{
				center_min = min(center_min, pixel);
				center_max = max(center_max, pixel);
			}
		}
	}
	float4 result = sum / max(total, 1e-8);
	if (mode >= 3.5 && scale.x <= 1.0 && scale.y <= 1.0)
		result = clamp(result, center_min, center_max); // Jinc anti-ringing.
	return final_decode_output(result, space);
}

static float4 present_sample(ConvertPSRes res, float2 uv, constant GSMTLPresentPSUniform& cb)
{
	return final_resample(uv, cb.scaling_mode, cb.color_space, res, cb);
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
