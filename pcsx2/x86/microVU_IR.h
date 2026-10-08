// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once
#include "microVU.h"
#include "AVX512Profile.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <string>

inline constexpr int mVUsoftAccRegisterIndex = 32;

struct regCycleInfo
{
	u8 x : 4;
	u8 y : 4;
	u8 z : 4;
	u8 w : 4;
};

// microRegInfo is carefully ordered for faster compares.  The "important" information is
// housed in a union that is accessed via 'quick32' so that several u8 fields can be compared
// using a pair of 32-bit equalities.
// vi15 is only used if microVU const-prop is enabled (it is *not* by default).  When constprop
// is disabled the vi15 field acts as additional padding that is required for 16 byte alignment
// needed by the xmm compare.
union alignas(16) microRegInfo
{
	struct
	{
		union
		{
			struct
			{
				u8 needExactMatch; // If set, block needs an exact match of pipeline state
				u8 flagInfo;       // xC * 2 | xM * 2 | xS * 2 | 0 * 1 | fullFlag Valid * 1
				u8 q;
				u8 p;
				u8 xgkick;
				u8 viBackUp;       // VI reg number that was written to on branch-delay slot
				u8 blockType;      // 0 = Normal; 1,2 = Compile one instruction (E-bit/Branch Ending)
				u8 r;
			};
			u64 quick64[1];
			u32 quick32[2];
		};

		u32 xgkickcycles;
		u8 unused;
		u8 vi15v; // 'vi15' constant is valid
		u16 vi15; // Constant Prop Info for vi15

		struct
		{
			u8 VI[16];
			regCycleInfo VF[32];
		};
	};

	u128 full128[96 / sizeof(u128)];
	u64  full64[96 / sizeof(u64)];
	u32  full32[96 / sizeof(u32)];
};

// Note: mVUcustomSearch needs to be updated if this is changed
static_assert(sizeof(microRegInfo) == 96, "microRegInfo was not 96 bytes");

struct microProgram;
struct microJumpCache
{
	microJumpCache() : prog(NULL), x86ptrStart(NULL) {}
	microProgram* prog; // Program to which the entry point below is part of
	void* x86ptrStart;  // Start of code (Entry point for block)
};

struct alignas(16) microBlock
{
	microRegInfo    pState;      // Detailed State of Pipeline
	microRegInfo    pStateEnd;   // Detailed State of Pipeline at End of Block (needed by JR/JALR opcodes)
	u8*             x86ptrStart; // Start of code (Entry point for block)
	microJumpCache* jumpCache;   // Will point to an array of entry points of size [16k/8] if block ends in JR/JALR
};

struct microTempRegInfo
{
	regCycleInfo VF[2]; // Holds cycle info for Fd, VF[0] = Upper Instruction, VF[1] = Lower Instruction
	u8 VFreg[2];        // Index of the VF reg
	u8 VI;              // Holds cycle info for Id
	u8 VIreg;           // Index of the VI reg
	u8 q;               // Holds cycle info for Q reg
	u8 p;               // Holds cycle info for P reg
	u8 r;               // Holds cycle info for R reg (Will never cause stalls, but useful to know if R is modified)
	u8 xgkick;          // Holds the cycle info for XGkick
};

struct microVFreg
{
	u8 reg; // Reg Index
	u8 x;   // X vector read/written to?
	u8 y;   // Y vector read/written to?
	u8 z;   // Z vector read/written to?
	u8 w;   // W vector read/written to?
};

struct microVIreg
{
	u8 reg;  // Reg Index
	u8 used; // Reg is Used? (Read/Written)
};

struct microConstInfo
{
	u8  isValid;  // Is the constant in regValue valid?
	u32 regValue; // Constant Value
};

struct microUpperOp
{
	bool eBit;             // Has E-bit set
	bool iBit;             // Has I-bit set
	bool mBit;             // Has M-bit set
	bool tBit;             // Has T-bit set
	bool dBit;             // Has D-bit set
	microVFreg VF_write;   // VF Vectors written to by this instruction
	microVFreg VF_read[2]; // VF Vectors read by this instruction
};

struct microLowerOp
{
	microVFreg VF_write;      // VF Vectors written to by this instruction
	microVFreg VF_read[2];    // VF Vectors read by this instruction
	microVIreg VI_write;      // VI reg written to by this instruction
	microVIreg VI_read[2];    // VI regs read by this instruction
	microConstInfo constJump; // Constant Reg Info for JR/JARL instructions
	u32  branch;     // Branch Type (0 = Not a Branch, 1 = B. 2 = BAL, 3~8 = Conditional Branches, 9 = JR, 10 = JALR)
	u32  kickcycles; // Number of xgkick cycles accumulated by this instruction
	bool badBranch;  // This instruction is a Branch who has another branch in its Delay Slot
	bool evilBranch; // This instruction is a Branch in a Branch Delay Slot (Instruction after badBranch)
	bool isNOP;      // This instruction is a NOP
	bool isFSSET;    // This instruction is a FSSET
	bool noWriteVF;  // Don't write back the result of a lower op to VF reg if upper op writes to same reg (or if VF = 0)
	bool backupVI;   // Backup VI reg to memory if modified before branch (branch uses old VI value unless opcode is ILW or ILWR)
	bool memReadIs;  // Read Is (VI reg) from memory (used by branches)
	bool memReadIt;  // Read If (VI reg) from memory (used by branches)
	bool readFlags;  // Current Instruction reads Status, Mac, or Clip flags
	bool isMemWrite; // Current Instruction writes to VU memory
	bool isKick;     // Op is a kick so don't count kick cycles
};

struct microFlagInst
{
	bool doFlag;      // Update Flag on this Instruction
	bool doNonSticky; // Update O,U,S,Z (non-sticky) bits on this Instruction (status flag only)
	bool doValue;     // The architectural flag value, rather than only its pipeline slot, is live.
	u8   write;       // Points to the instance that should be written to (s-stage write)
	u8   lastWrite;   // Points to the instance that was last written to (most up-to-date flag)
	u8   read;        // Points to the instance that should be read by a lower instruction (t-stage read)
};

struct microFlagCycles
{
	int xStatus[4];
	int xMac[4];
	int xClip[4];
	int cycles;
};

struct microOp
{
	u8   stall;          // Info on how much current instruction stalled
	bool isBadOp;        // Cur Instruction is a bad opcode (not a legal instruction)
	bool isEOB;          // Cur Instruction is last instruction in block (End of Block)
	bool isBdelay;       // Cur Instruction in Branch Delay slot
	bool swapOps;        // Run Lower Instruction before Upper Instruction
	bool backupVF;       // Backup mVUlow.VF_write.reg, and restore it before the Upper Instruction is called
	bool doXGKICK;       // Do XGKICK transfer on this instruction
	u32  XGKICKPC;       // The PC in which the XGKick has taken place, so if we break early (before it) we don run it.
	bool doDivFlag;      // Transfer Div flag to Status Flag on this instruction
	int  readQ;          // Q instance for reading
	int  writeQ;         // Q instance for writing
	int  readP;          // P instance for reading
	int  writeP;         // P instance for writing
	microFlagInst sFlag; // Status Flag Instance Info
	microFlagInst mFlag; // Mac    Flag Instance Info
	microFlagInst cFlag; // Clip   Flag Instance Info
	microUpperOp  uOp;   // Upper Op Info
	microLowerOp  lOp;   // Lower Op Info
};

