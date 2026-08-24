// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Common.h"
#include "EEMemory.h"

#include "Cache.h"
#include "R5900.h"

#include <algorithm>
#include <array>

namespace EEMemory
{
	static u8 s_recompiler_access_fault = 0;

	struct RecompilerTranslationCacheEntry
	{
		u32 virtual_page = 0xffffffff;
		u32 translation_generation = 0;
		u32 tlb_entry_index = RECOMPILER_TRANSLATION_NO_TLB_ENTRY;
		u32 status_key = 0;
		u8 config_key = 0;
		u8 asid = 0;
		EEMmu::AccessType access_type = EEMmu::AccessType::Fetch;
		EEMmu::TranslationResult translation;
	};

	static thread_local std::array<RecompilerTranslationCacheEntry, RECOMPILER_TRANSLATION_CACHE_SIZE>
		s_recompiler_translation_cache;
	alignas(64) static std::array<RecompilerJitTranslationCacheEntry, RECOMPILER_TRANSLATION_CACHE_SIZE> s_recompiler_jit_load_translation_cache;
	alignas(64) static std::array<RecompilerJitTranslationCacheEntry, RECOMPILER_TRANSLATION_CACHE_SIZE> s_recompiler_jit_store_translation_cache;
	alignas(64) static std::array<RecompilerJitTranslationCacheEntry, RECOMPILER_TRANSLATION_CACHE_SIZE> s_recompiler_jit_cache_translation_cache;
	static std::array<u32, vtlb_private::VTLB_PMAP_ITEMS> s_physical_write_generation;
	static u32 s_scratchpad_write_generation = 0;

	static constexpr size_t TRANSLATION_TRACE_SIZE = 2048;
	static constexpr size_t USER_FAULT_TRACE_SIZE = 512;
	static constexpr size_t USER_EXCEPTION_TRACE_SIZE = 256;
	static constexpr size_t DIAGNOSTIC_READ_TRACE_SIZE = 512;
	static constexpr size_t GP_RELOAD_TRACE_SIZE = 64;

	static bool IsFullTLBDiagnosticTraceEnabled()
	{
		return EmuConfig.Cpu.EnableExperimentalEETLB && EmuConfig.Cpu.EnableFullTLBDiagnosticTrace;
	}

	struct RawTLBTraceEntry
	{
		bool valid = false;
		u32 page_mask = 0;
		u32 entry_hi = 0;
		u32 entry_lo0 = 0;
		u32 entry_lo1 = 0;
	};

	struct TranslationTraceEntry
	{
		u64 sequence = 0;
		u64 cycle = 0;
		u32 pc = 0;
		u32 instruction = 0;
		u32 vaddr = 0;
		u32 status = 0;
		u32 cause = 0;
		u32 epc = 0;
		u32 bad_vaddr = 0;
		u32 entry_hi = 0;
		u32 sp = 0;
		u32 ra = 0;
		u32 translation_generation = 0;
		EEMmu::AccessType access_type = EEMmu::AccessType::Fetch;
		EEMmu::TranslationResult translation;
		RawTLBTraceEntry raw_tlb;
	};

	struct UserFaultTraceEntry
	{
		u64 sequence = 0;
		TranslationTraceEntry access;
		TranslationTraceEntry epc_translation;
		EEMmu::ExceptionRegisters before = {};
		EEMmu::ExceptionRegisters after = {};
		u32 exception_vector = 0;
		bool taken = false;
		bool actual_instruction_valid = false;
		u32 actual_instruction = 0;
		std::array<u64, 32> gpr = {};
	};

	struct UserExceptionTraceEntry
	{
		u64 sequence = 0;
		u64 cycle = 0;
		u32 pc = 0;
		u32 instruction = 0;
		u32 sp = 0;
		u32 ra = 0;
		u32 code = 0;
		bool branch_delay = false;
		EEMmu::ExceptionRegisters before = {};
		EEMmu::ExceptionRegisters after = {};
		u32 exception_vector = 0;
		TranslationTraceEntry epc_translation;
		bool actual_instruction_valid = false;
		u32 actual_instruction = 0;
		std::array<u64, 32> gpr = {};
	};

	struct DiagnosticReadTraceEntry
	{
		u64 sequence = 0;
		u64 cycle = 0;
		u64 returned_value = 0;
		u64 backing_value = 0;
		u32 pc = 0;
		u32 instruction = 0;
		u32 vaddr = 0;
		u32 status = 0;
		u32 entry_hi = 0;
		u32 translation_generation = 0;
		EEMmu::TranslationResult translation;
		RawTLBTraceEntry raw_tlb;
		bool backing_value_valid = false;
	};

	static std::array<TranslationTraceEntry, TRANSLATION_TRACE_SIZE> s_translation_trace;
	static size_t s_translation_trace_next = 0;
	static size_t s_translation_trace_count = 0;
	static u64 s_translation_trace_sequence = 0;
	static std::array<UserFaultTraceEntry, USER_FAULT_TRACE_SIZE> s_user_fault_trace;
	static size_t s_user_fault_trace_next = 0;
	static size_t s_user_fault_trace_count = 0;
	static u64 s_user_fault_trace_sequence = 0;
	static std::array<UserExceptionTraceEntry, USER_EXCEPTION_TRACE_SIZE> s_user_exception_trace;
	static size_t s_user_exception_trace_next = 0;
	static size_t s_user_exception_trace_count = 0;
	static u64 s_user_exception_trace_sequence = 0;
	static u32 s_diagnostic_read_vaddr = 0;
	static std::array<DiagnosticReadTraceEntry, DIAGNOSTIC_READ_TRACE_SIZE> s_diagnostic_read_trace;
	static size_t s_diagnostic_read_trace_next = 0;
	static size_t s_diagnostic_read_trace_count = 0;
	static u64 s_diagnostic_read_trace_sequence = 0;
	static std::array<DiagnosticReadTraceEntry, GP_RELOAD_TRACE_SIZE> s_gp_reload_trace;
	static size_t s_gp_reload_trace_next = 0;
	static size_t s_gp_reload_trace_count = 0;
	static DiagnosticReadTraceEntry s_zero_t9_read_trace;
	static bool s_zero_t9_read_trace_valid = false;

	static void NoteTranslatedWrite(const EEMmu::TranslationResult& translation)
	{
		if (translation.target == EEMmu::Target::Scratchpad)
		{
			++s_scratchpad_write_generation;
		}
		else if (translation.paddr < vtlb_private::VTLB_PMAP_SZ)
		{
			++s_physical_write_generation[translation.paddr >> vtlb_private::VTLB_PAGE_BITS];
		}
	}

	static EEMmu::TranslationContext GetCurrentContext()
	{
		return {
			cpuRegs.CP0.n.Status.val,
			cpuRegs.CP0.n.Config,
			static_cast<u8>(cpuRegs.CP0.n.EntryHi),
			tlb,
			EEMmu::TLB_ENTRY_COUNT,
		};
	}

	static EEMmu::ExceptionRegisters GetCurrentExceptionRegisters()
	{
		return {
			cpuRegs.CP0.n.Status.val,
			cpuRegs.CP0.n.Cause,
			cpuRegs.CP0.n.EPC,
			cpuRegs.CP0.n.BadVAddr,
			cpuRegs.CP0.n.Context,
			cpuRegs.CP0.n.EntryHi,
		};
	}

	static std::array<u64, 32> CaptureGPRs()
	{
		std::array<u64, 32> result;
		for (size_t i = 0; i < result.size(); i++)
			result[i] = cpuRegs.GPR.r[i].UD[0];
		return result;
	}

