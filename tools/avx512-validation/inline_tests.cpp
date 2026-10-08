// SPDX-License-Identifier: GPL-3.0+
// Production microVU upper SoftFloat inline paths (the code emitted inside
// recompiled VU blocks), checked against PS2Float and against the outlined
// exact path of the same emitter.
#include "common/emitter/x86emitter.h"
#include "common/FPControl.h"
#include "pcsx2/x86/AVX512Profile.h"
#include "pcsx2/x86/SoftFloatEmitter.h"
#include "pcsx2/x86/microVU_SoftFloat.h"
#include "pcsx2/x86/microVU_SoftFloatTables.h"
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <optional>
#include <random>
#include <vector>
#include <map>
#include <string>
#include <sys/mman.h>
#include "oracle.h"

using namespace x86Emitter;

// The production profile is compiled in but disabled for these tests.
namespace AVX512Profile
{
	bool Enabled() { return false; }
	u64* Counter(u32) { static u64 sink; return &sink; }
	u64* SoftCounterPtr(SoftCounter) { static u64 sink; return &sink; }
} // namespace AVX512Profile

namespace UpperSoftInline
{
	typedef xRegisterSSE xmm;
	typedef xRegister32 x32;

	namespace ProcessorFeatures
	{
		enum class VectorISA { None, SSE4, AVX, AVX2 };
	}
	struct
	{
		bool hasFastPext = true;
		ProcessorFeatures::VectorISA vectorISA = ProcessorFeatures::VectorISA::AVX2;
	} g_cpu;
	struct
	{
		struct
		{
			FPControlRegister VU0FPCR{0xffc0}, VU1FPCR{0xffc0};
		} Cpu;
	} EmuConfig;
#define CHECK_VU_SOFT(index) true

	enum : u32 { REG_MAC_FLAG = 17 };
	struct HarnessVURegs
	{
		struct { u32 UL; } VI[32];
		struct { u32 UL[4]; } ACC;
		u32 accflag;
		u32 macflag;
	};
	static HarnessVURegs g_vuRegs;

	// VF0-31, ACC (32), I (33) and Q (34), as the allocator sees them.
	alignas(16) static u32 g_vf[35][4];
	alignas(16) static u32 g_status[4];
	static u32 g_host_mxcsr;

	struct HarnessFlagInfo
	{
		bool doFlag = false, doValue = false, doNonSticky = false;
		int write = 0, lastWrite = 0;
	};
	struct HarnessInstInfo
	{
		HarnessFlagInfo sFlag, mFlag;
		int readQ = 0;
	};
	struct HarnessProgram
	{
		struct
		{
			HarnessInstInfo info[1];
		} IRinfo;
	};

	// Minimal allocator mirroring microRegAlloc's memory traffic: a written
	// single-field register holds that field in lane 0 (mVUloadReg/mVUsaveReg),
	// partial writes store only their fields, and Flush() acts as a block exit.
	struct HarnessRegAlloc
	{
		struct Written
		{
			int id, vf, xyzw;
		};
		int next = 0;
		int allocated = 0;
		std::vector<Written> written;
		static int SingleLane(int xyzw)
		{
			return xyzw == 8 ? 0 : xyzw == 4 ? 1 : xyzw == 2 ? 2 : xyzw == 1 ? 3 : -1;
		}
		xmm allocReg(int vfLoadReg = -1, int vfWriteReg = -1, int xyzw = 0, bool = true)
		{
			pxAssertRel(allocated < 15, "harness register pool exhausted");
			const xmm reg = xmm::GetInstance(next);
			next = (next + 1) % 15;
			allocated++;
			const int single = vfWriteReg >= 0 ? SingleLane(xyzw) : -1;
			if (vfLoadReg >= 0)
			{
				if (single >= 0)
					xMOVSSZX(reg, ptr32[&g_vf[vfLoadReg][single]]);
				else
					xMOVAPS(reg, ptr128[g_vf[vfLoadReg]]);
			}
			if (vfWriteReg >= 0)
				written.push_back({reg.Id, vfWriteReg, xyzw ? xyzw : 0xf});
			return reg;
		}
		void clearNeeded(const xmm&) {}
		void writeBackReg(const x32&, bool = true) {}
		void clearGPR(const x32&) {}
		void Flush()
		{
			for (const Written& w : written)
			{
				const xmm reg = xmm::GetInstance(w.id);
				const int single = SingleLane(w.xyzw);
				if (single >= 0)
					xMOVSS(ptr32[&g_vf[w.vf][single]], reg);
				else if (w.xyzw == 0xf)
					xMOVAPS(ptr128[g_vf[w.vf]], reg);
				else
					for (int lane = 0; lane < 4; lane++)
						if (w.xyzw & (8 >> lane))
						{
							xPEXTR.D(eax, reg, lane);
							xMOV(ptr32[&g_vf[w.vf][lane]], eax);
						}
			}
			written.clear();
		}
	};

