// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "microVU.h"

#include "VUops.h"

#include "common/AlignedMalloc.h"
#include "common/Perf.h"
#include "common/StringUtil.h"

//------------------------------------------------------------------
// Micro VU - Main Functions
//------------------------------------------------------------------

// Only run this once per VU! ;)
void mVUinit(microVU& mVU, uint vuIndex)
{
	std::memset(&mVU.prog, 0, sizeof(mVU.prog));

	mVU.index        =  vuIndex;
	mVU.cop2         =  0;
	mVU.vuMemSize    = (mVU.index ? 0x4000 : 0x1000);
	mVU.microMemSize = (mVU.index ? 0x4000 : 0x1000);
	mVU.progSize     = (mVU.index ? 0x4000 : 0x1000) / 4;
	mVU.progMemMask  =  mVU.progSize-1;
	mVU.cache        = vuIndex ? SysMemory::GetVU1Rec() : SysMemory::GetVU0Rec();
	mVU.prog.x86end  = (vuIndex ? SysMemory::GetVU1RecEnd() : SysMemory::GetVU0RecEnd()) - (mVUcacheSafeZone * _1mb);

	mVU.regAlloc.reset(new microRegAlloc(mVU.index));
}

// Resets Rec Data
void mVUreset(microVU& mVU, bool resetReserve)
{
	if (THREAD_VU1)
	{
		DevCon.Warning("mVU Reset");
		// If MTVU is toggled on during gameplay we need to flush the running VU1 program, else it gets in a mess
		if (VU0.VI[REG_VPU_STAT].UL & 0x100)
		{
			CpuVU1->Execute(vu1RunCycles);
		}
		VU0.VI[REG_VPU_STAT].UL &= ~0x100;
	}

	xSetTextPtr(mVU.textPtr());
	xSetPtr(mVU.cache);
	mVUdispatcherAB(mVU);
	mVUdispatcherCD(mVU);
	mVUGenerateWaitMTVU(mVU);
	mVUGenerateCopyPipelineState(mVU);
	mVUGenerateCompareState(mVU);

	mVU.regs().nextBlockCycles = 0;
	memset(&mVU.prog.lpState, 0, sizeof(mVU.prog.lpState));
	mVU.profiler.Reset(mVU.index);

	// Program Variables
	mVU.prog.cleared  =  1;
	mVU.prog.isSame   = -1;
	mVU.prog.cur      = NULL;
	mVU.prog.total    =  0;
	mVU.prog.curFrame =  0;

	// Setup Dynarec Cache Limits for Each Program
	mVU.prog.x86start = xGetAlignedCallTarget();
	mVU.prog.x86ptr   = mVU.prog.x86start;

	for (u32 i = 0; i < (mVU.progSize / 2); i++)
	{
		if (!mVU.prog.prog[i])
		{
			mVU.prog.prog[i] = new std::deque<microProgram*>();
			continue;
		}
		for (auto it = mVU.prog.prog[i]->begin(); it != mVU.prog.prog[i]->end(); ++it)
		{
			mVUdeleteProg(mVU, it[0]);
		}
		mVU.prog.prog[i]->clear();
		mVU.prog.quick[i].block = NULL;
		mVU.prog.quick[i].prog = NULL;
	}
}

// Free Allocated Resources
void mVUclose(microVU& mVU)
{
	// Delete Programs and Block Managers
	for (u32 i = 0; i < (mVU.progSize / 2); i++)
	{
		if (!mVU.prog.prog[i])
			continue;
		for (auto it = mVU.prog.prog[i]->begin(); it != mVU.prog.prog[i]->end(); ++it)
		{
			mVUdeleteProg(mVU, it[0]);
		}
		safe_delete(mVU.prog.prog[i]);
	}
}

// Clears Block Data in specified range
__fi void mVUclear(mV, u32 addr, u32 size)
{
	if (!mVU.prog.cleared)
	{
		mVU.prog.cleared = 1; // Next execution searches/creates a new microprogram
		std::memset(&mVU.prog.lpState, 0, sizeof(mVU.prog.lpState)); // Clear pipeline state
		for (u32 i = 0; i < (mVU.progSize / 2); i++)
		{
			mVU.prog.quick[i].block = NULL; // Clear current quick-reference block
			mVU.prog.quick[i].prog = NULL; // Clear current quick-reference prog
		}
	}
}