	static bool ReadTranslatedInstruction(const EEMmu::TranslationResult& translation,
		u32 vaddr, u32* instruction)
	{
		if (!instruction || (vaddr & 3) != 0 || translation.fault != EEMmu::Fault::None)
			return false;
		if (translation.target == EEMmu::Target::Scratchpad)
		{
			*instruction = vtlb_sprRead<u32>(translation.scratch_offset);
			return true;
		}
		if (!vtlb_IsPhysicalAddressMapped(translation.paddr))
			return false;
		*instruction = vtlb_physRead<u32>(translation.paddr);
		return true;
	}

	static RawTLBTraceEntry CaptureRawTLBEntry(int index)
	{
		RawTLBTraceEntry result;
		if (index < 0 || index >= static_cast<int>(EEMmu::TLB_ENTRY_COUNT))
			return result;

		result.valid = true;
		EEMmu::ReadTLBEntry(tlb[index], &result.page_mask, &result.entry_hi,
			&result.entry_lo0, &result.entry_lo1);
		return result;
	}

	static TranslationTraceEntry BuildTranslationTraceEntry(u64 sequence, u32 vaddr,
		EEMmu::AccessType access_type, u32 fault_pc, const EEMmu::TranslationResult& translation)
	{
		TranslationTraceEntry entry;
		entry.sequence = sequence;
		entry.cycle = cpuRegs.cycle;
		entry.pc = fault_pc;
		entry.instruction = cpuRegs.code;
		entry.vaddr = vaddr;
		entry.status = cpuRegs.CP0.n.Status.val;
		entry.cause = cpuRegs.CP0.n.Cause;
		entry.epc = cpuRegs.CP0.n.EPC;
		entry.bad_vaddr = cpuRegs.CP0.n.BadVAddr;
		entry.entry_hi = cpuRegs.CP0.n.EntryHi;
		entry.sp = cpuRegs.GPR.n.sp.UL[0];
		entry.ra = cpuRegs.GPR.n.ra.UL[0];
		entry.translation_generation = EEMmu::GetTranslationGeneration();
		entry.access_type = access_type;
		entry.translation = translation;
		entry.raw_tlb = CaptureRawTLBEntry(translation.matched_tlb_index);
		return entry;
	}

	static TranslationTraceEntry RecordTranslation(u32 vaddr, EEMmu::AccessType access_type,
		u32 fault_pc, const EEMmu::TranslationResult& translation)
	{
		if (!IsFullTLBDiagnosticTraceEnabled())
			return {};
		// Successful fetches dominate the stream and each saved exception already records its EPC mapping.
		if (access_type == EEMmu::AccessType::Fetch && translation.fault == EEMmu::Fault::None)
			return {};

		TranslationTraceEntry entry = BuildTranslationTraceEntry(
			++s_translation_trace_sequence, vaddr, access_type, fault_pc, translation);
		s_translation_trace[s_translation_trace_next] = entry;
		s_translation_trace_next = (s_translation_trace_next + 1) % TRANSLATION_TRACE_SIZE;
		s_translation_trace_count = std::min(s_translation_trace_count + 1, TRANSLATION_TRACE_SIZE);
		return entry;
	}

	static bool RaiseTrackedFault(EEMmu::Fault fault, EEMmu::AccessType access_type,
		u32 vaddr, u32 fault_pc, bool branch_delay, const TranslationTraceEntry& access)
	{
		const bool capture_user_fault = IsFullTLBDiagnosticTraceEnabled() &&
		                                EEMmu::IsUserMode(cpuRegs.CP0.n.Status.val);
		UserFaultTraceEntry fault_trace;
		if (capture_user_fault)
		{
			fault_trace.sequence = ++s_user_fault_trace_sequence;
			fault_trace.access = access;
			fault_trace.before = GetCurrentExceptionRegisters();
			fault_trace.gpr = CaptureGPRs();
			const u32 architectural_epc = branch_delay ? fault_pc - 4 : fault_pc;
			const EEMmu::TranslationResult epc_translation =
				EEMmu::TranslateAddress(GetCurrentContext(), architectural_epc, EEMmu::AccessType::Fetch);
			fault_trace.epc_translation = BuildTranslationTraceEntry(
				access.sequence, architectural_epc, EEMmu::AccessType::Fetch, architectural_epc, epc_translation);
			fault_trace.actual_instruction_valid = ReadTranslatedInstruction(
				epc_translation, architectural_epc, &fault_trace.actual_instruction);
		}

		const bool taken = cpuEETlbException(fault, access_type, vaddr, fault_pc, branch_delay);
		if (capture_user_fault)
		{
			fault_trace.taken = taken;
			fault_trace.after = GetCurrentExceptionRegisters();
			fault_trace.exception_vector = cpuRegs.pc;
			s_user_fault_trace[s_user_fault_trace_next] = fault_trace;
			s_user_fault_trace_next = (s_user_fault_trace_next + 1) % USER_FAULT_TRACE_SIZE;
			s_user_fault_trace_count = std::min(s_user_fault_trace_count + 1, USER_FAULT_TRACE_SIZE);
		}
		return taken;
	}

	static void LogWarnings(EEMmu::Warning warnings, u32 vaddr)
	{
#ifdef PCSX2_DEVBUILD
		static u32 reported_warnings = 0;
		const u32 new_warnings = static_cast<u32>(warnings) & ~reported_warnings;
		if (new_warnings == 0)
			return;

		reported_warnings |= new_warnings;
		// Undefined MMU inputs use deterministic fallbacks; needs proper testing.
		Console.Warning("Experimental EE TLB warning 0x%08x at vaddr 0x%08x", new_warnings, vaddr);
#else
		(void)warnings;
		(void)vaddr;
#endif
	}

	static EEMmu::TranslationResult Translate(u32 vaddr, EEMmu::AccessType access_type,
		u32 fault_pc, bool branch_delay, bool suppress_fault, bool recompiler_access = false)
	{
		EEMmu::TranslationResult result = EEMmu::TranslateAddress(GetCurrentContext(), vaddr, access_type);
		LogWarnings(result.warnings, vaddr);
		const TranslationTraceEntry trace = RecordTranslation(vaddr, access_type, fault_pc, result);
		if (result.fault != EEMmu::Fault::None && !suppress_fault)
		{
			RaiseTrackedFault(result.fault, access_type, vaddr, fault_pc, branch_delay, trace);
			if (recompiler_access)
				s_recompiler_access_fault = 1;
			else
				Cpu->CancelInstruction();
		}
		return result;
	}

	static bool IsPhysicalBusError(
		const EEMmu::TranslationResult& translation, bool use_data_cache = true)
	{
		if (translation.fault != EEMmu::Fault::None || translation.target != EEMmu::Target::Physical)
			return false;

		// Hardware can retain accesses to an invalid PFN in the data cache without issuing a bus cycle.
		if (use_data_cache && CHECK_CACHE && translation.cache_mode == 3)
			return false;

		return !vtlb_IsPhysicalAddressMapped(translation.paddr);
	}

	static bool HandlePhysicalBusError(u32 vaddr, const EEMmu::TranslationResult& translation,
		EEMmu::AccessType access_type, bool use_data_cache = true, bool suppress_fault = false,
		bool recompiler_access = false)
	{
		if (!IsPhysicalBusError(translation, use_data_cache))
			return false;
		if (suppress_fault)
			return true;

		const u32 fault_pc = access_type == EEMmu::AccessType::Fetch ? vaddr : cpuRegs.pc - 4;
		EEMmu::TranslationResult bus_error_translation = translation;
		bus_error_translation.fault = EEMmu::Fault::BusError;
		const TranslationTraceEntry trace =
			RecordTranslation(vaddr, access_type, fault_pc, bus_error_translation);
		const bool taken = RaiseTrackedFault(EEMmu::Fault::BusError, access_type, vaddr,
			fault_pc, cpuRegs.branch != 0, trace);
		// Data bus errors are imprecise and the load/store instruction completes with undefined results.
		// An instruction bus error cannot execute the unavailable instruction.
		if (taken && access_type == EEMmu::AccessType::Fetch)
			Cpu->CancelInstruction();
		else if (taken && recompiler_access)
			s_recompiler_access_fault = 1;
		return true;
	}

