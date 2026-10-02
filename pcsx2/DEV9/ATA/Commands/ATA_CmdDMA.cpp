// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "DEV9/ATA/ATA.h"
#include "DEV9/DEV9.h"

void ATA::DRQCmdDMADataToHost()
{
	//Ready to Start DMA
	rdTransferred = 0; // ps2linux rig: an aborted transfer must not offset the next one
	regStatus &= ~ATA_STAT_BUSY;
	regStatus |= ATA_STAT_DRQ;
	dmaReady = true;
	DEV9runFIFO();
	//PCSX2 will Start DMA
}
void ATA::PostCmdDMADataToHost()
{
	//readBuffer = null;
	nsectorLeft = 0;

	regStatus &= ~ATA_STAT_DRQ;
	regStatus &= ~ATA_STAT_BUSY;
	dmaReady = false;

	SetPendingInterrupt();
}

void ATA::DRQCmdDMADataFromHost()
{
	//Ready to Start DMA
	if (!HDD_CanAssessOrSetError())
		return;

	nsectorLeft = nsector;
	// ps2linux rig: an aborted transfer must not offset the next one
	delete[] currentWrite;
	wrTransferred = 0;
	currentWrite = new u8[nsector * 512];
	currentWriteLength = nsector * 512;
	currentWriteSectors = HDD_GetLBA();
	currentWriteLinuxSwap = IsSelectedLinuxSwap();


	regStatus &= ~ATA_STAT_BUSY;
	regStatus |= ATA_STAT_DRQ;
	dmaReady = true;
	DEV9runFIFO();
	//PCSX2 will Start DMA
}
void ATA::PostCmdDMADataFromHost()
{
	const bool write_completed = currentWriteLinuxSwap;
	if (write_completed)
	{
		// The slave is already host memory. Avoid a queue wake-up and a second copy on
		// the I/O thread for every small Linux swap write; IRQ ordering is unchanged.
		const u64 image_pos = currentWriteSectors * 512;
		if (image_pos > LINUX_SWAP_SIZE || currentWriteLength > LINUX_SWAP_SIZE - image_pos)
		{
			Console.Error("DEV9: ATA: RAM disk write exceeds device bounds (offset %" PRIu64 ", length %u)",
				image_pos, currentWriteLength);
			regStatus |= ATA_STAT_ERR;
			regError |= ATA_ERR_ID;
		}
		else
		{
			std::memcpy(&linuxSwapData[static_cast<size_t>(image_pos)], currentWrite, currentWriteLength);
		}
		delete[] currentWrite;
	}
	else
	{
		WriteQueueEntry entry{0};
		entry.data = currentWrite;
		entry.length = currentWriteLength;
		entry.sector = currentWriteSectors;
		entry.linuxSwap = false;
		writeQueue.Enqueue(entry);
	}
	currentWrite = nullptr;
	currentWriteLength = 0;
	currentWriteSectors = 0;
	currentWriteLinuxSwap = false;
	nsectorLeft = 0;

	regStatus &= ~ATA_STAT_DRQ;
	dmaReady = false;

	if (fetWriteCacheEnabled || write_completed)
	{
		regStatus &= ~ATA_STAT_BUSY;
		SetPendingInterrupt();
	}
	else
		awaitFlush = true;

	if (!write_completed)
		Async(-1);
}

int ATA::ReadDMAToFIFO(u8* buffer, int space)
{
	if (udmaMode >= 0 || mdmaMode >= 0)
	{
		if (space <= 0 || nsector <= 0 || !buffer || !readBuffer)
			return 0;

		// Read to FIFO
		const s64 remaining = static_cast<s64>(nsector) * 512 - rdTransferred;
		if (remaining <= 0 || rdTransferred < 0 || rdTransferred >= readBufferLen)
			return 0;
		const int size = std::min({space, static_cast<int>(remaining), readBufferLen - rdTransferred});
		memcpy(buffer, &readBuffer[rdTransferred], size);

		rdTransferred += size;

		if (rdTransferred >= nsector * 512)
		{
			HDD_SetErrorAtTransferEnd();

			nsector = 0;
			rdTransferred = 0;
			PostCmdDMADataToHost();
		}

		return size;
	}
	return 0;
}

int ATA::WriteDMAFromFIFO(u8* buffer, int available)
{
	if (udmaMode >= 0 || mdmaMode >= 0)
	{
		if (available <= 0 || nsector <= 0 || !buffer || !currentWrite)
			return 0;

		// Write to FIFO
		const s64 remaining = static_cast<s64>(currentWriteLength) - wrTransferred;
		if (remaining <= 0 || wrTransferred < 0)
			return 0;
		const int size = std::min(available, static_cast<int>(remaining));
		memcpy(&currentWrite[wrTransferred], buffer, size);

		wrTransferred += size;

		if (wrTransferred >= nsector * 512)
		{
			HDD_SetErrorAtTransferEnd();

			nsector = 0;
			wrTransferred = 0;
			PostCmdDMADataFromHost();
		}

		return size;
	}
	return 0;
}

//GENRAL FEATURE SET

void ATA::HDD_ReadDMA(bool isLBA48)
{
	if (!PreCmd())
		return;
	DevCon.WriteLn(isLBA48 ? "DEV9: HDD_ReadDMA48" : "DEV9: HDD_ReadDMA");

	IDE_CmdLBA48Transform(isLBA48);

	regStatus &= ~ATA_STAT_SEEK;
	if (!HDD_CanSeek())
	{
		Console.Error("DEV9: ATA: Transfer from invalid LBA %lu", HDD_GetLBA());
		nsector = -1;
		regStatus |= ATA_STAT_ERR;
		regStatusSeekLock = -1;
		regError |= ATA_ERR_ID;
		PostCmdNoData();
		return;
	}
	else
		regStatus |= ATA_STAT_SEEK;

	//Do Sync Read
	HDD_ReadSync(&ATA::DRQCmdDMADataToHost);
}

void ATA::HDD_WriteDMA(bool isLBA48)
{
	if (!PreCmd())
		return;
	DevCon.WriteLn(isLBA48 ? "DEV9: HDD_WriteDMA48" : "DEV9: HDD_WriteDMA");

	IDE_CmdLBA48Transform(isLBA48);

	regStatus &= ~ATA_STAT_SEEK;
	if (!HDD_CanSeek())
	{
		Console.Error("DEV9: ATA: Transfer from invalid LBA %lu", HDD_GetLBA());
		nsector = -1;
		regStatus |= ATA_STAT_ERR;
		regStatusSeekLock = -1;
		regError |= ATA_ERR_ID;
		PostCmdNoData();
		return;
	}
	else
		regStatus |= ATA_STAT_SEEK;

	//Do Async write
	DRQCmdDMADataFromHost();
}
