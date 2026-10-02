// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Common.h"
#include "PS2Float.h"

#include <gtest/gtest.h>

TEST(SoftFloat, ProcessorSwitchesPreserveFullTLBAndFastmemChoices)
{
	Pcsx2Config::CpuOptions original;
	original.EnableExperimentalEETLB = true;
	original.Recompiler.EnableEE = true;
	original.Recompiler.EnableFastmem = true;
	for (const u32 mask : {1u, 2u, 4u, 7u})
	{
		auto options = original;
		options.FPUSoftFloat = (mask & 1) != 0;
		options.VU0SoftFloat = (mask & 2) != 0;
		options.VU1SoftFloat = (mask & 4) != 0;
		EXPECT_NE(options, original);
		EXPECT_FALSE(options.CpusChanged(original));
		EXPECT_TRUE(options.IsFullTLBKsegFastmemEnabled());
		EXPECT_FALSE(options.IsFastmemEnabled());
	}
}

TEST(SoftFloat, ExtendedExponentIsFiniteAndDenormalsFlush)
{
	EXPECT_EQ(PS2Float(0x7f800000u).Add(PS2Float(0u)).raw, 0x7f800000u);
	EXPECT_EQ(PS2Float(0x00000001u).Add(PS2Float::One()).raw, PS2Float::ONE);
	EXPECT_EQ(PS2Float(0x80000001u).Mul(PS2Float::One()).raw, 0x80000000u);
}

TEST(SoftFloat, DivisionCachePreservesExceptionFlags)
{
	for (int repeat = 0; repeat < 2; repeat++)
	{
		const PS2Float divided = PS2Float::One().Div(PS2Float(0u));
		EXPECT_EQ(divided.raw, PS2Float::MAX_FLOATING_POINT_VALUE);
		EXPECT_TRUE(divided.HasDivideByZero());
		const PS2Float invalid = PS2Float(0u).Div(PS2Float(0u));
		EXPECT_TRUE(invalid.HasInvalid());
		EXPECT_FALSE(invalid.HasDivideByZero());
	}
}

TEST(SoftFloat, NegativeSquareRootReturnsMagnitudeWithInvalidFlag)
{
	const PS2Float result = PS2Float(0xc0800000u).Sqrt();
	EXPECT_EQ(result.raw, 0x40000000u);
	EXPECT_TRUE(result.HasInvalid());
}