	EEMmu::TranslationResult TranslateForRecompiler(u32 vaddr, EEMmu::AccessType access_type)
	{
		s_recompiler_access_fault = 0;

		const u32 virtual_page = vaddr & ~vtlb_private::VTLB_PAGE_MASK;
		const u32 page_offset = vaddr & vtlb_private::VTLB_PAGE_MASK;
		const u32 status_key = cpuRegs.CP0.n.Status.val & 0x1e;
		const u8 config_key = cpuRegs.CP0.n.Config & 0x7;
		const u8 asid = static_cast<u8>(cpuRegs.CP0.n.EntryHi);
		const size_t cache_index = ((virtual_page >> vtlb_private::VTLB_PAGE_BITS) ^
									   (static_cast<u32>(asid) << 1) ^ (static_cast<u32>(access_type) << 6)) &
		                           (RECOMPILER_TRANSLATION_CACHE_SIZE - 1);
		RecompilerTranslationCacheEntry& entry = s_recompiler_translation_cache[cache_index];
		const bool generation_matches = entry.tlb_entry_index == RECOMPILER_TRANSLATION_NO_TLB_ENTRY ||
		                                (entry.tlb_entry_index < EEMmu::TLB_ENTRY_COUNT &&
											entry.translation_generation == EEMmu::GetTLBEntryGeneration(entry.tlb_entry_index));
		if (entry.virtual_page == virtual_page &&
			generation_matches && entry.status_key == status_key && entry.config_key == config_key &&
			entry.asid == asid && entry.access_type == access_type)
		{
			EEMmu::TranslationResult result = entry.translation;
			if (result.target == EEMmu::Target::Scratchpad)
				result.scratch_offset += page_offset;
			else
				result.paddr += page_offset;
			RecordTranslation(vaddr, access_type, cpuRegs.pc - 4, result);
			return result;
		}

		EEMmu::TranslationResult result =
			Translate(vaddr, access_type, cpuRegs.pc - 4, cpuRegs.branch != 0, false, true);
		if (result.fault == EEMmu::Fault::None)
		{
			entry.virtual_page = virtual_page;
			entry.tlb_entry_index = result.matched_tlb_index >= 0 ?
			                            static_cast<u32>(result.matched_tlb_index) :
			                            RECOMPILER_TRANSLATION_NO_TLB_ENTRY;
			entry.translation_generation = entry.tlb_entry_index != RECOMPILER_TRANSLATION_NO_TLB_ENTRY ?
			                                   EEMmu::GetTLBEntryGeneration(entry.tlb_entry_index) :
			                                   0;
			entry.status_key = status_key;
			entry.config_key = config_key;
			entry.asid = asid;
			entry.access_type = access_type;
			entry.translation = result;
			if (entry.translation.target == EEMmu::Target::Scratchpad)
				entry.translation.scratch_offset -= page_offset;
			else
				entry.translation.paddr -= page_offset;
		}
		return result;
	}

	EEMmu::TranslationResult TranslateForCurrentInstruction(u32 vaddr, EEMmu::AccessType access_type)
	{
		return Translate(vaddr, access_type, cpuRegs.pc - 4, cpuRegs.branch != 0, false);
	}

	EEMmu::TranslationResult TranslateAndSuppressFault(u32 vaddr, EEMmu::AccessType access_type)
	{
		return Translate(vaddr, access_type, cpuRegs.pc - 4, cpuRegs.branch != 0, true);
	}

	template <typename DataType>
	static DataType ReadTranslated(u32 vaddr, const EEMmu::TranslationResult& translation, bool use_data_cache = true)
	{
		if (translation.fault != EEMmu::Fault::None) [[unlikely]]
			return {};
		if (use_data_cache && CHECK_CACHE && translation.target == EEMmu::Target::Physical && translation.cache_mode == 3)
		{
			if constexpr (sizeof(DataType) == 1)
				return readCache8Physical(vaddr, translation.paddr);
			else if constexpr (sizeof(DataType) == 2)
				return readCache16Physical(vaddr, translation.paddr);
			else if constexpr (sizeof(DataType) == 4)
				return readCache32Physical(vaddr, translation.paddr);
			else
				return readCache64Physical(vaddr, translation.paddr);
		}
		return translation.target == EEMmu::Target::Scratchpad ?
		           vtlb_sprRead<DataType>(translation.scratch_offset) :
		           vtlb_physRead<DataType>(translation.paddr);
	}

	static u128 ReadTranslated128(u32 vaddr, const EEMmu::TranslationResult& translation)
	{
		if (translation.fault != EEMmu::Fault::None) [[unlikely]]
			return {};
		if (CHECK_CACHE && translation.target == EEMmu::Target::Physical && translation.cache_mode == 3)
			return r128_to_u128(readCache128Physical(vaddr, translation.paddr));
		const r128 value = translation.target == EEMmu::Target::Scratchpad ?
		                       vtlb_sprRead128(translation.scratch_offset) :
		                       vtlb_physRead128(translation.paddr);
		return r128_to_u128(value);
	}

	template <typename DataType>
	static void WriteTranslated(u32 vaddr, const EEMmu::TranslationResult& translation, DataType value)
	{
		if (translation.fault != EEMmu::Fault::None) [[unlikely]]
			return;
		NoteTranslatedWrite(translation);
		if (CHECK_CACHE && translation.target == EEMmu::Target::Physical && translation.cache_mode == 3)
		{
			if constexpr (sizeof(DataType) == 1)
				writeCache8Physical(vaddr, translation.paddr, value);
			else if constexpr (sizeof(DataType) == 2)
				writeCache16Physical(vaddr, translation.paddr, value);
			else if constexpr (sizeof(DataType) == 4)
				writeCache32Physical(vaddr, translation.paddr, value);
			else
				writeCache64Physical(vaddr, translation.paddr, value);
			return;
		}
		if (translation.target == EEMmu::Target::Scratchpad)
			vtlb_sprWrite<DataType>(translation.scratch_offset, value);
		else
			vtlb_physWrite<DataType>(translation.paddr, value);
	}

	static void WriteTranslated128(u32 vaddr, const EEMmu::TranslationResult& translation, const u128& value)
	{
		if (translation.fault != EEMmu::Fault::None) [[unlikely]]
			return;
		NoteTranslatedWrite(translation);
		if (CHECK_CACHE && translation.target == EEMmu::Target::Physical && translation.cache_mode == 3)
		{
			writeCache128Physical(vaddr, translation.paddr, &value);
			return;
		}
		if (translation.target == EEMmu::Target::Scratchpad)
			vtlb_sprWrite128(translation.scratch_offset, r128_load(&value));
		else
			vtlb_physWrite128(translation.paddr, r128_load(&value));
	}

	u32 Fetch32(u32 vaddr)
	{
		if (vaddr & 3) [[unlikely]]
		{
			EEMmu::TranslationResult translation;
			translation.fault = EEMmu::Fault::AddressError;
			const TranslationTraceEntry trace =
				RecordTranslation(vaddr, EEMmu::AccessType::Fetch, vaddr, translation);
			RaiseTrackedFault(EEMmu::Fault::AddressError, EEMmu::AccessType::Fetch,
				vaddr, vaddr, cpuRegs.branch != 0, trace);
			Cpu->CancelInstruction();
			return 0;
		}
		const EEMmu::TranslationResult translation =
			Translate(vaddr, EEMmu::AccessType::Fetch, vaddr, cpuRegs.branch != 0, false);
		if (HandlePhysicalBusError(vaddr, translation, EEMmu::AccessType::Fetch, false))
			return 0;
		return ReadTranslated<u32>(vaddr, translation, false);
	}

