// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Config.h"
#include "EEMemory.h"
#include "EEMmu.h"
#include "R5900OpcodeTables.h"

#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <random>

namespace
{
	constexpr u32 STATUS_EXL = 1U << 1;
	constexpr u32 STATUS_ERL = 1U << 2;
	constexpr u32 STATUS_SUPERVISOR = 1U << 3;
	constexpr u32 STATUS_USER = 2U << 3;
	constexpr u32 STATUS_RESERVED_KSU = 3U << 3;

	tlbs MakeEntry(u32 virtual_base, u32 physical_even, u32 physical_odd, u32 page_mask = 0,
		u8 asid = 0, u8 cache_mode = 3, bool valid = true, bool dirty = true, bool global = false)
	{
		tlbs entry = {};
		entry.PageMask.Mask = page_mask;
		entry.EntryHi.VPN2 = (virtual_base >> 13) | page_mask;
		entry.EntryHi.ASID = asid;
		entry.EntryLo0.PFN = (physical_even >> 12) | page_mask;
		entry.EntryLo1.PFN = (physical_odd >> 12) | page_mask;
		entry.EntryLo0.C = cache_mode;
		entry.EntryLo1.C = cache_mode;
		entry.EntryLo0.V = valid;
		entry.EntryLo1.V = valid;
		entry.EntryLo0.D = dirty;
		entry.EntryLo1.D = dirty;
		entry.EntryLo0.G = global;
		entry.EntryLo1.G = global;
		return entry;
	}

	tlbs MakeScratchpadEntry(u32 virtual_base, bool valid = true, bool dirty = true)
	{
		tlbs entry = MakeEntry(virtual_base, 0x12345000, 0xabcde000, 0, 0, 7, valid, dirty, true);
		entry.EntryLo0.S = 1;
		return entry;
	}

	template <size_t Size>
	EEMmu::TranslationContext MakeContext(const std::array<tlbs, Size>& entries, u32 status = 0,
		u32 config = 3, u8 asid = 0)
	{
		return {status, config, asid, entries.data(), entries.size()};
	}

	void ExpectPhysical(const EEMmu::TranslationResult& result, u32 paddr, int index = 0)
	{
		EXPECT_EQ(result.fault, EEMmu::Fault::None);
		EXPECT_EQ(result.target, EEMmu::Target::Physical);
		EXPECT_EQ(result.paddr, paddr);
		EXPECT_EQ(result.matched_tlb_index, index);
	}

	class LiveEEStateGuard
	{
	public:
		LiveEEStateGuard()
		{
			std::memcpy(&m_registers, &cpuRegs, sizeof(cpuRegs));
			std::memcpy(m_tlb.data(), tlb, sizeof(tlb));
			std::memset(&cpuRegs, 0, sizeof(cpuRegs));
			std::memset(tlb, 0, sizeof(tlb));
		}

		~LiveEEStateGuard()
		{
			std::memcpy(&cpuRegs, &m_registers, sizeof(cpuRegs));
			std::memcpy(tlb, m_tlb.data(), sizeof(tlb));
		}

	private:
		cpuRegisters m_registers = {};
		std::array<tlbs, EEMmu::TLB_ENTRY_COUNT> m_tlb = {};
	};
} // namespace

TEST(EEFullTLB, SyscallRaisesTheArchitecturalGeneralException)
{
	LiveEEStateGuard guard;
	const bool previous_full_tlb = EmuConfig.Cpu.EnableExperimentalEETLB;
	EmuConfig.Cpu.EnableExperimentalEETLB = true;

	// Use kernel mode as well: Full TLB must not route any privilege level through
	// the BIOS syscall-number hooks (needs proper testing with a retail BIOS).
	cpuRegs.pc = 0x00100004;
	cpuRegs.CP0.n.Status.val = 0;
	cpuRegs.CP0.n.Cause = 0x0000ac08;
	cpuRegs.GPR.n.v1.UC[0] = Syscall::GetMemorySize;
	R5900::Interpreter::OpcodeImpl::SYSCALL();

	EXPECT_EQ(cpuRegs.CP0.n.Cause & EXC_CODE__MASK, EXC_CODE_Sys);
	EXPECT_EQ(cpuRegs.CP0.n.Cause & ~EXC_CODE__MASK, 0x0000ac08U & ~EXC_CODE__MASK);
	EXPECT_EQ(cpuRegs.CP0.n.EPC, 0x00100000U);
	EXPECT_EQ(cpuRegs.pc, 0x80000180U);
	EXPECT_NE(cpuRegs.CP0.n.Status.val & STATUS_EXL, 0U);

	EmuConfig.Cpu.EnableExperimentalEETLB = previous_full_tlb;
}

TEST(EEException, SynchronousExceptionPreservesPendingCauseBitsAndPreciseEPC)
{
	constexpr u32 cause = 0x0000ff08;
	constexpr EEMmu::ExceptionRegisters registers = {
		STATUS_USER, cause, 0x11111111, 0x22222222, 0x33333333, 0x44444444};
	const EEMmu::ExceptionResult result =
		EEMmu::BuildSynchronousException(registers, EXC_CODE_Sys, 0x20001004, true);

	EXPECT_EQ(result.vector, 0x80000180U);
	EXPECT_EQ(result.registers.status, STATUS_USER | STATUS_EXL);
	EXPECT_EQ(result.registers.cause & EXC_CODE__MASK, EXC_CODE_Sys);
	EXPECT_EQ(result.registers.cause & 0x0000ff00, cause & 0x0000ff00);
	EXPECT_NE(result.registers.cause & (1U << 31), 0U);
	EXPECT_EQ(result.registers.epc, 0x20001000U);
	EXPECT_EQ(result.registers.bad_vaddr, registers.bad_vaddr);
	EXPECT_EQ(result.registers.context, registers.context);
	EXPECT_EQ(result.registers.entry_hi, registers.entry_hi);
}

