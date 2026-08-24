// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Memory.h"
#include "vtlb.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <memory>

namespace
{
	using namespace vtlb_private;

	constexpr u32 TEST_PHYSICAL_PAGE = 0x01234000;
	constexpr vtlbHandler TEST_HANDLER = VTLB_HANDLER_ITEMS - 1;

	u32 s_handler_read_address;
	u32 s_handler_write_address;
	u32 s_handler_write_value;

	mem32_t TestHandlerRead32(u32 paddr)
	{
		s_handler_read_address = paddr;
		return 0x89abcdef;
	}

	void TestHandlerWrite32(u32 paddr, mem32_t value)
	{
		s_handler_write_address = paddr;
		s_handler_write_value = value;
	}

	class PhysicalMapGuard
	{
	public:
		PhysicalMapGuard()
			: m_old_mapping(vtlbdata.pmap[TEST_PHYSICAL_PAGE >> VTLB_PAGE_BITS])
			, m_old_vmap(vtlbdata.vmap)
		{
			// A null vmap makes accidental virtual-map use fail immediately.
			vtlbdata.vmap = nullptr;
		}

		~PhysicalMapGuard()
		{
			vtlbdata.pmap[TEST_PHYSICAL_PAGE >> VTLB_PAGE_BITS] = m_old_mapping;
			vtlbdata.vmap = m_old_vmap;
		}

	private:
		VTLBPhysical m_old_mapping;
		VTLBVirtual* m_old_vmap;
	};

	class HandlerTableGuard
	{
	public:
		HandlerTableGuard()
			: m_old_read(vtlbdata.RWFT[vtlbMemFP<32, false>::Index][0][TEST_HANDLER])
			, m_old_write(vtlbdata.RWFT[vtlbMemFP<32, true>::Index][1][TEST_HANDLER])
		{
			vtlbdata.RWFT[vtlbMemFP<32, false>::Index][0][TEST_HANDLER] = reinterpret_cast<void*>(&TestHandlerRead32);
			vtlbdata.RWFT[vtlbMemFP<32, true>::Index][1][TEST_HANDLER] = reinterpret_cast<void*>(&TestHandlerWrite32);
		}

		~HandlerTableGuard()
		{
			vtlbdata.RWFT[vtlbMemFP<32, false>::Index][0][TEST_HANDLER] = m_old_read;
			vtlbdata.RWFT[vtlbMemFP<32, true>::Index][1][TEST_HANDLER] = m_old_write;
		}

	private:
		void* m_old_read;
		void* m_old_write;
	};

	class ScratchpadGuard
	{
	public:
		ScratchpadGuard()
			: m_old_ee_mem(eeMem)
		{
			if (!eeMem)
			{
				m_memory.reset(new EEVM_MemoryAllocMess);
				eeMem = m_memory.get();
			}
			std::copy_n(eeMem->Scratch, m_original.size(), m_original.data());
		}

		~ScratchpadGuard()
		{
			std::copy_n(m_original.data(), m_original.size(), eeMem->Scratch);
			eeMem = m_old_ee_mem;
		}

	private:
		EEVM_MemoryAllocMess* m_old_ee_mem;
		std::unique_ptr<EEVM_MemoryAllocMess> m_memory;
		std::array<u8, Ps2MemSize::Scratch> m_original;
	};
} // namespace

