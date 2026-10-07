// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// Opt-in Full TLB counters for finding where EE time goes under Linux.
// Set PCSX2_FULLTLB_STATS=1 before starting PCSX2; one summary line per second is
// written to the console log. Diagnostic only, needs proper testing.

#include "common/Pcsx2Types.h"

#include <cstdlib>

namespace FullTLBStats
{
	enum Counter : u32
	{
		DiscardStatus,
		DiscardConfig,
		DiscardAsid,
		DiscardTlbGeneration,
		DiscardWriteGeneration,
		DiscardContent,
		DiscardKernel,
		DiscardUser,
		Recompiles,
		RecompilesKernel,
		RecompileBytes,
		JitResets,
		JitResetsFromClear,
		RecClears,
		FastmemFaults,
		FastmemFaultsSlowpath,
		TranslationMissPage,
		TranslationMissContext,
		TranslationMissGeneration,
		EventTests,
		ContextContinued,
		TlbRevalidated,
		TlbFetchMiss,
		CounterCount
	};

	// Discard reasons as written by the block-entry guard. Content mismatches jump
	// straight to the discard dispatcher, so Content is the default.
	enum DiscardReason : u32
	{
		ReasonStatus = DiscardStatus,
		ReasonConfig = DiscardConfig,
		ReasonAsid = DiscardAsid,
		ReasonTlbGeneration = DiscardTlbGeneration,
		ReasonWriteGeneration = DiscardWriteGeneration,
		ReasonContent = DiscardContent,
	};

	inline const bool g_enabled = [] {
		const char* value = std::getenv("PCSX2_FULLTLB_STATS");
		return value && value[0] && value[0] != '0';
	}();

	inline u32 g_counters[CounterCount] = {};
	inline u32 g_exceptions[32] = {};
	inline u32 g_last_discard_reason = ReasonContent;
	inline u64 g_compile_ticks = 0;

	inline void Add(Counter counter, u32 amount = 1)
	{
		if (g_enabled)
			g_counters[counter] += amount;
	}

	inline void AddException(u32 exc_code)
	{
		if (g_enabled)
			g_exceptions[exc_code & 31]++;
	}

	// Implemented in x86/ix86-32/iR5900.cpp.
	void RecordDiscard(u32 startpc);
	void MaybeReport();
} // namespace FullTLBStats