template <u32 pSize>
struct microIR
{
	microBlock       block;           // Block/Pipeline info
	microBlock*      pBlock;          // Pointer to a block in mVUblocks
	microTempRegInfo regsTemp;        // Temp Pipeline info (used so that new pipeline info isn't conflicting between upper and lower instructions in the same cycle)
	microOp          info[pSize / 2]; // Info for Instructions in current block
	microConstInfo   constReg[16];    // Simple Const Propagation Info for VI regs within blocks
	u8  branch;
	u32 cycles;    // Cycles for current block
	u32 count;     // Number of VU 64bit instructions ran (starts at 0 for each block)
	u32 curPC;     // Current PC
	u32 startPC;   // Start PC for Cur Block
	u32 sFlagHack; // Optimize out all Status flag updates if microProgram doesn't use Status flags
};

//------------------------------------------------------------------
// Reg Alloc
//------------------------------------------------------------------

//#define MVURALOG(...) fprintf(stderr, __VA_ARGS__)
#define MVURALOG(...)

struct microMapXMM
{
	int  VFreg;    // VF Reg Number Stored (-1 = Temp; 0 = vf0 and will not be written back; 32 = ACC; 33 = I reg)
	int  xyzw;     // xyzw to write back (0 = Don't write back anything AND cached vfReg has all vectors valid)
	int  count;    // Count of when last used
	bool isNeeded; // Is needed for current instruction
	bool isZero;   // Register was loaded from VF00 and doesn't need clamping
};

struct microMapGPR
{
	int VIreg;
	int count;
	bool isNeeded;
	bool dirty;
	bool isZeroExtended;
	bool usable;
};

// AVX-512: VF/ACC "home" registers. Each listed VF owns one of XMM16-31 for the
// whole micro program and the register always equals the VF in memory: every
// allocator write-back updates it as well (write-through), and it is refilled
// from memory at dispatcher entry and after helper calls (XMM16-31 are caller
// saved). Because it never holds newer data than memory, it is valid at every
// block boundary without extending the block state, and a VF read becomes a
// register move instead of a load. Remaining slots are used for in-block
// copies (see HighCopy). Needs proper testing in games.
//
// Opt-in: off by default. In Katamari Damacy the homes served ~65% of VU1 VF
// loads, yet the VU thread time (OSD, Ice Lake) did not change measurably, so
// they are not worth the extra guest state by default.
// PCSX2_AVX512_VF_HOMES selects the VU1 VFs, e.g. "12,13,16,18-28,31,acc" (the
// 16 most requested VF/ACC registers in that profile; vu_vf.csv shows the
// per-VF loads) or "1-15,acc". PCSX2_AVX512_VU0_VF_HOMES does the same for VU0
// micro; VU0 programs are short, so the 16 loads at every dispatcher entry
// tend to cost more than the ~2 VF loads per block they could save.
// PCSX2_AVX512_NO_HIGHCOPY=1 turns homes off as well.
//
// PCSX2_AVX512_VF_HOME_LAZY=1 (experimental) makes the homes authoritative:
// write-backs of a homed VF update only the register, and all homes are stored
// to memory at program end and before helper calls (spillHomes). Trades ~4
// stores per block run for 16 stores per program run / call.
struct microVFHomeMap
{
	int count = 0;
	bool lazy = false;
	int vf[iREGCNT_XMM_EVEX - iREGCNT_XMM] = {};
	int slotOf[33] = {}; // VF0-31, ACC(32) -> slot or -1
};

static microVFHomeMap mVUparseVFHomeMap(const char* env_name, const char* default_spec)
{
	microVFHomeMap m;
	std::fill(std::begin(m.slotOf), std::end(m.slotOf), -1);
	const char* off = std::getenv("PCSX2_AVX512_NO_HIGHCOPY");
	if ((off && off[0] == '1') || !x86Emitter::avx512.HasCore())
		return m;
	const char* lazy = std::getenv("PCSX2_AVX512_VF_HOME_LAZY");
	m.lazy = lazy && lazy[0] == '1';
	const char* env = std::getenv(env_name);
	const std::string spec = env ? env : default_spec;
	const auto add = [&m](int vf) {
		if (vf >= 1 && vf <= 32 && m.slotOf[vf] < 0 && m.count < static_cast<int>(std::size(m.vf)))
		{
			m.slotOf[vf] = m.count;
			m.vf[m.count++] = vf;
		}
	};
	size_t pos = 0;
	while (pos < spec.size())
	{
		size_t end = spec.find(',', pos);
		if (end == std::string::npos)
			end = spec.size();
		const std::string item = spec.substr(pos, end - pos);
		pos = end + 1;
		if (item == "acc" || item == "ACC")
		{
			add(32);
			continue;
		}
		int lo = 0, hi = 0;
		const int n = std::sscanf(item.c_str(), "%d-%d", &lo, &hi);
		if (n == 1)
			add(lo);
		else if (n == 2)
		{
			for (int vf = lo; vf <= hi; vf++)
				add(vf);
		}
	}
	return m;
}

static const microVFHomeMap& mVUgetVFHomeMap(int index)
{
	static const microVFHomeMap vu0 = mVUparseVFHomeMap("PCSX2_AVX512_VU0_VF_HOMES", "none");
	static const microVFHomeMap vu1 = mVUparseVFHomeMap("PCSX2_AVX512_VF_HOMES", "none");
	return index ? vu1 : vu0;
}

// Loads every home register from guest memory.
static void mVUloadVFHomes(int index)
{
	const microVFHomeMap& map = mVUgetVFHomeMap(index);
	VURegs& regs = ::vuRegs[index];
	for (int slot = 0; slot < map.count; slot++)
	{
		const int vf = map.vf[slot];
		xVMOVDQA32(xRegisterSSE(iREGCNT_XMM + slot), ptr[(vf == 32) ? &regs.ACC : &regs.VF[vf]]);
	}
}

// Stores every home register to guest memory (lazy homes only).
static void mVUstoreVFHomes(int index)
{
	const microVFHomeMap& map = mVUgetVFHomeMap(index);
	VURegs& regs = ::vuRegs[index];
	for (int slot = 0; slot < map.count; slot++)
	{
		const int vf = map.vf[slot];
		xVMOVDQA32(ptr[(vf == 32) ? &regs.ACC : &regs.VF[vf]], xRegisterSSE(iREGCNT_XMM + slot));
	}
}

class microRegAlloc
{
protected:
	static const int xmmTotal = iREGCNT_XMM - 1; // PQ register is reserved
	static const int gprTotal = iREGCNT_GPR;

