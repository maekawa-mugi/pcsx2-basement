// SPDX-License-Identifier: GPL-3.0+
#include "AgentConsole.h"
#include "Config.h"
#include "Hw.h"
#include "R5900.h"
#include "VMManager.h"
#include "DebugTools/DebugInterface.h"
#include "EEMmu.h"
#include "INISettingsInterface.h"
#include "Memory.h"
#include "MemoryTypes.h"
#include "R3000A.h"
#include "VU.h"
#include "SIO/Pad/PadTypes.h"
#include "common/SettingsWrapper.h"

#include <deque>
#include <algorithm>
#include <mutex>
#include <utility>

namespace AgentConsole
{
	std::atomic_bool g_enabled{false};
	std::atomic_bool g_deterministic{false};
	static std::unique_ptr<Pcsx2Config> s_pinned;
	static Action s_action;
	static std::shared_ptr<Snapshot> s_snapshot;
	static bool s_boundary = false;
	static u64 s_event = 0; // EE event tests only; host polling never advances this clock.
	static std::atomic<u64> s_run_target{0};
	static std::mutex s_mutex;
	static Status s_status;
	static std::array<std::vector<u8>, CHANNEL_COUNT> s_output;
	struct Input
	{
		InputResult result;
		std::vector<u8> bytes;
	};
	static std::deque<Input> s_inputs;
	static size_t s_next_input = 0;
	static std::atomic_bool s_needs_service{false};
	static u64 s_polls = 0; // CPU thread only, initialized before the thread is started.
	static bool s_stop = false;
	static constexpr size_t MAX_BUFFER = 4 * 1024 * 1024;

	void ConfigureDeterministic(bool enabled) { g_deterministic.store(enabled); }
	void PinSettings(Pcsx2Config& config)
	{
		if (!IsDeterministic())
			return;
		if (s_pinned)
		{
			Pcsx2Config pinned(*s_pinned);
			pinned.CopyRuntimeConfig(config);
			config = std::move(pinned);
			return;
		}
		config.Speedhacks.vuThread = false;
		config.Speedhacks.EECycleRate = 0;
		config.Speedhacks.EECycleSkip = 0;
		config.GS.Renderer = GSRendererType::Null;
		config.GS.SynchronousMTGS = true;
		config.EmulationSpeed.SyncToHostRefreshRate = false;
		config.EmulationSpeed.UseVSyncForTiming = false;
		config.SPU2.Backend = AudioBackend::Null;
		config.SPU2.SyncMode = Pcsx2Config::SPU2Options::SPU2SyncMode::Disabled;
		config.DEV9.EthEnable = false;
		config.DEV9.HddEnable = false;
		config.DEV9.HddEnableLinuxSwap = false;
		for (auto& port : config.USB.Ports)
			port.DeviceType = -1;
		for (auto& port : config.Pad.Ports)
			port.Type = Pad::ControllerType::NotConnected;
		for (auto& card : config.Mcd)
			card.Enabled = false;
		config.EnablePatches = false;
		config.EnableCheats = false;
		config.EnableGameFixes = false;
		config.EnableWideScreenPatches = false;
		config.EnableNoInterlacingPatches = false;
		config.EnablePINE = false;
		config.EnableRecordingTools = false;
		config.EnableDiscordPresence = false;
		config.Achievements.Enabled = false;
		config.Achievements.HardcoreMode = false;
		config.HostFs = false;
		config.ManuallySetRealTimeClock = true;
		config.RtcYear = 20;
		config.RtcMonth = 1;
		config.RtcDay = 1;
		config.RtcHour = 0;
		config.RtcMinute = 0;
		config.RtcSecond = 0;
		s_pinned = std::make_unique<Pcsx2Config>(config);
	}

