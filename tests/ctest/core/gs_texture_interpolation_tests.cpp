// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "GS/Renderers/HW/GSTextureCache.h"
#include "GS/Renderers/Vulkan/JincLUT.h"

#include <gtest/gtest.h>

#include <cmath>

TEST(GSTextureInterpolation, PartialBlockUploadCannotSupplyNeighboringTaps)
{
	GSTextureCache::Source source(GIFRegTEX0{}, GIFRegTEXA{});
	// A valid block is enough for bilinear but does not guarantee its cubic/Jinc neighbors.
	source.m_valid = std::make_unique<u32[]>(GS_MAX_PAGES);
	source.m_valid[0] = 1u;
	EXPECT_FALSE(source.HasCompleteBaseLayer());
}

TEST(GSTextureInterpolation, CompletedBaseLayerSuppliesClampAndRepeatNeighbors)
{
	GSTextureCache::Source source(GIFRegTEX0{}, GIFRegTEXA{});
	source.m_complete_layers = 1u;
	EXPECT_TRUE(source.HasCompleteBaseLayer());
	// Source invalidation clears completeness even if most previously uploaded blocks survive.
	source.m_complete_layers = 0;
	EXPECT_FALSE(source.HasCompleteBaseLayer());
}

TEST(GSTextureInterpolation, CompletedMipLayersCannotSubstituteForBaseLayer)
{
	GSTextureCache::Source source(GIFRegTEX0{}, GIFRegTEXA{});
	source.m_complete_layers = 0x7eu;
	EXPECT_FALSE(source.HasCompleteBaseLayer());
}

TEST(GSTextureInterpolation, HashCachedTextureSuppliesTheCompleteBaseLayer)
{
	GSTextureCache::HashCacheEntry entry{};
	GSTextureCache::Source source(GIFRegTEX0{}, GIFRegTEXA{});
	source.m_from_hash_cache = &entry;
	EXPECT_TRUE(source.HasCompleteBaseLayer());
}

TEST(GSTextureInterpolation, TrilinearRequiresEveryReachableLayer)
{
	GSTextureCache::Source source(GIFRegTEX0{}, GIFRegTEXA{});
	source.m_complete_layers = 0x5u;
	EXPECT_TRUE(source.HasCompleteBaseLayer());
	EXPECT_FALSE(source.HasCompleteMipChain(3));
	source.m_complete_layers = 0x7u;
	EXPECT_TRUE(source.HasCompleteMipChain(3));
	EXPECT_FALSE(source.HasCompleteMipChain(4));
	source.m_complete_layers = 0;
	EXPECT_FALSE(source.HasCompleteMipChain(3));
	EXPECT_FALSE(source.HasCompleteMipChain(0));
	EXPECT_FALSE(source.HasCompleteMipChain(8));
}

TEST(GSTextureInterpolation, HashCachedMipChainIsUploadedInFull)
{
	GSTextureCache::HashCacheEntry entry{};
	GSTextureCache::Source source(GIFRegTEX0{}, GIFRegTEXA{});
	source.m_from_hash_cache = &entry;
	EXPECT_TRUE(source.HasCompleteMipChain(7));
}

TEST(GSTextureInterpolation, JincLUTMatchesVerifiedBesselFormula)
{
	constexpr double blur = 0.9812505644269356;
	constexpr double support = 3.2383154841662362 * blur;
	constexpr double pi = 3.141592653589793;
	const auto jinc = [](double x) {
		// Independent convergent series for 2*J1(x)/x; no platform-specific special-functions library.
		double term = 1.0;
		double sum = term;
		for (int k = 1; k < 60; k++)
		{
			term *= -(x * x / 4.0) / (k * (k + 1));
			sum += term;
		}
		return sum;
	};
	static_assert(sizeof(JINC_LUT) == 1024 * sizeof(float));
	for (int i = 0; i < 1024; i++)
	{
		const double q = (support * i / 1023.0) / blur;
		EXPECT_NEAR(JINC_LUT[i], jinc(pi * q) * jinc(pi * q / 3.0), 1e-7) << "LUT entry " << i;
	}
}