TEST(EERecompilerMemory, ReportsPreciseAlignmentFaultWithoutCancelInstruction)
{
	LiveEEStateGuard guard;
	cpuRegs.pc = 0x20001004;
	EEMemory::RecompilerRead32(0x10000002);

	EXPECT_EQ(*EEMemory::GetRecompilerAccessFaultAddress(), 1U);
	EXPECT_EQ(cpuRegs.CP0.n.Cause & EXC_CODE__MASK, EXC_CODE_AdEL);
	EXPECT_EQ(cpuRegs.CP0.n.BadVAddr, 0x10000002U);
	EXPECT_EQ(cpuRegs.CP0.n.EPC, 0x20001000U);
	EXPECT_EQ(cpuRegs.pc, 0x80000180U);
}

TEST(EERecompilerMemory, PreservesDelaySlotEPCAndBDConvention)
{
	LiveEEStateGuard guard;
	// Recompiler helpers expose the same post-instruction PC convention as the Interpreter.
	cpuRegs.pc = 0x20001008;
	cpuRegs.branch = 1;
	EEMemory::RecompilerWrite64(0x10000004, 0);

	EXPECT_EQ(*EEMemory::GetRecompilerAccessFaultAddress(), 1U);
	EXPECT_EQ(cpuRegs.CP0.n.Cause & EXC_CODE__MASK, EXC_CODE_AdES);
	EXPECT_NE(cpuRegs.CP0.n.Cause & (1U << 31), 0U);
	EXPECT_EQ(cpuRegs.CP0.n.EPC, 0x20001000U);
}

TEST(EERecompilerMemory, StoreUsesModifiedFaultBeforePhysicalAccess)
{
	LiveEEStateGuard guard;
	cpuRegs.pc = 0x20001004;
	tlb[0] = MakeEntry(0x10000000, 0x01000000, 0x01001000, 0, 0, 3, true, false);
	const u32 generation_before = *EEMemory::GetPhysicalWriteGenerationAddress(0x01000020);
	EEMemory::RecompilerWrite32(0x10000020, 0x12345678);

	EXPECT_EQ(*EEMemory::GetRecompilerAccessFaultAddress(), 1U);
	EXPECT_EQ(cpuRegs.CP0.n.Cause & EXC_CODE__MASK, EXC_CODE_Mod);
	EXPECT_EQ(cpuRegs.CP0.n.BadVAddr, 0x10000020U);
	EXPECT_EQ(*EEMemory::GetPhysicalWriteGenerationAddress(0x01000020), generation_before);
}

TEST(EERecompilerMemory, TranslationCacheRejectsOldTLBGeneration)
{
	LiveEEStateGuard guard;
	cpuRegs.pc = 0x20001004;
	tlb[0] = MakeEntry(0x10000000, 0x01000000, 0x01001000);
	EEMmu::InvalidateTLBEntry(0);
	EXPECT_EQ(EEMemory::TranslateForRecompiler(0x10000020, EEMmu::AccessType::Load).paddr, 0x01000020U);

	// Updating an unrelated entry must leave the cached entry usable.
	const u32 entry_zero_generation = EEMmu::GetTLBEntryGeneration(0);
	EEMmu::InvalidateTLBEntry(1);
	EXPECT_EQ(EEMmu::GetTLBEntryGeneration(0), entry_zero_generation);
	EXPECT_EQ(EEMemory::TranslateForRecompiler(0x10000020, EEMmu::AccessType::Load).paddr, 0x01000020U);

	tlb[0] = MakeEntry(0x10000000, 0x02000000, 0x02001000);
	EEMmu::InvalidateTLBEntry(0);
	EXPECT_EQ(EEMemory::TranslateForRecompiler(0x10000020, EEMmu::AccessType::Load).paddr, 0x02000020U);
}

TEST(EERecompilerMemory, JitTranslationResolverPublishesTaggedPhysicalPage)
{
	LiveEEStateGuard guard;
	cpuRegs.pc = 0x20001004;
	tlb[0] = MakeEntry(0x10000000, 0x01000000, 0x01001000);

	const u64 packed = EEMemory::ResolveRecompilerJitTranslation(
		0x10000020, static_cast<u32>(EEMmu::AccessType::Load));
	EXPECT_EQ(*EEMemory::GetRecompilerAccessFaultAddress(), 0U);
	EXPECT_EQ(static_cast<u32>(packed), 0x01000000U);
	EXPECT_EQ(static_cast<u32>(packed >> 32) & EEMemory::RECOMPILER_TRANSLATION_CACHE_MODE_MASK,
		3U << EEMemory::RECOMPILER_TRANSLATION_CACHE_MODE_SHIFT);

	const u32 virtual_page = 0x10000020 >> 12;
	const EEMemory::RecompilerJitTranslationCacheEntry& entry =
		EEMemory::GetRecompilerJitTranslationCacheBase(EEMmu::AccessType::Load)
			[virtual_page & (EEMemory::RECOMPILER_TRANSLATION_CACHE_SIZE - 1)];
	EXPECT_EQ(entry.virtual_page, virtual_page);
	EXPECT_EQ(entry.tlb_entry_index, 0U);
	EXPECT_EQ(entry.translation_generation, EEMmu::GetTLBEntryGeneration(0));
	EXPECT_EQ(entry.context_key, EEMemory::GetRecompilerJitTranslationContextKey(false));
	EXPECT_EQ(entry.translation, packed);
}