	std::array<microMapXMM, xmmTotal> xmmMap;
	std::array<microMapGPR, gprTotal> gprMap;
	std::array<u8, mVUsoftAccRegisterIndex + 1> softNonExtendedMask = {};

	// AVX-512: clean copies of evicted VF/ACC values in XMM16-31. Memory stays
	// authoritative (write-through); a copy only replaces a later reload with a
	// register move. Micro mode only, and not with SoftFloat, whose exact paths
	// use XMM16-30 as fixed scratch. Needs proper testing in games.
	struct HighCopy
	{
		int vf = -1;
		int count = 0;
	};
	std::array<HighCopy, iREGCNT_XMM_EVEX - iREGCNT_XMM> highCopies;
	bool highCopiesEnabled = false;
	// VF home registers (microVFHomeMap) take slots [0, homeBase); copies use the rest.
	bool homesEnabled = false;
	int homeBase = 0;
	// Profile only: VF/ACC registers touched in this block, and since copies were last invalidated.
	u64 touchedInBlock = 0;
	u64 touchedSinceInvalidate = 0;

	int         counter; // Current allocation count
	int         index;   // VU0 or VU1

	// DO NOT REMOVE THIS.
	// This is here for a reason. MSVC likes to turn global writes into a load+conditional move+store.
	// That creates a race with the EE thread when we're compiling on the VU thread, even though
	// regAllocCOP2 is false. By adding another level of indirection, it emits a branch instead.
	_xmmregs*   pxmmregs;

	bool        regAllocCOP2;    // Local COP2 check

	int homeSlot(int vfreg) const
	{
		return (homesEnabled && vfreg >= 1 && vfreg <= 32) ? mVUgetVFHomeMap(index).slotOf[vfreg] : -1;
	}

	// Keeps a home register equal to memory: applies the same lanes the
	// write-back stores. Single-lane values sit in lane 0 (modXYZW).
	void updateHome(const xmm& reg, int vfreg, int xyzw)
	{
		const int slot = homeSlot(vfreg);
		if (slot < 0)
			return;
		const xRegisterSSE home(iREGCNT_XMM + slot);
		const auto insertIdx = [](int src, int dst) { return static_cast<u8>((src << 6) | (dst << 4)); };
		switch (xyzw)
		{
			case 0xf: xVMOVDQA32(home, reg); break;
			case 8: xVINSERTPS(home, home, reg, insertIdx(0, 0)); break;
			case 4: xVINSERTPS(home, home, reg, insertIdx(0, 1)); break;
			case 2: xVINSERTPS(home, home, reg, insertIdx(0, 2)); break;
			case 1: xVINSERTPS(home, home, reg, insertIdx(0, 3)); break;
			default:
				for (int lane = 0; lane < 4; lane++)
				{
					if (xyzw & (8 >> lane))
						xVINSERTPS(home, home, reg, insertIdx(lane, lane));
				}
				break;
		}
	}

	// Write-back of a guest VF/ACC value to memory, counted for the opt-in
	// AVX-512 planning profile (microVU blocks only, never COP2 macro ops).
	void profiledSaveReg(const xmm& reg, int vfreg, xAddressVoid ptr, int xyzw, bool modXYZW)
	{
		if (profileBlock)
			profileBlock->vf_writebacks++;
		updateHome(reg, vfreg, xyzw); // before the store, which may modify reg
		if (homeSlot(vfreg) >= 0 && mVUgetVFHomeMap(index).lazy)
		{
			if (profileBlock)
				profileBlock->vf_home_only_writebacks++;
			return; // memory is updated by spillHomes()
		}
		mVUsaveReg(reg, ptr, xyzw, modXYZW);
	}

public:
	AVX512Profile::VUBlock* profileBlock = nullptr;

protected:
	// Helper functions to get VU regs
	VURegs& regs() const { return ::vuRegs[index]; }
	__fi REG_VI& getVI(uint reg) const { return regs().VI[reg]; }
	__fi VECTOR& getVF(uint reg) const { return regs().VF[reg]; }

	__ri void loadIreg(const xmm& reg, int xyzw)
	{
		for (int i = 0; i < gprTotal; i++)
		{
			if (gprMap[i].VIreg == REG_I)
			{
				xMOVDZX(reg, xRegister32(i));
				if (!_XYZWss(xyzw))
					xSHUF.PS(reg, reg, 0);

				return;
			}
		}

		xMOVSSZX(reg, ptr32[&getVI(REG_I)]);
		if (!_XYZWss(xyzw))
			xSHUF.PS(reg, reg, 0);
	}

	void touchVF(int vfreg)
	{
		if (profileBlock && vfreg >= 1 && vfreg <= 32)
		{
			touchedInBlock |= 1ull << vfreg;
			touchedSinceInvalidate |= 1ull << vfreg;
		}
	}

	int findHighCopy(int vfreg) const
	{
		for (size_t i = homeBase; i < highCopies.size(); i++)
		{
			if (highCopies[i].vf == vfreg)
				return static_cast<int>(i);
		}
		return -1;
	}

	void dropHighCopy(int vfreg)
	{
		const int slot = findHighCopy(vfreg);
		if (slot >= 0)
			highCopies[slot].vf = -1;
	}

	// Called when a host register is about to be reused. A clean, complete
	// VF/ACC value is kept in a free (or least recently used) high register.
	void stashHighCopy(int regId, int keepVF)
	{
		if (!highCopiesEnabled)
			return;
		const microMapXMM& map = xmmMap[regId];
		if (map.VFreg < 1 || map.VFreg > 32 || map.xyzw != 0 || homeSlot(map.VFreg) >= 0)
			return;
		// A clean register can coexist with a not yet written back new value of
		// the same VF (cleared by clearNeeded). Its contents are stale then.
		for (int i = 0; i < xmmTotal; i++)
		{
			if (i != regId && xmmMap[i].VFreg == map.VFreg && xmmMap[i].xyzw != 0)
			{
				if (profileBlock)
					profileBlock->vf_copy_refused++;
				return;
			}
		}
		const int existing = findHighCopy(map.VFreg);
		if (existing >= 0)
		{
			highCopies[existing].count = counter;
			return;
		}
		int slot = -1;
		for (size_t i = homeBase; i < highCopies.size(); i++)
		{
			if (highCopies[i].vf == keepVF && keepVF >= 0)
				continue;
			if (slot < 0 || highCopies[i].vf < 0 ||
				(highCopies[slot].vf >= 0 && highCopies[i].count < highCopies[slot].count))
				slot = static_cast<int>(i);
			if (highCopies[slot].vf < 0)
				break;
		}
		if (slot < 0)
			return;
		if (profileBlock && highCopies[slot].vf >= 0)
			profileBlock->vf_copy_replaced++;
		xVMOVDQA32(xRegisterSSE(iREGCNT_XMM + slot), xRegisterSSE::GetInstance(regId));
		highCopies[slot] = {map.VFreg, counter};
	}

