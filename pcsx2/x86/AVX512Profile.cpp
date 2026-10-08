// SPDX-FileCopyrightText: 2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "AVX512Profile.h"

#include "common/Console.h"
#include "common/FileSystem.h"
#include "common/Path.h"

#include <cstdio>
#include <cstdlib>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <tuple>

namespace AVX512Profile
{
	static constexpr u32 COUNTER_CAPACITY = 1u << 20;

	// Index 0..SoftCounterCount-1 are SoftFloat counters; blocks follow.
	alignas(64) static u64 s_counters[COUNTER_CAPACITY];
	static u32 s_next_counter = SoftCounterCount;
	static std::mutex s_mutex;
	static std::deque<EEBlock> s_ee_blocks;
	static std::deque<VUBlock> s_vu_blocks;
	// (unit, opcode name, block counter) -> static occurrences in that compiled block.
	static std::map<std::tuple<char, std::string, u32>, u32> s_ops;

	static const std::string& OutputDirectory()
	{
		static const std::string dir = [] {
			const char* env = std::getenv("PCSX2_AVX512_PROFILE");
			return std::string(env ? env : "");
		}();
		return dir;
	}

	bool Enabled()
	{
		return !OutputDirectory().empty();
	}

	static bool AllocateCounter(u32* index)
	{
		if (s_next_counter >= COUNTER_CAPACITY)
			return false;
		*index = s_next_counter++;
		s_counters[*index] = 0;
		return true;
	}

	void CountOp(u32 counter, char unit, const char* name)
	{
		std::lock_guard lock(s_mutex);
		s_ops[{unit, name, counter}]++;
	}

	EEBlock* NewEEBlock(u32 startpc)
	{
		if (!Enabled())
			return nullptr;
		std::lock_guard lock(s_mutex);
		EEBlock block = {};
		block.startpc = startpc;
		if (!AllocateCounter(&block.counter))
			return nullptr;
		return &s_ee_blocks.emplace_back(block);
	}

	VUBlock* NewVUBlock(u32 vu, u32 startpc)
	{
		if (!Enabled())
			return nullptr;
		std::lock_guard lock(s_mutex);
		VUBlock block = {};
		block.vu = vu;
		block.startpc = startpc;
		if (!AllocateCounter(&block.counter))
			return nullptr;
		return &s_vu_blocks.emplace_back(block);
	}

	u64* Counter(u32 index)
	{
		return &s_counters[index];
	}

	u64* SoftCounterPtr(SoftCounter counter)
	{
		return &s_counters[counter];
	}

	static std::FILE* OpenOutput(const char* name)
	{
		const std::string path = Path::Combine(OutputDirectory(), name);
		std::FILE* fp = FileSystem::OpenCFile(path.c_str(), "wb");
		if (!fp)
			Console.ErrorFmt("AVX512Profile: failed to open {}", path);
		return fp;
	}

