// SPDX-License-Identifier: GPL-3.0+
#include "common/emitter/x86emitter.h"
#include "common/BitUtils.h"
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>
#include <x86intrin.h>

using namespace x86Emitter;

namespace Phase2
{
#include "phase2-allocator-state.h"

	// VM storage and input liveness only. The XMM allocator and MMI templates
	// below are extracted verbatim, including allocation, eviction and flush.
	union alignas(16) GuestRegister
	{
		u32 UL[4];
		s32 SL[4];
		s64 SD[2];
		u64 UQ[2];
	};
	struct
	{
		struct
		{
			GuestRegister r[34];
		} GPR;
	} cpuRegs;
	union FloatRegister
	{
		u32 UL;
		float f;
	};
	struct
	{
		FloatRegister fpr[32], ACC;
	} fpuRegs;
	struct
	{
		struct
		{
			float F[4];
		} VF[32], ACC;
		struct
		{
			float F;
		} VI[32];
	} VU0;
	constexpr int REG_I = 21;
	GuestRegister g_cpuConstRegs[32];
	u32 g_cpuHasConstReg, g_cpuFlushedConstReg;
	_xmmregs xmmregs[iREGCNT_XMM_EVEX], s_saveXMMregs[iREGCNT_XMM_EVEX];
	_x86regs x86regs[iREGCNT_GPR], s_saveX86regs[iREGCNT_GPR];
	u16 g_xmmAllocCounter, g_x86AllocCounter;
	thread_local EEXMMAllocatorStats* g_eeXMMAllocatorStats = nullptr;
	EEINST instruction_info;
	EEINST* g_pCurInstInfo = &instruction_info;
	unsigned rs, rt, rd;
#define _Rs_ rs
#define _Rt_ rt
#define _Rd_ rd
#define RALOG(...)
#define GPR_IS_CONST1(reg) ((reg) < 32 && (g_cpuHasConstReg & (1u << (reg))))
#define GPR_IS_DIRTY_CONST(reg) (GPR_IS_CONST1(reg) && !(g_cpuFlushedConstReg & (1u << (reg))))
#define GPR_DEL_CONST(reg) \
	do \
	{ \
		if ((reg) < 32) \
			g_cpuHasConstReg &= ~(1u << (reg)); \
	} while (0)
#define GPR_SET_CONST(reg) \
	do \
	{ \
		if ((reg) < 32) \
		{ \
			g_cpuHasConstReg |= 1u << (reg); \
			g_cpuFlushedConstReg &= ~(1u << (reg)); \
		} \
	} while (0)
	enum class eeOpcode
	{
		PLZCW,
		PADDW,
		PSUBW,
		PXOR,
		POR,
		PNOR,
		PMFLO,
		PMFHI,
		PMTLO,
		PMTHI
	};
	namespace EE
	{
		struct
		{
			void EmitOp(eeOpcode) {}
		} Profiler;
	} // namespace EE
	void mVUFreeCOP2XMMreg(int) {}
	void _eeOnWriteReg(int reg, int) { GPR_DEL_CONST(reg); }
	void _freeX86reg(int reg) { x86regs[reg] = {}; }
	void _freeX86regWithoutWriteback(int reg) { x86regs[reg] = {}; }
	void _flushConstReg(int reg)
	{
		xMOV64(rax, g_cpuConstRegs[reg].SD[0]);
		xMOV(ptr64[&cpuRegs.GPR.r[reg].UL[0]], rax);
		g_cpuFlushedConstReg |= 1u << reg;
	}
	int _checkX86reg(int type, int reg, int mode)
	{
		for (unsigned i = 0; i < iREGCNT_GPR; i++)
			if (x86regs[i].inuse && x86regs[i].type == type && x86regs[i].reg == reg)
			{
				x86regs[i].mode |= mode;
				return i;
			}
		return -1;
	}
#include "pcsx2/x86/MMIAVX512.inl"
// recPLZCW picks the LZCNT form through cpuinfo; every validation host has LZCNT.
static bool cpuinfo_initialize() { return true; }
static bool cpuinfo_has_x86_lzcnt() { return true; }
#include "phase2-allocator-functions.h"