	// Replaces a full 128-bit reload of vfreg when a home or high copy exists.
	bool loadHighCopy(const xmm& reg, int vfreg)
	{
		if (const int home = homeSlot(vfreg); home >= 0)
		{
			xVMOVDQA32(reg, xRegisterSSE(iREGCNT_XMM + home));
			if (profileBlock)
			{
				profileBlock->vf_high_loads++;
				profileBlock->vf_home_loads++;
			}
			return true;
		}
		if (!highCopiesEnabled)
			return false;
		const int slot = findHighCopy(vfreg);
		if (slot < 0)
			return false;
		xVMOVDQA32(reg, xRegisterSSE(iREGCNT_XMM + slot));
		highCopies[slot].count = counter;
		if (profileBlock)
			profileBlock->vf_high_loads++;
		return true;
	}

	// Replaces a single-lane load (value moved to lane 0, like the cached-register
	// path; the other lanes are don't-care) when vfreg has a home register.
	bool loadHomeLane(const xmm& reg, int vfreg, int xyzw)
	{
		const int home = homeSlot(vfreg);
		if (home < 0)
			return false;
		const xRegisterSSE src(iREGCNT_XMM + home);
		if (xyzw == 8)
			xVMOVDQA32(reg, src);
		else
			xVPSHUFD(reg, src, (xyzw == 4) ? 1 : (xyzw == 2) ? 2 : 3);
		if (profileBlock)
		{
			profileBlock->vf_high_loads++;
			profileBlock->vf_home_loads++;
		}
		return true;
	}

	int findFreeRegRec(int startIdx)
	{
		for (int i = startIdx; i < xmmTotal; i++)
		{
			if (!xmmMap[i].isNeeded)
			{
				int x = findFreeRegRec(i + 1);
				if (x == -1)
					return i;
				return ((xmmMap[i].count < xmmMap[x].count) ? i : x);
			}
		}
		return -1;
	}

	int findFreeReg(int vfreg)
	{
		if (regAllocCOP2)
		{
			return _allocVFtoXMMreg(vfreg, 0);
		}

		for (int i = 0; i < xmmTotal; i++)
		{
			if (!xmmMap[i].isNeeded && (xmmMap[i].VFreg < 0))
			{
				return i; // Reg is not needed and was a temp reg
			}
		}
		int x = findFreeRegRec(0);
		pxAssertMsg(x >= 0, "microVU register allocation failure!");
		return x;
	}

	int findFreeGPRRec(int startIdx)
	{
		for (int i = startIdx; i < gprTotal; i++)
		{
			if (gprMap[i].usable && !gprMap[i].isNeeded)
			{
				int x = findFreeGPRRec(i + 1);
				if (x == -1)
					return i;
				return ((gprMap[i].count < gprMap[x].count) ? i : x);
			}
		}
		return -1;
	}

	int findFreeGPR(int vireg)
	{
		if (regAllocCOP2)
			return _allocX86reg(X86TYPE_VIREG, vireg, MODE_COP2);

		for (int i = 0; i < gprTotal; i++)
		{
			if (gprMap[i].usable && !gprMap[i].isNeeded && (gprMap[i].VIreg < 0))
			{
				return i; // Reg is not needed and was a temp reg
			}
		}
		int x = findFreeGPRRec(0);
		pxAssertMsg(x >= 0, "microVU register allocation failure!");
		return x;
	}

	void writeVIBackup(const xRegisterInt& reg);

public:
	microRegAlloc(int _index)
	{
		index = _index;

		// mark gpr registers as usable
		gprMap.fill({0, 0, false, false, false, false});
		for (int i = 0; i < gprTotal; i++)
		{
			if (i == gprT1.GetId() || i == gprT2.GetId() ||
				i == gprF0.GetId() || i == gprF1.GetId() || i == gprF2.GetId() || i == gprF3.GetId() ||
				i == rsp.GetId())
			{
				continue;
			}

			gprMap[i].usable = true;
		}

		reset(false);
	}

	// Lazy homes hold the only current copy: store them before anything outside
	// generated block code may read VF memory or clobber XMM16-31.
	void spillHomes()
	{
		if (!homesEnabled || !mVUgetVFHomeMap(index).lazy)
			return;
		mVUstoreVFHomes(index);
		if (profileBlock)
			profileBlock->vf_home_spills += mVUgetVFHomeMap(index).count;
	}

	// Refills the home registers after a helper call clobbered XMM16-31.
	// Memory is current there because homes are write-through.
	void reloadHomes()
	{
		if (!homesEnabled)
			return;
		mVUloadVFHomes(index);
		if (profileBlock)
			profileBlock->vf_home_refills += mVUgetVFHomeMap(index).count;
	}

	// Host registers XMM16-31 are caller-saved and clobbered by helpers:
	// forget every copy at calls, flushes and block boundaries.
	void invalidateHighCopies()
	{
		for (HighCopy& copy : highCopies)
			copy.vf = -1;
		touchedSinceInvalidate = 0;
	}

	// Fully resets the regalloc by clearing all cached data
	void reset(bool cop2mode)
	{
		invalidateHighCopies();
		touchedInBlock = 0;
		// PCSX2_AVX512_NO_HIGHCOPY=1 turns the copies off for A/B testing.
		static const bool highCopiesDisabled = [] {
			const char* env = std::getenv("PCSX2_AVX512_NO_HIGHCOPY");
			return env && env[0] == '1';
		}();
		highCopiesEnabled = !cop2mode && !highCopiesDisabled && x86Emitter::avx512.HasCore() && !CHECK_VU_SOFT(index);
		homesEnabled = highCopiesEnabled && mVUgetVFHomeMap(index).count > 0;
		homeBase = homesEnabled ? mVUgetVFHomeMap(index).count : 0;
		// we run this at the of cop2, so don't free fprs
		regAllocCOP2 = false;

		for (int i = 0; i < xmmTotal; i++)
			clearReg(i);
		for (int i = 0; i < gprTotal; i++)
			clearGPR(i);
		softNonExtendedMask.fill(0);
		softNonExtendedMask[0] = 0xf;

		counter = 0;
		regAllocCOP2 = cop2mode;
		pxmmregs = cop2mode ? xmmregs : nullptr;

		if (cop2mode)
		{
			for (int i = 0; i < xmmTotal; i++)
			{
				if (!pxmmregs[i].inuse || pxmmregs[i].type != XMMTYPE_VFREG)
					continue;

				// we shouldn't have any temp registers in here.. except for PQ, which
				// isn't allocated here yet.
				// pxAssertRel(fprregs[i].reg >= 0, "Valid full register preserved");
				if (pxmmregs[i].reg >= 0)
				{
					MVURALOG("Preserving VF reg %d in host reg %d across instruction\n", pxmmregs[i].reg, i);
					pxAssert(pxmmregs[i].reg >= 0);
					pxmmregs[i].needed = false;
					xmmMap[i].isNeeded = false;
					xmmMap[i].VFreg = pxmmregs[i].reg;
					xmmMap[i].xyzw = ((pxmmregs[i].mode & MODE_WRITE) != 0) ? 0xf : 0x0;
				}
			}

			for (int i = 0; i < gprTotal; i++)
			{
				if (!x86regs[i].inuse || x86regs[i].type != X86TYPE_VIREG)
					continue;

				// pxAssertRel(armregs[i].reg >= 0, "Valid full register preserved");
				if (x86regs[i].reg >= 0)
				{
					MVURALOG("Preserving VI reg %d in host reg %d across instruction\n", x86regs[i].reg, i);
					x86regs[i].needed = false;
					gprMap[i].isNeeded = false;
					gprMap[i].isZeroExtended = false;
					gprMap[i].VIreg = x86regs[i].reg;
					gprMap[i].dirty = ((x86regs[i].mode & MODE_WRITE) != 0);
				}
			}
		}

		gprMap[RTEXTPTR.GetId()].usable = !xGetTextPtr();
		gprMap[RFASTMEMBASE.GetId()].usable = !cop2mode || !CHECK_FASTMEM;
	}