TEST(EERecompilerMemory, JitStoreResolverStopsAtModifiedFault)
{
	LiveEEStateGuard guard;
	cpuRegs.pc = 0x20001004;
	tlb[0] = MakeEntry(0x10000000, 0x01000000, 0x01001000, 0, 0, 3, true, false);

	EXPECT_EQ(EEMemory::ResolveRecompilerJitTranslation(
				  0x10000020, static_cast<u32>(EEMmu::AccessType::Store)),
		0U);
	EXPECT_EQ(*EEMemory::GetRecompilerAccessFaultAddress(), 1U);
	EXPECT_EQ(cpuRegs.CP0.n.Cause & EXC_CODE__MASK, EXC_CODE_Mod);
}

TEST(EETLBTranslator, FastLookupMatchesLinearScan)
{
	LiveEEStateGuard guard;
	constexpr std::array<u32, 7> page_masks = {0x000, 0x003, 0x00f, 0x03f, 0x0ff, 0x3ff, 0xfff};
	std::mt19937 rng(12345);
	auto random = [&rng](u32 limit) { return static_cast<u32>(rng() % limit); };

	EEMmu::EnableFastTLBLookup(true);
	for (int round = 0; round < 200; round++)
	{
		std::array<tlbs, EEMmu::TLB_ENTRY_COUNT> copy;
		for (size_t i = 0; i < EEMmu::TLB_ENTRY_COUNT; i++)
		{
			// Few distinct bases and ASIDs so that overlaps and multiple matches occur.
			const u32 base = (random(8) << 24) | (random(16) << 13);
			const u32 mask = random(4) == 0 ? page_masks[random(page_masks.size())] : 0;
			tlb[i] = MakeEntry(base, random(0x200) << 12, random(0x200) << 12, mask,
				static_cast<u8>(random(4)), static_cast<u8>(2 + random(2)), random(4) != 0, random(2) != 0,
				random(4) == 0);
			if (random(16) == 0)
				tlb[i].EntryLo0.S = 1;
			copy[i] = tlb[i];
			EEMmu::InvalidateTLBEntry(i);
		}

		for (int probe = 0; probe < 200; probe++)
		{
			const u32 vaddr = random(2) ? (copy[random(EEMmu::TLB_ENTRY_COUNT)].EntryHi.VPN2 << 13) + random(0x4000)
			                            : static_cast<u32>(rng());
			const u32 status = random(2) ? 0 : STATUS_USER;
			const u8 asid = static_cast<u8>(random(4));
			const auto access = random(2) ? EEMmu::AccessType::Load : EEMmu::AccessType::Store;
			const EEMmu::TranslationContext live = {status, 3, asid, tlb, EEMmu::TLB_ENTRY_COUNT};
			const EEMmu::TranslationResult fast = EEMmu::TranslateAddress(live, vaddr, access);
			const EEMmu::TranslationResult slow = EEMmu::TranslateAddress(MakeContext(copy, status, 3, asid), vaddr, access);
			ASSERT_EQ(fast.fault, slow.fault) << std::hex << vaddr;
			ASSERT_EQ(fast.target, slow.target) << std::hex << vaddr;
			ASSERT_EQ(fast.paddr, slow.paddr) << std::hex << vaddr;
			ASSERT_EQ(fast.scratch_offset, slow.scratch_offset) << std::hex << vaddr;
			ASSERT_EQ(fast.cache_mode, slow.cache_mode) << std::hex << vaddr;
			ASSERT_EQ(fast.matched_tlb_index, slow.matched_tlb_index) << std::hex << vaddr;
			ASSERT_EQ(fast.warnings, slow.warnings) << std::hex << vaddr;
		}
	}
	EEMmu::EnableFastTLBLookup(false);
}

TEST(EETLBTranslator, TranslatesAllSupportedPageSizesAndBoundaries)
{
	constexpr std::array<u32, 7> page_masks = {0x000, 0x003, 0x00f, 0x03f, 0x0ff, 0x3ff, 0xfff};
	constexpr u32 virtual_base = 0x20000000;
	constexpr u32 physical_even = 0x04000000;
	constexpr u32 physical_odd = 0x08000000;

	for (const u32 page_mask : page_masks)
	{
		SCOPED_TRACE(testing::Message() << "page mask 0x" << std::hex << page_mask);
		const u32 page_size = (page_mask + 1) << 12;
		const std::array entries = {MakeEntry(virtual_base, physical_even, physical_odd, page_mask)};
		const EEMmu::TranslationContext context = MakeContext(entries);

		ExpectPhysical(EEMmu::TranslateAddress(context, virtual_base, EEMmu::AccessType::Load), physical_even);
		ExpectPhysical(EEMmu::TranslateAddress(context, virtual_base + page_size - 1, EEMmu::AccessType::Fetch),
			physical_even + page_size - 1);
		ExpectPhysical(EEMmu::TranslateAddress(context, virtual_base + page_size, EEMmu::AccessType::Load), physical_odd);
		ExpectPhysical(EEMmu::TranslateAddress(context, virtual_base + (page_size * 2) - 1, EEMmu::AccessType::Load),
			physical_odd + page_size - 1);
		EXPECT_EQ(EEMmu::TranslateAddress(context, virtual_base + page_size * 2, EEMmu::AccessType::Load).fault,
			EEMmu::Fault::Refill);
	}
}