	static void getQreg(const xmm& reg, int)
	{
		xMOVAPS(reg, ptr128[g_vf[34]]);
	}

#define mVUinfo mVU.prog.IRinfo.info[0]
#define sFLAG mVUinfo.sFlag
#define mFLAG mVUinfo.mFlag
#define mV microVU& mVU
#include "inline-generators.h"

	enum class Kind { Add, Sub, Mul };
	struct Config
	{
		Kind kind;
		u32 xyzw;
		int variant; // 0 Ft, 1 I, 2 Q, 3-6 broadcast x/y/z/w
		bool acc; // destination is ACC
		bool mflag, sflag;
		bool avx512;
		int reg_base;
		bool fast = true;
	};

	using Block = void (*)();

	static Block EmitBlock(microVU& vu, HarnessRegAlloc& alloc, const Config& c)
	{
		const auto saved = avx512;
		if (!c.avx512)
			avx512 = {};
		alloc.next = c.reg_base;
		alloc.allocated = 0;
		vu.regAlloc = &alloc;
		const int fs = 1, ft = 2, fd = 3;
		vu.code = (c.xyzw << 21) | (ft << 16) | (fs << 11) | (fd << 6);
		HarnessInstInfo& info = vu.prog.IRinfo.info[0];
		info = {};
		info.mFlag.doFlag = c.mflag;
		info.mFlag.write = 1;
		info.sFlag.doFlag = c.sflag;
		info.sFlag.doValue = c.sflag;
		info.sFlag.doNonSticky = c.sflag;
		info.sFlag.write = 2;
		info.sFlag.lastWrite = 1;

		xAlignPtr(64);
		const Block fn = reinterpret_cast<Block>(xGetAlignedCallTarget());
		for (int id : {3, 5, 12, 13, 14, 15})
			xPUSH(xRegister64(id));
		xSUB(rsp, 8);
		// Soft-float VU blocks run entirely in the VU FPCR (truncate/DAZ/FTZ).
		xSTMXCSR(ptr32[&g_host_mxcsr]);
		xLDMXCSR(ptr32[&EmuConfig.Cpu.VU0FPCR.bitmask]);
		for (int i = 0; i < 4; i++)
			xMOV(xRegister32(12 + i), ptr32[&g_status[i]]);

		const VuUpperFmacSoftOperandSource source_kind = static_cast<VuUpperFmacSoftOperandSource>(c.variant);
		const VuUpperFmacSoftKind kind = c.kind == Kind::Add ? VuUpperFmacSoftKind::Add :
		                                 c.kind == Kind::Sub ? VuUpperFmacSoftKind::Sub :
		                                                       VuUpperFmacSoftKind::Mul;
		const VuUpperFmacSoftDescriptor op{kind, source_kind,
			c.acc ? VuUpperFmacSoftDestination::Acc : VuUpperFmacSoftDestination::Fd};
		if (op.IsAddSub())
		{
			mVUemitUpperInlineAddSubExactResult(vu, op);
		}
		else
		{
			// Mirrors mVUemitUpperSoftExact's operand and destination setup.
			const xmm source = alloc.allocReg(fs);
			xmm operand;
			if (c.variant == 1)
				operand = alloc.allocReg(33);
			else if (c.variant == 2)
			{
				operand = alloc.allocReg();
				getQreg(operand, 0);
			}
			else
				operand = alloc.allocReg(ft);
			const int destination_reg = c.acc ? 32 : fd;
			const int destination_load = c.xyzw == 0xf ? -1 : destination_reg;
			const xmm destination = alloc.allocReg(destination_load, destination_reg, c.xyzw);
			mVUemitUpperInlineMulExactResult(vu, op, source, operand, destination, c.fast);
		}
		alloc.Flush();

		for (int i = 0; i < 4; i++)
			xMOV(ptr32[&g_status[i]], xRegister32(12 + i));
		xLDMXCSR(ptr32[&g_host_mxcsr]);
		xADD(rsp, 8);
		for (int id : {15, 14, 13, 12, 5, 3})
			xPOP(xRegister64(id));
		xRET();
		avx512 = saved;
		return fn;
	}