	int getXmmCount()
	{
		return xmmTotal + 1;
	}

	int getFreeXmmCount()
	{
		int count = 0;

		for (int i = 0; i < xmmTotal; i++)
		{
			if (!xmmMap[i].isNeeded && (xmmMap[i].VFreg < 0))
				count++;
		}

		return count;
	}

	bool hasRegVF(int vfreg)
	{
		for (int i = 0; i < xmmTotal; i++)
		{
			if (xmmMap[i].VFreg == vfreg)
				return true;
		}

		return false;
	}

	int getRegVF(int i)
	{
		return (i < xmmTotal) ? xmmMap[i].VFreg : -1;
	}

	u8 getSoftNonExtendedMask(int vfreg) const
	{
		return (vfreg >= 0 && vfreg < static_cast<int>(softNonExtendedMask.size())) ?
			softNonExtendedMask[vfreg] : 0;
	}

	void markSoftNonExtended(int vfreg, int xyzw)
	{
		if (vfreg > 0 && vfreg < static_cast<int>(softNonExtendedMask.size()))
			softNonExtendedMask[vfreg] |= static_cast<u8>(xyzw & 0xf);
	}

	int getGPRCount()
	{
		return gprTotal;
	}

	int getFreeGPRCount()
	{
		int count = 0;

		for (int i = 0; i < gprTotal; i++)
		{
			if (!gprMap[i].usable && (gprMap[i].VIreg < 0))
				count++;
		}

		return count;
	}

	bool hasRegVI(int vireg)
	{
		for (int i = 0; i < gprTotal; i++)
		{
			if (gprMap[i].VIreg == vireg)
				return true;
		}

		return false;
	}

	int getRegVI(int i)
	{
		return (i < gprTotal) ? gprMap[i].VIreg : -1;
	}

	// Flushes all allocated registers (i.e. writes-back to memory all modified registers).
	// If clearState is 0, then it keeps cached reg data valid
	// If clearState is 1, then it invalidates all cached reg data after write-back
	void flushAll(bool clearState = true)
	{
		invalidateHighCopies();
		for (int i = 0; i < xmmTotal; i++)
		{
			writeBackReg(xmm(i));
			if (clearState)
				clearReg(i);
		}

		for (int i = 0; i < gprTotal; i++)
		{
			writeBackReg(xRegister32(i), true);
			if (clearState)
				clearGPR(i);
		}
	}

	void flushCallerSavedGPRs()
	{
		for (int i = 0; i < gprTotal; i++)
		{
			if (!xRegister32::IsCallerSaved(i))
				continue;

			writeBackReg(xRegister32(i), true);
			clearGPR(i);
		}
	}

	void flushCallerSavedRegisters(bool clearNeeded = false)
	{
		invalidateHighCopies();
		for (int i = 0; i < xmmTotal; i++)
		{
			if (!xRegisterSSE::IsCallerSaved(i))
				continue;

			writeBackReg(xmm(i));
			if (clearNeeded || !xmmMap[i].isNeeded)
				clearReg(i);
		}

		for (int i = 0; i < gprTotal; i++)
		{
			if (!xRegister32::IsCallerSaved(i))
				continue;

			writeBackReg(xRegister32(i), true);
			if (clearNeeded || !gprMap[i].isNeeded)
				clearGPR(i);
		}
	}

	void flushPartialForCOP2()
	{
		invalidateHighCopies();
		for (int i = 0; i < xmmTotal; i++)
		{
			microMapXMM& clear = xmmMap[i];

			// toss away anything which is not a full cached register
			if (pxmmregs[i].inuse && pxmmregs[i].type == XMMTYPE_VFREG)
			{
				// Should've been done in clearNeeded()
				if (clear.xyzw != 0 && clear.xyzw != 0xf)
					writeBackReg(xRegisterSSE::GetInstance(i), false);

				if (clear.VFreg <= 0)
				{
					// temps really shouldn't be here..
					_freeXMMreg(i);
				}
			}

			// needed gets cleared in iCore.
			clear = {-1, 0, 0, false, false};
		}

		for (int i = 0; i < gprTotal; i++)
		{
			microMapGPR& clear = gprMap[i];
			if (clear.VIreg < 0)
				clearGPR(i);
		}
	}

	void TDwritebackAll()
	{
		// NOTE: We don't clear state here, this happens in an optional branch

		for (int i = 0; i < xmmTotal; i++)
		{
			microMapXMM& mapX = xmmMap[xmm(i).Id];

			if ((mapX.VFreg > 0) && mapX.xyzw) // Reg was modified and not Temp or vf0
			{
				if (mapX.VFreg == 33)
					xMOVSS(ptr32[&getVI(REG_I)], xmm(i));
				else if (mapX.VFreg == 32)
					profiledSaveReg(xmm(i), 32, ptr[&regs().ACC], mapX.xyzw, 1);
				else
					profiledSaveReg(xmm(i), mapX.VFreg, ptr[&getVF(mapX.VFreg)], mapX.xyzw, 1);
			}
		}

		for (int i = 0; i < gprTotal; i++)
			writeBackReg(xRegister32(i), false);
	}

	bool checkVFClamp(int regId)
	{
		if (regId != xmmPQ.Id && ((xmmMap[regId].VFreg == 33 && !EmuConfig.Gamefixes.IbitHack) || xmmMap[regId].isZero))
			return false;
		else
			return true;
	}

	bool checkCachedReg(int regId)
	{
		if (regId < xmmTotal)
			return xmmMap[regId].VFreg >= 0;
		else
			return false;
	}

	bool checkCachedGPR(int regId)
	{
		if (regId < gprTotal)
			return gprMap[regId].VIreg >= 0 || gprMap[regId].isNeeded;
		else
			return false;
	}

	void clearReg(const xmm& reg) { clearReg(reg.Id); }
	void clearReg(int regId)
	{
		microMapXMM& clear = xmmMap[regId];
		if (regAllocCOP2 && (clear.isNeeded || clear.VFreg >= 0))
		{
			pxAssert(pxmmregs[regId].type == XMMTYPE_VFREG);
			pxmmregs[regId].inuse = false;
		}

		clear = {-1, 0, 0, false, false};
	}