	bool TryFetch32(u32 vaddr, u32* value)
	{
		if ((vaddr & 3) != 0 || !value)
			return false;

		const EEMmu::TranslationResult translation =
			Translate(vaddr, EEMmu::AccessType::Fetch, vaddr, false, true);
		if (translation.fault != EEMmu::Fault::None)
			return false;
		if (HandlePhysicalBusError(vaddr, translation, EEMmu::AccessType::Fetch, false, true))
			return false;

		*value = ReadTranslated<u32>(vaddr, translation, false);
		return true;
	}

	FetchPage TranslateFetchPage(u32 vaddr)
	{
		FetchPage page;
		page.virtual_page = vaddr & ~vtlb_private::VTLB_PAGE_MASK;
		page.translation = Translate(vaddr, EEMmu::AccessType::Fetch, vaddr, false, false);
		if (page.translation.fault != EEMmu::Fault::None)
			return page;
		if (HandlePhysicalBusError(vaddr, page.translation, EEMmu::AccessType::Fetch, false))
		{
			page.translation.fault = EEMmu::Fault::BusError;
			return page;
		}

		const u32 page_offset = vaddr & vtlb_private::VTLB_PAGE_MASK;
		if (page.translation.target == EEMmu::Target::Scratchpad)
			page.translation.scratch_offset -= page_offset;
		else
			page.translation.paddr -= page_offset;
		return page;
	}

	u32 Fetch32(const FetchPage& page, u32 vaddr)
	{
		pxAssert((vaddr & ~vtlb_private::VTLB_PAGE_MASK) == page.virtual_page);
		if (page.translation.fault != EEMmu::Fault::None) [[unlikely]]
			return 0;
		const u32 page_offset = vaddr & vtlb_private::VTLB_PAGE_MASK;
		return page.translation.target == EEMmu::Target::Scratchpad ?
		           vtlb_sprRead<u32>(page.translation.scratch_offset + page_offset) :
		           vtlb_physRead<u32>(page.translation.paddr + page_offset);
	}

	u8 Read8(u32 vaddr)
	{
		const EEMmu::TranslationResult translation = TranslateForCurrentInstruction(vaddr, EEMmu::AccessType::Load);
		return HandlePhysicalBusError(vaddr, translation, EEMmu::AccessType::Load) ?
		           0 :
		           ReadTranslated<u8>(vaddr, translation);
	}

	u16 Read16(u32 vaddr)
	{
		const EEMmu::TranslationResult translation = TranslateForCurrentInstruction(vaddr, EEMmu::AccessType::Load);
		return HandlePhysicalBusError(vaddr, translation, EEMmu::AccessType::Load) ?
		           0 :
		           ReadTranslated<u16>(vaddr, translation);
	}

	u32 Read32(u32 vaddr)
	{
		const EEMmu::TranslationResult translation = TranslateForCurrentInstruction(vaddr, EEMmu::AccessType::Load);
		return HandlePhysicalBusError(vaddr, translation, EEMmu::AccessType::Load) ?
		           0 :
		           ReadTranslated<u32>(vaddr, translation);
	}

	u64 Read64(u32 vaddr)
	{
		const EEMmu::TranslationResult translation = TranslateForCurrentInstruction(vaddr, EEMmu::AccessType::Load);
		return HandlePhysicalBusError(vaddr, translation, EEMmu::AccessType::Load) ?
		           0 :
		           ReadTranslated<u64>(vaddr, translation);
	}

	u128 Read128(u32 vaddr)
	{
		const EEMmu::TranslationResult translation = TranslateForCurrentInstruction(vaddr, EEMmu::AccessType::Load);
		return HandlePhysicalBusError(vaddr, translation, EEMmu::AccessType::Load) ?
		           u128{} :
		           ReadTranslated128(vaddr, translation);
	}

	void Write8(u32 vaddr, u8 value)
	{
		const EEMmu::TranslationResult translation = TranslateForCurrentInstruction(vaddr, EEMmu::AccessType::Store);
		if (!HandlePhysicalBusError(vaddr, translation, EEMmu::AccessType::Store))
			WriteTranslated(vaddr, translation, value);
	}

	void Write16(u32 vaddr, u16 value)
	{
		const EEMmu::TranslationResult translation = TranslateForCurrentInstruction(vaddr, EEMmu::AccessType::Store);
		if (!HandlePhysicalBusError(vaddr, translation, EEMmu::AccessType::Store))
			WriteTranslated(vaddr, translation, value);
	}

	void Write32(u32 vaddr, u32 value)
	{
		const EEMmu::TranslationResult translation = TranslateForCurrentInstruction(vaddr, EEMmu::AccessType::Store);
		if (!HandlePhysicalBusError(vaddr, translation, EEMmu::AccessType::Store))
			WriteTranslated(vaddr, translation, value);
	}

	void Write64(u32 vaddr, u64 value)
	{
		const EEMmu::TranslationResult translation = TranslateForCurrentInstruction(vaddr, EEMmu::AccessType::Store);
		if (!HandlePhysicalBusError(vaddr, translation, EEMmu::AccessType::Store))
			WriteTranslated(vaddr, translation, value);
	}

	void Write128(u32 vaddr, const u128& value)
	{
		const EEMmu::TranslationResult translation = TranslateForCurrentInstruction(vaddr, EEMmu::AccessType::Store);
		if (!HandlePhysicalBusError(vaddr, translation, EEMmu::AccessType::Store))
			WriteTranslated128(vaddr, translation, value);
	}

	const u8* GetRecompilerAccessFaultAddress()
	{
		return &s_recompiler_access_fault;
	}

	u32* GetPhysicalWriteGenerationBase()
	{
		return s_physical_write_generation.data();
	}

	const u32* GetPhysicalWriteGenerationAddress(u32 paddr)
	{
		pxAssert(paddr < vtlb_private::VTLB_PMAP_SZ);
		return &s_physical_write_generation[paddr >> vtlb_private::VTLB_PAGE_BITS];
	}

	const u32* GetScratchpadWriteGenerationAddress()
	{
		return &s_scratchpad_write_generation;
	}

	void ClearRecompilerAccessFault()
	{
		s_recompiler_access_fault = 0;
	}

	RecompilerJitTranslationCacheEntry* GetRecompilerJitTranslationCacheBase(EEMmu::AccessType access_type)
	{
		pxAssert(access_type == EEMmu::AccessType::Load || access_type == EEMmu::AccessType::Store ||
				 access_type == EEMmu::AccessType::Cache);
		if (access_type == EEMmu::AccessType::Load)
			return s_recompiler_jit_load_translation_cache.data();
		if (access_type == EEMmu::AccessType::Store)
			return s_recompiler_jit_store_translation_cache.data();
		return s_recompiler_jit_cache_translation_cache.data();
	}

	u32 GetRecompilerJitTranslationContextKey()
	{
		return (cpuRegs.CP0.n.Status.val & 0x1e) |
		       ((cpuRegs.CP0.n.Config & 0x7) << 8) |
		       ((cpuRegs.CP0.n.EntryHi & 0xff) << 16);
	}