//------------------------------------------------------------------
// Micro VU - Private Functions
//------------------------------------------------------------------

// Deletes a program
__ri void mVUdeleteProg(microVU& mVU, microProgram*& prog)
{
	for (u32 i = 0; i < (mVU.progSize / 2); i++)
	{
		safe_delete(prog->block[i]);
	}
	safe_delete(prog->ranges);
	safe_aligned_free(prog);
}

// Creates a new Micro Program
__ri microProgram* mVUcreateProg(microVU& mVU, int startPC)
{
	microProgram* prog = (microProgram*)_aligned_malloc(sizeof(microProgram), 64);
	memset(prog, 0, sizeof(microProgram));
	prog->idx = mVU.prog.total++;
	prog->ranges = new std::deque<microRange>();
	prog->startPC = startPC;
	if(doWholeProgCompare)
		mVUcacheProg(mVU, *prog); // Cache Micro Program
	double cacheSize = (double)((uptr)mVU.prog.x86end - (uptr)mVU.prog.x86start);
	double cacheUsed = ((double)((uptr)mVU.prog.x86ptr - (uptr)mVU.prog.x86start)) / (double)_1mb;
	double cachePerc = ((double)((uptr)mVU.prog.x86ptr - (uptr)mVU.prog.x86start)) / cacheSize * 100;
	ConsoleColors c = mVU.index ? Color_Orange : Color_Magenta;
	DevCon.WriteLn(c, "microVU%d: Cached Prog = [%03d] [PC=%04x] [List=%02d] (Cache=%3.3f%%) [%3.1fmb]",
		mVU.index, prog->idx, startPC * 8, mVU.prog.prog[startPC]->size() + 1, cachePerc, cacheUsed);
	return prog;
}

// Caches Micro Program
__ri void mVUcacheProg(microVU& mVU, microProgram& prog)
{
	if (!doWholeProgCompare)
	{
		auto cmpOffset = [&](void* x) { return (u8*)x + mVUrange.start; };
		memcpy(cmpOffset(prog.data), cmpOffset(mVU.regs().Micro), (mVUrange.end - mVUrange.start));
	}
	else
	{
		if (!mVU.index)
			memcpy(prog.data, mVU.regs().Micro, 0x1000);
		else
			memcpy(prog.data, mVU.regs().Micro, 0x4000);
	}
	mVUdumpProg(mVU, prog);
}

// Generate Hash for partial program based on compiled ranges...
u64 mVUrangesHash(microVU& mVU, microProgram& prog)
{
	union
	{
		u64 v64;
		u32 v32[2];
	} hash = {0};

	std::deque<microRange>::const_iterator it(prog.ranges->begin());
	for (; it != prog.ranges->end(); ++it)
	{
		if ((it[0].start < 0) || (it[0].end < 0))
		{
			DevCon.Error("microVU%d: Negative Range![%d][%d]", mVU.index, it[0].start, it[0].end);
		}
		for (int i = it[0].start / 4; i < it[0].end / 4; i++)
		{
			hash.v32[0] -= prog.data[i];
			hash.v32[1] ^= prog.data[i];
		}
	}
	return hash.v64;
}

// Prints the ratio of unique programs to total programs
void mVUprintUniqueRatio(microVU& mVU)
{
	std::vector<u64> v;
	for (u32 pc = 0; pc < mProgSize / 2; pc++)
	{
		microProgramList* list = mVU.prog.prog[pc];
		if (!list)
			continue;
		for (auto it = list->begin(); it != list->end(); ++it)
		{
			v.push_back(mVUrangesHash(mVU, *it[0]));
		}
	}
	u32 total = v.size();
	sortVector(v);
	makeUnique(v);
	if (!total)
		return;
	DevCon.WriteLn("%d / %d [%3.1f%%]", v.size(), total, 100. - (double)v.size() / (double)total * 100.);
}