	void clearRegVF(int VFreg)
	{
		for (int i = 0; i < xmmTotal; i++)
		{
			if (xmmMap[i].VFreg == VFreg)
				clearReg(i);
		}
	}

	void clearRegCOP2(int xmmReg)
	{
		if (regAllocCOP2)
			clearReg(xmmReg);
	}

	void updateCOP2AllocState(int rn)
	{
		if (!regAllocCOP2)
			return;

		const bool dirty = (xmmMap[rn].VFreg > 0 && xmmMap[rn].xyzw != 0);
		pxAssert(pxmmregs[rn].type == XMMTYPE_VFREG);
		pxmmregs[rn].reg = xmmMap[rn].VFreg;
		pxmmregs[rn].mode = dirty ? (MODE_READ | MODE_WRITE) : MODE_READ;
		pxmmregs[rn].needed = xmmMap[rn].isNeeded;
	}

	// Writes back modified reg to memory.
	// If all vectors modified, then keeps the VF reg cached in the xmm register.
	// If reg was not modified, then keeps the VF reg cached in the xmm register.
	void writeBackReg(const xmm& reg, bool invalidateRegs = true)
	{
		microMapXMM& mapX = xmmMap[reg.Id];

		if ((mapX.VFreg > 0) && mapX.xyzw) // Reg was modified and not Temp or vf0
		{
			dropHighCopy(mapX.VFreg); // memory is about to change
			if (mapX.VFreg == 33)
				xMOVSS(ptr32[&getVI(REG_I)], reg);
			else if (mapX.VFreg == 32)
				profiledSaveReg(reg, 32, ptr[&regs().ACC], mapX.xyzw, true);
			else
				profiledSaveReg(reg, mapX.VFreg, ptr[&getVF(mapX.VFreg)], mapX.xyzw, true);

			if (invalidateRegs)
			{
				for (int i = 0; i < xmmTotal; i++)
				{
					microMapXMM& mapI = xmmMap[i];

					if ((i == reg.Id) || mapI.isNeeded)
						continue;

					if (mapI.VFreg == mapX.VFreg)
					{
						if (mapI.xyzw && mapI.xyzw < 0xf)
							DevCon.Error("microVU Error: writeBackReg() [%d]", mapI.VFreg);
						clearReg(i); // Invalidate any Cached Regs of same vf Reg
					}
				}
			}
			if (mapX.xyzw == 0xf) // Make Cached Reg if All Vectors were Modified
			{
				mapX.count    = counter;
				mapX.xyzw     = 0;
				mapX.isNeeded = false;
				updateCOP2AllocState(reg.Id);
				return;
			}
			clearReg(reg);
		}
		else if (mapX.xyzw) // Clear reg if modified and is VF0 or temp reg...
		{
			clearReg(reg);
		}
	}

	// Use this when done using the allocated register, it clears its "Needed" status.
	// The register that was written to, should be cleared before other registers are cleared.
	// This is to guarantee proper merging between registers... When a written-to reg is cleared,
	// it invalidates other cached registers of the same VF reg, and merges partial-vector
	// writes into them.
	void clearNeeded(const xmm& reg)
	{

		if ((reg.Id < 0) || (reg.Id >= xmmTotal)) // Sometimes xmmPQ hits this
			return;

		microMapXMM& clear = xmmMap[reg.Id];
		clear.isNeeded = false;
		if (clear.xyzw) // Reg was modified
		{
			if (clear.VFreg > 0)
			{
				int mergeRegs = 0;
				if (clear.xyzw < 0xf) // Try to merge partial writes
					mergeRegs = 1;
				for (int i = 0; i < xmmTotal; i++) // Invalidate any other read-only regs of same vfReg
				{
					if (i == reg.Id)
						continue;
					microMapXMM& mapI = xmmMap[i];
					if (mapI.VFreg == clear.VFreg)
					{
						if (mapI.xyzw && mapI.xyzw < 0xf)
						{
							DevCon.Error("microVU Error: clearNeeded() [%d]", mapI.VFreg);
						}
						if (mergeRegs == 1)
						{
							mVUmergeRegs(xmm(i), reg, clear.xyzw, true);
							mapI.xyzw  = 0xf;
							mapI.count = counter;
							mergeRegs  = 2;
							updateCOP2AllocState(i);
						}
						else
							clearReg(i); // Clears when mergeRegs is 0 or 2
					}
				}
				if (mergeRegs == 2) // Clear Current Reg if Merged
					clearReg(reg);
				else if (mergeRegs == 1) // Write Back Partial Writes if couldn't merge
					writeBackReg(reg);
			}
			else
				clearReg(reg); // If Reg was temp or vf0, then invalidate itself
		}
		else if (regAllocCOP2 && clear.VFreg < 0)
		{
			// free on the EE side
			pxAssert(pxmmregs[reg.Id].type == XMMTYPE_VFREG);
			pxmmregs[reg.Id].inuse = false;
		}
	}