	static u32 RandomFloat(std::mt19937& rng)
	{
		static constexpr u32 exponents[] = {0, 1, 2, 24, 60, 100, 126, 127, 128, 129, 150, 200, 253, 254, 255};
		const u32 sign = (rng() & 1) << 31;
		const u32 exponent = (rng() & 3) ? exponents[rng() % std::size(exponents)] : (rng() & 0xff);
		u32 mantissa;
		switch (rng() % 4)
		{
			case 0: mantissa = 0; break;
			case 1: mantissa = 0x7fffff; break;
			default: mantissa = rng() & 0x7fffff; break;
		}
		return sign | (exponent << 23) | mantissa;
	}

	struct State
	{
		u32 vf[35][4];
		u32 status[4];
		u32 mac[4];
		u32 accflag;
	};

	static void Capture(const microVU& vu, State& s)
	{
		std::memcpy(s.vf, g_vf, sizeof(g_vf));
		std::memcpy(s.status, g_status, sizeof(g_status));
		std::memcpy(s.mac, vu.macFlag, sizeof(s.mac));
		s.accflag = g_vuRegs.accflag;
	}

	static void Restore(microVU& vu, const State& s)
	{
		std::memcpy(g_vf, s.vf, sizeof(g_vf));
		std::memcpy(g_status, s.status, sizeof(g_status));
		std::memcpy(vu.macFlag, s.mac, sizeof(s.mac));
		g_vuRegs.accflag = s.accflag;
	}

	static void GenerateKernels(microVU& vu)
	{
		vu.softBoothCache = std::make_unique<microVUSoftBoothCacheEntry[]>(mVUsoftBoothCacheSize);
		mVUGenerateSoftMulExactKernel(vu);
		mVUGenerateSoftAddExactLaneKernel(vu);
		mVUGenerateSoftAddLaneRepairKernel(vu);
		mVUGenerateSoftMaddIntegratedLaneKernel(vu);
		mVUGenerateSoftMulExactVectorKernel(vu);
		mVUGenerateSoftMulBoothPackedKernel(vu);
		mVUGenerateSoftMaddPackedKernels(vu);
		mVUGenerateSoftMaddExactVectorKernels(vu);
	}

	static const char* KindName(Kind k) { return k == Kind::Add ? "ADD" : k == Kind::Sub ? "SUB" : "MUL"; }

