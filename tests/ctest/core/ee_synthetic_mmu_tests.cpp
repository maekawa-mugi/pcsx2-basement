// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Cache.h"
#include "EEMmu.h"

#include <gtest/gtest.h>

#include <array>

namespace
{
	constexpr u32 STATUS_EXL = 1U << 1;
	constexpr u32 CODE_VADDR = 0x00400000;
	constexpr u32 DATA_VADDR = 0x10000000;
	constexpr u32 REPLACEMENT_VADDR = 0x18000000;
	constexpr u32 FAULT_VADDR = 0x20000000;
	constexpr u32 CODE_PADDR = 0x00100000;
	constexpr u32 DATA_PADDR = 0x00200000;
	constexpr u32 REPLACEMENT_PADDR = 0x00300000;
	constexpr u32 FAULT_PADDR = 0x00400000;

	class SyntheticEE
	{
	public:
		SyntheticEE()
		{
			m_code.fill(0);
			m_data.fill(0);
			m_code[0] = 0x3c081000; // lui t0, 0x1000
		}

		EEMmu::TranslationResult Translate(u32 vaddr, EEMmu::AccessType access_type) const
		{
			return EEMmu::TranslateAddress({m_registers.status, 3, 0, m_tlb.data(), m_tlb.size()}, vaddr, access_type);
		}

		u32 ServiceRefill(u32 vaddr, u32 paddr, EEMmu::AccessType access_type, u32 fault_pc)
		{
			const EEMmu::ExceptionResult exception = EEMmu::BuildException(
				m_registers, {EEMmu::Fault::Refill, access_type, vaddr, fault_pc, false});
			EXPECT_EQ(exception.vector, 0x80000000U);
			EXPECT_EQ(exception.registers.epc, fault_pc);
			EXPECT_NE(exception.registers.status & STATUS_EXL, 0U);
			m_registers = exception.registers;

			m_last_written_index = m_random;
			InstallEntry(m_last_written_index, vaddr, paddr, true, true);
			m_random = EEMmu::AdvanceRandom(m_random, 0);

			// Synthetic ERET: the real instruction restores PC from EPC and clears EXL.
			m_registers.status &= ~STATUS_EXL;
			return m_registers.epc;
		}

		void InstallEntry(u32 index, u32 vaddr, u32 paddr, bool valid, bool dirty)
		{
			const u32 flags = (3U << 3) | (dirty ? 0x4U : 0U) | (valid ? 0x2U : 0U) | 0x1U;
			const u32 lo0 = ((paddr >> 12) << 6) | flags;
			const u32 lo1 = ((((paddr & ~0x1fffU) + 0x1000) >> 12) << 6) | flags;
			m_tlb[index] = EEMmu::BuildTLBEntry(0, vaddr & 0xffffe000, lo0, lo1);
		}

		u32 LastWrittenIndex() const { return m_last_written_index; }

		EEMmu::ExceptionResult BuildFault(
			EEMmu::Fault fault, EEMmu::AccessType access_type, u32 vaddr, u32 fault_pc) const
		{
			return EEMmu::BuildException(m_registers, {fault, access_type, vaddr, fault_pc, false});
		}

		u32 ReadPhysical32(u32 paddr) const
		{
			if (paddr >= CODE_PADDR && paddr < CODE_PADDR + sizeof(m_code))
				return m_code[(paddr - CODE_PADDR) / sizeof(u32)];
			return m_data[(paddr - DATA_PADDR) / sizeof(u32)];
		}

		void WritePhysical32(u32 paddr, u32 value)
		{
			m_data[(paddr - DATA_PADDR) / sizeof(u32)] = value;
		}

	private:
		std::array<tlbs, EEMmu::TLB_ENTRY_COUNT> m_tlb = {};
		EEMmu::ExceptionRegisters m_registers = {};
		std::array<u32, 0x1000 / sizeof(u32)> m_code;
		std::array<u32, 0x1000 / sizeof(u32)> m_data;
		u32 m_random = 47;
		u32 m_last_written_index = 0;
	};
} // namespace