	// vfLoadReg  = VF reg to be loaded to the xmm register
	// vfWriteReg = VF reg that the returned xmm register will be considered as
	// xyzw       = XYZW vectors that will be modified (and loaded)
	// cloneWrite = When loading a reg that will be written to, it copies it to its own xmm reg instead of overwriting the cached one...
	// Notes:
	// To load a temp reg use the default param values, vfLoadReg = -1 and vfWriteReg = -1.
	// To load a full reg which won't be modified and you want cached, specify vfLoadReg >= 0 and vfWriteReg = -1
	// To load a reg which you don't want written back or cached, specify vfLoadReg >= 0 and vfWriteReg = 0
	const xmm& allocReg(int vfLoadReg = -1, int vfWriteReg = -1, int xyzw = 0, bool cloneWrite = true)
	{
		//DevCon.WriteLn("vfLoadReg = %02d, vfWriteReg = %02d, xyzw = %x, clone = %d",vfLoadReg,vfWriteReg,xyzw,(int)cloneWrite);
		counter++;
		if (vfWriteReg > 0 && vfWriteReg < static_cast<int>(softNonExtendedMask.size()))
			softNonExtendedMask[vfWriteReg] &= static_cast<u8>(~xyzw);
		if (vfLoadReg >= 0) // Search For Cached Regs
		{
			for (int i = 0; i < xmmTotal; i++)
			{
				const xmm& xmmI = xmm::GetInstance(i);
				microMapXMM& mapI = xmmMap[i];
				if ((mapI.VFreg == vfLoadReg)
				 && (!mapI.xyzw                           // Reg Was Not Modified
				  || (mapI.VFreg && (mapI.xyzw == 0xf)))) // Reg Had All Vectors Modified and != VF0
				{
					int z = i;
					if (vfWriteReg >= 0) // Reg will be modified
					{
						if (cloneWrite) // Clone Reg so as not to use the same Cached Reg
						{
							z = findFreeReg(vfWriteReg);
							const xmm& xmmZ = xmm::GetInstance(z);
							writeBackReg(xmmZ);
							if (z != i)
								stashHighCopy(z, vfLoadReg);

							if (xyzw == 4)
								xPSHUF.D(xmmZ, xmmI, 1);
							else if (xyzw == 2)
								xPSHUF.D(xmmZ, xmmI, 2);
							else if (xyzw == 1)
								xPSHUF.D(xmmZ, xmmI, 3);
							else if (z != i)
								xMOVAPS(xmmZ, xmmI);

							mapI.count = counter; // Reg i was used, so update counter
						}
						else // Don't clone reg, but shuffle to adjust for SS ops
						{
							if ((vfLoadReg != vfWriteReg) || (xyzw != 0xf))
								writeBackReg(xmmI);

							if (xyzw == 4)
								xPSHUF.D(xmmI, xmmI, 1);
							else if (xyzw == 2)
								xPSHUF.D(xmmI, xmmI, 2);
							else if (xyzw == 1)
								xPSHUF.D(xmmI, xmmI, 3);
						}
						xmmMap[z].VFreg = vfWriteReg;
						xmmMap[z].xyzw = xyzw;
						xmmMap[z].isZero = (vfLoadReg == 0);
					}
					xmmMap[z].count = counter;
					xmmMap[z].isNeeded = true;
					updateCOP2AllocState(z);
					if (vfWriteReg > 0)
						dropHighCopy(vfWriteReg);
					touchVF(vfLoadReg);
					touchVF(vfWriteReg);

					return xmm::GetInstance(z);
				}
			}
		}
		int x = findFreeReg((vfWriteReg >= 0) ? vfWriteReg : vfLoadReg);
		const xmm& xmmX = xmm::GetInstance(x);
		writeBackReg(xmmX);
		stashHighCopy(x, vfLoadReg);
		const u32 highLoadsBefore = profileBlock ? profileBlock->vf_high_loads : 0;

		if (vfWriteReg >= 0) // Reg Will Be Modified (allow partial reg loading)
		{
			if ((vfLoadReg == 0) && !(xyzw & 1))
				xPXOR(xmmX, xmmX);
			else if (vfLoadReg == 33)
				loadIreg(xmmX, xyzw);
			else if (vfLoadReg > 0 && !_XYZWss(xyzw) && loadHighCopy(xmmX, vfLoadReg))
				; // full reload served from a home or high copy
			else if (vfLoadReg > 0 && _XYZWss(xyzw) && loadHomeLane(xmmX, vfLoadReg, xyzw))
				; // single-lane load served from a home register
			else if (vfLoadReg == 32)
				mVUloadReg(xmmX, ptr[&regs().ACC], xyzw);
			else if (vfLoadReg >= 0)
				mVUloadReg(xmmX, ptr[&getVF(vfLoadReg)], xyzw);

			xmmMap[x].VFreg = vfWriteReg;
			xmmMap[x].xyzw  = xyzw;
		}
		else // Reg Will Not Be Modified (always load full reg for caching)
		{
			if (vfLoadReg == 33)
				loadIreg(xmmX, 0xf);
			else if (vfLoadReg > 0 && loadHighCopy(xmmX, vfLoadReg))
				; // reload served from a high copy
			else if (vfLoadReg == 32)
				xMOVAPS (xmmX, ptr128[&regs().ACC]);
			else if (vfLoadReg >= 0)
				xMOVAPS (xmmX, ptr128[&getVF(vfLoadReg)]);

			xmmMap[x].VFreg = vfLoadReg;
			xmmMap[x].xyzw  = 0;
		}
		if (profileBlock && vfLoadReg >= 1 && vfLoadReg <= 32)
		{
			profileBlock->vf_loads++;
			profileBlock->vf_requests_by_reg[vfLoadReg]++;
			if (profileBlock->vf_high_loads == highLoadsBefore)
				profileBlock->vf_memory_loads_by_reg[vfLoadReg]++;
			const u64 bit = 1ull << vfLoadReg;
			if (profileBlock->vf_high_loads != highLoadsBefore)
				; // served from a copy
			else if (!(touchedInBlock & bit))
				profileBlock->vf_first_loads++;
			else if (!(touchedSinceInvalidate & bit))
				profileBlock->vf_flushed_reloads++;
			else if (vfWriteReg >= 0 && _XYZWss(xyzw))
				profileBlock->vf_partial_reloads++;
			else
				profileBlock->vf_lost_reloads++;
		}
		touchVF(vfLoadReg);
		touchVF(vfWriteReg);
		if (vfWriteReg > 0)
			dropHighCopy(vfWriteReg); // the old value is about to change
		xmmMap[x].isZero = (vfLoadReg == 0);
		xmmMap[x].count    = counter;
		xmmMap[x].isNeeded = true;
		updateCOP2AllocState(x);
		return xmmX;
	}

	void clearGPR(const xRegisterInt& reg) { clearGPR(reg.GetId()); }

	void clearGPR(int regId)
	{
		microMapGPR& clear = gprMap[regId];

		if (regAllocCOP2)
		{
			if (x86regs[regId].inuse && x86regs[regId].type == X86TYPE_VIREG)
			{
				pxAssert(x86regs[regId].reg == clear.VIreg);
				_freeX86regWithoutWriteback(regId);
			}
		}

		clear.VIreg = -1;
		clear.count = 0;
		clear.isNeeded = 0;
		clear.dirty = false;
		clear.isZeroExtended = false;
	}

	void clearGPRCOP2(int regId)
	{
		if (regAllocCOP2)
			clearGPR(regId);
	}

	void updateCOP2AllocState(const xRegisterInt& reg)
	{
		if (!regAllocCOP2)
			return;

		const u32 rn = reg.GetId();
		const bool dirty = (gprMap[rn].VIreg >= 0 && gprMap[rn].dirty);
		pxAssert(x86regs[rn].type == X86TYPE_VIREG);
		x86regs[rn].reg = gprMap[rn].VIreg;
		x86regs[rn].counter = gprMap[rn].count;
		x86regs[rn].mode = dirty ? (MODE_READ | MODE_WRITE) : MODE_READ;
		x86regs[rn].needed = gprMap[rn].isNeeded;
	}

	void writeBackReg(const xRegisterInt& reg, bool clearDirty)
	{
		microMapGPR& mapX = gprMap[reg.GetId()];
		pxAssert(mapX.usable || !mapX.dirty);
		if (mapX.dirty)
		{
			pxAssert(mapX.VIreg > 0);
			if (mapX.VIreg < 16)
				xMOV(ptr16[&getVI(mapX.VIreg)], xRegister16(reg));
			if (clearDirty)
			{
				mapX.dirty = false;
				updateCOP2AllocState(reg);
			}
		}
	}

	void clearNeeded(const xRegisterInt& reg)
	{
		pxAssert(reg.GetId() < gprTotal);
		microMapGPR& clear = gprMap[reg.GetId()];
		clear.isNeeded = false;
		if (regAllocCOP2)
			x86regs[reg.GetId()].needed = false;
	}