	u64 ResolveRecompilerJitTranslation(u32 vaddr, u32 raw_access_type)
	{
		const EEMmu::AccessType access_type = static_cast<EEMmu::AccessType>(raw_access_type);
		pxAssert(access_type == EEMmu::AccessType::Load || access_type == EEMmu::AccessType::Store ||
				 access_type == EEMmu::AccessType::Cache);
		s_recompiler_access_fault = 0;

		const EEMmu::TranslationResult translation =
			Translate(vaddr, access_type, cpuRegs.pc - 4, cpuRegs.branch != 0, false, true);
		if (translation.fault != EEMmu::Fault::None)
			return 0;
		if (access_type != EEMmu::AccessType::Cache &&
			HandlePhysicalBusError(vaddr, translation, access_type, true, false, true))
		{
			return static_cast<u64>(RECOMPILER_TRANSLATION_NO_ACCESS) << 32;
		}

		const u32 page_offset = vaddr & vtlb_private::VTLB_PAGE_MASK;
		const u32 translated_page = translation.target == EEMmu::Target::Scratchpad ?
		                                translation.scratch_offset - page_offset :
		                                translation.paddr - page_offset;
		u32 attributes = static_cast<u32>(translation.cache_mode) << RECOMPILER_TRANSLATION_CACHE_MODE_SHIFT;
		if (translation.target == EEMmu::Target::Scratchpad)
			attributes |= RECOMPILER_TRANSLATION_SCRATCHPAD;
		const u64 packed_translation = translated_page | (static_cast<u64>(attributes) << 32);
		uptr host_page = 0;
		if (access_type != EEMmu::AccessType::Cache)
		{
			if (translation.target == EEMmu::Target::Scratchpad)
			{
				host_page = reinterpret_cast<uptr>(&eeMem->Scratch[translated_page]);
			}
			else if ((!CHECK_CACHE || translation.cache_mode != 3) &&
					 translated_page < vtlb_private::VTLB_PMAP_SZ)
			{
				const vtlb_private::VTLBPhysical mapping =
					vtlb_private::vtlbdata.pmap[translated_page >> vtlb_private::VTLB_PAGE_BITS];
				if (!mapping.isHandler())
					host_page = mapping.assumePtr();
			}
		}

		const u32 virtual_page = vaddr >> vtlb_private::VTLB_PAGE_BITS;
		RecompilerJitTranslationCacheEntry* const cache = GetRecompilerJitTranslationCacheBase(access_type);
		RecompilerJitTranslationCacheEntry& entry = cache[virtual_page & (RECOMPILER_TRANSLATION_CACHE_SIZE - 1)];
		entry.host_page = host_page;
		entry.translation = packed_translation;
		entry.context_key = GetRecompilerJitTranslationContextKey();
		entry.tlb_entry_index = translation.matched_tlb_index >= 0 ?
		                            static_cast<u32>(translation.matched_tlb_index) :
		                            RECOMPILER_TRANSLATION_NO_TLB_ENTRY;
		entry.translation_generation = entry.tlb_entry_index != RECOMPILER_TRANSLATION_NO_TLB_ENTRY ?
		                                   EEMmu::GetTLBEntryGeneration(entry.tlb_entry_index) :
		                                   0;
		entry.virtual_page = virtual_page;
		return packed_translation;
	}

	void RaiseRecompilerAddressError(u32 vaddr, u32 raw_access_type)
	{
		const EEMmu::AccessType access_type = static_cast<EEMmu::AccessType>(raw_access_type);
		pxAssert(access_type == EEMmu::AccessType::Load || access_type == EEMmu::AccessType::Store);
		s_recompiler_access_fault = 0;
		EEMmu::TranslationResult translation;
		translation.fault = EEMmu::Fault::AddressError;
		const TranslationTraceEntry trace =
			RecordTranslation(vaddr, access_type, cpuRegs.pc - 4, translation);
		RaiseTrackedFault(EEMmu::Fault::AddressError, access_type, vaddr,
			cpuRegs.pc - 4, cpuRegs.branch != 0, trace);
		s_recompiler_access_fault = 1;
	}

	u32* GetFullTLBDiagnosticReadVAddrAddress()
	{
		return &s_diagnostic_read_vaddr;
	}

	u64 RecordFullTLBDiagnosticReadResult(u64 value, u32 pc, u32 instruction)
	{
		if (!IsFullTLBDiagnosticTraceEnabled())
			return value;

		DiagnosticReadTraceEntry trace;
		trace.sequence = ++s_diagnostic_read_trace_sequence;
		trace.cycle = cpuRegs.cycle;
		trace.returned_value = value;
		trace.pc = pc;
		trace.instruction = instruction;
		trace.vaddr = s_diagnostic_read_vaddr;
		trace.status = cpuRegs.CP0.n.Status.val;
		trace.entry_hi = cpuRegs.CP0.n.EntryHi;
		trace.translation_generation = EEMmu::GetTranslationGeneration();
		trace.translation = EEMmu::TranslateAddress(
			GetCurrentContext(), trace.vaddr, EEMmu::AccessType::Load);
		trace.raw_tlb = CaptureRawTLBEntry(trace.translation.matched_tlb_index);
		if (trace.translation.fault == EEMmu::Fault::None)
		{
			if (trace.translation.target == EEMmu::Target::Scratchpad)
			{
				trace.backing_value = vtlb_sprRead<u32>(trace.translation.scratch_offset);
				trace.backing_value_valid = true;
			}
			else if (const void* const physical_ptr = vtlb_GetPhyPtr(trace.translation.paddr))
			{
				std::memcpy(&trace.backing_value, physical_ptr, sizeof(u32));
				trace.backing_value_valid = true;
			}
		}

		s_diagnostic_read_trace[s_diagnostic_read_trace_next] = trace;
		s_diagnostic_read_trace_next =
			(s_diagnostic_read_trace_next + 1) % DIAGNOSTIC_READ_TRACE_SIZE;
		s_diagnostic_read_trace_count =
			std::min(s_diagnostic_read_trace_count + 1, DIAGNOSTIC_READ_TRACE_SIZE);
		if ((instruction & 0xffff0000U) == 0x8fbc0000U)
		{
			s_gp_reload_trace[s_gp_reload_trace_next] = trace;
			s_gp_reload_trace_next = (s_gp_reload_trace_next + 1) % GP_RELOAD_TRACE_SIZE;
			s_gp_reload_trace_count = std::min(s_gp_reload_trace_count + 1, GP_RELOAD_TRACE_SIZE);
		}
		else if ((instruction & 0xffff0000U) == 0x8f990000U && static_cast<u32>(value) == 0)
		{
			// Preserve the exact indirect-call failure even if the signal path executes
			// hundreds of additional GOT loads before the VM is paused.
			s_zero_t9_read_trace = trace;
			s_zero_t9_read_trace_valid = true;
		}
		return value;
	}

	static bool CheckRecompilerAlignment(u32 vaddr, u32 alignment_mask, EEMmu::AccessType access_type)
	{
		s_recompiler_access_fault = 0;
		if ((vaddr & alignment_mask) == 0)
			return true;

		EEMmu::TranslationResult translation;
		translation.fault = EEMmu::Fault::AddressError;
		const TranslationTraceEntry trace =
			RecordTranslation(vaddr, access_type, cpuRegs.pc - 4, translation);
		RaiseTrackedFault(EEMmu::Fault::AddressError, access_type, vaddr,
			cpuRegs.pc - 4, cpuRegs.branch != 0, trace);
		s_recompiler_access_fault = 1;
		return false;
	}

	template <typename DataType>
	static DataType RecompilerRead(u32 vaddr)
	{
		if (!CheckRecompilerAlignment(vaddr, sizeof(DataType) - 1, EEMmu::AccessType::Load))
			return {};

		const EEMmu::TranslationResult translation =
			TranslateForRecompiler(vaddr, EEMmu::AccessType::Load);
		return HandlePhysicalBusError(vaddr, translation, EEMmu::AccessType::Load,
				   true, false, true) ?
		           DataType{} :
		           ReadTranslated<DataType>(vaddr, translation);
	}

	template <typename DataType>
	static void RecompilerWrite(u32 vaddr, DataType value)
	{
		if (!CheckRecompilerAlignment(vaddr, sizeof(DataType) - 1, EEMmu::AccessType::Store))
			return;

		const EEMmu::TranslationResult translation =
			TranslateForRecompiler(vaddr, EEMmu::AccessType::Store);
		if (!HandlePhysicalBusError(vaddr, translation, EEMmu::AccessType::Store,
				true, false, true))
		{
			WriteTranslated(vaddr, translation, value);
		}
	}

