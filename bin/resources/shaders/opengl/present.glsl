// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

//#version 420 // Keep it for editor detection


#ifdef VERTEX_SHADER

layout(location = 0) in vec2 POSITION;
layout(location = 1) in vec2 TEXCOORD0;
layout(location = 7) in vec4 COLOR;

// FIXME set the interpolation (don't know what dx do)
// flat means that there is no interpolation. The value given to the fragment shader is based on the provoking vertex conventions.
//
// noperspective means that there will be linear interpolation in window-space. This is usually not what you want, but it can have its uses.
//
// smooth, the default, means to do perspective-correct interpolation.
//
// The centroid qualifier only matters when multisampling. If this qualifier is not present, then the value is interpolated to the pixel's center, anywhere in the pixel, or to one of the pixel's samples. This sample may lie outside of the actual primitive being rendered, since a primitive can cover only part of a pixel's area. The centroid qualifier is used to prevent this; the interpolation point must fall within both the pixel's area and the primitive's area.
out vec4 PSin_p;
out vec2 PSin_t;
out vec4 PSin_c;

void vs_main()
{
	PSin_p = vec4(POSITION, 0.5f, 1.0f);
	PSin_t = TEXCOORD0;
	PSin_c = COLOR;
	gl_Position = vec4(POSITION, 0.5f, 1.0f); // NOTE I don't know if it is possible to merge POSITION_OUT and gl_Position
}

#endif

#ifdef FRAGMENT_SHADER

uniform vec4 u_source_rect;
uniform vec4 u_target_rect;
uniform vec2 u_source_size;
uniform vec2 u_target_size;
uniform vec2 u_target_resolution;
uniform vec2 u_rcp_target_resolution; // 1 / u_target_resolution
uniform vec2 u_source_resolution;
uniform vec2 u_rcp_source_resolution; // 1 / u_source_resolution
uniform float u_time;
uniform float u_final_scaling_mode;
uniform float u_final_color_space;

in vec4 PSin_p;
in vec2 PSin_t;
in vec4 PSin_c;

layout(binding = 0) uniform sampler2D TextureSampler;

