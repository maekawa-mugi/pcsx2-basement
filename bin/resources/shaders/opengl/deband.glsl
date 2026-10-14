// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

//#version 420 // Keep it for editor detection

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

#ifdef FRAGMENT_SHADER

uniform vec4 params;  // threshold, radius, grain, iterations
uniform vec4 params2; // frame, unused

in vec4 PSin_p;
in vec2 PSin_t;
in vec4 PSin_c;

layout(binding = 0) uniform sampler2D TextureSampler;

layout(location = 0) out vec4 SV_Target0;

// IbukiHash by Andante. This work is marked with CC0 1.0.
float ibuki(vec4 v)
{
	const uvec4 mult = uvec4(0xae3cc725u, 0x9fe72885u, 0xae36bfb5u, 0x82c1fcadu);

	uvec4 u = uvec4(v);
	u = u * mult;
	u ^= u.wxyz ^ (u >> 13);

	// GLSL has no dot() for integer vectors.
	uint r = u.x * mult.x + u.y * mult.y + u.z * mult.z + u.w * mult.w;

	r ^= r >> 11;
	r = (r * r) ^ r;

	return float(r) * 2.3283064365386962890625e-10;
}

ivec2 g_size;

vec4 fetch(ivec2 p)
{
	return texelFetch(TextureSampler, clamp(p, ivec2(0), g_size - ivec2(1)), 0);
}

void ps_main()
{
	g_size = textureSize(TextureSampler, 0);
	const ivec2 pos = clamp(ivec2(PSin_t * vec2(g_size)), ivec2(0), g_size - ivec2(1));

	const float threshold = params.x;
	const float radius = params.y;
	const float grain = params.z;
	const int iterations = int(params.w);
	const float frame = params2.x;

	const vec4 src = fetch(pos);
	vec4 color = src;
	float counter = 0.0;

	for (int i = 1; i <= iterations; i++)
	{
		// Random direction and distance, distance grows with every iteration.
		const float dist = ibuki(vec4(vec2(pos), frame, counter)) * radius * float(i);
		const float dir = ibuki(vec4(vec2(pos), frame, counter + 1.0)) * 6.28318530718;
		counter += 2.0;

		const ivec2 o = ivec2(floor(dist * vec2(cos(dir), sin(dir)) + 0.5));
		const ivec2 q = ivec2(-o.y, o.x); // quarter turn

		const vec4 avg = 0.25 * (fetch(pos + o) + fetch(pos - o) + fetch(pos + q) + fetch(pos - q));
		const vec4 diff = abs(color - avg);
		const float bound = threshold / (1000.0 * float(i));

		// Keep the original where the difference is large (a real edge), otherwise smooth.
		color = mix(avg, color, vec4(greaterThan(diff, vec4(bound))));
	}

	if (grain > 0.0)
	{
		const vec3 noise = vec3(ibuki(vec4(vec2(pos), frame, counter)),
			ibuki(vec4(vec2(pos), frame, counter + 1.0)),
			ibuki(vec4(vec2(pos), frame, counter + 2.0)));
		color.rgb += (noise - 0.5) * (grain / 1000.0);
	}

	SV_Target0 = vec4(color.rgb, src.a);
}

#endif
