// SPDX-License-Identifier: GPL-3.0+
#pragma once
#include <cstdint>
namespace usb_hid
{
	// Windows/Qt native make codes use set 1, with E0 or bit 8 for extended keys.
	// QEMU numbers use bit 7 to request the E0 prefix. E1/Pause needs a separate
	// sequence and is deliberately unsupported here; no fabricated key is sent.
	constexpr std::uint32_t NormalizeWindowsScanCode(std::uint32_t code)
	{
		const std::uint32_t prefix = code & ~0xffu;
		if (prefix != 0 && prefix != 0x100 && prefix != 0xe000)
			return 0;
		const std::uint32_t make = code & 0xffu;
		if (make == 0 || make >= 0x80 || (prefix && (make == 0x2a || make == 0x36)))
			return 0;
		return make | (prefix ? 0x80u : 0u);
	}
} // namespace usb_hid