	u8 RecompilerRead8(u32 vaddr)
	{
		return RecompilerRead<u8>(vaddr);
	}

	u16 RecompilerRead16(u32 vaddr)
	{
		return RecompilerRead<u16>(vaddr);
	}

	u32 RecompilerRead32(u32 vaddr)
	{
		return RecompilerRead<u32>(vaddr);
	}

	u64 RecompilerRead64(u32 vaddr)
	{
		return RecompilerRead<u64>(vaddr);
	}

	RETURNS_R128 RecompilerRead128(u32 vaddr)
	{
		if (!CheckRecompilerAlignment(vaddr, 0xf, EEMmu::AccessType::Load))
			return r128_zero();

		const EEMmu::TranslationResult translation =
			TranslateForRecompiler(vaddr, EEMmu::AccessType::Load);
		if (HandlePhysicalBusError(vaddr, translation, EEMmu::AccessType::Load,
				true, false, true))
		{
			return r128_zero();
		}
		return r128_from_u128(ReadTranslated128(vaddr, translation));
	}

	void RecompilerWrite8(u32 vaddr, u8 value)
	{
		RecompilerWrite(vaddr, value);
	}

	void RecompilerWrite16(u32 vaddr, u16 value)
	{
		RecompilerWrite(vaddr, value);
	}

	void RecompilerWrite32(u32 vaddr, u32 value)
	{
		RecompilerWrite(vaddr, value);
	}

	void RecompilerWrite64(u32 vaddr, u64 value)
	{
		RecompilerWrite(vaddr, value);
	}

	void TAKES_R128 RecompilerWrite128(u32 vaddr, r128 value)
	{
		if (!CheckRecompilerAlignment(vaddr, 0xf, EEMmu::AccessType::Store))
			return;

		const EEMmu::TranslationResult translation =
			TranslateForRecompiler(vaddr, EEMmu::AccessType::Store);
		if (!HandlePhysicalBusError(vaddr, translation, EEMmu::AccessType::Store,
				true, false, true))
		{
			const u128 converted = r128_to_u128(value);
			WriteTranslated128(vaddr, translation, converted);
		}
	}

	template <typename DataType>
	static void RecompilerReadModifyWrite(u32 vaddr, DataType preserve_mask, DataType value)
	{
		const EEMmu::TranslationResult translation =
			TranslateForRecompiler(vaddr, EEMmu::AccessType::Store);
		if (HandlePhysicalBusError(vaddr, translation, EEMmu::AccessType::Store,
				true, false, true))
		{
			return;
		}

		const DataType merged = (ReadTranslated<DataType>(vaddr, translation) & preserve_mask) | value;
		WriteTranslated(vaddr, translation, merged);
	}

	void RecompilerWriteLeft32(u32 vaddr, u32 value)
	{
		static constexpr u32 masks[4] = {0xffffff00, 0xffff0000, 0xff000000, 0};
		static constexpr u8 shifts[4] = {24, 16, 8, 0};
		const u32 index = vaddr & 3;
		RecompilerReadModifyWrite(vaddr & ~3u, masks[index], value >> shifts[index]);
	}

	void RecompilerWriteRight32(u32 vaddr, u32 value)
	{
		static constexpr u32 masks[4] = {0, 0xff, 0xffff, 0xffffff};
		static constexpr u8 shifts[4] = {0, 8, 16, 24};
		const u32 index = vaddr & 3;
		RecompilerReadModifyWrite(vaddr & ~3u, masks[index], value << shifts[index]);
	}

	void RecompilerWriteLeft64(u32 vaddr, u64 value)
	{
		static constexpr u64 masks[8] = {
			0xffffffffffffff00ULL, 0xffffffffffff0000ULL, 0xffffffffff000000ULL,
			0xffffffff00000000ULL, 0xffffff0000000000ULL, 0xffff000000000000ULL,
			0xff00000000000000ULL, 0};
		static constexpr u8 shifts[8] = {56, 48, 40, 32, 24, 16, 8, 0};
		const u32 index = vaddr & 7;
		RecompilerReadModifyWrite(vaddr & ~7u, masks[index], value >> shifts[index]);
	}

	void RecompilerWriteRight64(u32 vaddr, u64 value)
	{
		static constexpr u64 masks[8] = {
			0, 0xff, 0xffff, 0xffffff, 0xffffffffULL, 0xffffffffffULL,
			0xffffffffffffULL, 0xffffffffffffffULL};
		static constexpr u8 shifts[8] = {0, 8, 16, 24, 32, 40, 48, 56};
		const u32 index = vaddr & 7;
		RecompilerReadModifyWrite(vaddr & ~7u, masks[index], value << shifts[index]);
	}

	void ReadModifyWrite32(u32 vaddr, u32 preserve_mask, u32 value)
	{
		const EEMmu::TranslationResult translation = TranslateForCurrentInstruction(vaddr, EEMmu::AccessType::Store);
		if (HandlePhysicalBusError(vaddr, translation, EEMmu::AccessType::Store))
			return;
		const u32 merged = (ReadTranslated<u32>(vaddr, translation) & preserve_mask) | value;
		WriteTranslated(vaddr, translation, merged);
	}

	void ReadModifyWrite64(u32 vaddr, u64 preserve_mask, u64 value)
	{
		const EEMmu::TranslationResult translation = TranslateForCurrentInstruction(vaddr, EEMmu::AccessType::Store);
		if (HandlePhysicalBusError(vaddr, translation, EEMmu::AccessType::Store))
			return;
		const u64 merged = (ReadTranslated<u64>(vaddr, translation) & preserve_mask) | value;
		WriteTranslated(vaddr, translation, merged);
	}

	static const char* GetAccessTypeName(EEMmu::AccessType access_type)
	{
		switch (access_type)
		{
			case EEMmu::AccessType::Fetch:
				return "fetch";
			case EEMmu::AccessType::Load:
				return "load";
			case EEMmu::AccessType::Store:
				return "store";
			case EEMmu::AccessType::Cache:
				return "cache";
			case EEMmu::AccessType::Prefetch:
				return "prefetch";
			default:
				return "unknown";
		}
	}

	static const char* GetFaultName(EEMmu::Fault fault)
	{
		switch (fault)
		{
			case EEMmu::Fault::None:
				return "none";
			case EEMmu::Fault::AddressError:
				return "address-error";
			case EEMmu::Fault::Refill:
				return "refill";
			case EEMmu::Fault::Invalid:
				return "invalid";
			case EEMmu::Fault::Modified:
				return "modified";
			case EEMmu::Fault::BusError:
				return "bus-error";
			default:
				return "unknown";
		}
	}