	static void reset()
	{
		_initXMMregs();
		std::memset(x86regs, 0, sizeof(x86regs));
		std::memset(&instruction_info, 0, sizeof(instruction_info));
		std::fill(std::begin(instruction_info.regs), std::end(instruction_info.regs), EEINST_USED | EEINST_LIVE);
		g_cpuHasConstReg = g_cpuFlushedConstReg = 0;
	}

	using Kernel = void (*)();
	struct Op
	{
		unsigned opcode, s, t, d;
	};
	static constexpr std::array generators = {recPLZCW, recPADDW, recPSUBW, recPXOR, recPOR,
		recPNOR, recPMFLO, recPMFHI, recPMTLO, recPMTHI};

	static void reference(const Op& op, std::array<GuestRegister, 34>& state)
	{
		const GuestRegister s = state[op.s], t = state[op.t];
		if (op.opcode == 8 || op.opcode == 9)
		{
			state[op.opcode == 8 ? XMMGPR_LO : XMMGPR_HI] = s;
			return;
		}
		if (op.d == 0)
			return;
		if (op.opcode == 6 || op.opcode == 7)
		{
			state[op.d] = state[op.opcode == 6 ? XMMGPR_LO : XMMGPR_HI];
			return;
		}
		for (int lane = 0; lane < 4; lane++)
		{
			const u32 a = s.UL[lane], b = t.UL[lane];
			switch (op.opcode)
			{
				case 0:
					if (lane < 2)
						state[op.d].UL[lane] = std::countl_zero(a ^ ((a & 0x80000000u) ? 0xffffffffu : 0)) - 1;
					break;
				case 1:
					state[op.d].UL[lane] = a + b;
					break;
				case 2:
					state[op.d].UL[lane] = a - b;
					break;
				case 3:
					state[op.d].UL[lane] = a ^ b;
					break;
				case 4:
					state[op.d].UL[lane] = a | b;
					break;
				case 5:
					state[op.d].UL[lane] = ~(a | b);
					break;
			}
		}
	}

	static Kernel block(const std::vector<Op>& ops, int backend, size_t& bytes, EEXMMAllocatorStats& stats)
	{
		const auto saved_features = avx512;
		if (backend == 0)
			avx512 = {};
		reset();
		stats = {};
		g_eeXMMAllocatorStats = &stats;
		xAlignPtr(64);
		Kernel kernel = reinterpret_cast<Kernel>(xGetPtr());
		{
			xScopedStackFrame frame(false);
			// Preload a full low bank to reproduce pressure from resident guests.
			for (int reg = 1; reg <= 16; reg++)
				_allocGPRtoXMMreg(reg, MODE_READ);
			_clearNeededXMMregs();
			for (const Op& op : ops)
			{
				rs = op.s;
				rt = op.t;
				rd = op.d;
				if (backend == 1 && op.opcode == 0)
					recPLZCWPhase1();
				else if (backend == 2 && op.opcode == 0)
					recPLZCWPhase2();
				else
					generators[op.opcode]();
				_clearNeededXMMregs(); // legacy PLZCW does not call this itself
			}
			_flushXMMregs();
		}
		xRET();
		bytes = xGetPtr() - reinterpret_cast<u8*>(kernel);
		avx512 = saved_features;
		g_eeXMMAllocatorStats = nullptr;
		return kernel;
	}

