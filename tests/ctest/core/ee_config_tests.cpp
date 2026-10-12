// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Config.h"
#include "common/MemorySettingsInterface.h"
#include "common/SettingsWrapper.h"

#include <gtest/gtest.h>

TEST(GSFinalScalingConfig, RetiredModesLoadAsBilinearAndLinearLight)
{
	MemorySettingsInterface settings;
	settings.SetIntValue("EmuCore/GS", "linear_present_mode", 2);
	settings.SetIntValue("EmuCore/GS", "FinalScalingColorSpace", 2);
	SettingsLoadWrapper load(settings);
	Pcsx2Config::GSOptions options;
	options.LoadSave(load);
	EXPECT_EQ(options.LinearPresent, GSPostBilinearMode::BilinearSmooth);
	EXPECT_EQ(options.FinalScalingColorSpace, GSFinalScalingColorSpace::LinearLight);

	SettingsSaveWrapper save(settings);
	options.LoadSave(save);
	EXPECT_EQ(settings.GetIntValue("EmuCore/GS", "linear_present_mode", -1), 1);
	EXPECT_EQ(settings.GetIntValue("EmuCore/GS", "FinalScalingColorSpace", -1), 1);
}

TEST(GSFinalScalingConfig, RemovingSharpPreservesSavedCatmullRomAndJinc)
{
	for (const auto& [saved, mode] : {std::pair{3, GSPostBilinearMode::CatmullRom}, std::pair{4, GSPostBilinearMode::Jinc}})
	{
		MemorySettingsInterface settings;
		settings.SetIntValue("EmuCore/GS", "linear_present_mode", saved);
		SettingsLoadWrapper load(settings);
		Pcsx2Config::GSOptions options;
		options.LoadSave(load);
		EXPECT_EQ(options.LinearPresent, mode);
	}
}

TEST(GSTextureLinearLightConfig, OptInPersistsAndTriggersSettingsUpdate)
{
	Pcsx2Config::GSOptions original;
	EXPECT_FALSE(original.TextureLinearLight);
	Pcsx2Config::GSOptions changed(original);
	changed.TextureLinearLight = true;
	EXPECT_NE(original, changed);
	MemorySettingsInterface settings;
	SettingsSaveWrapper save(settings);
	changed.LoadSave(save);
	SettingsLoadWrapper load(settings);
	original.LoadSave(load);
	EXPECT_TRUE(original.TextureLinearLight);
}

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