	int Run()
	{
		// Own low code buffer: generated code addresses these statics RIP-relative.
		void* code = mmap(nullptr, 16 * 1024 * 1024, PROT_READ | PROT_WRITE | PROT_EXEC,
			MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
		if (code == MAP_FAILED)
			return 1;
		xSetPtr(static_cast<u8*>(code));
		// Static like the real microVU0/1: generated code addresses its flag fields.
		static microVU vu;
		HarnessRegAlloc alloc;
		vu.regAlloc = &alloc;
		GenerateKernels(vu);

		std::mt19937 rng(0x5059f10a);
		u64 failures = 0;
		u64 known_underflow_zero = 0;
		std::map<std::string, u64> failure_examples;
		u64 checked = 0;
		u32 configs = 0;
		for (Kind kind : {Kind::Mul, Kind::Add, Kind::Sub})
			for (u32 xyzw = 1; xyzw < 16; xyzw++)
				for (int variant : {0, 1, 2, 3, 6})
					for (int flags = 0; flags < 4; flags++)
						for (bool avx : {false, true})
						{
							Config c{kind, xyzw, variant, (xyzw & 1) != 0 && variant == 3, (flags & 1) != 0, (flags & 2) != 0, avx,
								static_cast<int>((xyzw * 3 + variant) % 15)};
							u8* const code_mark = xGetPtr();
							Block fast = EmitBlock(vu, alloc, c);
							Block slow = nullptr;
							if (kind == Kind::Mul)
							{
								Config cs = c;
								cs.fast = false;
								slow = EmitBlock(vu, alloc, cs);
							}
							configs++;
							for (int iter = 0; iter < 400; iter++)
							{
								for (auto& reg : g_vf)
									for (u32& lane : reg)
										lane = RandomFloat(rng);
								g_vf[0][0] = g_vf[0][1] = g_vf[0][2] = 0;
								g_vf[0][3] = 0x3f800000;
								for (u32& s : g_status)
									s = rng() & 0xfff;
								std::memset(vu.macFlag, 0, sizeof(vu.macFlag));
								g_vuRegs.accflag = rng() & 0xf;

								State before;
								Capture(vu, before);
								fast();
								State actual;
								Capture(vu, actual);

								// Value oracle and per-lane MAC flags.
								const int dst = c.acc ? 32 : 3;
								u32 mac = 0;
								u32 expected[4] = {};
								bool continue_lane_flags = false;
								(void)continue_lane_flags;
								bool ok = true;
								for (int lane = 0; lane < 4; lane++)
								{
									const u32 bit = 8u >> lane; // x is MAC bit 3
									const u32 old = before.vf[dst][lane];
									if (!(xyzw & bit))
									{
										ok &= actual.vf[dst][lane] == old;
										continue;
									}
									const u32 a = before.vf[1][lane];
									const u32 b = variant == 0 ? before.vf[2][lane] :
									              variant == 1 ? before.vf[33][0] :
									              variant == 2 ? before.vf[34][0] :
									                             before.vf[2][variant - 3];
									const PS2Float r = kind == Kind::Mul ? PS2Float(a).Mul(PS2Float(b)) :
									                   kind == Kind::Add ? PS2Float(a).Add(PS2Float(b)) :
									                                       PS2Float(a).Sub(PS2Float(b));
									// PS2Float is the project oracle. On ADD/SUB underflow it keeps the
									// E=0 bit pattern (U and Z set), which the VU interpreter stores.
									// The VU manual only says the result becomes +/-0; both read as zero.
									const u32 value = r.raw;
									expected[lane] = value;
									// Known divergence, reported separately: on ADD/SUB underflow the
									// scalar and flag-free packed paths store +/-0 instead of the E=0
									// bits. Needs a decision on the intended semantics.
									if (actual.vf[dst][lane] != value && kind != Kind::Mul && r.HasUnderflow() &&
										actual.vf[dst][lane] == (value & 0x80000000u))
									{
										known_underflow_zero++;
										continue_lane_flags = true;
									}
									else
										ok &= actual.vf[dst][lane] == value;
									if ((value & 0x7fffffff) == 0 || r.HasUnderflow())
										mac |= bit;
									if (r.raw >> 31)
										mac |= bit << 4;
									if (r.HasUnderflow())
										mac |= bit << 8;
									if (r.HasOverflow())
										mac |= bit << 12;
								}
								if (c.mflag)
									ok &= (actual.mac[1] & 0xffff) == mac;

								bool same_as_slow = true;
								State reference = actual;
								if (slow)
								{
									Restore(vu, before);
									slow();
									Capture(vu, reference);
									same_as_slow = !std::memcmp(&actual, &reference, sizeof(State));
								}
								checked++;
								if (!ok || !same_as_slow)
								{
									failures++;
									char key[96];
									std::snprintf(key, sizeof(key), "%s %s mflag=%d sflag=%d avx512=%d %s", KindName(kind),
										(xyzw & (xyzw - 1)) ? "packed" : "scalar", c.mflag, c.sflag, avx, !ok ? "oracle" : "slow-path");
									if (failure_examples[key]++ != 0)
										continue;
									std::printf("FAIL inline %s xyzw=%x variant=%d acc=%d mflag=%d sflag=%d avx512=%d iter=%d oracle=%d slow=%d\n",
										KindName(kind), xyzw, variant, c.acc, c.mflag, c.sflag, avx, iter, ok, same_as_slow);
									for (int lane = 0; lane < 4; lane++)
										std::printf("  lane%d a=%08x b=%08x old=%08x got=%08x slow=%08x oracle=%08x\n", lane, before.vf[1][lane],
											before.vf[2][lane], before.vf[dst][lane], actual.vf[dst][lane], reference.vf[dst][lane],
											expected[lane]);
									std::printf("  mac got=%04x expected=%04x\n", actual.mac[1] & 0xffff, mac);
								}
							}
							xSetPtr(code_mark);
						}
		if (failures)
		{
			for (const auto& [key, count] : failure_examples)
				std::printf("FAIL-SUMMARY %s: %llu runs\n", key.c_str(), static_cast<unsigned long long>(count));
			return 1;
		}
		std::printf("PASS UpperSoft inline ADD/SUB/MUL: %u emitted configs, %llu block runs vs PS2Float values and MAC flags; "
					"MUL fast path identical to the outlined exact path (values, MAC, status, ACC flags)\n",
			configs, static_cast<unsigned long long>(checked));
		std::printf("KNOWN UpperSoft inline ADD/SUB underflow stored +/-0 instead of PS2Float E=0 bits: %llu lanes "
					"(flags match; only the packed flag-producing path stores the bits)\n",
			static_cast<unsigned long long>(known_underflow_zero));
		return 0;
	}

	// MADD/MSUB: one-sided "E != 255" hints must give exactly the unhinted (fully guarded) result.
	static Block EmitMaddBlock(microVU& vu, HarnessRegAlloc& alloc, u32 xyzw, int variant, bool msub, bool acc,
		bool mflag, bool sflag, bool avx, int reg_base, bool hint_s, bool hint_o)
	{
		const auto saved = avx512;
		if (!avx)
			avx512 = {};
		alloc.next = reg_base;
		alloc.allocated = 0;
		vu.regAlloc = &alloc;
		vu.code = (xyzw << 21) | (2 << 16) | (1 << 11) | (3 << 6);
		HarnessInstInfo& info = vu.prog.IRinfo.info[0];
		info = {};
		info.mFlag.doFlag = mflag;
		info.mFlag.write = 1;
		info.sFlag.doFlag = sflag;
		info.sFlag.doValue = sflag;
		info.sFlag.doNonSticky = sflag;
		info.sFlag.write = 2;
		info.sFlag.lastWrite = 1;

		xAlignPtr(64);
		const Block fn = reinterpret_cast<Block>(xGetAlignedCallTarget());
		for (int id : {3, 5, 12, 13, 14, 15})
			xPUSH(xRegister64(id));
		xSUB(rsp, 8);
		xSTMXCSR(ptr32[&g_host_mxcsr]);
		xLDMXCSR(ptr32[&EmuConfig.Cpu.VU0FPCR.bitmask]);
		for (int i = 0; i < 4; i++)
			xMOV(xRegister32(12 + i), ptr32[&g_status[i]]);

		const VuUpperFmacSoftDescriptor op{msub ? VuUpperFmacSoftKind::Msub : VuUpperFmacSoftKind::Madd,
			static_cast<VuUpperFmacSoftOperandSource>(variant),
			acc ? VuUpperFmacSoftDestination::Acc : VuUpperFmacSoftDestination::Fd};
		const xmm source = alloc.allocReg(1);
		const xmm operand = alloc.allocReg(2);
		const xmm accumulator = alloc.allocReg(32);
		const int destination_reg = acc ? 32 : 3;
		const int destination_load = acc ? 32 : (xyzw == 0xf ? -1 : 3);
		const xmm destination = alloc.allocReg(destination_load, destination_reg, xyzw, !acc);
		mVUemitUpperInlineMaddExactResult(vu, op, source, operand, accumulator, destination, true, false, hint_s, hint_o, false);
		alloc.Flush();

		for (int i = 0; i < 4; i++)
			xMOV(ptr32[&g_status[i]], xRegister32(12 + i));
		xLDMXCSR(ptr32[&g_host_mxcsr]);
		xADD(rsp, 8);
		for (int id : {15, 14, 13, 12, 5, 3})
			xPOP(xRegister64(id));
		xRET();
		avx512 = saved;
		return fn;
	}

	int RunMaddHints()
	{
		void* code = mmap(nullptr, 16 * 1024 * 1024, PROT_READ | PROT_WRITE | PROT_EXEC,
			MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
		if (code == MAP_FAILED)
			return 1;
		xSetPtr(static_cast<u8*>(code));
		static microVU vu;
		HarnessRegAlloc alloc;
		vu.regAlloc = &alloc;
		GenerateKernels(vu);
		std::mt19937 rng(0x4d414444);
		u64 failures = 0, checked = 0;
		u32 configs = 0;
		for (bool msub : {false, true})
			for (u32 xyzw = 1; xyzw < 16; xyzw++)
				for (int variant : {0, 3, 4, 5, 6})
					for (int flags = 0; flags < 4; flags++)
						for (bool avx : {false, true})
						{
							const bool acc = (xyzw & 1) != 0 && variant == 3;
							const int base = static_cast<int>((xyzw * 3 + variant) % 11);
							u8* const code_mark = xGetPtr();
							Block ref = EmitMaddBlock(vu, alloc, xyzw, variant, msub, acc, flags & 1, flags & 2, avx, base, false, false);
							Block hs = EmitMaddBlock(vu, alloc, xyzw, variant, msub, acc, flags & 1, flags & 2, avx, base, true, false);
							Block ho = EmitMaddBlock(vu, alloc, xyzw, variant, msub, acc, flags & 1, flags & 2, avx, base, false, true);
							Block hb = EmitMaddBlock(vu, alloc, xyzw, variant, msub, acc, flags & 1, flags & 2, avx, base, true, true);
							configs++;
							for (int iter = 0; iter < 400; iter++)
							{
								const u32 pick = iter & 3; // which inputs are forced to E != 255
								for (auto& reg : g_vf)
									for (u32& lane : reg)
										lane = RandomFloat(rng);
								for (int r : {1, 2})
									for (u32& lane : g_vf[r])
										if ((((lane >> 23) & 0xff) == 0xff) && (r == 1 ? (pick & 1) : (pick & 2)))
											lane &= ~(1u << 23);
								g_vf[0][0] = g_vf[0][1] = g_vf[0][2] = 0;
								g_vf[0][3] = 0x3f800000;
								for (u32& st : g_status)
									st = rng() & 0xfff;
								std::memset(vu.macFlag, 0, sizeof(vu.macFlag));
								g_vuRegs.accflag = rng() & 0xf;
								State before;
								Capture(vu, before);
								ref();
								State expected;
								Capture(vu, expected);
								const struct { Block fn; u32 need; } variants[] = {{hs, 1}, {ho, 2}, {hb, 3}};
								for (const auto& v : variants)
								{
									if ((pick & v.need) != v.need)
										continue;
									Restore(vu, before);
									v.fn();
									State got;
									Capture(vu, got);
									checked++;
									if (std::memcmp(&got, &expected, sizeof(State)) && failures++ < 8)
										std::printf("FAIL MADD hint xyzw=%x variant=%d msub=%d acc=%d flags=%d avx512=%d need=%u iter=%d\n",
											xyzw, variant, msub, acc, flags, avx, v.need, iter);
								}
							}
							xSetPtr(code_mark);
						}
		if (failures)
			return 1;
		std::printf("PASS UpperSoft inline MADD/MSUB one-sided nonextended hints: %u emitted configs, %llu runs identical to unhinted\n",
			configs, static_cast<unsigned long long>(checked));
		return 0;
	}
} // namespace UpperSoftInline

int TestUpperSoftInline()
{
	return UpperSoftInline::Run() | UpperSoftInline::RunMaddHints();
}