	static void PrintTranslationTraceEntry(const char* label, const TranslationTraceEntry& entry)
	{
		const EEMmu::TranslationResult& translation = entry.translation;
		if (translation.fault == EEMmu::Fault::None)
		{
			const char* const target = translation.target == EEMmu::Target::Scratchpad ? "spr" : "phys";
			const u32 translated_address = translation.target == EEMmu::Target::Scratchpad ?
			                                   translation.scratch_offset :
			                                   translation.paddr;
			Console.WriteLn("[FullTLBTrace] %s seq=%llu cycle=%llu pc=%08x code=%08x %s vaddr=%08x -> %s=%08x C=%u tlb=%d status=%08x asid=%02x gen=%u warn=%08x sp=%08x ra=%08x",
				label, static_cast<unsigned long long>(entry.sequence),
				static_cast<unsigned long long>(entry.cycle), entry.pc, entry.instruction,
				GetAccessTypeName(entry.access_type), entry.vaddr, target, translated_address,
				translation.cache_mode, translation.matched_tlb_index, entry.status,
				entry.entry_hi & 0xff, entry.translation_generation,
				static_cast<u32>(translation.warnings), entry.sp, entry.ra);
		}
		else
		{
			Console.WriteLn("[FullTLBTrace] %s seq=%llu cycle=%llu pc=%08x code=%08x %s vaddr=%08x -> fault=%s tlb=%d status=%08x asid=%02x gen=%u warn=%08x sp=%08x ra=%08x",
				label, static_cast<unsigned long long>(entry.sequence),
				static_cast<unsigned long long>(entry.cycle), entry.pc, entry.instruction,
				GetAccessTypeName(entry.access_type), entry.vaddr, GetFaultName(translation.fault),
				translation.matched_tlb_index, entry.status, entry.entry_hi & 0xff,
				entry.translation_generation, static_cast<u32>(translation.warnings), entry.sp, entry.ra);
		}

		if (entry.raw_tlb.valid)
		{
			Console.WriteLn("[FullTLBTrace]   raw-tlb[%d] PageMask=%08x EntryHi=%08x EntryLo0=%08x EntryLo1=%08x",
				translation.matched_tlb_index, entry.raw_tlb.page_mask, entry.raw_tlb.entry_hi,
				entry.raw_tlb.entry_lo0, entry.raw_tlb.entry_lo1);
		}
	}

	static void PrintRegisterTrace(bool actual_instruction_valid, u32 actual_instruction,
		u32 fallback_instruction, const std::array<u64, 32>& gpr)
	{
		const u32 instruction = actual_instruction_valid ? actual_instruction : fallback_instruction;
		const u32 rs = (instruction >> 21) & 0x1f;
		const u32 rt = (instruction >> 16) & 0x1f;
		Console.WriteLn("[FullTLBTrace]   actual-code=%s%08x rs=r%u:%016llx rt=r%u:%016llx",
			actual_instruction_valid ? "" : "unavailable/",
			instruction, rs, static_cast<unsigned long long>(gpr[rs]),
			rt, static_cast<unsigned long long>(gpr[rt]));
		Console.WriteLn("[FullTLBTrace]   gpr v0=%016llx v1=%016llx a0=%016llx a1=%016llx a2=%016llx a3=%016llx t9=%016llx gp=%016llx sp=%016llx ra=%016llx",
			static_cast<unsigned long long>(gpr[2]), static_cast<unsigned long long>(gpr[3]),
			static_cast<unsigned long long>(gpr[4]), static_cast<unsigned long long>(gpr[5]),
			static_cast<unsigned long long>(gpr[6]), static_cast<unsigned long long>(gpr[7]),
			static_cast<unsigned long long>(gpr[25]), static_cast<unsigned long long>(gpr[28]),
			static_cast<unsigned long long>(gpr[29]), static_cast<unsigned long long>(gpr[31]));
	}

	static void PrintDiagnosticReadTraceEntry(const char* label, const DiagnosticReadTraceEntry& read)
	{
		const char* const target = read.translation.target == EEMmu::Target::Scratchpad ? "scratch" : "phys";
		if (read.translation.fault == EEMmu::Fault::None)
		{
			const u32 translated = read.translation.target == EEMmu::Target::Scratchpad ?
			                           read.translation.scratch_offset :
			                           read.translation.paddr;
			Console.WriteLn("[FullTLBTrace] %s #%llu cycle=%llu pc=%08x code=%08x vaddr=%08x -> %s=%08x tlb=%d returned=%016llx backing=%s%016llx status=%08x asid=%02x gen=%u",
				label, static_cast<unsigned long long>(read.sequence),
				static_cast<unsigned long long>(read.cycle), read.pc, read.instruction,
				read.vaddr, target, translated, read.translation.matched_tlb_index,
				static_cast<unsigned long long>(read.returned_value),
				read.backing_value_valid ? "" : "unavailable/",
				static_cast<unsigned long long>(read.backing_value), read.status,
				static_cast<u8>(read.entry_hi), read.translation_generation);
		}
		else
		{
			Console.WriteLn("[FullTLBTrace] %s #%llu cycle=%llu pc=%08x code=%08x vaddr=%08x -> fault=%s tlb=%d returned=%016llx backing=%s%016llx status=%08x asid=%02x gen=%u",
				label, static_cast<unsigned long long>(read.sequence),
				static_cast<unsigned long long>(read.cycle), read.pc, read.instruction,
				read.vaddr, GetFaultName(read.translation.fault), read.translation.matched_tlb_index,
				static_cast<unsigned long long>(read.returned_value),
				read.backing_value_valid ? "" : "unavailable/",
				static_cast<unsigned long long>(read.backing_value), read.status,
				static_cast<u8>(read.entry_hi), read.translation_generation);
		}
		if (read.raw_tlb.valid)
		{
			Console.WriteLn("[FullTLBTrace]   raw-tlb[%d] PageMask=%08x EntryHi=%08x EntryLo0=%08x EntryLo1=%08x",
				read.translation.matched_tlb_index, read.raw_tlb.page_mask, read.raw_tlb.entry_hi,
				read.raw_tlb.entry_lo0, read.raw_tlb.entry_lo1);
		}
	}

	void RecordFullTLBUserException(u32 code, bool branch_delay, u64 cycle, u32 pc,
		u32 instruction, u32 sp, u32 ra, const EEMmu::ExceptionRegisters& before)
	{
		if (!IsFullTLBDiagnosticTraceEnabled() || !EEMmu::IsUserMode(before.status))
			return;

		UserExceptionTraceEntry trace;
		trace.sequence = ++s_user_exception_trace_sequence;
		trace.cycle = cycle;
		trace.pc = pc;
		trace.instruction = instruction;
		trace.sp = sp;
		trace.ra = ra;
		trace.code = code;
		trace.branch_delay = branch_delay;
		trace.before = before;
		trace.after = GetCurrentExceptionRegisters();
		trace.exception_vector = cpuRegs.pc;
		trace.gpr = CaptureGPRs();

		const EEMmu::TranslationContext context = {
			before.status,
			cpuRegs.CP0.n.Config,
			static_cast<u8>(before.entry_hi),
			tlb,
			EEMmu::TLB_ENTRY_COUNT,
		};
		const EEMmu::TranslationResult epc_translation =
			EEMmu::TranslateAddress(context, trace.after.epc, EEMmu::AccessType::Fetch);
		trace.epc_translation = BuildTranslationTraceEntry(
			s_translation_trace_sequence, trace.after.epc, EEMmu::AccessType::Fetch,
			trace.after.epc, epc_translation);
		trace.epc_translation.cycle = cycle;
		trace.epc_translation.instruction = instruction;
		trace.epc_translation.status = before.status;
		trace.epc_translation.cause = before.cause;
		trace.epc_translation.epc = before.epc;
		trace.epc_translation.bad_vaddr = before.bad_vaddr;
		trace.epc_translation.entry_hi = before.entry_hi;
		trace.epc_translation.sp = sp;
		trace.epc_translation.ra = ra;
		trace.actual_instruction_valid = ReadTranslatedInstruction(
			epc_translation, trace.after.epc, &trace.actual_instruction);

		s_user_exception_trace[s_user_exception_trace_next] = trace;
		s_user_exception_trace_next = (s_user_exception_trace_next + 1) % USER_EXCEPTION_TRACE_SIZE;
		s_user_exception_trace_count = std::min(s_user_exception_trace_count + 1, USER_EXCEPTION_TRACE_SIZE);
	}