// Compare Cached microProgram to mVU.regs().Micro
__fi bool mVUcmpProg(microVU& mVU, microProgram& prog)
{
	if (doWholeProgCompare)
	{
		if (memcmp((u8*)prog.data, mVU.regs().Micro, mVU.microMemSize))
			return false;
	}
	else
	{
		for (const auto& range : *prog.ranges)
		{
#if defined(PCSX2_DEVBUILD) || defined(_DEBUG)
			if ((range.start < 0) || (range.end < 0))
				DevCon.Error("microVU%d: Negative Range![%d][%d]", mVU.index, range.start, range.end);
#endif
			auto cmpOffset = [&](void* x) { return (u8*)x + range.start; };

			if (memcmp(cmpOffset(prog.data), cmpOffset(mVU.regs().Micro), (range.end - range.start)))
				return false;
		}
	}
	mVU.prog.cleared = 0;
	mVU.prog.cur = &prog;
	mVU.prog.isSame = doWholeProgCompare ? 1 : -1;
	return true;
}

// Searches for Cached Micro Program and sets prog.cur to it (returns entry-point to program)
_mVUt __fi void* mVUsearchProg(u32 startPC, uptr pState)
{
	microVU& mVU = mVUx;
	microProgramQuick& quick = mVU.prog.quick[mVU.regs().start_pc / 8];
	microProgramList*  list  = mVU.prog.prog [mVU.regs().start_pc / 8];

	if (!quick.prog) // If null, we need to search for new program
	{
		for (auto it = list->begin(); it != list->end(); ++it)
		{
			bool b = mVUcmpProg(mVU, *it[0]);

			if (b)
			{
				quick.block = it[0]->block[startPC / 8];
				quick.prog  = it[0];
				list->erase(it);
				list->push_front(quick.prog);

				// Sanity check, in case for some reason the program compilation aborted half way through (JALR for example)
				if (quick.block == nullptr)
				{
					void* entryPoint = mVUblockFetch(mVU, startPC, pState);
					return entryPoint;
				}
				return mVUentryGet(mVU, quick.block, startPC, pState);
			}
		}

		// If cleared and program not found, make a new program instance
		mVU.prog.cleared = 0;
		mVU.prog.isSame  = 1;
		mVU.prog.cur     = mVUcreateProg(mVU, mVU.regs().start_pc/8);
		void* entryPoint = mVUblockFetch(mVU,  startPC, pState);
		quick.block      = mVU.prog.cur->block[startPC/8];
		quick.prog       = mVU.prog.cur;
		list->push_front(mVU.prog.cur);
		//mVUprintUniqueRatio(mVU);
		return entryPoint;
	}

	// If list.quick, then we've already found and recompiled the program ;)
	mVU.prog.isSame = -1;
	mVU.prog.cur = quick.prog;
	// Because the VU's can now run in sections and not whole programs at once
	// we need to set the current block so it gets the right program back
	quick.block = mVU.prog.cur->block[startPC / 8];

	// Sanity check, in case for some reason the program compilation aborted half way through
	if (quick.block == nullptr)
	{
		void* entryPoint = mVUblockFetch(mVU, startPC, pState);
		return entryPoint;
	}
	return mVUentryGet(mVU, quick.block, startPC, pState);
}

//------------------------------------------------------------------
// recMicroVU0 / recMicroVU1
//------------------------------------------------------------------

recMicroVU0 CpuMicroVU0;
recMicroVU1 CpuMicroVU1;
bool mVU1Stage1NativeAllowed = false;

recMicroVU0::recMicroVU0() { m_Idx = 0; IsInterpreter = false; }
recMicroVU1::recMicroVU1() { m_Idx = 1; IsInterpreter = false; }

struct mVU1Stage1ScanResult
{
	bool native_path = false;
	const char* reason = "unknown";
	u32 pc = 0;
	u32 code = 0;
};

static bool mVU1UpperOpUsesSoftAddSub(u32 code)
{
	switch (code & 0x3f)
	{
		case 0x00: case 0x01: case 0x02: case 0x03: // ADDx/y/z/w
		case 0x04: case 0x05: case 0x06: case 0x07: // SUBx/y/z/w
		case 0x20: case 0x22: // ADDq/ADDi
		case 0x24: case 0x26: // SUBq/SUBi
		case 0x28: case 0x2c: // ADD/SUB
		case 0x08: case 0x09: case 0x0a: case 0x0b: // MADDx/y/z/w
		case 0x0c: case 0x0d: case 0x0e: case 0x0f: // MSUBx/y/z/w
		case 0x21: case 0x23: // MADDq/MADDi
		case 0x25: case 0x27: // MSUBq/MSUBi
		case 0x29: case 0x2d: // MADD/MSUB
			return true;
		default:
			break;
	}

	const u32 upper_op = code & 0x3f;
	const u32 fd_op = (code >> 6) & 0x1f;
	if (upper_op == 0x3c || upper_op == 0x3d)
		return fd_op <= 3 || fd_op == 8 || fd_op == 9 || fd_op == 10 || fd_op == 11;
	if (upper_op == 0x3e || upper_op == 0x3f)
		return fd_op <= 3 || fd_op == 8 || fd_op == 9;
	return false;
}