TEST(EETLBTranslator, AppliesASIDGlobalValidAndDirtyBits)
{
	std::array entries = {MakeEntry(0x10000000, 0x01000000, 0x01001000, 0, 7)};
	ExpectPhysical(EEMmu::TranslateAddress(MakeContext(entries, 0, 3, 7), 0x10000020, EEMmu::AccessType::Load),
		0x01000020);
	EXPECT_EQ(EEMmu::TranslateAddress(MakeContext(entries, 0, 3, 8), 0x10000020, EEMmu::AccessType::Load).fault,
		EEMmu::Fault::Refill);

	entries[0].EntryLo0.G = 1;
	EXPECT_EQ(EEMmu::TranslateAddress(MakeContext(entries, 0, 3, 8), 0x10000020, EEMmu::AccessType::Load).fault,
		EEMmu::Fault::Refill);
	entries[0].EntryLo1.G = 1;
	ExpectPhysical(EEMmu::TranslateAddress(MakeContext(entries, 0, 3, 8), 0x10000020, EEMmu::AccessType::Load),
		0x01000020);
	entries[0].EntryLo0.V = 0;
	EXPECT_EQ(EEMmu::TranslateAddress(MakeContext(entries), 0x10000000, EEMmu::AccessType::Load).fault,
		EEMmu::Fault::Invalid);
	entries[0].EntryLo0.V = 1;
	entries[0].EntryLo0.D = 0;
	EXPECT_EQ(EEMmu::TranslateAddress(MakeContext(entries), 0x10000000, EEMmu::AccessType::Store).fault,
		EEMmu::Fault::Modified);
	EXPECT_EQ(EEMmu::TranslateAddress(MakeContext(entries), 0x10000000, EEMmu::AccessType::Load).fault,
		EEMmu::Fault::None);
	EXPECT_EQ(EEMmu::TranslateAddress(MakeContext(entries), 0x10000000, EEMmu::AccessType::Cache).fault,
		EEMmu::Fault::None);
}

TEST(EETLBTranslator, SearchesExactlyFortyEightEntriesAndChoosesLowestMatch)
{
	std::array<tlbs, EEMmu::TLB_ENTRY_COUNT + 1> entries = {};
	entries[0] = MakeEntry(0x10000000, 0x01000000, 0x01001000);
	entries[1] = MakeEntry(0x10000000, 0x02000000, 0x02001000);
	entries[EEMmu::TLB_ENTRY_COUNT] = MakeEntry(0x20000000, 0x03000000, 0x03001000);

	const EEMmu::TranslationResult result =
		EEMmu::TranslateAddress(MakeContext(entries), 0x10000040, EEMmu::AccessType::Load);
	ExpectPhysical(result, 0x01000040);
	EXPECT_TRUE(EEMmu::HasWarning(result.warnings, EEMmu::Warning::MultipleMatch));
	EXPECT_EQ(EEMmu::TranslateAddress(MakeContext(entries), 0x20000020, EEMmu::AccessType::Load).fault,
		EEMmu::Fault::Refill);
}

TEST(EETLBTranslator, TranslatesValidScratchpadMappings)
{
	constexpr u32 virtual_base = 0x70000000;
	std::array entries = {MakeScratchpadEntry(virtual_base)};
	for (const u32 offset : {0U, 0x1234U, 0x3fffU})
	{
		const EEMmu::TranslationResult result =
			EEMmu::TranslateAddress(MakeContext(entries), virtual_base + offset, EEMmu::AccessType::Load);
		EXPECT_EQ(result.fault, EEMmu::Fault::None);
		EXPECT_EQ(result.target, EEMmu::Target::Scratchpad);
		EXPECT_EQ(result.scratch_offset, offset);
	}

	entries[0].EntryLo0.V = entries[0].EntryLo1.V = 0;
	EXPECT_EQ(EEMmu::TranslateAddress(MakeContext(entries), virtual_base, EEMmu::AccessType::Load).fault,
		EEMmu::Fault::Invalid);
	entries[0].EntryLo0.V = entries[0].EntryLo1.V = 1;
	entries[0].EntryLo0.D = entries[0].EntryLo1.D = 0;
	EXPECT_EQ(EEMmu::TranslateAddress(MakeContext(entries), virtual_base, EEMmu::AccessType::Store).fault,
		EEMmu::Fault::Modified);
}

TEST(EETLBTranslator, InvalidScratchpadUsesDocumentedDeterministicFallback)
{
	std::array entries = {MakeScratchpadEntry(0x30000000)};
	entries[0].EntryLo1.V = 0;
	const EEMmu::TranslationResult result =
		EEMmu::TranslateAddress(MakeContext(entries), 0x30000020, EEMmu::AccessType::Load);
	ExpectPhysical(result, 0x12345020);
	EXPECT_TRUE(EEMmu::HasWarning(result.warnings, EEMmu::Warning::InvalidScratchpad));
}

