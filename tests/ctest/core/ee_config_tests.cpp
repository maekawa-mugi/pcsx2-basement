// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Config.h"

#include <gtest/gtest.h>

TEST(EEFullTLBConfig, DefaultsOffAndPreservesNormalEffectiveSettings)
{
	Pcsx2Config::CpuOptions options;
	EXPECT_FALSE(options.EnableExperimentalEETLB);
	EXPECT_FALSE(options.EnableFullTLBDiagnosticTrace);
	EXPECT_EQ(options.IsEERecompilerEnabled(), options.Recompiler.EnableEE);
	EXPECT_EQ(options.IsFastmemEnabled(), options.Recompiler.EnableEE && options.Recompiler.EnableFastmem);
}

TEST(EEFullTLBConfig, DiagnosticTraceIsIndependentAndDoesNotChangeCpuImplementation)
{
	Pcsx2Config::CpuOptions left;
	Pcsx2Config::CpuOptions right;
	right.EnableFullTLBDiagnosticTrace = true;

	EXPECT_NE(left, right);
	EXPECT_FALSE(left.CpusChanged(right));
}

TEST(EEFullTLBConfig, AllowsRecompilerButDisablesFastmemWithoutMutatingSavedChoices)
{
	Pcsx2Config::CpuOptions options;
	options.Recompiler.EnableEE = true;
	options.Recompiler.EnableFastmem = true;
	options.EnableExperimentalEETLB = true;

	EXPECT_TRUE(options.IsEERecompilerEnabled());
	EXPECT_FALSE(options.IsFastmemEnabled());
	EXPECT_TRUE(options.Recompiler.EnableEE);
	EXPECT_TRUE(options.Recompiler.EnableFastmem);

	options.EnableExperimentalEETLB = false;
	EXPECT_TRUE(options.IsEERecompilerEnabled());
	EXPECT_TRUE(options.IsFastmemEnabled());
}

TEST(EEFullTLBConfig, CpuOptionEqualityIncludesTheExperimentalMode)
{
	Pcsx2Config::CpuOptions left;
	Pcsx2Config::CpuOptions right;

	EXPECT_EQ(left.bitset, 0u);
	EXPECT_EQ(left, right);
	EXPECT_FALSE(left.CpusChanged(right));

	right.EnableExperimentalEETLB = true;
	EXPECT_NE(left, right);
	EXPECT_TRUE(left.CpusChanged(right));
}