static bool mVU1UpperOpUsesSoftMul(u32 code)
{
	switch (code & 0x3f)
	{
		case 0x08: case 0x09: case 0x0a: case 0x0b: // MADDx/y/z/w
		case 0x0c: case 0x0d: case 0x0e: case 0x0f: // MSUBx/y/z/w
		case 0x18: case 0x19: case 0x1a: case 0x1b: // MULx/y/z/w
		case 0x1c: case 0x1e: // MULq/MULi
		case 0x21: case 0x23: // MADDq/MADDi
		case 0x25: case 0x27: // MSUBq/MSUBi
		case 0x29: case 0x2a: case 0x2d: case 0x2e: // MADD/MUL/MSUB/OPMSUB
			return true;
		default:
			break;
	}

	const u32 upper_op = code & 0x3f;
	const u32 fd_op = (code >> 6) & 0x1f;
	if (upper_op == 0x3c)
		return fd_op == 2 || fd_op == 3 || fd_op == 6 || fd_op == 7;
	if (upper_op == 0x3d)
		return fd_op == 2 || fd_op == 3 || fd_op == 6 || fd_op == 8 || fd_op == 9 || fd_op == 10 || fd_op == 11;
	if (upper_op == 0x3e)
		return fd_op == 2 || fd_op == 3 || fd_op == 6 || fd_op == 7 || fd_op == 10 || fd_op == 11;
	if (upper_op == 0x3f)
		return fd_op == 2 || fd_op == 3 || fd_op == 6 || fd_op == 8 || fd_op == 9;
	return false;
}

static bool mVU1UpperOpIsStage1Native(u32 code)
{
	const u32 upper_op = code & 0x3f;
	const u32 xyzw = (code >> 21) & 0xf;
	if (upper_op == 0x28 || upper_op == 0x2a || upper_op == 0x2c)
		return xyzw == 0xf || IsVU1SoftNativeStageAllowed(2);
	if ((upper_op <= 0x07) || (upper_op >= 0x18 && upper_op <= 0x1b))
		return IsVU1SoftNativeStageAllowed(3);
	if (upper_op == 0x1e || upper_op == 0x22 || upper_op == 0x26)
		return IsVU1SoftNativeStageAllowed(4);
	if ((upper_op >= 0x08 && upper_op <= 0x0f) || upper_op == 0x23 || upper_op == 0x27 || upper_op == 0x29 || upper_op == 0x2d)
		return IsVU1SoftNativeStageAllowed(5);
	if (upper_op == 0x1c)
		return IsVU1SoftNativeStageAllowed(6);
	return false;
}

static bool mVU1LowerOpIsUnsafeForStage1Native(u32 code)
{
	const u32 lower_op = code & 0x7f;
	if ((lower_op >= 0x10 && lower_op <= 0x1c) || (lower_op >= 0x20 && lower_op <= 0x2f))
		return true;
	if (lower_op == 0x40)
	{
		const u32 t3_op = (code >> 4) & 0x3;
		const u32 sub_op = (code >> 6) & 0x1f;
		return t3_op == 0 && sub_op == 27; // XGKICK
	}
	return false;
}