layout(location = 0) out vec4 SV_Target0;


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
vec4 final_encode_input(vec4 pixel, float space)
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
vec4 final_decode_output(vec4 pixel, float space)
{
	pixel = clamp(pixel, vec4(0.0), vec4(1.0));
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
vec4 final_read(ivec2 coord, float space)
{
	ivec2 limit = ivec2(u_source_resolution) - ivec2(1);
	return final_encode_input(texelFetch(TextureSampler, clamp(coord, ivec2(0), limit), 0), space);
}
vec4 final_resample(vec2 uv, float mode, float space)
{
	if (mode < 1.5 && space < 0.5)
		return texture(TextureSampler, uv);

	// Area is always a true weighted average in Native RGB or Linear Light.
	if (mode >= 1.5 && mode < 2.5 && space >= 1.5)
		space = 1.0;

	vec2 location = uv * u_source_resolution - vec2(0.5);
	ivec2 base = ivec2(floor(location));
	if (mode < 1.5)
	{
		// Manual bilinear is required to decode RGB before mixing.
		vec2 f = fract(location);
		vec4 a = final_read(base, space);
		vec4 b = final_read(base + ivec2(1, 0), space);
		vec4 c = final_read(base + ivec2(0, 1), space);
		vec4 d = final_read(base + ivec2(1, 1), space);
		return final_decode_output(mix(mix(a, b, f.x), mix(c, d, f.x), f.y), space);
	}

	vec2 source_per_output = abs(u_source_size / max(abs(u_target_size), vec2(1.0)));
	if (mode < 2.5)
	{
		// Exact pixel-overlap box resampling, directly from the source.
		vec2 low = uv * u_source_resolution - 0.5 * source_per_output;
		vec2 high = low + source_per_output;
		ivec2 first = ivec2(floor(low));
		ivec2 last = ivec2(ceil(high));
		vec4 sum = vec4(0.0);
		for (int y = first.y; y < last.y; y++)
		{
			for (int x = first.x; x < last.x; x++)
			{
				vec2 overlap = max(vec2(0.0), min(high, vec2(x + 1, y + 1)) - max(low, vec2(x, y)));
				sum += final_read(ivec2(x, y), space) * (overlap.x * overlap.y);
			}
		}
		return final_decode_output(sum / max(source_per_output.x * source_per_output.y, 1e-8), space);
	}

	// Widen the kernel for minification instead of using a fixed 4x4 footprint.
	vec2 scale = max(vec2(1.0), source_per_output);
	ivec2 first = ivec2(ceil(location - 2.0 * scale));
	ivec2 last = ivec2(floor(location + 2.0 * scale));
	vec4 sum = vec4(0.0);
	vec4 center_min = vec4(1.0);
	vec4 center_max = vec4(0.0);
	float total = 0.0;
	for (int y = first.y; y <= last.y; y++)
	{
		for (int x = first.x; x <= last.x; x++)
		{
			vec2 delta = (vec2(x, y) - location) / scale;
			float weight = 0.0;
			if (mode < 3.5)
				weight = final_cubic(delta.x) * final_cubic(delta.y);
			else
			{
				float radius = length(delta);
				if (radius < 2.0) weight = final_jinc(radius) * final_jinc(radius * 0.5);
			}
			if (weight == 0.0) continue;
			vec4 pixel = final_read(ivec2(x, y), space);
			sum += pixel * weight;
			total += weight;
			if (x >= base.x && x <= base.x + 1 && y >= base.y && y <= base.y + 1)
			{
				center_min = min(center_min, pixel);
				center_max = max(center_max, pixel);
			}
		}
	}
	vec4 result = sum / max(total, 1e-8);
	if (mode >= 3.5 && scale.x <= 1.0 && scale.y <= 1.0)
		result = clamp(result, center_min, center_max); // Jinc anti-ringing.
	return final_decode_output(result, space);
}

vec4 sample_c(vec2 uv)
{
	return final_resample(uv, u_final_scaling_mode, u_final_color_space);
}

vec4 sample_c()
{
	return sample_c(PSin_t);
}

vec4 ps_crt(uint i)
{
	vec4 mask[4] = vec4[4](
		vec4(1, 0, 0, 0),
		vec4(0, 1, 0, 0),
		vec4(0, 0, 1, 0),
		vec4(1, 1, 1, 0));
	return sample_c() * clamp((mask[i] + 0.5f), 0.0f, 1.0f);
}

#ifdef ps_copy
void ps_copy()
{
	SV_Target0 = sample_c();
}
#endif

#ifdef ps_filter_scanlines
vec4 ps_scanlines(uint i)
{
	vec4 mask[2] = vec4[2]
	(
		vec4(1, 1, 1, 0),
		vec4(0, 0, 0, 0)
	);

	return sample_c() * clamp((mask[i] + 0.5f), 0.0f, 1.0f);
}

void ps_filter_scanlines() // scanlines
{
	highp uvec4 p = uvec4(gl_FragCoord);

	vec4 c = ps_scanlines(p.y % 2u);

	SV_Target0 = c;
}
#endif

#ifdef ps_filter_diagonal
void ps_filter_diagonal() // diagonal
{
	highp uvec4 p = uvec4(gl_FragCoord);

	vec4 c = ps_crt((p.x + (p.y % 3u)) % 3u);

	SV_Target0 = c;
}
#endif

#ifdef ps_filter_triangular
void ps_filter_triangular() // triangular
{
	highp uvec4 p = uvec4(gl_FragCoord);

	vec4 c = ps_crt(((p.x + ((p.y >> 1u) & 1u) * 3u) >> 1u) % 3u);

	SV_Target0 = c;
}
#endif

#ifdef ps_filter_complex
void ps_filter_complex()
{
	const float PI = 3.14159265359f;
	vec2 texdim = vec2(textureSize(TextureSampler, 0));
	float factor = (0.9f - 0.4f * cos(2.0f * PI * PSin_t.y * texdim.y));
	vec4 c = factor * texture(TextureSampler, vec2(PSin_t.x, (floor(PSin_t.y * texdim.y) + 0.5f) / texdim.y));

	SV_Target0 = c;
}
#endif

#ifdef ps_filter_lottes

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

float ToLinear1(float c)
{
	return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}

vec3 ToLinear(vec3 c)
{
	return vec3(ToLinear1(c.r), ToLinear1(c.g), ToLinear1(c.b));
}

float ToSrgb1(float c)
{
	return c < 0.0031308 ? c * 12.92 : 1.055 * pow(c, 0.41666) - 0.055;
}

vec3 ToSrgb(vec3 c)
{
	return vec3(ToSrgb1(c.r), ToSrgb1(c.g), ToSrgb1(c.b));
}

vec3 Fetch(vec2 pos, vec2 off)
{
	pos = (floor(pos * u_target_size + off) + vec2(0.5, 0.5)) / u_target_size;
	if (max(abs(pos.x - 0.5), abs(pos.y - 0.5)) > 0.5)
	{
		return vec3(0.0, 0.0, 0.0);
	}
	else
	{
		return ToLinear(sample_c(pos.xy).rgb);
	}
}

vec2 Dist(vec2 pos)
{
	pos = pos * vec2(640, 480);

	return -((pos - floor(pos)) - vec2(0.5, 0.5));
}

float Gaus(float pos, float scale)
{
	return exp2(scale * pow(abs(pos), Shape));
}

vec3 Horz3(vec2 pos, float off)
{
	vec3 b = Fetch(pos, vec2(-1.0, off));
	vec3 c = Fetch(pos, vec2(0.0, off));
	vec3 d = Fetch(pos, vec2(1.0, off));
	float dst = Dist(pos).x;

	// Convert distance to weight.
	float scale = FilterCRTAmount;
	float wb = Gaus(dst - 1.0, scale);
	float wc = Gaus(dst + 0.0, scale);
	float wd = Gaus(dst + 1.0, scale);

	return (b * wb + c * wc + d * wd) / (wb + wc + wd);
}

vec3 Horz5(vec2 pos, float off)
{
	vec3 a = Fetch(pos, vec2(-2.0, off));
	vec3 b = Fetch(pos, vec2(-1.0, off));
	vec3 c = Fetch(pos, vec2(0.0, off));
	vec3 d = Fetch(pos, vec2(1.0, off));
	vec3 e = Fetch(pos, vec2(2.0, off));
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

vec3 Horz7(vec2 pos, float off)
{
	vec3 a = Fetch(pos, vec2(-3.0, off));
	vec3 b = Fetch(pos, vec2(-2.0, off));
	vec3 c = Fetch(pos, vec2(-1.0, off));
	vec3 d = Fetch(pos, vec2( 0.0, off));
	vec3 e = Fetch(pos, vec2( 1.0, off));
	vec3 f = Fetch(pos, vec2( 2.0, off));
	vec3 g = Fetch(pos, vec2( 3.0, off));

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
float Scan(vec2 pos, float off)
{
	float dst = Dist(pos).y;
	return Gaus(dst + off, ScanBrightness);
}

float BloomScan(vec2 pos, float off)
{
	float dst = Dist(pos).y;

	return Gaus(dst + off, BloomScanLine);
}

vec3 Tri(vec2 pos)
{
	vec3 a = Horz3(pos, -1.0);
	vec3 b = Horz5(pos, 0.0);
	vec3 c = Horz3(pos, 1.0);

	float wa = Scan(pos, -1.0);
	float wb = Scan(pos, 0.0);
	float wc = Scan(pos, 1.0);

	return (a * wa) + (b * wb) + (c * wc);
}

vec3 Bloom(vec2 pos)
{
	vec3 a = Horz5(pos,-2.0);
	vec3 b = Horz7(pos,-1.0);
	vec3 c = Horz7(pos, 0.0);
	vec3 d = Horz7(pos, 1.0);
	vec3 e = Horz5(pos, 2.0);

	float wa = BloomScan(pos,-2.0);
	float wb = BloomScan(pos,-1.0);
	float wc = BloomScan(pos, 0.0);
	float wd = BloomScan(pos, 1.0);
	float we = BloomScan(pos, 2.0);

	return a * wa + b * wb + c * wc + d * wd + e * we;
}

vec2 Warp(vec2 pos)
{
	pos = pos * 2.0 - 1.0;
	pos *= vec2(1.0 + (pos.y * pos.y) * HorizontalWarp, 1.0 + (pos.x * pos.x) * VerticalWarp);
	return pos * 0.5 + 0.5;
}

vec3 Mask(vec2 pos)
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
	vec3 mask = vec3(MaskAmountDark, MaskAmountDark, MaskAmountDark);

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
	vec3 mask = vec3(MaskAmountDark, MaskAmountDark, MaskAmountDark);

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
	vec3 mask = vec3(MaskAmountDark, MaskAmountDark, MaskAmountDark);
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
	pos.xy = floor(pos.xy * vec2(1.0, 0.5));
	pos.x += pos.y * 3.0;

	vec3 mask = vec3(MaskAmountDark, MaskAmountDark, MaskAmountDark);
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

vec4 LottesCRTPass()
{
	//flipped y axis in opengl
	vec2 fragcoord = vec2(gl_FragCoord.x, u_target_resolution.y - gl_FragCoord.y) - u_target_rect.xy;
	vec4 color;
	vec2 inSize = u_target_resolution - (2.0 * u_target_rect.xy);

	vec2 pos = Warp(fragcoord.xy / inSize);
	color.rgb = Tri(pos);
	color.rgb += Bloom(pos) * BloomAmount;
#if UseShadowMask
	color.rgb *= Mask(fragcoord.xy);
#endif
	color.rgb = ToSrgb(color.rgb);

	return color;
}

void ps_filter_lottes()
{
	SV_Target0 = LottesCRTPass();
}

#endif

#ifdef ps_4x_rgss
void ps_4x_rgss()
{
	vec2 dxy = vec2(dFdx(PSin_t.x), dFdy(PSin_t.y));
	vec3 color = vec3(0);

	float s = 1.0/8.0;
	float l = 3.0/8.0;

	color += sample_c(PSin_t + vec2( s, l) * dxy).rgb;
	color += sample_c(PSin_t + vec2( l,-s) * dxy).rgb;
	color += sample_c(PSin_t + vec2(-s,-l) * dxy).rgb;
	color += sample_c(PSin_t + vec2(-l, s) * dxy).rgb;

	SV_Target0 = vec4(color * 0.25,1);
}
#endif

#ifdef ps_automagical_supersampling
void ps_automagical_supersampling()
{
	vec2 ratio = (u_source_size / u_target_size) * 0.5;
	vec2 steps = floor(ratio);
	vec3 col = sample_c(PSin_t).rgb;
	float div = 1;

	for (float y = 0; y < steps.y; y++)
	{
		for (float x = 0; x < steps.x; x++)
		{
			vec2 offset = vec2(x,y) - ratio * 0.5;
			col += sample_c(PSin_t + offset * u_rcp_source_resolution * 2.0).rgb;
			div++;
		}
	}

	SV_Target0 = vec4(col / div, 1);
}
#endif

#endif