	static int highIDsAndCalls(const void* exact_helper)
	{
		alignas(16) static std::array<GuestRegister, 16> sentinels{}, result{};
		static u32 windows_entry_alignment;
		for (int id = 0; id < 16; id++)
			for (int lane = 0; lane < 4; lane++)
				sentinels[id].UL[lane] = 0x51200000u + id * 16 + lane;
		// This generated helper deliberately destroys every high register.
		xAlignPtr(64);
		const void* clobber = xGetPtr();
		for (int id = 16; id < 32; id++)
			xVPTERNLOGD(xRegisterSSE(id), xRegisterSSE(id), xRegisterSSE(id), 0);
		xRET();
		xAlignPtr(64);
		const void* windows_clobber = xGetPtr();
		xMOV(rax, rsp);
		xAND(eax, 15);
		xMOV(ptr32[&windows_entry_alignment], eax);
		for (int offset = 8; offset <= 32; offset += 8)
			xMOV(ptr64[rsp + offset], rax);
		for (int id = 16; id < 32; id++)
			xVPTERNLOGD(xRegisterSSE(id), xRegisterSSE(id), xRegisterSSE(id), 0);
		xRET();
		for (int call_case = 0; call_case < 3; call_case++)
		{
			const void* helper = call_case == 1 ? exact_helper : call_case == 2 ? windows_clobber :
			                                                                      clobber;
			reset();
			xAlignPtr(64);
			Kernel kernel = reinterpret_cast<Kernel>(xGetPtr());
			{
				xScopedStackFrame frame(false);
				for (int id = 16; id < 32; id++)
				{
					if (_allocTempXMMregEVEX(XMMT_INT, 1u << id) != id)
						return 1;
					xVMOVDQA32(xRegisterSSE(id), ptr128[&sentinels[id - 16]]);
				}
				xMOV(eax, 0x3fc12345u);
				xMOV(edx, 0x3fa54321u);
				if (call_case == 2)
					eeCallWithWindowsHighXMM(helper);
				else
					_eeCallWithHighXMM(helper);
				for (int id = 16; id < 32; id++)
				{
					xVMOVDQA32(ptr128[&result[id - 16]], xRegisterSSE(id));
					_freeXMMreg(id);
				}
				_clearNeededXMMregs();
			}
			xRET();
			kernel();
			if (std::memcmp(sentinels.data(), result.data(), sizeof(result)))
				return 1;
			if (call_case == 2 && windows_entry_alignment != 8)
				return 1;
		}
		// All IDs are obtained by the real allocator and consumed by the production
		// PLZCW kernel; guest source/destination alias and upper half preservation.
		std::mt19937 random(0x5120016);
		for (int id = 16; id < 32; id++)
			for (bool alias : {false, true})
			{
				reset();
				xAlignPtr(64);
				Kernel kernel = reinterpret_cast<Kernel>(xGetPtr());
				const int scratch = _allocTempXMMregEVEX(XMMT_INT, 1u << id);
				xMOVDQA(xmm0, ptr128[&sentinels[0]]);
				xMOVDQA(xmm1, ptr128[&sentinels[1]]);
				emitPLZCWAVX512(alias ? xmm0 : xmm1, xmm0, xRegisterSSE(scratch), xmm2);
				xMOVDQA(ptr128[&result[0]], alias ? xmm0 : xmm1);
				_freeXMMreg(scratch);
				_clearNeededXMMregs();
				xRET();
				for (int n = 0; n < 1000; n++)
				{
					for (auto& reg : sentinels)
						for (u32& lane : reg.UL)
							lane = random();
					const auto old = alias ? sentinels[0] : sentinels[1];
					kernel();
					for (int lane = 0; lane < 4; lane++)
					{
						const u32 a = sentinels[0].UL[lane];
						const u32 expected = lane < 2 ? std::countl_zero(a ^ ((a & 0x80000000u) ? 0xffffffffu : 0)) - 1 : old.UL[lane];
						if (result[0].UL[lane] != expected)
							return 1;
					}
				}
			}
		std::puts("PASS Phase2 real allocator: XMM16-31 PLZCW, 32000 alias vectors; all high values live across destructive and production exact helpers");
		return 0;
	}

