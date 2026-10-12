// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "JincLUT.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace TextureColorLUT
{
	inline constexpr int WIDTH = 4096;
	inline constexpr int HEIGHT = 3;
	inline constexpr int DECODE_WIDTH = 256;

	inline double Decode(double value)
	{
		return value <= 0.04045 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4);
	}

	inline double Encode(double value)
	{
		return value <= 0.0031308 ? value * 12.92 : 1.055 * std::pow(value, 1.0 / 2.4) - 0.055;
	}

	inline std::array<float, WIDTH * HEIGHT> Generate()
	{
		std::array<float, WIDTH * HEIGHT> table{};
		// Share the unused palette descriptor with Jinc; its coefficients stay exact.
		std::copy_n(JINC_LUT, 1024, table.begin());
		for (int i = 0; i < WIDTH; i++)
		{
			const double value = static_cast<double>(i) / (WIDTH - 1);
			// Colour sources are normally UNORM8: keep their exact entries cache-local.
			if (i < DECODE_WIDTH)
				table[WIDTH + i] = static_cast<float>(Decode(static_cast<double>(i) / (DECODE_WIDTH - 1)));
			table[2 * WIDTH + i] = static_cast<float>(Encode(value));
		}
		return table;
	}
} // namespace TextureColorLUT