TEST(EETLBTranslator, EnforcesUserAndSupervisorAddressSpaces)
{
	const std::array user_entry = {MakeEntry(0x10000000, 0x01000000, 0x01001000)};
	ExpectPhysical(EEMmu::TranslateAddress(MakeContext(user_entry, STATUS_USER), 0x10000020, EEMmu::AccessType::Load),
		0x01000020);
	EXPECT_EQ(EEMmu::TranslateAddress(MakeContext(user_entry, STATUS_USER), 0x80000000, EEMmu::AccessType::Load).fault,
		EEMmu::Fault::AddressError);

	const std::array supervisor_entries = {
		MakeEntry(0x10000000, 0x01000000, 0x01001000),
		MakeEntry(0xc0000000, 0x02000000, 0x02001000),
	};
	ExpectPhysical(EEMmu::TranslateAddress(MakeContext(supervisor_entries, STATUS_SUPERVISOR), 0xc0000020,
					   EEMmu::AccessType::Load),
		0x02000020, 1);
	EXPECT_EQ(EEMmu::TranslateAddress(MakeContext(supervisor_entries, STATUS_SUPERVISOR), 0x80000000,
				  EEMmu::AccessType::Load)
				  .fault,
		EEMmu::Fault::AddressError);
	EXPECT_EQ(EEMmu::TranslateAddress(MakeContext(supervisor_entries, STATUS_SUPERVISOR), 0xe0000000,
				  EEMmu::AccessType::Load)
				  .fault,
		EEMmu::Fault::AddressError);
}

TEST(EETLBTranslator, PrivilegeAddressErrorPrecedesAnOtherwiseMatchingTLBEntry)
{
	const std::array entries = {MakeEntry(0x80000000, 0x01000000, 0x01001000)};
	const EEMmu::TranslationResult result =
		EEMmu::TranslateAddress(MakeContext(entries, STATUS_USER), 0x80000020, EEMmu::AccessType::Load);
	EXPECT_EQ(result.fault, EEMmu::Fault::AddressError);
	EXPECT_EQ(result.matched_tlb_index, -1);
}

TEST(EETLBTranslator, AppliesKernelDirectMappedAndERLSegments)
{
	const std::array entries = {
		MakeEntry(0xc0000000, 0x02000000, 0x02001000),
		MakeEntry(0xe0000000, 0x03000000, 0x03001000),
	};
	const EEMmu::TranslationContext context = MakeContext(entries, 0, 7);
	EEMmu::TranslationResult result = EEMmu::TranslateAddress(context, 0x80001234, EEMmu::AccessType::Load);
	EXPECT_EQ(result.paddr, 0x1234U);
	EXPECT_EQ(result.cache_mode, 7U);
	EXPECT_EQ(result.matched_tlb_index, -1);
	result = EEMmu::TranslateAddress(context, 0xa0001234, EEMmu::AccessType::Load);
	EXPECT_EQ(result.paddr, 0x1234U);
	EXPECT_EQ(result.cache_mode, 2U);
	ExpectPhysical(EEMmu::TranslateAddress(context, 0xc0000020, EEMmu::AccessType::Load), 0x02000020);
	ExpectPhysical(EEMmu::TranslateAddress(context, 0xe0000020, EEMmu::AccessType::Load), 0x03000020, 1);

	const std::array<tlbs, 0> no_entries = {};
	result = EEMmu::TranslateAddress(MakeContext(no_entries, STATUS_USER | STATUS_ERL), 0x12345678,
		EEMmu::AccessType::Load);
	EXPECT_EQ(result.fault, EEMmu::Fault::None);
	EXPECT_EQ(result.paddr, 0x12345678U);
	EXPECT_EQ(result.cache_mode, 2U);
	result = EEMmu::TranslateAddress(MakeContext(no_entries, STATUS_USER | STATUS_EXL, 3), 0x80001234,
		EEMmu::AccessType::Load);
	EXPECT_EQ(result.paddr, 0x1234U);
	result = EEMmu::TranslateAddress(MakeContext(no_entries, STATUS_RESERVED_KSU | STATUS_EXL, 3), 0x80001234,
		EEMmu::AccessType::Load);
	EXPECT_TRUE(EEMmu::HasWarning(result.warnings, EEMmu::Warning::ReservedKSU));
}

