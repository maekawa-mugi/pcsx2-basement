// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "R5900.h"

#include <cstddef>

namespace EEMmu
{
	static constexpr size_t TLB_ENTRY_COUNT = 48;

	enum class AccessType : u8
	{
		Fetch,
		Load,
		Store,
		Cache,
		Prefetch,
	};

	enum class Fault : u8
	{
		None,
		AddressError,
		Refill,
		Invalid,
		Modified,
		BusError,
	};

	enum class Target : u8
	{
		Physical,
		Scratchpad,
	};

	enum class Warning : u32
	{
		None = 0,
		ReservedKSU = 1U << 0,
		InvalidPageMask = 1U << 1,
		InvalidScratchpad = 1U << 2,
		MultipleMatch = 1U << 3,
		ReservedCacheMode = 1U << 4,
		InvalidWired = 1U << 5,
	};

	struct TranslationContext
	{
		u32 status = 0;
		u32 config = 0;
		u8 asid = 0;
		const tlbs* tlb_entries = nullptr;
		size_t tlb_entry_count = 0;
	};

	struct TranslationResult
	{
		Fault fault = Fault::None;
		Target target = Target::Physical;
		u32 paddr = 0;
		u32 scratch_offset = 0;
		u8 cache_mode = 0;
		int matched_tlb_index = -1;
		Warning warnings = Warning::None;
	};

	struct ExceptionRegisters
	{
		u32 status;
		u32 cause;
		u32 epc;
		u32 bad_vaddr;
		u32 context;
		u32 entry_hi;
	};

	struct ExceptionRequest
	{
		Fault fault;
		AccessType access_type;
		u32 vaddr;
		u32 fault_pc;
		bool branch_delay;
	};

	struct ExceptionResult
	{
		ExceptionRegisters registers;
		u32 vector;
		bool taken = true;
	};

	struct ProbeResult
	{
		int index = -1;
		Warning warnings = Warning::None;
	};

	constexpr Warning operator|(Warning left, Warning right)
	{
		return static_cast<Warning>(static_cast<u32>(left) | static_cast<u32>(right));
	}

	constexpr Warning& operator|=(Warning& left, Warning right)
	{
		left = left | right;
		return left;
	}

	constexpr bool HasWarning(Warning warnings, Warning warning)
	{
		return (static_cast<u32>(warnings) & static_cast<u32>(warning)) != 0;
	}

	constexpr bool IsKernelMode(u32 status)
	{
		constexpr u32 STATUS_EXL_ERL = (1U << 1) | (1U << 2);
		constexpr u32 STATUS_KSU_MASK = 3U << 3;
		return (status & STATUS_EXL_ERL) != 0 || (status & STATUS_KSU_MASK) == 0;
	}

	// The Status bits that affect address translation, folded so that EXL/ERL and KSU values
	// which select the same operating mode compare equal: KSU-shaped mode bits plus ERL.
	// Reserved KSU=3 translates as kernel, matching TranslateAddress().
	constexpr u32 TranslationStatusKey(u32 status)
	{
		constexpr u32 STATUS_EXL = 1U << 1;
		constexpr u32 STATUS_ERL = 1U << 2;
		constexpr u32 STATUS_KSU_MASK = 3U << 3;
		const u32 erl = status & STATUS_ERL;
		if ((status & (STATUS_EXL | STATUS_ERL)) != 0)
			return erl;
		const u32 ksu = status & STATUS_KSU_MASK;
		return ksu == STATUS_KSU_MASK ? 0 : ksu;
	}

	constexpr bool IsUserMode(u32 status)
	{
		constexpr u32 STATUS_EXL_ERL = (1U << 1) | (1U << 2);
		constexpr u32 STATUS_KSU_MASK = 3U << 3;
		return (status & STATUS_EXL_ERL) == 0 && (status & STATUS_KSU_MASK) == (2U << 3);
	}

	/// Translates one EE virtual access without reading or modifying global CPU state.
	TranslationResult TranslateAddress(const TranslationContext& context, u32 vaddr, AccessType access_type);

	// Lets TranslateAddress match the live tlb[] array through a precomputed table. Every write
	// to tlb[] must then be followed by InvalidateTLBEntry()/InvalidateTranslations().
	void EnableFastTLBLookup(bool enable);

	/// Builds precise COP0 exception state without reading or modifying global CPU state.
	ExceptionResult BuildException(const ExceptionRegisters& registers, const ExceptionRequest& request);
	ExceptionResult BuildSynchronousException(
		const ExceptionRegisters& registers, u32 cause_code, u32 fault_pc, bool branch_delay);

	tlbs BuildTLBEntry(u32 page_mask, u32 entry_hi, u32 entry_lo0, u32 entry_lo1);
	void ReadTLBEntry(const tlbs& entry, u32* page_mask, u32* entry_hi, u32* entry_lo0, u32* entry_lo1);
	ProbeResult ProbeTLB(const tlbs* entries, size_t entry_count, u32 entry_hi);
	u32 AdvanceRandom(u32 random, u32 wired, Warning* warnings = nullptr);

	// The global generation is retained for diagnostics. Recompiler caches use the
	// entry generations so an unrelated TLB write does not invalidate every mapping.
	u32 GetTranslationGeneration();
	const u32* GetTranslationGenerationAddress();
	u32 GetTLBEntryGeneration(size_t index);
	const u32* GetTLBEntryGenerationAddress(size_t index);
	const u32* GetTLBEntryGenerationBase();
	void InvalidateTLBEntry(size_t index);
	void InvalidateTranslations();
} // namespace EEMmu
