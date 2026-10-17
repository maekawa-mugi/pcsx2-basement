// SPDX-License-Identifier: GPL-3.0+
#include "AgentConsole.h"
#include "Config.h"
#include <gtest/gtest.h>

class AgentConsoleTest : public testing::Test
{
protected:
	void SetUp() override
	{
		AgentConsole::ConfigureDeterministic(false);
		AgentConsole::Enable();
	}
	void TearDown() override
	{
		AgentConsole::Disable();
		AgentConsole::ConfigureDeterministic(false);
	}
};

TEST_F(AgentConsoleTest, PreservesEveryByteWithoutNewlinesOrTextConversion)
{
	const std::vector<u8> bytes{0, '\r', '\n', 0xff, 0xe3, 0x81, 0x82, '#', ' '};
	AgentConsole::Capture(AgentConsole::Channel::EESio, bytes);
	EXPECT_EQ(AgentConsole::Drain()[0], bytes);
	EXPECT_TRUE(AgentConsole::Drain()[0].empty());
	EXPECT_EQ(AgentConsole::GetStatus().produced[0], bytes.size());
}

TEST_F(AgentConsoleTest, ResetCancelsInputAndSeparatesOutputEpochs)
{
	const std::vector<u8> bytes{'x'};
	AgentConsole::Capture(AgentConsole::Channel::EESio, bytes);
	EXPECT_EQ(AgentConsole::Submit("request", 0, bytes).state, "queued");
	AgentConsole::Reset();
	EXPECT_EQ(AgentConsole::GetInput("request").state, "cancelled_by_reset");
	EXPECT_EQ(AgentConsole::GetStatus().epoch_start[0], 1u);
	EXPECT_EQ(AgentConsole::Submit("other", 0, bytes).state, "epoch_mismatch");
}

TEST_F(AgentConsoleTest, DuplicateInputIsNotQueuedAgainAndConflictsAreExplicit)
{
	const std::vector<u8> bytes{'a'}, other{'b'};
	EXPECT_EQ(AgentConsole::Submit("request", 0, bytes).state, "queued");
	EXPECT_EQ(AgentConsole::Submit("request", 0, bytes).state, "queued");
	EXPECT_EQ(AgentConsole::Submit("request", 0, other).state, "request_id_conflict");
}

TEST_F(AgentConsoleTest, OverflowInvalidatesObservationAndRejectsInput)
{
	const std::vector<u8> bytes(4 * 1024 * 1024 + 1, 0xff);
	AgentConsole::Capture(AgentConsole::Channel::EESio, bytes);
	EXPECT_EQ(AgentConsole::GetStatus().fault, "capture_buffer_overflow");
	EXPECT_TRUE(AgentConsole::Drain()[0].empty());
	EXPECT_EQ(AgentConsole::GetStatus().produced[0], bytes.size());
	const std::vector<u8> input{'x'};
	EXPECT_EQ(AgentConsole::Submit("request", 0, input).state, "observation_fault");
}

TEST_F(AgentConsoleTest, DeterministicProfilePinsSettingsAcrossReloads)
{
	AgentConsole::ConfigureDeterministic(true);
	Pcsx2Config original;
	original.Cpu.EnableExperimentalEETLB = true;
	original.DEV9.EthEnable = true;
	original.Speedhacks.vuThread = true;
	AgentConsole::PinSettings(original);
	EXPECT_FALSE(original.DEV9.EthEnable);
	EXPECT_FALSE(original.Speedhacks.vuThread);
	EXPECT_EQ(original.GS.Renderer, GSRendererType::Null);
	Pcsx2Config changed;
	changed.Cpu.EnableExperimentalEETLB = false;
	changed.DEV9.EthEnable = true;
	AgentConsole::PinSettings(changed);
	EXPECT_TRUE(changed.Cpu.EnableExperimentalEETLB);
	EXPECT_FALSE(changed.DEV9.EthEnable);
	EXPECT_TRUE(changed.ManuallySetRealTimeClock);
}

TEST_F(AgentConsoleTest, DeterministicResetInvalidatesObservation)
{
	AgentConsole::ConfigureDeterministic(true);
	AgentConsole::Reset();
	EXPECT_TRUE(AgentConsole::GetStatus().fault.empty());
	AgentConsole::Reset();
	EXPECT_EQ(AgentConsole::GetStatus().fault, "reset_during_deterministic_run");
}