	static std::shared_ptr<Snapshot> CaptureSnapshot(const std::string& id)
	{
		auto snapshot = std::make_shared<Snapshot>();
		snapshot->id = id;
		snapshot->epoch = s_status.epoch;
		snapshot->event = s_event;
		snapshot->ee_cycle = cpuRegs.cycle;
		snapshot->iop_cycle = psxRegs.cycle;
		for (int category = 0; category < r5900Debug.getRegisterCategoryCount(); category++)
		{
			RegisterBank bank{"ee." + std::string(r5900Debug.getRegisterCategoryName(category)), {}};
			for (int index = 0; index < r5900Debug.getRegisterCount(category); index++)
			{
				const auto value = r5900Debug.getRegister(category, index);
				bank.values.push_back({r5900Debug.getRegisterName(category, index),
					static_cast<u32>(r5900Debug.getRegisterSize(category)), value._u64[0], value._u64[1]});
			}
			snapshot->banks.push_back(std::move(bank));
		}
		RegisterBank ee{"ee.control", {}};
		ee.values = {{"pc", 32, cpuRegs.pc}, {"code", 32, cpuRegs.code}, {"sa", 32, cpuRegs.sa},
			{"branch", 32, static_cast<u32>(cpuRegs.branch)}, {"delay_slot", 32, cpuRegs.IsDelaySlot},
			{"interrupt", 32, cpuRegs.interrupt}, {"next_event_cycle", 64, cpuRegs.nextEventCycle}};
		ee.values.push_back({"PCCR", 32, cpuRegs.PERF.n.pccr.val});
		ee.values.push_back({"PCR0", 32, cpuRegs.PERF.n.pcr0});
		ee.values.push_back({"PCR1", 32, cpuRegs.PERF.n.pcr1});
		for (u32 i = 0; i < 32; i++)
		{
			ee.values.push_back({"event_cycles." + std::to_string(i), 32, cpuRegs.eCycle[i]});
			ee.values.push_back({"start_cycles." + std::to_string(i), 64, cpuRegs.sCycle[i]});
		}
		snapshot->banks.push_back(std::move(ee));
		RegisterBank iop{"iop", {}};
		for (u32 i = 0; i < 34; i++)
			iop.values.push_back({"gpr" + std::to_string(i), 32, psxRegs.GPR.r[i]});
		for (u32 i = 0; i < 32; i++)
		{
			iop.values.push_back({"cp0." + std::to_string(i), 32, psxRegs.CP0.r[i]});
			iop.values.push_back({"cp2.data." + std::to_string(i), 32, psxRegs.CP2D.r[i]});
			iop.values.push_back({"cp2.control." + std::to_string(i), 32, psxRegs.CP2C.r[i]});
		}
		iop.values.push_back({"pc", 32, psxRegs.pc});
		iop.values.push_back({"code", 32, psxRegs.code});
		iop.values.push_back({"interrupt", 32, psxRegs.interrupt});
		snapshot->banks.push_back(std::move(iop));
		for (auto* vu : {&VU0, &VU1})
		{
			RegisterBank bank{vu == &VU1 ? "vu1" : "vu0", {}};
			for (u32 i = 0; i < 32; i++)
			{
				bank.values.push_back({"vf" + std::to_string(i), 128, vu->VF[i].UD[0], vu->VF[i].UD[1]});
				bank.values.push_back({"vi" + std::to_string(i), 32, vu->VI[i].UL});
			}
			bank.values.push_back({"ACC", 128, vu->ACC.UD[0], vu->ACC.UD[1]});
			bank.values.push_back({"Q", 32, vu->q.UL});
			bank.values.push_back({"P", 32, vu->p.UL});
			bank.values.push_back({"cycle", 64, vu->cycle});
			bank.values.push_back({"flags", 32, vu->flags});
			snapshot->banks.push_back(std::move(bank));
		}
		RegisterBank tlb_bank{"tlb", {}};
		for (size_t i = 0; i < EEMmu::TLB_ENTRY_COUNT; i++)
		{
			u32 mask, hi, lo0, lo1;
			EEMmu::ReadTLBEntry(tlb[i], &mask, &hi, &lo0, &lo1);
			for (auto [name, value] : {std::pair{"PageMask", mask}, {"EntryHi", hi}, {"EntryLo0", lo0}, {"EntryLo1", lo1}})
				tlb_bank.values.push_back({std::to_string(i) + "." + name, 32, value});
		}
		snapshot->banks.push_back(std::move(tlb_bank));
		auto region = [&](const char* name, const u8* data, size_t size) {
			snapshot->memory.push_back({name, {data, data + size}});
		};
		region("ee.ram", eeMem->Main, Ps2MemSize::ExposedRam);
		region("ee.scratch", eeMem->Scratch, Ps2MemSize::Scratch);
		region("iop.ram", iopMem->Main, Ps2MemSize::ExposedIopRam);
		region("ee.mmio", eeHw, Ps2MemSize::Hardware);
		region("iop.mmio", iopHw, Ps2MemSize::IopHardware);
		region("vu0.data", VU0.Mem, 4096);
		region("vu0.micro", VU0.Micro, 4096);
		region("vu1.data", VU1.Mem, 16384);
		region("vu1.micro", VU1.Micro, 16384);
		snapshot->memory.push_back({"ee.sio.rx", {ee_sio_rx_fifo.begin(), ee_sio_rx_fifo.end()}});
		INISettingsInterface settings("");
		SettingsSaveWrapper wrapper(settings);
		Pcsx2Config copy(EmuConfig);
		copy.LoadSave(wrapper);
		snapshot->settings = settings.Serialize();
		return snapshot;
	}
	Action RequestAction(std::string id, std::string command, u64 target)
	{
		std::lock_guard lock(s_mutex);
		if (s_action.id == id)
			return s_action.command == command && s_action.target == target ? s_action : Action{id, command, "request_id_conflict", target};
		if (s_action.state == "queued" || s_action.state == "running")
			return {id, command, "busy", target};
		if (!VMManager::HasValidVM())
			return {id, command, "vm_not_ready", target};
		if (IsDeterministic() && s_next_input < s_inputs.size())
			return {id, command, "input_pending", target};
		if (command == "run" && (VMManager::GetState() != VMState::Paused || target <= s_status.event))
			return {id, command, "run_requires_paused_and_future_target", target};
		if (command == "snapshot" && IsDeterministic() && VMManager::GetState() != VMState::Paused)
			return {id, command, "deterministic_snapshot_requires_paused", target};
		if (command != "run" && command != "snapshot")
			return {id, command, "unknown_action", target};
		s_action = {std::move(id), std::move(command), "queued", target};
		s_needs_service.store(true, std::memory_order_release);
		return s_action;
	}
	Action GetAction()
	{
		std::lock_guard lock(s_mutex);
		return s_action;
	}
	std::shared_ptr<const Snapshot> GetSnapshot()
	{
		std::lock_guard lock(s_mutex);
		return s_snapshot;
	}

