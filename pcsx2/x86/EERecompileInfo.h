// SPDX-FileCopyrightText: 2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Assertions.h"
#include "common/Pcsx2Types.h"

#ifndef PROCESS_EE_S
#error "Include EERecompileInfo.h through iCore.h, which defines the operand flags."
#endif

// Compiler metadata, not guest state. No conversion back to a packed integer:
// passing this through an old int callback must be a compile-time error.
class EERecompileInfo
{
public:
	enum Slot : unsigned
	{
		S,
		T,
		D,
		LO,
		HI,
		ACC = LO
	};
	constexpr EERecompileInfo() = default;
	// Explicit: a bare flag word must never silently become EE metadata.
	explicit constexpr EERecompileInfo(u32 flags)
		: m_flags(flags)
	{
	}
	static EERecompileInfo Operand(Slot slot, int reg, u32 flag)
	{
		pxAssertRel(reg >= 0 && reg < 32, "Invalid EE host register ID");
		EERecompileInfo info(flag);
		info.m_regs[slot] = static_cast<u8>(reg);
		return info;
	}
	constexpr int Get(Slot slot) const { return m_regs[slot]; }
	constexpr u32 operator&(u32 flags) const { return m_flags & flags; }
	EERecompileInfo& operator|=(const EERecompileInfo& other)
	{
		// Operand flags identify the valid fields; LO and ACC share one slot.
		constexpr u32 valid[] = {PROCESS_EE_S, PROCESS_EE_T, PROCESS_EE_D, PROCESS_EE_LO, PROCESS_EE_HI};
		for (unsigned i = 0; i < 5; i++)
		{
			if (other.m_flags & valid[i])
				m_regs[i] = other.m_regs[i];
		}
		m_flags |= other.m_flags;
		return *this;
	}
	EERecompileInfo operator|(u32 flags) const
	{
		EERecompileInfo result = *this;
		result.m_flags |= flags;
		return result;
	}

private:
	u32 m_flags = 0;
	u8 m_regs[5] = {};
};

inline int EEGetHostRegister(const EERecompileInfo& info, EERecompileInfo::Slot slot)
{
	return info.Get(slot);
}

// IOP continues to use its original 16-GPR packed metadata. It has its own
// accessor so EE code holding a stray integer cannot decode only four bits.
inline int PSXGetHostRegister(u32 info, EERecompileInfo::Slot slot)
{
	return (info >> (8 + static_cast<unsigned>(slot) * 4)) & 0xf;
}