	void Dump()
	{
		if (!Enabled())
			return;
		std::lock_guard lock(s_mutex);
		FileSystem::CreateDirectoryPath(OutputDirectory().c_str(), true);

		// A guest block can be recompiled several times. Execution counts are
		// summed; per-compile allocator events are weighted by each compile's runs.
		struct EETotals
		{
			u64 exec = 0, compiles = 0, reloads = 0, writebacks = 0, evictions = 0, temporaries = 0, high_temporaries = 0, helper_spills = 0;
		};
		std::map<u32, EETotals> ee;
		u64 ee_exec_total = 0, ee_reload_total = 0, ee_writeback_total = 0, ee_eviction_total = 0;
		for (const EEBlock& block : s_ee_blocks)
		{
			const u64 exec = s_counters[block.counter];
			EETotals& t = ee[block.startpc];
			t.exec += exec;
			t.compiles++;
			t.reloads += exec * block.reloads;
			t.writebacks += exec * block.writebacks;
			t.evictions += exec * block.evictions;
			t.temporaries += exec * block.temporaries;
			t.high_temporaries += exec * block.high_temporaries;
			t.helper_spills += exec * block.helper_spills;
			ee_exec_total += exec;
			ee_reload_total += exec * block.reloads;
			ee_writeback_total += exec * block.writebacks;
			ee_eviction_total += exec * block.evictions;
		}
		if (std::FILE* fp = OpenOutput("ee_blocks.csv"))
		{
			std::fprintf(fp, "startpc,compiles,exec,weighted_reloads,weighted_writebacks,weighted_evictions,weighted_temporaries,weighted_high_temporaries,weighted_helper_spills\n");
			for (const auto& [pc, t] : ee)
				std::fprintf(fp, "%08x,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu\n", pc, (unsigned long long)t.compiles,
					(unsigned long long)t.exec, (unsigned long long)t.reloads, (unsigned long long)t.writebacks,
					(unsigned long long)t.evictions, (unsigned long long)t.temporaries,
					(unsigned long long)t.high_temporaries, (unsigned long long)t.helper_spills);
			std::fclose(fp);
		}

		struct VUTotals
		{
			u64 exec = 0, compiles = 0, vf_loads = 0, vf_writebacks = 0, vf_high_loads = 0;
			u64 first = 0, flushed = 0, lost = 0, partial = 0, replaced = 0, refused = 0;
			u64 home_loads = 0, home_refills = 0, home_spills = 0, home_only_writebacks = 0;
		};
		u64 vf_requests[2][33] = {}, vf_memory_loads[2][33] = {};
		std::map<std::tuple<u32, u32>, VUTotals> vu;
		u64 vu_exec_total[2] = {}, vu_load_total[2] = {}, vu_writeback_total[2] = {};
		for (const VUBlock& block : s_vu_blocks)
		{
			const u64 exec = s_counters[block.counter];
			VUTotals& t = vu[{block.vu, block.startpc}];
			t.exec += exec;
			t.compiles++;
			t.vf_loads += exec * block.vf_loads;
			t.vf_writebacks += exec * block.vf_writebacks;
			t.vf_high_loads += exec * block.vf_high_loads;
			t.first += exec * block.vf_first_loads;
			t.flushed += exec * block.vf_flushed_reloads;
			t.lost += exec * block.vf_lost_reloads;
			t.partial += exec * block.vf_partial_reloads;
			t.replaced += exec * block.vf_copy_replaced;
			t.refused += exec * block.vf_copy_refused;
			t.home_loads += exec * block.vf_home_loads;
			t.home_refills += exec * block.vf_home_refills;
			t.home_spills += exec * block.vf_home_spills;
			t.home_only_writebacks += exec * block.vf_home_only_writebacks;
			for (int r = 0; r < 33; r++)
			{
				vf_requests[block.vu & 1][r] += exec * block.vf_requests_by_reg[r];
				vf_memory_loads[block.vu & 1][r] += exec * block.vf_memory_loads_by_reg[r];
			}
			vu_exec_total[block.vu & 1] += exec;
			vu_load_total[block.vu & 1] += exec * block.vf_loads;
			vu_writeback_total[block.vu & 1] += exec * block.vf_writebacks;
		}
		if (std::FILE* fp = OpenOutput("vu_blocks.csv"))
		{
			std::fprintf(fp, "vu,startpc,compiles,exec,weighted_vf_loads,weighted_vf_writebacks,weighted_vf_high_loads,"
				"weighted_first_loads,weighted_flushed_reloads,weighted_lost_reloads,weighted_partial_reloads,"
				"weighted_copy_replaced,weighted_copy_refused,weighted_home_loads,weighted_home_refills,"
				"weighted_home_spills,weighted_home_only_writebacks\n");
			for (const auto& [key, t] : vu)
				std::fprintf(fp, "%u,%04x,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu\n", std::get<0>(key),
					std::get<1>(key), (unsigned long long)t.compiles, (unsigned long long)t.exec,
					(unsigned long long)t.vf_loads, (unsigned long long)t.vf_writebacks,
					(unsigned long long)t.vf_high_loads, (unsigned long long)t.first, (unsigned long long)t.flushed,
					(unsigned long long)t.lost, (unsigned long long)t.partial, (unsigned long long)t.replaced,
					(unsigned long long)t.refused, (unsigned long long)t.home_loads,
					(unsigned long long)t.home_refills, (unsigned long long)t.home_spills,
					(unsigned long long)t.home_only_writebacks);
			std::fclose(fp);
		}
		if (std::FILE* fp = OpenOutput("vu_vf.csv"))
		{
			std::fprintf(fp, "vu,vf,weighted_requests,weighted_memory_loads\n");
			for (u32 v = 0; v < 2; v++)
			{
				for (u32 r = 1; r < 33; r++)
					std::fprintf(fp, "%u,%u,%llu,%llu\n", v, r, (unsigned long long)vf_requests[v][r],
						(unsigned long long)vf_memory_loads[v][r]);
			}
			std::fclose(fp);
		}

		static constexpr const char* soft_names[SoftCounterCount] = {"vu_addsub_stackless_done",
			"vu_addsub_stackless_fallback", "vu_addsub_flag_path", "vu_addsub_repair", "vu_mul", "vu_madd",
			"vu_addsub_mxcsr_path", "vu_mul_stackless_done", "vu_mul_fast_done", "vu_mul_booth_call",
			"vu_mul_exact_vector_call", "vu_madd_stackless_done", "vu_madd_compact_done", "vu_madd_register_done",
			"vu_madd_exact_vector_call", "vu_madd_packed_call", "vu_madd_lane_call",
			"vu_mul_fail_input", "vu_mul_fail_result", "vu_mul_fail_corrected", "vu_mul_fail_table", "vu_madd_fail_acc_overflow", "vu_madd_fail_input", "vu_madd_fail_product", "vu_madd_fail_corrected", "vu_madd_fail_acc_input", "vu_madd_fail_result",
			"vu0_program_runs", "vu1_program_runs"};
		if (std::FILE* fp = OpenOutput("softfloat.csv"))
		{
			std::fprintf(fp, "counter,count\n");
			for (u32 i = 0; i < SoftCounterCount; i++)
				std::fprintf(fp, "%s,%llu\n", soft_names[i], (unsigned long long)s_counters[i]);
			std::fclose(fp);
		}

		// Opcode execution estimate: static count per block times block runs
		// (early block exits are not subtracted).
		std::map<std::tuple<char, std::string>, u64> ops;
		for (const auto& [key, count] : s_ops)
			ops[{std::get<0>(key), std::get<1>(key)}] += count * s_counters[std::get<2>(key)];
		if (std::FILE* fp = OpenOutput("ops.csv"))
		{
			std::fprintf(fp, "unit,op,weighted_exec\n");
			for (const auto& [key, exec] : ops)
				std::fprintf(fp, "%c,%s,%llu\n", std::get<0>(key), std::get<1>(key).c_str(), (unsigned long long)exec);
			std::fclose(fp);
		}

		Console.WriteLnFmt("AVX512Profile: EE block runs {} reloads {} writebacks {} evictions {}; "
						"VU0 block runs {} VF loads {} writebacks {}; VU1 block runs {} VF loads {} writebacks {}",
			ee_exec_total, ee_reload_total, ee_writeback_total, ee_eviction_total, vu_exec_total[0],
			vu_load_total[0], vu_writeback_total[0], vu_exec_total[1], vu_load_total[1], vu_writeback_total[1]);

		// Generated code that references these counters is discarded with the VM.
		s_ee_blocks.clear();
		s_ops.clear();
		s_vu_blocks.clear();
		std::fill(std::begin(s_counters), std::end(s_counters), 0);
		s_next_counter = SoftCounterCount;
	}
} // namespace AVX512Profile