	void Enable()
	{
		std::lock_guard lock(s_mutex);
		s_status = {};
		s_pinned.reset();
		s_action = {};
		s_snapshot.reset();
		s_boundary = false;
		s_event = 0;
		s_run_target.store(0);
		s_output = {};
		s_inputs.clear();
		s_next_input = 0;
		s_polls = 0;
		s_needs_service.store(false, std::memory_order_relaxed);
		s_stop = false;
		g_enabled.store(true, std::memory_order_release);
	}
	void Disable() { g_enabled.store(false, std::memory_order_release); }
	void Capture(Channel channel, std::span<const u8> bytes)
	{
		if (!IsEnabled())
			return;
		std::lock_guard lock(s_mutex);
		const size_t index = static_cast<size_t>(channel);
		s_status.produced[index] += bytes.size();
		if (!s_status.fault.empty())
			return;
		auto& buffer = s_output[index];
		if (bytes.size() > MAX_BUFFER - buffer.size())
		{
			s_status.fault = "capture_buffer_overflow";
			return;
		}
		buffer.insert(buffer.end(), bytes.begin(), bytes.end());
	}
	std::array<std::vector<u8>, CHANNEL_COUNT> Drain()
	{
		std::lock_guard lock(s_mutex);
		std::array<std::vector<u8>, CHANNEL_COUNT> result;
		result.swap(s_output);
		return result;
	}
	Status GetStatus()
	{
		std::lock_guard lock(s_mutex);
		return s_status;
	}
	void SetFault(std::string fault)
	{
		std::lock_guard lock(s_mutex);
		if (s_status.fault.empty())
			s_status.fault = std::move(fault);
	}
	void SetStartupError(std::string error)
	{
		std::lock_guard lock(s_mutex);
		s_status.startup_error = std::move(error);
	}
	void Reset()
	{
		if (!IsEnabled())
			return;
		std::lock_guard lock(s_mutex);
		s_status.epoch++;
		if (IsDeterministic() && s_status.epoch > 1)
			s_status.fault = "reset_during_deterministic_run";
		s_status.epoch_start = s_status.produced;
		for (auto& input : s_inputs)
			if (input.result.state == "queued")
				input.result.state = "cancelled_by_reset";
		s_next_input = s_inputs.size();
	}
	void RecordRead()
	{
		if (!IsEnabled())
			return;
		std::lock_guard lock(s_mutex);
		s_status.rx_consumed++;
	}
	InputResult Submit(std::string id, u64 epoch, std::span<const u8> bytes)
	{
		std::lock_guard lock(s_mutex);
		for (const auto& input : s_inputs)
		{
			if (input.result.id != id)
				continue;
			if (input.result.epoch != epoch || input.bytes.size() != bytes.size() ||
				!std::equal(bytes.begin(), bytes.end(), input.bytes.begin()))
				return {id, "request_id_conflict", epoch};
			return input.result;
		}
		if (epoch != s_status.epoch)
			return {id, "epoch_mismatch", s_status.epoch};
		if (IsDeterministic() && VMManager::GetState() != VMState::Paused)
			return {id, "deterministic_input_requires_paused", epoch};
		if (IsDeterministic() && (s_action.state == "queued" || s_action.state == "running"))
			return {id, "action_in_progress", epoch};
		if (!s_status.fault.empty())
			return {id, "observation_fault", epoch};
		if (bytes.empty() || bytes.size() > 1024)
			return {id, "invalid_input_size", epoch};
		// Keep all IDs for this bounded session; never evict an ID and accidentally replay it.
		if (s_inputs.size() >= 4096)
			return {id, "request_limit", epoch};
		Input input{{std::move(id), "queued", epoch, 0, 0, static_cast<u32>(bytes.size())}, {bytes.begin(), bytes.end()}};
		s_inputs.push_back(std::move(input));
		s_needs_service.store(true, std::memory_order_release);
		return s_inputs.back().result;
	}
	InputResult GetInput(const std::string& id)
	{
		std::lock_guard lock(s_mutex);
		for (const auto& input : s_inputs)
			if (input.result.id == id)
				return input.result;
		return {id, "unknown_request"};
	}
	void RequestStop()
	{
		std::lock_guard lock(s_mutex);
		s_stop = true;
		s_needs_service.store(true, std::memory_order_release);
	}
	void Poll(bool executing)
	{
		if (!IsEnabled())
			return;
		if (executing)
		{
			++s_event;
			const u64 target = s_run_target.load(std::memory_order_relaxed);
			if (target && s_event >= target)
				s_needs_service.store(true, std::memory_order_release);
		}
		// Idle guests can execute millions of event tests per second. Only sample
		// telemetry periodically; new input and stop requests are serviced immediately.
		++s_polls;
		if (!s_needs_service.load(std::memory_order_acquire) && (s_polls & 1023) != 0)
			return;
		bool stop;
		bool exit_execution = false;
		bool capture = false;
		bool resume = false;
		std::string snapshot_id;
		{
			std::lock_guard lock(s_mutex);
			s_status.polls = s_polls;
			s_status.pc = cpuRegs.pc;
			s_status.cycle = cpuRegs.cycle;
			s_status.event = s_event;
			if (s_action.state == "queued" && s_action.command == "run" && !executing)
			{
				s_run_target.store(s_action.target);
				s_action.state = "running";
				resume = true;
			}
			else if ((s_action.state == "queued" && s_action.command == "snapshot") ||
					 (s_action.state == "running" && s_event >= s_action.target) || s_boundary)
			{
				if (executing)
				{
					s_boundary = true;
					exit_execution = true;
				}
				else
				{
					capture = true;
					snapshot_id = s_action.id;
					s_boundary = false;
					s_run_target.store(0);
				}
			}
			s_status.full_tlb = EmuConfig.Cpu.EnableExperimentalEETLB;
			s_status.ee_recompiler = EmuConfig.Cpu.IsEERecompilerEnabled();
			s_status.extra_memory = EmuConfig.Cpu.ExtraMemory;
			while (s_next_input < s_inputs.size())
			{
				auto& input = s_inputs[s_next_input++];
				if (input.result.state != "queued")
					continue;
				input.result.output_offset = s_status.produced[0];
				input.result.consumed_before = s_status.rx_consumed;
				input.result.at_event = s_event;
				if (!s_status.fault.empty())
					input.result.state = "observation_fault";
				else if (!VMManager::HasValidVM())
					input.result.state = "vm_not_ready";
				else if (ee_sio_rx_fifo.size() + input.bytes.size() > 1024)
					input.result.state = "rx_fifo_full";
				else
				{
					ee_sio_rx_fifo.insert(ee_sio_rx_fifo.end(), input.bytes.begin(), input.bytes.end());
					input.result.state = "accepted";
				}
			}
			// Interpreter exits use a longjmp. A running VM must only be stopped
			// inside its active event test, never between calls to Execute().
			stop = s_stop && (executing || VMManager::GetState() != VMState::Running);
			if (stop)
			{
				s_stop = false;
				resume = false;
				capture = false;
			}
			s_needs_service.store(s_stop || s_boundary, std::memory_order_release);
		}
		if (exit_execution)
			Cpu->ExitExecution();
		if (capture)
		{
			if (VMManager::GetState() != VMState::Paused)
				VMManager::SetPaused(true);
			auto snapshot = CaptureSnapshot(snapshot_id);
			std::lock_guard lock(s_mutex);
			s_snapshot = std::move(snapshot);
			s_action.state = "done";
		}
		if (resume)
			VMManager::SetPaused(false);
		if (stop && VMManager::HasValidVM())
			VMManager::SetState(VMState::Stopping);
	}
} // namespace AgentConsole
