// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "EEMmu.h"

#include "common/SingleRegisterTypes.h"

namespace EEMemory
{
	// Linux regularly keeps more than 64 virtual pages hot across kernel and user mappings.
	// A larger direct-mapped micro-TLB reduces conflict misses without adding hit-path branches.
	static constexpr u32 RECOMPILER_TRANSLATION_CACHE_SIZE = 256;
	static constexpr u32 RECOMPILER_TRANSLATION_NO_TLB_ENTRY = 0xffffffffU;
	static constexpr u32 RECOMPILER_TRANSLATION_SCRATCHPAD = 1U << 0;
	static constexpr u32 RECOMPILER_TRANSLATION_NO_ACCESS = 1U << 1;
	static constexpr u32 RECOMPILER_TRANSLATION_CACHE_MODE_SHIFT = 8;
	static constexpr u32 RECOMPILER_TRANSLATION_CACHE_MODE_MASK = 7U << RECOMPILER_TRANSLATION_CACHE_MODE_SHIFT;

	struct alignas(32) RecompilerJitTranslationCacheEntry
	{
		u32 virtual_page = 0xffffffff;
		u32 translation_generation = 0;
		u32 context_key = 0;
		u32 tlb_entry_index = RECOMPILER_TRANSLATION_NO_TLB_ENTRY;
		u64 translation = 0;
		u64 host_page = 0;
	};
	static_assert(sizeof(RecompilerJitTranslationCacheEntry) == 32);

	struct FetchPage
	{
		EEMmu::TranslationResult translation;
		u32 virtual_page = 0;
	};

	u32 Fetch32(u32 vaddr);
	bool TryFetch32(u32 vaddr, u32* value);
	FetchPage TranslateFetchPage(u32 vaddr);
	u32 Fetch32(const FetchPage& page, u32 vaddr);

	u8 Read8(u32 vaddr);
	u16 Read16(u32 vaddr);
	u32 Read32(u32 vaddr);
	u64 Read64(u32 vaddr);
	u128 Read128(u32 vaddr);

	void Write8(u32 vaddr, u8 value);
	void Write16(u32 vaddr, u16 value);
	void Write32(u32 vaddr, u32 value);
	void Write64(u32 vaddr, u64 value);
	void Write128(u32 vaddr, const u128& value);

	void ReadModifyWrite32(u32 vaddr, u32 preserve_mask, u32 value);
	void ReadModifyWrite64(u32 vaddr, u64 preserve_mask, u64 value);

	// Full-TLB recompiler entry points. These use the same translation and physical
	// access implementation as the Interpreter, but report precise exceptions to
	// generated code instead of unwinding through Cpu::CancelInstruction().
	const u8* GetRecompilerAccessFaultAddress();
	void ClearRecompilerAccessFault();
	RecompilerJitTranslationCacheEntry* GetRecompilerJitTranslationCacheBase(EEMmu::AccessType access_type);
	u32 GetRecompilerJitTranslationContextKey();
	u64 ResolveRecompilerJitTranslation(u32 vaddr, u32 access_type);
	void RaiseRecompilerAddressError(u32 vaddr, u32 access_type);
	u32* GetFullTLBDiagnosticReadVAddrAddress();
	u64 RecordFullTLBDiagnosticReadResult(u64 value, u32 pc, u32 instruction);
	u32* GetPhysicalWriteGenerationBase();
	const u32* GetPhysicalWriteGenerationAddress(u32 paddr);
	const u32* GetScratchpadWriteGenerationAddress();
	u8 RecompilerRead8(u32 vaddr);
	u16 RecompilerRead16(u32 vaddr);
	u32 RecompilerRead32(u32 vaddr);
	u64 RecompilerRead64(u32 vaddr);
	RETURNS_R128 RecompilerRead128(u32 vaddr);
	void RecompilerWrite8(u32 vaddr, u8 value);
	void RecompilerWrite16(u32 vaddr, u16 value);
	void RecompilerWrite32(u32 vaddr, u32 value);
	void RecompilerWrite64(u32 vaddr, u64 value);
	void TAKES_R128 RecompilerWrite128(u32 vaddr, r128 value);
	void RecompilerWriteLeft32(u32 vaddr, u32 value);
	void RecompilerWriteRight32(u32 vaddr, u32 value);
	void RecompilerWriteLeft64(u32 vaddr, u64 value);
	void RecompilerWriteRight64(u32 vaddr, u64 value);
	EEMmu::TranslationResult TranslateForRecompiler(u32 vaddr, EEMmu::AccessType access_type);

	EEMmu::TranslationResult TranslateForCurrentInstruction(u32 vaddr, EEMmu::AccessType access_type);
	EEMmu::TranslationResult TranslateAndSuppressFault(u32 vaddr, EEMmu::AccessType access_type);

	// Captures recent Full-TLB translations in memory and emits them only on demand.
	// This is deliberately separate from TraceLog so release builds can diagnose guest faults.
	void RecordFullTLBUserException(u32 code, bool branch_delay, u64 cycle, u32 pc,
		u32 instruction, u32 sp, u32 ra, const EEMmu::ExceptionRegisters& before);
	void ResetFullTLBDiagnosticTrace();
	void DumpFullTLBDiagnosticTrace();
} // namespace EEMemory