TEST(EESyntheticMMU, SoftwareRefillERETAndVirtualAccessContinuation)
{
	SyntheticEE ee;
	u32 pc = CODE_VADDR;

	EEMmu::TranslationResult translation = ee.Translate(pc, EEMmu::AccessType::Fetch);
	ASSERT_EQ(translation.fault, EEMmu::Fault::Refill);
	pc = ee.ServiceRefill(pc, CODE_PADDR, EEMmu::AccessType::Fetch, pc);
	ASSERT_EQ(pc, CODE_VADDR);

	translation = ee.Translate(pc, EEMmu::AccessType::Fetch);
	ASSERT_EQ(translation.fault, EEMmu::Fault::None);
	EXPECT_EQ(ee.ReadPhysical32(translation.paddr), 0x3c081000U);

	translation = ee.Translate(DATA_VADDR, EEMmu::AccessType::Load);
	ASSERT_EQ(translation.fault, EEMmu::Fault::Refill);
	EXPECT_EQ(ee.ServiceRefill(DATA_VADDR, DATA_PADDR, EEMmu::AccessType::Load, pc), pc);
	const u32 data_entry = ee.LastWrittenIndex();

	translation = ee.Translate(DATA_VADDR, EEMmu::AccessType::Store);
	ASSERT_EQ(translation.fault, EEMmu::Fault::None);
	ee.WritePhysical32(translation.paddr, 0x50415353); // "PASS"
	translation = ee.Translate(DATA_VADDR, EEMmu::AccessType::Load);
	ASSERT_EQ(translation.fault, EEMmu::Fault::None);
	EXPECT_EQ(ee.ReadPhysical32(translation.paddr), 0x50415353U);

	// Replacing the data mapping forces the old virtual page through the refill path again.
	ee.InstallEntry(data_entry, REPLACEMENT_VADDR, REPLACEMENT_PADDR, true, true);
	EXPECT_EQ(ee.Translate(DATA_VADDR, EEMmu::AccessType::Load).fault, EEMmu::Fault::Refill);
	EXPECT_EQ(ee.ServiceRefill(DATA_VADDR, DATA_PADDR, EEMmu::AccessType::Load, pc), pc);
	translation = ee.Translate(DATA_VADDR, EEMmu::AccessType::Load);
	ASSERT_EQ(translation.fault, EEMmu::Fault::None);
	EXPECT_EQ(ee.ReadPhysical32(translation.paddr), 0x50415353U);

	// The synthetic handler can also repair Invalid and Modified entries and retry the access.
	ee.InstallEntry(10, FAULT_VADDR, FAULT_PADDR, false, false);
	translation = ee.Translate(FAULT_VADDR, EEMmu::AccessType::Load);
	ASSERT_EQ(translation.fault, EEMmu::Fault::Invalid);
	EXPECT_EQ(ee.BuildFault(translation.fault, EEMmu::AccessType::Load, FAULT_VADDR, pc).vector, 0x80000180U);

	ee.InstallEntry(10, FAULT_VADDR, FAULT_PADDR, true, false);
	translation = ee.Translate(FAULT_VADDR, EEMmu::AccessType::Store);
	ASSERT_EQ(translation.fault, EEMmu::Fault::Modified);
	EXPECT_EQ(ee.BuildFault(translation.fault, EEMmu::AccessType::Store, FAULT_VADDR, pc).vector, 0x80000180U);

	ee.InstallEntry(10, FAULT_VADDR, FAULT_PADDR, true, true);
	EXPECT_EQ(ee.Translate(FAULT_VADDR, EEMmu::AccessType::Store).fault, EEMmu::Fault::None);
}

TEST(EESyntheticMMU, CacheIndexOperationsDoNotTranslateAddresses)
{
	EXPECT_FALSE(CacheOpUsesAddressTranslation(0x07)); // IXIN
	EXPECT_FALSE(CacheOpUsesAddressTranslation(0x10)); // DXLTG
	EXPECT_FALSE(CacheOpUsesAddressTranslation(0x11)); // DXLDT
	EXPECT_FALSE(CacheOpUsesAddressTranslation(0x12)); // DXSTG
	EXPECT_FALSE(CacheOpUsesAddressTranslation(0x13)); // DXSDT
	EXPECT_FALSE(CacheOpUsesAddressTranslation(0x14)); // DXWBIN
	EXPECT_FALSE(CacheOpUsesAddressTranslation(0x16)); // DXIN
	EXPECT_FALSE(CacheOpUsesAddressTranslation(0x0c)); // BFH

	EXPECT_TRUE(CacheOpUsesAddressTranslation(0x18)); // DHWBIN
	EXPECT_TRUE(CacheOpUsesAddressTranslation(0x1a)); // DHIN
	EXPECT_TRUE(CacheOpUsesAddressTranslation(0x1c)); // DHWOIN
}

TEST(EESyntheticMMU, EffectiveKernelModeAccountsForExceptionLevels)
{
	EXPECT_TRUE(EEMmu::IsKernelMode(0));
	EXPECT_TRUE(EEMmu::IsKernelMode(2U << 3 | (1U << 1))); // User KSU while EXL is set.
	EXPECT_TRUE(EEMmu::IsKernelMode(2U << 3 | (1U << 2))); // User KSU while ERL is set.
	EXPECT_FALSE(EEMmu::IsKernelMode(1U << 3)); // Supervisor mode.
	EXPECT_FALSE(EEMmu::IsKernelMode(2U << 3)); // User mode.
	EXPECT_FALSE(EEMmu::IsKernelMode(0x00010c11)); // Observed PS2 Linux user syscall state.
}

TEST(EESyntheticMMU, EffectiveUserModeExcludesExceptionLevels)
{
	EXPECT_TRUE(EEMmu::IsUserMode(2U << 3));
	EXPECT_TRUE(EEMmu::IsUserMode(0x00010c11)); // Observed PS2 Linux user syscall state.
	EXPECT_FALSE(EEMmu::IsUserMode(0));
	EXPECT_FALSE(EEMmu::IsUserMode(1U << 3));
	EXPECT_FALSE(EEMmu::IsUserMode(2U << 3 | (1U << 1)));
	EXPECT_FALSE(EEMmu::IsUserMode(2U << 3 | (1U << 2)));
}
