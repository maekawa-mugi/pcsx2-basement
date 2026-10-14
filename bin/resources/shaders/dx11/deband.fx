// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

/*
** Debanding filter.
**
** The algorithm (random-direction neighbourhood averaging that is only applied where the local
** difference stays below a threshold, followed by optional grain) follows pl_shader_deband() from
** libplacebo, Copyright (C) the libplacebo authors, licensed under LGPL-2.1-or-later
** (https://code.videolan.org/videolan/libplacebo). This is a re-implementation for PCSX2.
**
** The random number generator is IbukiHash by Andante, marked CC0 1.0
** (https://creativecommons.org/publicdomain/zero/1.0/).
**
** Needs proper testing on all backends.
*/

Texture2D Texture;
SamplerState Sampler;

cbuffer cb0
{
	float4 params;  // threshold, radius, grain, iterations
	float4 params2; // frame, unused
};

// IbukiHash by Andante. This work is marked with CC0 1.0.
float ibuki(float4 v)
{
	const uint4 mult = uint4(0xae3cc725, 0x9fe72885, 0xae36bfb5, 0x82c1fcad);

	uint4 u = uint4(v);
	u = u * mult;
	u ^= u.wxyz ^ (u >> 13);

	uint r = u.x * mult.x + u.y * mult.y + u.z * mult.z + u.w * mult.w;

	r ^= r >> 11;
	r = (r * r) ^ r;

	return float(r) * 2.3283064365386962890625e-10;
}

static int2 g_size;

float4 fetch(int2 p)
{
	return Texture.Load(int3(clamp(p, int2(0, 0), g_size - int2(1, 1)), 0));
}

struct PS_INPUT
{
	float4 p : SV_Position;
	float2 t : TEXCOORD0;
};

float4 ps_main(PS_INPUT input) : SV_Target0
{
	uint w, h;
	Texture.GetDimensions(w, h);
	g_size = int2(w, h);
	const int2 pos = clamp(int2(input.t * float2(g_size)), int2(0, 0), g_size - int2(1, 1));

	const float threshold = params.x;
	const float radius = params.y;
	const float grain = params.z;
	const int iterations = int(params.w);
	const float frame = params2.x;

	const float4 src = fetch(pos);
	float4 color = src;
	float counter = 0.0;

	for (int i = 1; i <= iterations; i++)
	{
		// Random direction and distance, distance grows with every iteration.
		const float dist = ibuki(float4(float2(pos), frame, counter)) * radius * float(i);
		const float dir = ibuki(float4(float2(pos), frame, counter + 1.0)) * 6.28318530718;
		counter += 2.0;

		const int2 o = int2(floor(dist * float2(cos(dir), sin(dir)) + 0.5));
		const int2 q = int2(-o.y, o.x); // quarter turn

		const float4 avg = 0.25 * (fetch(pos + o) + fetch(pos - o) + fetch(pos + q) + fetch(pos - q));
		const float4 diff = abs(color - avg);
		const float bound = threshold / (1000.0 * float(i));

		// Keep the original where the difference is large (a real edge), otherwise smooth.
		color = lerp(avg, color, float4(diff > bound));
	}

	if (grain > 0.0)
	{
		const float3 noise = float3(ibuki(float4(float2(pos), frame, counter)),
			ibuki(float4(float2(pos), frame, counter + 1.0)),
			ibuki(float4(float2(pos), frame, counter + 2.0)));
		color.rgb += (noise - 0.5) * (grain / 1000.0);
	}

	return float4(color.rgb, src.a);
}