static mVU1Stage1ScanResult mVU1ScanNativeSoftFloatStage1(microVU& mVU, s32 start, s32 end)
{
	const bool soft_addsub = CHECK_VU_SOFT_ADDSUB(1) != 0;
	const bool soft_mul = CHECK_VU_SOFT_MUL(1) != 0;
	if (!soft_addsub && !soft_mul)
		return {false, "soft-disabled", static_cast<u32>(start), 0};
	if (mVU.index != 1)
		return {false, "not-vu1", static_cast<u32>(start), 0};
	if (start < 0 || end <= start)
		return {false, "empty-range", static_cast<u32>(start), 0};

	const u32* micro = reinterpret_cast<u32*>(mVU.regs().Micro);
	bool has_stage1_native_op = false;
	bool has_unsafe_lower_op = false;
	for (u32 byte_pc = static_cast<u32>(start) & ~7u; byte_pc < static_cast<u32>(end); byte_pc += 8)
	{
		const u32 pc = ((byte_pc & (mVU.microMemSize - 8)) / 4) + 1;
		const u32 code0 = micro[pc & mVU.progMemMask];
		const u32 code1 = micro[(pc ^ 1) & mVU.progMemMask];
		const bool needs_soft0 = (soft_addsub && mVU1UpperOpUsesSoftAddSub(code0)) || (soft_mul && mVU1UpperOpUsesSoftMul(code0));
		const bool needs_soft1 = (soft_addsub && mVU1UpperOpUsesSoftAddSub(code1)) || (soft_mul && mVU1UpperOpUsesSoftMul(code1));
		const bool stage1_native0 = needs_soft0 && mVU1UpperOpIsStage1Native(code0);
		const bool stage1_native1 = needs_soft1 && mVU1UpperOpIsStage1Native(code1);
		if (needs_soft0 && !stage1_native0)
			return {false, "unsupported-upper-soft-op", pc * 4, code0};
		if (needs_soft1 && !stage1_native1)
			return {false, "unsupported-upper-soft-op", (pc ^ 1) * 4, code1};
		has_stage1_native_op |= stage1_native0 || stage1_native1;
		if (mVU1LowerOpIsUnsafeForStage1Native(code0))
			has_unsafe_lower_op = true;
		if (mVU1LowerOpIsUnsafeForStage1Native(code1))
			has_unsafe_lower_op = true;
		if (has_stage1_native_op && has_unsafe_lower_op)
			return {false, "unsafe-lower-op", pc * 4, code0};
	}

	if (!has_stage1_native_op)
		return {false, "no-stage1-native-op", static_cast<u32>(start), 0};

	return {true, "accepted", static_cast<u32>(start), 0};
}

void mVU1UpdateStage1NativeAllowed(microVU& mVU, s32 start, s32 end)
{
	const mVU1Stage1ScanResult scan = mVU1ScanNativeSoftFloatStage1(mVU, start, end);
	mVU1Stage1NativeAllowed = scan.native_path;
}

void recMicroVU0::Reserve()
{
	mVUinit(microVU0, 0);
}
void recMicroVU1::Reserve()
{
	mVUinit(microVU1, 1);
	vu1Thread.Open();
}

void recMicroVU0::Shutdown()
{
	mVUclose(microVU0);
}
void recMicroVU1::Shutdown()
{
	if (vu1Thread.IsOpen())
		vu1Thread.WaitVU();
	mVUclose(microVU1);
}

void recMicroVU0::Reset()
{
	mVUreset(microVU0, true);
}

void recMicroVU0::Step()
{
}

void recMicroVU1::Reset()
{
	vu1Thread.WaitVU();
	vu1Thread.Get_MTVUChanges();
	mVUreset(microVU1, true);
}

void recMicroVU0::SetStartPC(u32 startPC)
{
	VU0.start_pc = startPC;
}

void recMicroVU0::Execute(u32 cycles)
{
	VU0.flags &= ~VUFLAG_MFLAGSET;

	if (!(VU0.VI[REG_VPU_STAT].UL & 1))
		return;
	VU0.VI[REG_TPC].UL <<= 3;

	((mVUrecCall)microVU0.startFunct)(VU0.VI[REG_TPC].UL, cycles);
	VU0.VI[REG_TPC].UL >>= 3;
	if (microVU0.regs().flags & 0x4)
	{
		microVU0.regs().flags &= ~0x4;
		hwIntcIrq(6);
	}
}

void recMicroVU1::SetStartPC(u32 startPC)
{
	VU1.start_pc = startPC;
}

void recMicroVU1::Step()
{
}

void recMicroVU1::Execute(u32 cycles)
{
	if (!THREAD_VU1)
	{
		if (!(VU0.VI[REG_VPU_STAT].UL & 0x100))
			return;
	}
	VU1.VI[REG_TPC].UL <<= 3;
	((mVUrecCall)microVU1.startFunct)(VU1.VI[REG_TPC].UL, cycles);
	VU1.VI[REG_TPC].UL >>= 3;
	if (microVU1.regs().flags & 0x4 && !THREAD_VU1)
	{
		microVU1.regs().flags &= ~0x4;
		hwIntcIrq(7);
	}
}