	int run(const void* exact_helper)
	{
		if (highIDsAndCalls(exact_helper))
			return 1;
		std::mt19937 random(0x590032);
		std::array<GuestRegister, 34> initial{}, expected{};
		// Generated-byte fingerprint per backend. Compare the AVX2 value across
		// source revisions to prove an unchanged AVX2 code generator.
		u64 code_hash[3] = {0xcbf29ce484222325ull, 0xcbf29ce484222325ull, 0xcbf29ce484222325ull};
		// Optional raw AVX2 code dump for a disassembly diff between revisions.
		FILE* code_dump = std::getenv("PHASE2_AVX2_CODE_DUMP") ? std::fopen(std::getenv("PHASE2_AVX2_CODE_DUMP"), "wb") : nullptr;
		for (int n = 0; n < 256; n++)
		{
			std::vector<Op> ops;
			// A full low bank with a cached Rd makes the first PLZCW use high scratch.
			ops.push_back({0, 1, 2, 3});
			for (int i = 1; i < 64; i++)
				ops.push_back({static_cast<unsigned>(random() % generators.size()), static_cast<unsigned>(random() % 32), static_cast<unsigned>(random() % 32), static_cast<unsigned>(random() % 32)});
			size_t bytes[3];
			EEXMMAllocatorStats stats[3];
			const Kernel kernels[] = {block(ops, 0, bytes[0], stats[0]), block(ops, 1, bytes[1], stats[1]), block(ops, 2, bytes[2], stats[2])};
			for (int backend = 0; backend < 3; backend++)
			{
				const u8* code_bytes = reinterpret_cast<const u8*>(kernels[backend]);
				for (size_t i = 0; i < bytes[backend]; i++)
					code_hash[backend] = (code_hash[backend] ^ code_bytes[i]) * 0x100000001b3ull;
				if (backend == 0 && code_dump)
					std::fwrite(code_bytes, 1, bytes[backend], code_dump);
			}
			for (int trial = 0; trial < 64; trial++)
			{
				for (auto& reg : initial)
					for (u32& lane : reg.UL)
						lane = random();
				initial[0] = {};
				expected = initial;
				for (const Op& op : ops)
					reference(op, expected);
				for (Kernel kernel : kernels)
				{
					std::memcpy(cpuRegs.GPR.r, initial.data(), sizeof(initial));
					kernel();
					if (std::memcmp(cpuRegs.GPR.r, expected.data(), sizeof(expected)))
					{
						std::printf("FAIL Phase2 random block=%d trial=%d seed=0x590032\n", n, trial);
						return 1;
					}
				}
			}
			if (n == 0)
				for (int backend = 0; backend < 3; backend++)
				{
					unsigned aux;
					const auto start = std::chrono::steady_clock::now();
					const u64 cycles = __rdtscp(&aux);
					for (int run = 0; run < 100000; run++)
						kernels[backend]();
					const u64 end_cycles = __rdtscp(&aux);
					const auto duration = std::chrono::steady_clock::now() - start;
					std::printf("BENCH Phase2 synthetic pressure block backend=%s bytes=%zu tsc_ticks/block=%.3f ns/block=%.3f\n",
						backend == 0 ? "AVX2" : backend == 1 ? "AVX512-Phase1-policy" :
															   "AVX512-Phase2",
						bytes[backend], (end_cycles - cycles) / 100000.0,
						std::chrono::duration<double, std::nano>(duration).count() / 100000);
					std::printf("COUNTERS reloads=%llu writebacks=%llu evictions=%llu temps=%llu high_temps=%llu helper_spills=%llu\n",
						static_cast<unsigned long long>(stats[backend].guest_reloads), static_cast<unsigned long long>(stats[backend].guest_writebacks),
						static_cast<unsigned long long>(stats[backend].evictions), static_cast<unsigned long long>(stats[backend].temporaries),
						static_cast<unsigned long long>(stats[backend].high_temporaries), static_cast<unsigned long long>(stats[backend].helper_stack_spills));
				}
		}
		if (code_dump)
			std::fclose(code_dump);
		std::printf("CODEHASH Phase2 256 blocks AVX2=%016llx AVX512-Phase1-policy=%016llx AVX512-Phase2=%016llx\n",
			static_cast<unsigned long long>(code_hash[0]), static_cast<unsigned long long>(code_hash[1]),
			static_cast<unsigned long long>(code_hash[2]));
		reset();
		std::puts("PASS Phase2 256 real allocator MMI blocks x 64 inputs x 3 backends, 64 opcodes/block, independent GPR/LO/HI reference");
		return 0;
	}
} // namespace Phase2

int TestPhase2Allocator(const void* exact_helper) { return Phase2::run(exact_helper); }