TEST(EETLBTranslator, PreservesRawCacheModesAndWarnsForReservedInputs)
{
	constexpr std::array<u8, 3> valid_modes = {2, 3, 7};
	for (const u8 cache_mode : valid_modes)
	{
		const std::array entries = {MakeEntry(0x10000000, 0x01000000, 0x01001000, 0, 0, cache_mode)};
		const EEMmu::TranslationResult result =
			EEMmu::TranslateAddress(MakeContext(entries), 0x10000020, EEMmu::AccessType::Load);
		EXPECT_EQ(result.cache_mode, cache_mode);
		EXPECT_FALSE(EEMmu::HasWarning(result.warnings, EEMmu::Warning::ReservedCacheMode));
	}

	std::array entries = {MakeEntry(0x10000000, 0x01000000, 0x01001000, 0x001, 0, 6)};
	EEMmu::TranslationResult result =
		EEMmu::TranslateAddress(MakeContext(entries), 0x10000123, EEMmu::AccessType::Load);
	EXPECT_EQ(result.cache_mode, 6U);
	EXPECT_TRUE(EEMmu::HasWarning(result.warnings, EEMmu::Warning::InvalidPageMask));
	EXPECT_TRUE(EEMmu::HasWarning(result.warnings, EEMmu::Warning::ReservedCacheMode));

	const std::array<tlbs, 0> no_entries = {};
	result = EEMmu::TranslateAddress(MakeContext(no_entries, STATUS_RESERVED_KSU, 0), 0x80000020,
		EEMmu::AccessType::Load);
	EXPECT_EQ(result.paddr, 0x20U);
	EXPECT_TRUE(EEMmu::HasWarning(result.warnings, EEMmu::Warning::ReservedKSU));
}

TEST(EEException, RefillBuildsPreciseCop0State)
{
	constexpr u32 cause_with_ip = 0x0000ff7c;
	constexpr u32 context = 0xaa80000f;
	constexpr u32 entry_hi = 0x0000125a;
	const EEMmu::ExceptionRegisters registers = {0, cause_with_ip, 0, 0, context, entry_hi};
	const EEMmu::ExceptionRequest request = {
		EEMmu::Fault::Refill, EEMmu::AccessType::Load, 0x12345678, 0x20001000, false};
	const EEMmu::ExceptionResult result = EEMmu::BuildException(registers, request);

	EXPECT_EQ(result.vector, 0x80000000U);
	EXPECT_EQ(result.registers.status & STATUS_EXL, STATUS_EXL);
	EXPECT_EQ(result.registers.cause & 0x7c, EXC_CODE_TLBL);
	EXPECT_EQ(result.registers.cause & 0x0000ff00, cause_with_ip & 0x0000ff00);
	EXPECT_EQ(result.registers.cause >> 31, 0U);
	EXPECT_EQ(result.registers.epc, 0x20001000U);
	EXPECT_EQ(result.registers.bad_vaddr, 0x12345678U);
	EXPECT_EQ(result.registers.context & 0xff80000f, context & 0xff80000f);
	EXPECT_EQ(result.registers.context & 0x007ffff0, (0x12345678U >> 9) & 0x007ffff0);
	EXPECT_EQ(result.registers.entry_hi & 0x1fff, entry_hi & 0x1fff);
	EXPECT_EQ(result.registers.entry_hi & 0xffffe000, 0x12345678U & 0xffffe000);
}

TEST(EEException, StoreRefillUsesBootstrapVectorAndDelaySlotEPC)
{
	constexpr u32 status_bev = 1U << 22;
	const EEMmu::ExceptionRegisters registers = {status_bev, 0, 0, 0, 0, 0x44};
	const EEMmu::ExceptionRequest request = {
		EEMmu::Fault::Refill, EEMmu::AccessType::Store, 0x40000000, 0x20001004, true};
	const EEMmu::ExceptionResult result = EEMmu::BuildException(registers, request);

	EXPECT_EQ(result.vector, 0xbfc00200U);
	EXPECT_EQ(result.registers.cause & 0x7c, EXC_CODE_TLBS);
	EXPECT_NE(result.registers.cause & (1U << 31), 0U);
	EXPECT_EQ(result.registers.epc, 0x20001000U);
}

TEST(EEException, InvalidModifiedAndAddressErrorUseCommonVector)
{
	struct TestCase
	{
		EEMmu::Fault fault;
		EEMmu::AccessType access_type;
		u32 exception_code;
	};
	constexpr std::array cases = {
		TestCase{EEMmu::Fault::Invalid, EEMmu::AccessType::Fetch, EXC_CODE_TLBL},
		TestCase{EEMmu::Fault::Invalid, EEMmu::AccessType::Store, EXC_CODE_TLBS},
		TestCase{EEMmu::Fault::Modified, EEMmu::AccessType::Store, EXC_CODE_Mod},
		TestCase{EEMmu::Fault::AddressError, EEMmu::AccessType::Load, EXC_CODE_AdEL},
		TestCase{EEMmu::Fault::AddressError, EEMmu::AccessType::Store, EXC_CODE_AdES},
		TestCase{EEMmu::Fault::AddressError, EEMmu::AccessType::Cache, EXC_CODE_AdEL},
	};

	for (const TestCase& test : cases)
	{
		const EEMmu::ExceptionRegisters registers = {0, 0, 0, 0, 0x55667788, 0x11223344};
		const EEMmu::ExceptionRequest request = {test.fault, test.access_type, 0x87654321, 0x20000000, false};
		const EEMmu::ExceptionResult result = EEMmu::BuildException(registers, request);
		EXPECT_EQ(result.vector, 0x80000180U);
		EXPECT_EQ(result.registers.cause & 0x7c, test.exception_code);
		EXPECT_EQ(result.registers.bad_vaddr, 0x87654321U);
		if (test.fault == EEMmu::Fault::AddressError)
		{
			EXPECT_EQ(result.registers.context, registers.context);
			EXPECT_EQ(result.registers.entry_hi, registers.entry_hi);
		}
	}
}