TEST(EEPhysicalMemory, DirectRAMAccessNeverConsultsVirtualMap)
{
	PhysicalMapGuard guard;
	alignas(16) std::array<u8, VTLB_PAGE_SIZE> ram = {};
	vtlbdata.pmap[TEST_PHYSICAL_PAGE >> VTLB_PAGE_BITS] = VTLBPhysical::fromPointer(ram.data());

	vtlb_physWrite<mem8_t>(TEST_PHYSICAL_PAGE + 0x10, 0x12);
	vtlb_physWrite<mem16_t>(TEST_PHYSICAL_PAGE + 0x12, 0x3456);
	vtlb_physWrite<mem32_t>(TEST_PHYSICAL_PAGE + 0x14, 0x789abcde);
	vtlb_physWrite<mem64_t>(TEST_PHYSICAL_PAGE + 0x18, 0x0123456789abcdefULL);
	alignas(16) const u128 expected128 = {{0x1122334455667788ULL, 0x99aabbccddeeff00ULL}};
	vtlb_physWrite128(TEST_PHYSICAL_PAGE + 0x20, r128_load(&expected128));

	EXPECT_EQ(vtlb_physRead<mem8_t>(TEST_PHYSICAL_PAGE + 0x10), 0x12);
	EXPECT_EQ(vtlb_physRead<mem16_t>(TEST_PHYSICAL_PAGE + 0x12), 0x3456);
	EXPECT_EQ(vtlb_physRead<mem32_t>(TEST_PHYSICAL_PAGE + 0x14), 0x789abcdeU);
	EXPECT_EQ(vtlb_physRead<mem64_t>(TEST_PHYSICAL_PAGE + 0x18), 0x0123456789abcdefULL);
	EXPECT_EQ(r128_to_u128(vtlb_physRead128(TEST_PHYSICAL_PAGE + 0x20)), expected128);
}

TEST(EEPhysicalMemory, MMIOHandlerReceivesExactPhysicalAddress)
{
	PhysicalMapGuard map_guard;
	HandlerTableGuard handler_guard;
	vtlbdata.pmap[TEST_PHYSICAL_PAGE >> VTLB_PAGE_BITS] = VTLBPhysical::fromHandler(TEST_HANDLER);
	s_handler_read_address = s_handler_write_address = s_handler_write_value = 0;

	constexpr u32 paddr = TEST_PHYSICAL_PAGE + 0x5a0;
	EXPECT_EQ(vtlb_physRead<mem32_t>(paddr), 0x89abcdefU);
	EXPECT_EQ(s_handler_read_address, paddr);
	vtlb_physWrite<mem32_t>(paddr, 0x13579bdf);
	EXPECT_EQ(s_handler_write_address, paddr);
	EXPECT_EQ(s_handler_write_value, 0x13579bdfU);
}

TEST(EEPhysicalMemory, PhysicalMapClassificationDoesNotPerformAnAccess)
{
	PhysicalMapGuard map_guard;
	HandlerTableGuard handler_guard;
	alignas(16) std::array<u8, VTLB_PAGE_SIZE> ram = {};
	s_handler_read_address = s_handler_write_address = 0;

	vtlbdata.pmap[TEST_PHYSICAL_PAGE >> VTLB_PAGE_BITS] = VTLBPhysical::fromPointer(ram.data());
	EXPECT_TRUE(vtlb_IsPhysicalAddressMapped(TEST_PHYSICAL_PAGE));

	vtlbdata.pmap[TEST_PHYSICAL_PAGE >> VTLB_PAGE_BITS] = VTLBPhysical::fromHandler(TEST_HANDLER);
	EXPECT_TRUE(vtlb_IsPhysicalAddressMapped(TEST_PHYSICAL_PAGE));
	EXPECT_EQ(s_handler_read_address, 0U);
	EXPECT_EQ(s_handler_write_address, 0U);

	EXPECT_FALSE(vtlb_IsPhysicalAddressMapped(VTLB_PMAP_SZ));
	EXPECT_FALSE(vtlb_IsPhysicalAddressMapped(0xffffffff));
}

TEST(EEPhysicalMemory, ScratchpadAccessUsesLowFourteenBits)
{
	ScratchpadGuard guard;
	vtlb_sprWrite<mem32_t>(0x4004, 0x12345678);
	EXPECT_EQ(vtlb_sprRead<mem32_t>(0x0004), 0x12345678U);
	EXPECT_EQ(vtlb_sprRead<mem32_t>(0x8004), 0x12345678U);

	alignas(16) const u128 expected128 = {{0x0123456789abcdefULL, 0xfedcba9876543210ULL}};
	vtlb_sprWrite128(0x7ff0, r128_load(&expected128));
	EXPECT_EQ(r128_to_u128(vtlb_sprRead128(0x3ff0)), expected128);
}