	void unbindAnyVIAllocations(int reg, bool& backup)
	{
		for (int i = 0; i < gprTotal; i++)
		{
			microMapGPR& mapI = gprMap[i];
			if (mapI.VIreg == reg)
			{
				if (backup)
				{
					writeVIBackup(xRegister32(i));
					backup = false;
				}

				// if it's needed, we just unbind the allocation and preserve it, otherwise clear
				if (mapI.isNeeded)
				{
					MVURALOG("  unbind %d to %d for write\n", i, reg);
					if (regAllocCOP2)
					{
						pxAssert(x86regs[i].type == X86TYPE_VIREG && x86regs[i].reg == static_cast<u8>(mapI.VIreg));
						x86regs[i].reg = -1;
					}

					mapI.VIreg = -1;
					mapI.dirty = false;
					mapI.isZeroExtended = false;
				}
				else
				{
					MVURALOG("  clear %d to %d for write\n", i, reg);
					clearGPR(i);
				}

				// shouldn't be any others...
				for (int j = i + 1; j < gprTotal; j++)
				{
					pxAssert(gprMap[j].VIreg != reg);
				}

				break;
			}
		}
	}

	const xRegister32& allocGPR(int viLoadReg = -1, int viWriteReg = -1, bool backup = false, bool zext_if_dirty = false)
	{
		// TODO: When load != write, we should check whether load is used later, and if so, copy it.

		//DevCon.WriteLn("viLoadReg = %02d, viWriteReg = %02d, backup = %d",viLoadReg,viWriteReg,(int)backup);
		const int this_counter = regAllocCOP2 ? (g_x86AllocCounter++) : (counter++);
		if (viLoadReg == 0 || viWriteReg == 0)
		{
			// write zero register as temp and discard later
			if (viWriteReg == 0)
			{
				int x = findFreeGPR(-1);
				const xRegister32& gprX = xRegister32::GetInstance(x);
				writeBackReg(gprX, true);
				xXOR(gprX, gprX);
				gprMap[x].VIreg = -1;
				gprMap[x].dirty = false;
				gprMap[x].count = this_counter;
				gprMap[x].isNeeded = true;
				gprMap[x].isZeroExtended = true;
				MVURALOG("  alloc zero to scratch %d\n", x);
				return gprX;
			}
		}

		if (viLoadReg >= 0) // Search For Cached Regs
		{
			for (int i = 0; i < gprTotal; i++)
			{
				microMapGPR& mapI = gprMap[i];
				if (mapI.VIreg == viLoadReg)
				{
					// Do this first, there is a case where when loadReg != writeReg, the findFreeGPR can steal the loadReg
					gprMap[i].count = this_counter;

					if (viWriteReg >= 0) // Reg will be modified
					{
						if (viLoadReg != viWriteReg)
						{
							// kill any allocations of viWriteReg
							unbindAnyVIAllocations(viWriteReg, backup);

							// allocate a new register for writing to
							int x = findFreeGPR(viWriteReg);
							const xRegister32& gprX = xRegister32::GetInstance(x);

							writeBackReg(gprX, true);

							// writeReg not cached, needs backing up
							if (backup && gprMap[x].VIreg != viWriteReg)
							{
								xMOVZX(gprX, ptr16[&getVI(viWriteReg)]);
								writeVIBackup(gprX);
								backup = false;
							}

							if (zext_if_dirty)
								xMOVZX(gprX, xRegister16(i));
							else
								xMOV(gprX, xRegister32(i));
							gprMap[x].isZeroExtended = zext_if_dirty;
							MVURALOG("  clone write %d in %d to %d for %d\n", viLoadReg, i, x, viWriteReg);
							std::swap(x, i);
						}
						else
						{
							// writing to it, no longer zero extended
							gprMap[i].isZeroExtended = false;
						}

						gprMap[i].VIreg = viWriteReg;
						gprMap[i].dirty = true;
					}
					else if (zext_if_dirty && !gprMap[i].isZeroExtended)
					{
						xMOVZX(xRegister32(i), xRegister16(i));
						gprMap[i].isZeroExtended = true;
					}

					gprMap[i].isNeeded = true;

					if (backup)
						writeVIBackup(xRegister32(i));

					if (regAllocCOP2)
					{
						pxAssert(x86regs[i].inuse && x86regs[i].type == X86TYPE_VIREG);
						x86regs[i].reg = gprMap[i].VIreg;
						x86regs[i].mode = gprMap[i].dirty ? (MODE_WRITE | MODE_READ) : (MODE_READ);
					}

					MVURALOG("  returning cached in %d\n", i);
					return xRegister32::GetInstance(i);
				}
			}
		}

		if (viWriteReg >= 0) // Writing a new value, make sure this register isn't cached already
			unbindAnyVIAllocations(viWriteReg, backup);

		int x = findFreeGPR(viLoadReg);
		const xRegister32& gprX = xRegister32::GetInstance(x);
		writeBackReg(gprX, true);

		// Special case: we need to back up the destination register, but it might not have already
		// been cached. If so, we need to load the old value from state and back it up. Otherwise,
		// it's going to get lost when we eventually write this register back.
		if (backup && viLoadReg >= 0 && viWriteReg > 0 && viLoadReg != viWriteReg)
		{
			xMOVZX(gprX, ptr16[&getVI(viWriteReg)]);
			writeVIBackup(gprX);
			backup = false;
		}

		if (viLoadReg > 0)
			xMOVZX(gprX, ptr16[&getVI(viLoadReg)]);
		else if (viLoadReg == 0)
			xXOR(gprX, gprX);

		gprMap[x].VIreg = viLoadReg;
		gprMap[x].isZeroExtended = true;
		if (viWriteReg >= 0)
		{
			gprMap[x].VIreg = viWriteReg;
			gprMap[x].dirty = true;
			gprMap[x].isZeroExtended = false;

			if (backup)
			{
				if (viLoadReg < 0 && viWriteReg > 0)
					xMOVZX(gprX, ptr16[&getVI(viWriteReg)]);

				writeVIBackup(gprX);
			}
		}

		gprMap[x].count = this_counter;
		gprMap[x].isNeeded = true;

		if (regAllocCOP2)
		{
			pxAssert(x86regs[x].inuse && x86regs[x].type == X86TYPE_VIREG);
			x86regs[x].reg = gprMap[x].VIreg;
			x86regs[x].mode = gprMap[x].dirty ? (MODE_WRITE | MODE_READ) : (MODE_READ);
		}

		MVURALOG("  returning new %d\n", x);
		return gprX;
	}

	void moveVIToGPR(const xRegisterInt& reg, int vi, bool signext = false)
	{
		pxAssert(vi >= 0);
		if (vi == 0)
		{
			xXOR(xRegister32(reg), xRegister32(reg));
			return;
		}

		// TODO: Check liveness/usedness before allocating.
		// TODO: Check whether zero-extend is needed everywhere heae. Loadstores are.
		const xRegister32& srcreg = allocGPR(vi);
		if (signext)
			xMOVSX(xRegister32(reg), xRegister16(srcreg));
		else
			xMOVZX(xRegister32(reg), xRegister16(srcreg));
		clearNeeded(srcreg);
	}
};
