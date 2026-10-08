// SPDX-License-Identifier: GPL-3.0+
// Register-only kernel shared with the standalone executable validation.
#pragma once
#include "common/emitter/x86emitter.h"

// VU MAX/MINI (packed). PS2 floats compare as sign-magnitude integers, which equals a signed compare of the
// raw bits except when both operands are negative (direction flips). So: k = (c1 > c2 signed raw) ^ (both negative),
// then a k-register blend. Ties are only reachable for identical bits, so the pick does not matter.
// t1 is clobbered (t2 is unused); to/from/t1 must be distinct. k4 and k5 are clobbered.
// Needs proper testing on more workloads.
static inline void emitVUMinMaxAVX512(const x86Emitter::xRegisterSSE& to, const x86Emitter::xRegisterSSE& from,
	const x86Emitter::xRegisterSSE& t1, const x86Emitter::xRegisterSSE& /*t2*/, bool min)
{
	using namespace x86Emitter;
	const xRegisterSSE& c1 = min ? from : to;
	const xRegisterSSE& c2 = min ? to : from;
	xPAND(t1, to, from); // sign bit set only where both are negative
	xVPMOVD2M(k5, t1);
	xVPCMPD(k4, c1, c2, 6); // NLE: c1 > c2, signed raw
	xKXORW(k4, k4, k5);
	xVPBLENDMD(to, from, to, k4); // k ? to : from
}