TEST(EEException, PhysicalBusErrorDistinguishesInstructionAndDataWithoutChangingBadVAddr)
{
	struct TestCase
	{
		EEMmu::AccessType access_type;
		u32 exception_code;
	};
	constexpr std::array cases = {
		TestCase{EEMmu::AccessType::Fetch, EXC_CODE_IBE},
		TestCase{EEMmu::AccessType::Load, EXC_CODE_DBE},
		TestCase{EEMmu::AccessType::Store, EXC_CODE_DBE},
	};

	for (const TestCase& test : cases)
	{
		constexpr EEMmu::ExceptionRegisters registers = {
			0, 0x00005500, 0x11111111, 0x22222222, 0x33333333, 0x44444444};
		const EEMmu::ExceptionResult result = EEMmu::BuildException(
			registers, {EEMmu::Fault::BusError, test.access_type, 0x87654321, 0x20001000, false});

		EXPECT_TRUE(result.taken);
		EXPECT_EQ(result.vector, 0x80000180U);
		EXPECT_EQ(result.registers.cause & EXC_CODE__MASK, test.exception_code);
		EXPECT_EQ(result.registers.epc, 0x20001000U);
		EXPECT_EQ(result.registers.bad_vaddr, registers.bad_vaddr);
		EXPECT_EQ(result.registers.context, registers.context);
		EXPECT_EQ(result.registers.entry_hi, registers.entry_hi);
	}
}

TEST(EEException, PhysicalBusErrorIsMaskedDuringEXLOrERL)
{
	for (const u32 status : {2U, 4U, 6U})
	{
		const EEMmu::ExceptionRegisters registers = {
			status, 0x80005500, 0x11111111, 0x22222222, 0x33333333, 0x44444444};
		const EEMmu::ExceptionResult result = EEMmu::BuildException(
			registers, {EEMmu::Fault::BusError, EEMmu::AccessType::Load, 0x87654321, 0x20001000, true});

		EXPECT_FALSE(result.taken);
		EXPECT_EQ(result.vector, 0U);
		EXPECT_EQ(result.registers.status, registers.status);
		EXPECT_EQ(result.registers.cause, registers.cause);
		EXPECT_EQ(result.registers.epc, registers.epc);
		EXPECT_EQ(result.registers.bad_vaddr, registers.bad_vaddr);
		EXPECT_EQ(result.registers.context, registers.context);
		EXPECT_EQ(result.registers.entry_hi, registers.entry_hi);
	}
}

TEST(EEException, NestedRefillPreservesEPCAndBDAndUsesCommonVector)
{
	constexpr u32 original_epc = 0x10002000;
	constexpr u32 original_cause = (1U << 31) | 0x00005500;
	const EEMmu::ExceptionRegisters registers = {STATUS_EXL, original_cause, original_epc, 0, 0, 0x77};
	const EEMmu::ExceptionRequest request = {
		EEMmu::Fault::Refill, EEMmu::AccessType::Load, 0x23456000, 0x30000000, false};
	const EEMmu::ExceptionResult result = EEMmu::BuildException(registers, request);

	EXPECT_EQ(result.vector, 0x80000180U);
	EXPECT_EQ(result.registers.epc, original_epc);
	EXPECT_EQ(result.registers.cause & (1U << 31), original_cause & (1U << 31));
	EXPECT_EQ(result.registers.cause & 0x7c, EXC_CODE_TLBL);
	EXPECT_EQ(result.registers.bad_vaddr, 0x23456000U);
}

TEST(EEException, ModifiedUpdatesTranslationFailureRegistersAndUsesBootstrapCommonVector)
{
	constexpr u32 status_bev = 1U << 22;
	constexpr u32 context = 0xaa80000f;
	constexpr u32 entry_hi = 0x0000125a;
	const EEMmu::ExceptionRegisters registers = {status_bev, 0x0000ff00, 0, 0, context, entry_hi};
	const EEMmu::ExceptionResult result = EEMmu::BuildException(
		registers, {EEMmu::Fault::Modified, EEMmu::AccessType::Store, 0x34567890, 0x20000000, false});

	EXPECT_EQ(result.vector, 0xbfc00380U);
	EXPECT_EQ(result.registers.cause & 0x7c, EXC_CODE_Mod);
	EXPECT_EQ(result.registers.context & 0xff80000f, context & 0xff80000f);
	EXPECT_EQ(result.registers.context & 0x007ffff0, (0x34567890U >> 9) & 0x007ffff0);
	EXPECT_EQ(result.registers.entry_hi & 0x1fff, entry_hi & 0x1fff);
	EXPECT_EQ(result.registers.entry_hi & 0xffffe000, 0x34567890U & 0xffffe000);
}