	void ResetFullTLBDiagnosticTrace()
	{
		s_translation_trace = {};
		s_translation_trace_next = 0;
		s_translation_trace_count = 0;
		s_translation_trace_sequence = 0;
		s_user_fault_trace = {};
		s_user_fault_trace_next = 0;
		s_user_fault_trace_count = 0;
		s_user_fault_trace_sequence = 0;
		s_user_exception_trace = {};
		s_user_exception_trace_next = 0;
		s_user_exception_trace_count = 0;
		s_user_exception_trace_sequence = 0;
		s_diagnostic_read_vaddr = 0;
		s_diagnostic_read_trace = {};
		s_diagnostic_read_trace_next = 0;
		s_diagnostic_read_trace_count = 0;
		s_diagnostic_read_trace_sequence = 0;
		s_gp_reload_trace = {};
		s_gp_reload_trace_next = 0;
		s_gp_reload_trace_count = 0;
		s_zero_t9_read_trace = {};
		s_zero_t9_read_trace_valid = false;
	}

	void DumpFullTLBDiagnosticTrace()
	{
		Console.WriteLn("[FullTLBTrace] ===== diagnostic snapshot begin =====");
		Console.WriteLn("[FullTLBTrace] current cycle=%llu pc=%08x code=%08x Status=%08x Cause=%08x ExcCode=%u BD=%u EPC=%08x BadVAddr=%08x Context=%08x EntryHi=%08x",
			static_cast<unsigned long long>(cpuRegs.cycle), cpuRegs.pc, cpuRegs.code,
			cpuRegs.CP0.n.Status.val, cpuRegs.CP0.n.Cause,
			(cpuRegs.CP0.n.Cause >> 2) & 0x1f, cpuRegs.CP0.n.Cause >> 31,
			cpuRegs.CP0.n.EPC, cpuRegs.CP0.n.BadVAddr, cpuRegs.CP0.n.Context,
			cpuRegs.CP0.n.EntryHi);
		Console.WriteLn("[FullTLBTrace] retained synchronous user exceptions=%u (oldest to newest)",
			static_cast<unsigned>(s_user_exception_trace_count));
		for (size_t i = 0; i < s_user_exception_trace_count; i++)
		{
			const size_t index =
				(s_user_exception_trace_next + USER_EXCEPTION_TRACE_SIZE - s_user_exception_trace_count + i) %
				USER_EXCEPTION_TRACE_SIZE;
			const UserExceptionTraceEntry& exception = s_user_exception_trace[index];
			Console.WriteLn("[FullTLBTrace] user-exception #%llu cycle=%llu pc=%08x code=%08x raw-cause=%08x requested-BD=%u sp=%08x ra=%08x vector=%08x",
				static_cast<unsigned long long>(exception.sequence),
				static_cast<unsigned long long>(exception.cycle), exception.pc, exception.instruction,
				exception.code, exception.branch_delay ? 1 : 0, exception.sp, exception.ra,
				exception.exception_vector);
			PrintRegisterTrace(exception.actual_instruction_valid, exception.actual_instruction,
				exception.instruction, exception.gpr);
			Console.WriteLn("[FullTLBTrace]   before Status=%08x Cause=%08x ExcCode=%u BD=%u EPC=%08x BadVAddr=%08x Context=%08x EntryHi=%08x",
				exception.before.status, exception.before.cause, (exception.before.cause >> 2) & 0x1f,
				exception.before.cause >> 31, exception.before.epc, exception.before.bad_vaddr,
				exception.before.context, exception.before.entry_hi);
			Console.WriteLn("[FullTLBTrace]   after  Status=%08x Cause=%08x ExcCode=%u BD=%u EPC=%08x BadVAddr=%08x Context=%08x EntryHi=%08x",
				exception.after.status, exception.after.cause, (exception.after.cause >> 2) & 0x1f,
				exception.after.cause >> 31, exception.after.epc, exception.after.bad_vaddr,
				exception.after.context, exception.after.entry_hi);
			PrintTranslationTraceEntry("exception-epc-map", exception.epc_translation);
		}

		Console.WriteLn("[FullTLBTrace] retained user MMU faults=%u (oldest to newest)",
			static_cast<unsigned>(s_user_fault_trace_count));
		for (size_t i = 0; i < s_user_fault_trace_count; i++)
		{
			const size_t index =
				(s_user_fault_trace_next + USER_FAULT_TRACE_SIZE - s_user_fault_trace_count + i) %
				USER_FAULT_TRACE_SIZE;
			const UserFaultTraceEntry& fault = s_user_fault_trace[index];
			Console.WriteLn("[FullTLBTrace] user-fault #%llu taken=%u vector=%08x access-seq=%llu",
				static_cast<unsigned long long>(fault.sequence), fault.taken ? 1 : 0,
				fault.exception_vector, static_cast<unsigned long long>(fault.access.sequence));
			PrintRegisterTrace(fault.actual_instruction_valid, fault.actual_instruction,
				fault.access.instruction, fault.gpr);
			Console.WriteLn("[FullTLBTrace]   before Status=%08x Cause=%08x ExcCode=%u BD=%u EPC=%08x BadVAddr=%08x Context=%08x EntryHi=%08x",
				fault.before.status, fault.before.cause, (fault.before.cause >> 2) & 0x1f,
				fault.before.cause >> 31, fault.before.epc, fault.before.bad_vaddr,
				fault.before.context, fault.before.entry_hi);
			Console.WriteLn("[FullTLBTrace]   after  Status=%08x Cause=%08x ExcCode=%u BD=%u EPC=%08x BadVAddr=%08x Context=%08x EntryHi=%08x",
				fault.after.status, fault.after.cause, (fault.after.cause >> 2) & 0x1f,
				fault.after.cause >> 31, fault.after.epc, fault.after.bad_vaddr,
				fault.after.context, fault.after.entry_hi);
			PrintTranslationTraceEntry("fault-access", fault.access);
			PrintTranslationTraceEntry("fault-epc-map", fault.epc_translation);
		}

		Console.WriteLn("[FullTLBTrace] preserved zero gp-relative t9 load=%u",
			s_zero_t9_read_trace_valid ? 1U : 0U);
		if (s_zero_t9_read_trace_valid)
			PrintDiagnosticReadTraceEntry("zero-gp-t9-load", s_zero_t9_read_trace);

		Console.WriteLn("[FullTLBTrace] retained sp-relative gp reloads=%u (oldest to newest)",
			static_cast<unsigned>(s_gp_reload_trace_count));
		for (size_t i = 0; i < s_gp_reload_trace_count; i++)
		{
			const size_t index =
				(s_gp_reload_trace_next + GP_RELOAD_TRACE_SIZE - s_gp_reload_trace_count + i) %
				GP_RELOAD_TRACE_SIZE;
			PrintDiagnosticReadTraceEntry("sp-gp-reload", s_gp_reload_trace[index]);
		}

		Console.WriteLn("[FullTLBTrace] retained gp-related loads=%u (oldest to newest)",
			static_cast<unsigned>(s_diagnostic_read_trace_count));
		for (size_t i = 0; i < s_diagnostic_read_trace_count; i++)
		{
			const size_t index =
				(s_diagnostic_read_trace_next + DIAGNOSTIC_READ_TRACE_SIZE - s_diagnostic_read_trace_count + i) %
				DIAGNOSTIC_READ_TRACE_SIZE;
			PrintDiagnosticReadTraceEntry("gp-related-load", s_diagnostic_read_trace[index]);
		}

		Console.WriteLn("[FullTLBTrace] retained translations=%u (oldest to newest; resolver/helper accesses)",
			static_cast<unsigned>(s_translation_trace_count));
		for (size_t i = 0; i < s_translation_trace_count; i++)
		{
			const size_t index =
				(s_translation_trace_next + TRANSLATION_TRACE_SIZE - s_translation_trace_count + i) %
				TRANSLATION_TRACE_SIZE;
			PrintTranslationTraceEntry("xlate", s_translation_trace[index]);
		}
		Console.WriteLn("[FullTLBTrace] ===== diagnostic snapshot end =====");
	}
} // namespace EEMemory
