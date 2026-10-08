// SPDX-FileCopyrightText: 2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include <gtest/gtest.h>

#ifdef ARCH_X86
#include "pcsx2/x86/iR5900.h"
#include <array>
#include <random>
#include <type_traits>

using namespace x86Emitter;

static_assert(!std::is_convertible_v<EERecompileInfo, int>);
static_assert(std::is_trivially_copyable_v<EERecompileInfo>);

// A stray integer must not decode as EE metadata (the old 4-bit truncation).
template <typename T>
concept EEDecodable = requires(T info) { EEGetHostRegister(info, EERecompileInfo::S); };
static_assert(EEDecodable<EERecompileInfo>);
static_assert(!EEDecodable<u32> && !EEDecodable<int>);
static_assert(!std::is_convertible_v<u32, EERecompileInfo>);

TEST(EERecompileMetadata, AllHostIDsAndIndependentFields)
{
	for (int slot = 0; slot < 5; slot++)
		for (int id = 0; id < 32; id++)
		{
			constexpr u32 flags[] = {PROCESS_EE_S, PROCESS_EE_T, PROCESS_EE_D, PROCESS_EE_LO, PROCESS_EE_HI};
			EERecompileInfo info(PROCESS_EE_XMM);
			for (int field = 0; field < 5; field++)
				info |= EERecompileInfo::Operand(static_cast<EERecompileInfo::Slot>(field), field == slot ? id : 31 - field, flags[field]);
			for (int field = 0; field < 5; field++)
			{
				EXPECT_EQ(info.Get(static_cast<EERecompileInfo::Slot>(field)), field == slot ? id : 31 - field);
				EXPECT_NE(info & flags[field], 0u);
			}
			EXPECT_EQ(EEREC_ACC, EEREC_LO);
			EXPECT_NE(info & PROCESS_EE_XMM, 0u);
		}
	for (u32 id = 0; id < 16; id++)
	{
		u32 info = PROCESS_PSX_SET_S(id) | PROCESS_PSX_SET_T(15 - id);
		EXPECT_EQ(PSXREC_S, id);
		EXPECT_EQ(PSXREC_T, 15 - id);
	}
}

class EEXMMAllocatorTest : public testing::Test
{
protected:
	alignas(64) std::array<u8, 256 * 1024> code{};
	AVX512Features saved_features;
	u8* saved_ptr;
	void SetUp() override
	{
		saved_features = avx512;
		saved_ptr = xGetPtr();
		xSetPtr(code.data());
		avx512 = {true, true, true, true, true, true, true, true, true, true, true};
		_initXMMregs();
	}
	void TearDown() override
	{
		_initXMMregs();
		avx512 = saved_features;
		xSetPtr(saved_ptr);
	}
};

TEST_F(EEXMMAllocatorTest, HighBankOwnershipAndExhaustion)
{
	for (int i = 16; i < 32; i++)
	{
		EXPECT_EQ(_allocTempXMMregEVEX(XMMT_INT, 1u << i), i);
		EXPECT_EQ(xmmregs[i].type, XMMTYPE_TEMP);
		EXPECT_TRUE(xmmregs[i].needed);
	}
	EXPECT_EQ(_allocTempXMMregEVEX(XMMT_INT, 0xffff0000u), 0);
	_freeXMMreg(0);
	for (int i = 31; i >= 16; i--)
	{
		_freeXMMreg(i);
		EXPECT_FALSE(xmmregs[i].inuse);
		EXPECT_EQ(_allocTempXMMregEVEX(XMMT_FPS, 1u << i), i);
		EXPECT_EQ(g_xmmtypes[i], XMMT_FPS);
		_freeXMMreg(i);
	}
	_clearNeededXMMregs();
}

TEST_F(EEXMMAllocatorTest, UnsupportedCPUAndLegacyPolicy)
{
	avx512 = {};
	for (int i = 0; i < 16; i++)
		EXPECT_EQ(_allocTempXMMregEVEX(XMMT_INT), i);
	for (int i = 0; i < 16; i++)
		_freeXMMreg(i);
	EXPECT_EQ(_allocTempXMMregEVEX(XMMT_INT, 1u << 31), 0);
	_freeXMMreg(0);
	for (int i = 16; i < 32; i++)
		EXPECT_FALSE(xmmregs[i].inuse);
}

TEST_F(EEXMMAllocatorTest, RandomTemporaryLifetimesAndCallEmission)
{
	std::mt19937 random(0x590032);
	std::array<bool, 32> live{};
	for (int n = 0; n < 10000; n++)
	{
		const int id = 16 + (random() & 15);
		if (live[id])
			_freeXMMreg(id);
		else
			ASSERT_EQ(_allocTempXMMregEVEX(XMMT_INT, 1u << id), id);
		live[id] = !live[id];
		for (int i = 16; i < 32; i++)
			ASSERT_EQ(xmmregs[i].inuse != 0, live[i]);
	}
	// Generation only: this Windows development CPU cannot execute AVX-512.
	_eeCallWithHighXMM(code.data());
	for (int i = 16; i < 32; i++)
	{
		EXPECT_EQ(xmmregs[i].inuse != 0, live[i]);
		_freeXMMreg(i);
	}
	_clearNeededXMMregs();
}

TEST_F(EEXMMAllocatorTest, BranchSnapshotAndFlushRetainHighOwnership)
{
	ASSERT_EQ(_allocTempXMMregEVEX(XMMT_INT, 1u << 31), 31);
	SaveBranchState();
	_freeXMMreg(31);
	LoadBranchState();
	EXPECT_TRUE(xmmregs[31].inuse);
	EXPECT_EQ(xmmregs[31].type, XMMTYPE_TEMP);
	u8* before = xGetPtr();
	_flushXMMregs();
	EXPECT_EQ(xGetPtr(), before);
	EXPECT_TRUE(xmmregs[31].inuse);
	_freeXMMreg(31);
	_clearNeededXMMregs();
}

TEST_F(EEXMMAllocatorTest, HighOperandEVEXEncodingAndABI)
{
	u8* begin = xGetPtr();
	xVPXORD(xRegisterSSE(16), xRegisterSSE(17), xRegisterSSE(31));
	xVPADDD(xRegisterSSE(16), xRegisterSSE(17), xRegisterSSE(31));
	xVPSRADImm(xRegisterSSE(16), xRegisterSSE(31), 31);
	constexpr u8 expected[] = {
		0x62, 0x81, 0x75, 0x00, 0xef, 0xc7,
		0x62, 0x81, 0x75, 0x00, 0xfe, 0xc7,
		0x62, 0x91, 0x7d, 0x00, 0x72, 0xe7, 0x1f};
	ASSERT_EQ(xGetPtr() - begin, sizeof(expected));
	EXPECT_EQ(std::memcmp(begin, expected, sizeof(expected)), 0);
	for (int i = 16; i < 32; i++)
		EXPECT_TRUE(xRegisterSSE::IsCallerSaved(i));
}
#endif