TEST(EETLBInstructions, WriteAndReadbackNormalizeArchitecturalFields)
{
	constexpr u32 page_mask = 0x003U << 13;
	constexpr u32 entry_hi = 0x2000605a;
	constexpr u32 entry_lo0 = 0x80000000U | (0x12347U << 6) | (1U << 3) | 0x7;
	constexpr u32 entry_lo1 = (0x23457U << 6) | (6U << 3) | 0x7;
	const tlbs entry = EEMmu::BuildTLBEntry(page_mask, entry_hi, entry_lo0, entry_lo1);

	EXPECT_EQ(entry.Mask(), 0x003U);
	EXPECT_EQ(entry.EntryHi.VPN2 & entry.Mask(), 0U);
	EXPECT_EQ(entry.EntryLo0.PFN & entry.Mask(), 0U);
	EXPECT_EQ(entry.EntryLo1.PFN & entry.Mask(), 0U);
	EXPECT_TRUE(entry.isGlobal());
	EXPECT_TRUE(entry.isSPR());
	EXPECT_EQ(entry.EntryLo0.C, 1U);
	EXPECT_EQ(entry.EntryLo1.C, 6U);

	u32 read_page_mask;
	u32 read_entry_hi;
	u32 read_entry_lo0;
	u32 read_entry_lo1;
	EEMmu::ReadTLBEntry(entry, &read_page_mask, &read_entry_hi, &read_entry_lo0, &read_entry_lo1);
	EXPECT_EQ(read_page_mask, page_mask);
	EXPECT_EQ(read_entry_hi & 0xff, entry_hi & 0xff);
	EXPECT_EQ(read_entry_hi & (0x003U << 13), 0U);
	EXPECT_NE(read_entry_lo0 & 0x80000000U, 0U);
	EXPECT_EQ(read_entry_lo0 & 1, 1U);
	EXPECT_EQ(read_entry_lo1 & 1, 1U);
}

TEST(EETLBInstructions, GlobalRequiresBothEntryLoBits)
{
	const tlbs entry = EEMmu::BuildTLBEntry(0, 0x10000022, (0x1000U << 6) | 0x7, (0x1001U << 6) | 0x6);
	EXPECT_FALSE(entry.isGlobal());
}

TEST(EETLBInstructions, ProbeUsesPageMaskASIDGlobalAndLowestIndex)
{
	std::array<tlbs, 3> entries = {
		EEMmu::BuildTLBEntry(0x003U << 13, 0x20000011, (0x1000U << 6) | 0x6, (0x1004U << 6) | 0x6),
		EEMmu::BuildTLBEntry(0x003U << 13, 0x20000022, (0x2000U << 6) | 0x7, (0x2004U << 6) | 0x7),
		EEMmu::BuildTLBEntry(0x003U << 13, 0x20000022, (0x3000U << 6) | 0x7, (0x3004U << 6) | 0x7),
	};
	EEMmu::ProbeResult result = EEMmu::ProbeTLB(entries.data(), entries.size(), 0x20006022);
	EXPECT_EQ(result.index, 1);
	EXPECT_TRUE(EEMmu::HasWarning(result.warnings, EEMmu::Warning::MultipleMatch));

	entries[1].EntryLo0.G = entries[1].EntryLo1.G = 0;
	entries[2].EntryLo0.G = entries[2].EntryLo1.G = 0;
	result = EEMmu::ProbeTLB(entries.data(), entries.size(), 0x20006033);
	EXPECT_EQ(result.index, -1);

	// Masked VPN2 bits are don't-care on both the stored entry and the probe value.
	// Keep the stored side deliberately unnormalized to catch asymmetric comparisons.
	tlbs unnormalized_entry =
		EEMmu::BuildTLBEntry(0x003U << 13, 0x20000044, (0x4000U << 6) | 0x6, (0x4004U << 6) | 0x6);
	unnormalized_entry.EntryHi.VPN2 |= 0x2;
	EXPECT_EQ(EEMmu::ProbeTLB(&unnormalized_entry, 1, 0x20006044).index, 0);
	EXPECT_EQ(EEMmu::ProbeTLB(&unnormalized_entry, 1, 0x20008044).index, -1);
}

TEST(EETLBInstructions, RandomDecrementsAndWrapsAtWired)
{
	EXPECT_EQ(EEMmu::AdvanceRandom(47, 0), 46U);
	EXPECT_EQ(EEMmu::AdvanceRandom(1, 0), 0U);
	EXPECT_EQ(EEMmu::AdvanceRandom(0, 0), 47U);
	EXPECT_EQ(EEMmu::AdvanceRandom(12, 12), 47U);
	EXPECT_EQ(EEMmu::AdvanceRandom(13, 12), 12U);

	EEMmu::Warning warnings = EEMmu::Warning::None;
	EXPECT_EQ(EEMmu::AdvanceRandom(10, 48, &warnings), 47U);
	EXPECT_TRUE(EEMmu::HasWarning(warnings, EEMmu::Warning::InvalidWired));
}

TEST(EETLBInstructions, TranslationGenerationInvalidatesOnlyChangedEntry)
{
	const u32 global_before = EEMmu::GetTranslationGeneration();
	const u32 entry_zero_before = EEMmu::GetTLBEntryGeneration(0);
	const u32 entry_one_before = EEMmu::GetTLBEntryGeneration(1);
	EXPECT_EQ(*EEMmu::GetTranslationGenerationAddress(), global_before);
	EXPECT_EQ(*EEMmu::GetTLBEntryGenerationAddress(0), entry_zero_before);

	EEMmu::InvalidateTLBEntry(1);
	EXPECT_NE(EEMmu::GetTranslationGeneration(), global_before);
	EXPECT_EQ(EEMmu::GetTLBEntryGeneration(0), entry_zero_before);
	EXPECT_NE(EEMmu::GetTLBEntryGeneration(1), entry_one_before);

	const u32 entry_zero_before_full_reset = EEMmu::GetTLBEntryGeneration(0);
	EEMmu::InvalidateTranslations();
	EXPECT_NE(EEMmu::GetTLBEntryGeneration(0), entry_zero_before_full_reset);
}
