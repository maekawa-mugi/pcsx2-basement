// SPDX-License-Identifier: GPL-3.0+
#pragma once

#include "common/Pcsx2Types.h"
#include <array>
#include <atomic>
#include <span>
#include <string>
#include <vector>
#include <memory>

struct Pcsx2Config;

namespace AgentConsole
{

	enum class Channel : u8
	{
		EESio,
		IOPStdout,
		IOPHLE,
		Count
	};
	constexpr size_t CHANNEL_COUNT = static_cast<size_t>(Channel::Count);
	extern std::atomic_bool g_enabled;
	extern std::atomic_bool g_deterministic;
	inline bool IsEnabled() { return g_enabled.load(std::memory_order_relaxed); }
	inline bool IsDeterministic() { return g_deterministic.load(std::memory_order_relaxed); }
	void ConfigureDeterministic(bool enabled);
	void PinSettings(Pcsx2Config& config);

	struct Status
	{
		u64 epoch = 0;
		u64 polls = 0;
		u32 pc = 0;
		u64 cycle = 0;
		u64 event = 0;
		u64 rx_consumed = 0;
		std::array<u64, CHANNEL_COUNT> produced{};
		std::array<u64, CHANNEL_COUNT> epoch_start{};
		bool full_tlb = false;
		bool ee_recompiler = false;
		bool extra_memory = false;
		std::string fault;
		std::string startup_error;
	};
	struct InputResult
	{
		std::string id;
		std::string state;
		u64 epoch = 0;
		u64 output_offset = 0;
		u64 consumed_before = 0;
		u32 size = 0;
		u64 at_event = 0;
	};
	struct Register
	{
		std::string name;
		u32 bits;
		u64 low;
		u64 high = 0;
	};
	struct RegisterBank
	{
		std::string name;
		std::vector<Register> values;
	};
	struct MemoryRegion
	{
		std::string name;
		std::vector<u8> bytes;
	};
	struct Snapshot
	{
		std::string id;
		u64 epoch = 0;
		u64 event = 0;
		u64 ee_cycle = 0;
		u64 iop_cycle = 0;
		std::vector<RegisterBank> banks;
		std::vector<MemoryRegion> memory;
		std::string settings;
	};
	struct Action
	{
		std::string id;
		std::string command;
		std::string state;
		u64 target = 0;
	};
	Action RequestAction(std::string id, std::string command, u64 target = 0);
	Action GetAction();
	std::shared_ptr<const Snapshot> GetSnapshot();

	void Enable();
	void Disable();
	void Capture(Channel channel, std::span<const u8> bytes);
	inline void CaptureByte(Channel channel, u8 value) { Capture(channel, {&value, 1}); }
	std::array<std::vector<u8>, CHANNEL_COUNT> Drain();
	Status GetStatus();
	void SetFault(std::string fault);
	void SetStartupError(std::string error);
	void Reset();
	void RecordRead();
	InputResult Submit(std::string id, u64 epoch, std::span<const u8> bytes);
	InputResult GetInput(const std::string& id);
	void RequestStop();
	// Called only by the CPU thread, including while paused.
	void Poll(bool executing = false);

} // namespace AgentConsole
