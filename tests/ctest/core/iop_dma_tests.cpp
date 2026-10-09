// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "IopDma.h"
#include "IopHw.h"
#include "IopMem.h"

#include <gtest/gtest.h>

namespace
{
	class IOPDMA8Test : public ::testing::Test
	{
	protected:
		void SetUp() override
		{
			m_madr = HW_DMA8_MADR;
			m_bcr = HW_DMA8_BCR;
			m_chcr = HW_DMA8_CHCR;
			m_icr = HW_DMA_ICR2;
			HW_DMA_ICR2 = 0; // Keep the test independent of the IOP event scheduler.
			HW_DMA8_MADR = 0x10000;
			HW_DMA8_BCR = 0x00010400; // One 4 KiB block.
			HW_DMA8_CHCR = 0x01000200;
		}

		void TearDown() override
		{
			HW_DMA8_MADR = m_madr;
			HW_DMA8_BCR = m_bcr;
			HW_DMA8_CHCR = m_chcr;
			HW_DMA_ICR2 = m_icr;
		}

	private:
		u32 m_madr;
		u32 m_bcr;
		u32 m_chcr;
		u32 m_icr;
	};
} // namespace

TEST_F(IOPDMA8Test, ConsecutiveBlocksAdvanceWithoutReprogrammingMADR)
{
	psxDMA8Interrupt(4096);
	EXPECT_EQ(HW_DMA8_MADR, 0x11000u);
	EXPECT_EQ(HW_DMA8_CHCR, 0x00000200u);

	// A repeated completion must not advance the address twice.
	psxDMA8Interrupt(4096);
	EXPECT_EQ(HW_DMA8_MADR, 0x11000u);

	HW_DMA8_CHCR = 0x01000200;
	psxDMA8Interrupt(4096);
	EXPECT_EQ(HW_DMA8_MADR, 0x12000u);
}

TEST_F(IOPDMA8Test, CompletionUsesTransferredBytesInsteadOfCurrentBCR)
{
	HW_DMA8_BCR = 0;
	psxDMA8Interrupt(4096);
	EXPECT_EQ(HW_DMA8_MADR, 0x11000u);
}

TEST_F(IOPDMA8Test, OversizedTransferDoesNotAdvanceMADR)
{
	HW_DMA8_BCR = 0xffffffff;
	psxDma8(HW_DMA8_MADR, HW_DMA8_BCR, HW_DMA8_CHCR);
	EXPECT_EQ(HW_DMA8_MADR, 0x10000u);
	EXPECT_EQ(HW_DMA8_CHCR, 0x00000200u);
}
