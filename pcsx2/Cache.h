// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "Common.h"

#include "common/SingleRegisterTypes.h"

void resetCache();
// Dumps all dirty cache entries to memory
// This is necessary to fix a bug when enabled the recompiler while the cache was enabled.
void writebackCache();
void writeCache8(u32 mem, u8 value, bool validPFN = true);
void writeCache16(u32 mem, u16 value, bool validPFN = true);
void writeCache32(u32 mem, u32 value, bool validPFN = true);
void writeCache64(u32 mem, const u64 value, bool validPFN = true);
void writeCache128(u32 mem, const mem128_t* value, bool validPFN = true);
u8 readCache8(u32 mem, bool validPFN = true);
u16 readCache16(u32 mem, bool validPFN = true);
u32 readCache32(u32 mem, bool validPFN = true);
u64 readCache64(u32 mem, bool validPFN = true);
RETURNS_R128 readCache128(u32 mem, bool validPFN = true);

void writeCache8Physical(u32 vaddr, u32 paddr, u8 value);
void writeCache16Physical(u32 vaddr, u32 paddr, u16 value);
void writeCache32Physical(u32 vaddr, u32 paddr, u32 value);
void writeCache64Physical(u32 vaddr, u32 paddr, u64 value);
void writeCache128Physical(u32 vaddr, u32 paddr, const mem128_t* value);
u8 readCache8Physical(u32 vaddr, u32 paddr);
u16 readCache16Physical(u32 vaddr, u32 paddr);
u32 readCache32Physical(u32 vaddr, u32 paddr);
u64 readCache64Physical(u32 vaddr, u32 paddr);
RETURNS_R128 readCache128Physical(u32 vaddr, u32 paddr);

constexpr bool CacheOpUsesAddressTranslation(u32 operation)
{
	// Only hit operations perform a TLB lookup. Index operations use the address solely to
	// select a cache set/way, and can therefore legally use an unmapped virtual address.
	return operation == 0x18 || operation == 0x1a || operation == 0x1c;
}

namespace R5900::Interpreter::OpcodeImpl
{
	void CACHEForRecompiler();
}
