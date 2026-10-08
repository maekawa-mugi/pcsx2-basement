// SPDX-FileCopyrightText: 2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Types.h"

// Opt-in hot-spot profile for deciding which AVX-512 work is worth doing.
// Enabled only when the environment variable PCSX2_AVX512_PROFILE names an
// output directory. When disabled, no instruction is added to generated code.
// Counts are per generated block: compile-time allocator events weighted by
// runtime execution count. They are host code events, not PS2 cycles.
namespace AVX512Profile
{
	enum SoftCounter : u32
	{
		VuAddSubStacklessDone, // common packed/scalar path finished without fallback
		VuAddSubStacklessFallback, // common path rejected input or result
		VuAddSubFlagPath, // flag-producing (or MXCSR-switching) path entered directly
		VuAddSubRepair, // per-lane repair helper called
		VuMul,
		VuMadd,
		VuAddSubMxcsrPath, // MXCSR switching required (rounding mode is not chop); subset of FlagPath
		VuMulStacklessDone,
		VuMulFastDone,
		VuMulBoothCall,
		VuMulExactVectorCall,
		VuMaddStacklessDone,
		VuMaddCompactDone,
		VuMaddRegisterDone,
		VuMaddExactVectorCall,
		VuMaddPackedCall,
		VuMaddLaneCall,
		VuMulFailInput,
		VuMulFailResult,
		VuMulFailCorrected,
		VuMulFailTable,
		VuMaddFailAccOverflow,
		VuMaddFailInput,
		VuMaddFailProduct,
		VuMaddFailCorrected,
		VuMaddFailAccInput,
		VuMaddFailResult,
		VuProgramRuns0, // microVU dispatcher entries (program starts and resumes)
		VuProgramRuns1,
		SoftCounterCount
	};

	struct EEBlock
	{
		u32 startpc;
		u32 counter; // index of the runtime execution counter
		u64 reloads, writebacks, evictions, temporaries, high_temporaries, helper_spills;
	};

	struct VUBlock
	{
		u32 vu;
		u32 startpc;
		u32 counter;
		u32 vf_loads; // VF/ACC memory loads emitted by the microVU allocator
		u32 vf_writebacks; // VF/ACC memory stores emitted by the microVU allocator
		u32 vf_high_loads; // VF/ACC reloads served from an XMM16-31 copy instead
		// Why the remaining loads still came from memory (subsets of vf_loads):
		u32 vf_first_loads; // VF not touched earlier in the block
		u32 vf_flushed_reloads; // touched earlier, but copies were invalidated since (flush/call)
		u32 vf_lost_reloads; // full reload, copy missing (replaced, dropped or never stashed)
		u32 vf_partial_reloads; // single-lane reload, not served from copies
		u32 vf_copy_replaced; // stash that overwrote another valid copy (all 16 in use)
		u32 vf_copy_refused; // stash skipped because a modified register of the same VF exists
		u32 vf_home_loads; // subset of vf_high_loads served from a block-spanning VF home register
		u32 vf_home_refills; // home registers reloaded from memory after a helper call
		u32 vf_home_spills; // lazy homes: home registers stored to memory (program end, before calls)
		u32 vf_home_only_writebacks; // lazy homes: write-backs that updated only the home register
		// Per VF/ACC (index 32) load requests that reached the allocator's load path, and those
		// of them that still read memory. Used to pick which VFs get home registers.
		u32 vf_requests_by_reg[33];
		u32 vf_memory_loads_by_reg[33];
	};

	bool Enabled();

	// Returns nullptr when profiling is disabled or the counter pool is full.
	EEBlock* NewEEBlock(u32 startpc);
	VUBlock* NewVUBlock(u32 vu, u32 startpc);

	// Records one compiled occurrence of an opcode in the block owning `counter`.
	// unit is 'E' (EE), '0' (VU0 micro) or '1' (VU1). Written to ops.csv.
	void CountOp(u32 counter, char unit, const char* name);

	// Runtime counters live in a static pool so generated code can address
	// them RIP-relative like other emulator globals.
	u64* Counter(u32 index);
	u64* SoftCounterPtr(SoftCounter counter);

	// Writes ee_blocks.csv, vu_blocks.csv, vu_vf.csv, softfloat.csv and ops.csv, then resets.
	void Dump();
} // namespace AVX512Profile