void recMicroVU0::Clear(u32 addr, u32 size)
{
	mVUclear(microVU0, addr, size);
}
void recMicroVU1::Clear(u32 addr, u32 size)
{
	mVUclear(microVU1, addr, size);
}

void recMicroVU1::ResumeXGkick()
{
	if (!(VU0.VI[REG_VPU_STAT].UL & 0x100))
		return;
	((mVUrecCallXG)microVU1.startFunctXG)();
}

bool SaveStateBase::vuJITFreeze()
{
	if (IsSaving())
		vu1Thread.WaitVU();

	Freeze(microVU0.prog.lpState);
	Freeze(microVU1.prog.lpState);
	return IsOkay();
}

#if 0

#include <zlib.h>

void DumpVUState(u32 n, u32 pc)
{
	const VURegs& r = vuRegs[n];
	const microVU& mVU = (n == 0) ? microVU0 : microVU1;
	static FILE* fp = nullptr;
	static bool fp_opened = false;
	static u32 counter = 0;

	u32 first = pc >> 31;
	pc &= 0x7FFFFFFFu;
	if (first)
		counter++;

#if 0
	if (counter == 184639 && pc == 0x0D70)
		__debugbreak();
#endif

	if (counter < 0)
		return;

	if (!fp_opened)
	{
		fp = std::fopen("C:\\Dumps\\comp\\vulog.txt", "wb");
		fp_opened = true;
	}
	if (fp)
	{
		const microVU& m = (n == 0) ? microVU0 : microVU1;
		fprintf(fp, "%08d VU%u SPC:%04X xPC:%04X BRANCH:%04X VIBACKUP:%04X", counter, n, r.start_pc, pc, mVU.branch, mVU.VIbackup);
#if 1
		//fprintf(fp, " MEM:%08X", crc32(0, (Bytef*)r.Mem, (n == 0) ? VU0_MEMSIZE : VU1_MEMSIZE));
		fprintf(fp, " MAC %08X %08X %08X %08X [%08X %08X %08X %08X]", r.micro_macflags[3], r.micro_macflags[2], r.micro_macflags[1], r.micro_macflags[0], m.macFlag[3], m.macFlag[2], m.macFlag[1], m.macFlag[0]);
		fprintf(fp, " CLIP %08X %08X %08X %08X [%08X %08X %08X %08X]", r.micro_clipflags[3], r.micro_clipflags[2], r.micro_clipflags[1], r.micro_clipflags[0], m.clipFlag[3], m.clipFlag[2], m.clipFlag[1], m.clipFlag[0]);
		fprintf(fp, " STATUS %08X %08X %08X %08X [%08X %08X %08X %08X]", r.micro_statusflags[3], r.micro_statusflags[2], r.micro_statusflags[1], r.micro_statusflags[0], m.statFlag[3], m.statFlag[2], m.statFlag[1], m.statFlag[0]);

		for (u32 i = 0; i < 32; i++)
		{
			const VECTOR& v = r.VF[i];
			fprintf(fp, " VF%u: %08X%08X%08X%08X (%f,%f,%f,%f)", i, v.UL[3], v.UL[2], v.UL[1], v.UL[0], v.F[3], v.F[2], v.F[1], v.F[0]);
		}

		for (u32 i = 0; i < 32; i++)
		{
			const REG_VI& v = r.VI[i];
			fprintf(fp, " VI%u: %08X", i, v.UL);
		}

		fprintf(fp, " ACC: %08X%08X%08X%08X (%f,%f,%f,%f)", r.ACC.UL[3], r.ACC.UL[2], r.ACC.UL[1], r.ACC.UL[0],
			r.ACC.F[3], r.ACC.F[2], r.ACC.F[1], r.ACC.F[0]);
		fprintf(fp, " Q: %08X (%f)", r.q.UL, r.q.F);
		fprintf(fp, " P: %08X (%f)\n", r.p.UL, r.p.F);
#else
		fprintf(fp, " REG:%08X\n", crc32(0, (Bytef*)&r, offsetof(VURegs, idx)));
#endif
		//fflush(fp);
	}
}

#endif
