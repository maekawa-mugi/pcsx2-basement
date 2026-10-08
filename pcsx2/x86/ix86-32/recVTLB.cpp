// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Common.h"
#include "EEMemory.h"
#include "vtlb.h"
#include "x86/iCore.h"
#include "x86/iR5900.h"

#include "common/Perf.h"

#include <array>
#include <memory>
#include <vector>

using namespace vtlb_private;
using namespace x86Emitter;

namespace vtlb_private
{
	static void DynGen_DirectRead(u32 bits, bool sign);
	static void DynGen_DirectWrite(u32 bits);
} // namespace vtlb_private

static u8* GetIndirectDispatcherPtr(int mode, int operandsize, int sign);

// we need enough for a 32-bit jump forwards (5 bytes)
static constexpr u32 LOADSTORE_PADDING = 5;

//#define LOG_STORES

static u32 GetAllocatedGPRBitmask()
{
	u32 mask = 0;
	for (u32 i = 0; i < iREGCNT_GPR; i++)
	{
		if (x86regs[i].inuse)
			mask |= (1u << i);
	}
	return mask;
}

static u32 GetAllocatedXMMBitmask()
{
	u32 mask = 0;
	for (u32 i = 0; i < iREGCNT_XMM_EVEX; i++)
	{
		if (xmmregs[i].inuse)
			mask |= (1u << i);
	}
	return mask;
}

struct FullTLBCompilerState
{
	std::array<_x86regs, iREGCNT_GPR> x86;
	std::array<_xmmregs, iREGCNT_XMM_EVEX> xmm;
	std::array<GPR_reg64, 32> constants;
	u32 has_const = 0;
	u32 flushed_const = 0;
};

struct FullTLBFastmemReadSlowpath
{
	FullTLBCompilerState state;
	std::unique_ptr<xForwardJNZ32> alignment_jump;
	uptr code_address = 0;
	uptr resume_address = 0;
	u32 code_size = 0;
	u32 guest_pc = 0;
	u32 context_pc = 0;
	u32 scaled_cycles = 0;
	u32 gpr_bitmask = 0;
	u32 fpr_bitmask = 0;
	u8 address_register = 0;
	u8 size_in_bits = 0;
	bool is_signed = false;
	bool is_xmm = false;
	bool branch_delay = false;
};

static std::vector<FullTLBFastmemReadSlowpath> s_full_tlb_fastmem_read_slowpaths;

static FullTLBCompilerState CaptureFullTLBCompilerState()
{
	FullTLBCompilerState state;
	std::memcpy(state.x86.data(), x86regs, sizeof(x86regs));
	std::memcpy(state.xmm.data(), xmmregs, sizeof(xmmregs));
	std::memcpy(state.constants.data(), g_cpuConstRegs, sizeof(g_cpuConstRegs));
	state.has_const = g_cpuHasConstReg;
	state.flushed_const = g_cpuFlushedConstReg;
	return state;
}

static void RestoreFullTLBCompilerState(const FullTLBCompilerState& state)
{
	std::memcpy(x86regs, state.x86.data(), sizeof(x86regs));
	std::memcpy(xmmregs, state.xmm.data(), sizeof(xmmregs));
	std::memcpy(g_cpuConstRegs, state.constants.data(), sizeof(g_cpuConstRegs));
	g_cpuHasConstReg = state.has_const;
	g_cpuFlushedConstReg = state.flushed_const;
}

void vtlb_BeginFullTLBFastmemBlock()
{
	s_full_tlb_fastmem_read_slowpaths.clear();
}

static const void* GetFullTLBReadFunction(u32 bits)
{
	switch (bits)
	{
		case 8:
			return reinterpret_cast<const void*>(EEMemory::RecompilerRead8);
		case 16:
			return reinterpret_cast<const void*>(EEMemory::RecompilerRead16);
		case 32:
			return reinterpret_cast<const void*>(EEMemory::RecompilerRead32);
		case 64:
			return reinterpret_cast<const void*>(EEMemory::RecompilerRead64);
		case 128:
			return reinterpret_cast<const void*>(EEMemory::RecompilerRead128);
		default:
			pxFailRel("Invalid Full TLB read size");
			return nullptr;
	}
}

static const void* GetFullTLBWriteFunction(u32 bits)
{
	switch (bits)
	{
		case 8:
			return reinterpret_cast<const void*>(EEMemory::RecompilerWrite8);
		case 16:
			return reinterpret_cast<const void*>(EEMemory::RecompilerWrite16);
		case 32:
			return reinterpret_cast<const void*>(EEMemory::RecompilerWrite32);
		case 64:
			return reinterpret_cast<const void*>(EEMemory::RecompilerWrite64);
		case 128:
			return reinterpret_cast<const void*>(EEMemory::RecompilerWrite128);
		default:
			pxFailRel("Invalid Full TLB write size");
			return nullptr;
	}
}

static void EmitFullTLBReadResultExtension(u32 bits, bool sign)
{
	switch (bits)
	{
		case 8:
			sign ? xMOVSX(rax, al) : xMOVZX(eax, al);
			break;
		case 16:
			sign ? xMOVSX(rax, ax) : xMOVZX(eax, ax);
			break;
		case 32:
			if (sign)
				xMOVSX(rax, eax);
			break;
		case 64:
			break;
		default:
			pxFailRel("Invalid Full TLB scalar read size");
			break;
	}
}

static bool CanUseFullTLBDirectSegmentFastPath()
{
	// The block-entry context guard makes this compile-time specialization safe until
	// Status changes. Individual kseg0 accesses still honor EEMemory's cache semantics.
	return EEMmu::IsKernelMode(cpuRegs.CP0.n.Status.val);
}

static bool IsFullTLBDirectSegmentAddress(u32 address)
{
	if (address >= 0xa0000000 && address < 0xc0000000)
		return true;
	return address >= 0x80000000 && address < 0xa0000000 &&
	       (!CHECK_CACHE || (cpuRegs.CP0.n.Config & 0x7) != 3);
}

static bool CanUseFullTLBKseg0FastPath()
{
	return !CHECK_CACHE || (cpuRegs.CP0.n.Config & 0x7) != 3;
}

static bool CanUseFullTLBInlineMemoryPath()
{
	// The apparent inline-memory corruption was an instruction-ordering bug when a
	// branch delay slot crossed into an unmapped page. Keep the shared C++ path as a
	// diagnostic reference, while normal execution uses the tagged read/write micro-TLB
	// and direct-segment fast paths (needs proper testing with broader Linux workloads).
	return !EmuConfig.Cpu.EnableFullTLBDiagnosticTrace;
}

static bool CanUseFullTLBSpeculativeFastmemRead()
{
	// This first speculative stage is intentionally kernel-only. KSEG RAM is host
	// mapped, while TLB/MMIO/invalid addresses fault once and are patched to an
	// architectural Full TLB slow path. Cache-emulated KSEG0 is excluded until the
	// fast mapping can encode that cache behavior.
	return EmuConfig.Cpu.IsFullTLBKsegFastmemEnabled() &&
		CanUseFullTLBDirectSegmentFastPath() && CanUseFullTLBKseg0FastPath() &&
		CanUseFullTLBInlineMemoryPath() && !vtlb_IsFaultingPC(pc);
}

static void QueueFullTLBFastmemReadSlowpath(std::unique_ptr<xForwardJNZ32> alignment_jump,
	uptr code_address, u32 code_size, uptr resume_address, int address_register,
	u32 bits, bool sign, bool xmm)
{
	FullTLBFastmemReadSlowpath slowpath;
	slowpath.state = CaptureFullTLBCompilerState();
	slowpath.alignment_jump = std::move(alignment_jump);
	slowpath.code_address = code_address;
	slowpath.resume_address = resume_address;
	slowpath.code_size = code_size;
	slowpath.guest_pc = pc;
	slowpath.context_pc = g_recompilingDelaySlot ? (pc + 4) : pc;
	slowpath.scaled_cycles = recGetFullTLBScaledBlockCycles();
	slowpath.gpr_bitmask = GetAllocatedGPRBitmask();
	slowpath.fpr_bitmask = GetAllocatedXMMBitmask();
	slowpath.address_register = static_cast<u8>(address_register);
	slowpath.size_in_bits = static_cast<u8>(bits);
	slowpath.is_signed = sign;
	slowpath.is_xmm = xmm;
	slowpath.branch_delay = g_recompilingDelaySlot;
	s_full_tlb_fastmem_read_slowpaths.emplace_back(std::move(slowpath));
}

static int DynGenFullTLBSpeculativeFastmemReadNonQuad(u32 bits, bool sign, bool xmm,
	int addr_reg, vtlb_ReadRegAllocCallback dest_reg_alloc)
{
	pxAssert(bits <= 64);
	pxAssert(addr_reg >= 0);

	std::unique_ptr<xForwardJNZ32> alignment_jump;
	const u32 alignment_mask = (bits / 8) - 1;
	if (alignment_mask != 0)
	{
		xTEST(xRegister32(addr_reg), alignment_mask);
		alignment_jump = std::make_unique<xForwardJNZ32>();
	}

	const xAddressReg x86addr(addr_reg);
	const u8* code_start = x86Ptr;
	switch (bits)
	{
		case 8:
			sign ? xMOVSX(rax, ptr8[RFASTMEMBASE + x86addr]) :
			       xMOVZX(eax, ptr8[RFASTMEMBASE + x86addr]);
			break;
		case 16:
			sign ? xMOVSX(rax, ptr16[RFASTMEMBASE + x86addr]) :
			       xMOVZX(eax, ptr16[RFASTMEMBASE + x86addr]);
			break;
		case 32:
			sign ? xMOVSX(rax, ptr32[RFASTMEMBASE + x86addr]) :
			       xMOV(eax, ptr32[RFASTMEMBASE + x86addr]);
			break;
		case 64:
			xMOV(rax, ptr64[RFASTMEMBASE + x86addr]);
			break;
		default:
			pxFailRel("Invalid speculative Full TLB read size");
			break;
	}

	const u32 padding = LOADSTORE_PADDING -
		std::min<u32>(static_cast<u32>(x86Ptr - code_start), LOADSTORE_PADDING);
	for (u32 i = 0; i < padding; i++)
		xNOP();

	const uptr resume_address = reinterpret_cast<uptr>(x86Ptr);
	QueueFullTLBFastmemReadSlowpath(std::move(alignment_jump),
		reinterpret_cast<uptr>(code_start), static_cast<u32>(x86Ptr - code_start),
		resume_address, addr_reg, bits, sign, xmm);

	if (!xmm)
	{
		const int reg = dest_reg_alloc ? dest_reg_alloc() : (_freeX86reg(eax), eax.GetId());
		if (reg != eax.GetId())
			xMOV(xRegister64(reg), rax);
		return reg;
	}

	pxAssert(bits == 32);
	const int reg = dest_reg_alloc ? dest_reg_alloc() : (_freeXMMreg(0), 0);
	xMOVDZX(xRegisterSSE(reg), eax);
	return reg;
}

static void EmitFullTLBFastmemReadSlowpath(FullTLBFastmemReadSlowpath& slowpath)
{
#ifdef _WIN32
	static constexpr u32 SHADOW_SIZE = 32;
#else
	static constexpr u32 SHADOW_SIZE = 0;
#endif
	static constexpr u32 RESULT_SIZE = 16;

	u32 num_gprs = 0;
	u32 num_fprs = 0;
	for (u32 i = 0; i < iREGCNT_GPR; i++)
	{
		if (slowpath.state.x86[i].inuse && xRegisterBase::IsCallerSaved(i))
			num_gprs++;
	}
	for (u32 i = 0; i < iREGCNT_XMM_EVEX; i++)
	{
		if (slowpath.state.xmm[i].inuse && xRegisterSSE::IsCallerSaved(i))
			num_fprs++;
	}

	const u32 gpr_save_size = ((num_gprs + 1) & ~1u) * 8;
	const u32 stack_size = SHADOW_SIZE + RESULT_SIZE + gpr_save_size + (num_fprs * 16);
	if (stack_size != 0)
		xSUB(rsp, stack_size);

	u32 stack_offset = SHADOW_SIZE + RESULT_SIZE;
	for (u32 i = 0; i < iREGCNT_XMM_EVEX; i++)
	{
		if (slowpath.state.xmm[i].inuse && xRegisterSSE::IsCallerSaved(i))
		{
			if (i >= iREGCNT_XMM)
				xVMOVDQA32(ptr128[rsp + stack_offset], xRegisterSSE(i));
			else
				xMOVAPS(ptr128[rsp + stack_offset], xRegisterSSE(i));
			stack_offset += 16;
		}
	}
	for (u32 i = 0; i < iREGCNT_GPR; i++)
	{
		if (slowpath.state.x86[i].inuse && xRegisterBase::IsCallerSaved(i))
		{
			xMOV(ptr64[rsp + stack_offset], xRegister64(i));
			stack_offset += 8;
		}
	}

	if (slowpath.address_register != arg1reg.GetId())
		xMOV(arg1regd, xRegister32(slowpath.address_register));
	xMOV(ptr32[&cpuRegs.pc], slowpath.context_pc);
	xMOV(ptr32[&cpuRegs.branch], slowpath.branch_delay ? 1 : 0);
	xFastCall(GetFullTLBReadFunction(slowpath.size_in_bits));
	EmitFullTLBReadResultExtension(slowpath.size_in_bits, slowpath.is_signed);
	xMOV(ptr64[rsp + SHADOW_SIZE], rax);

	stack_offset = SHADOW_SIZE + RESULT_SIZE;
	for (u32 i = 0; i < iREGCNT_XMM_EVEX; i++)
	{
		if (slowpath.state.xmm[i].inuse && xRegisterSSE::IsCallerSaved(i))
		{
			if (i >= iREGCNT_XMM)
				xVMOVDQA32(xRegisterSSE(i), ptr128[rsp + stack_offset]);
			else
				xMOVAPS(xRegisterSSE(i), ptr128[rsp + stack_offset]);
			stack_offset += 16;
		}
	}
	for (u32 i = 0; i < iREGCNT_GPR; i++)
	{
		if (slowpath.state.x86[i].inuse && xRegisterBase::IsCallerSaved(i))
		{
			xMOV(xRegister64(i), ptr64[rsp + stack_offset]);
			stack_offset += 8;
		}
	}

	xMOV(ptr32[&cpuRegs.branch], 0);
	RestoreFullTLBCompilerState(slowpath.state);
	recEmitFullTLBAccessFaultExitForThunk(stack_size, slowpath.scaled_cycles);

	// Success rejoins immediately after the faultable native read. The guest
	// destination write is emitted there, so a fault never commits a load result.
	xMOV(rax, ptr64[rsp + SHADOW_SIZE]);
	if (stack_size != 0)
		xADD(rsp, stack_size);
	xJMP(reinterpret_cast<const void*>(slowpath.resume_address));
}

void vtlb_EndFullTLBFastmemBlock()
{
	if (s_full_tlb_fastmem_read_slowpaths.empty())
		return;

	const FullTLBCompilerState block_end_state = CaptureFullTLBCompilerState();
	for (FullTLBFastmemReadSlowpath& slowpath : s_full_tlb_fastmem_read_slowpaths)
	{
		u8* const slow_path = xGetAlignedCallTarget();
		if (slowpath.alignment_jump)
			slowpath.alignment_jump->SetTarget();

		RestoreFullTLBCompilerState(slowpath.state);
		EmitFullTLBFastmemReadSlowpath(slowpath);
		RestoreFullTLBCompilerState(block_end_state);

		vtlb_AddLoadStoreInfo(slowpath.code_address, slowpath.code_size,
			slowpath.guest_pc, slowpath.gpr_bitmask, slowpath.fpr_bitmask,
			slowpath.address_register, 0, slowpath.size_in_bits,
			slowpath.is_signed, true, slowpath.is_xmm,
			reinterpret_cast<uptr>(slow_path));
	}
	s_full_tlb_fastmem_read_slowpaths.clear();
}

static bool TryDynGenFullTLBConstReadNonQuad(u32 bits, bool sign, bool xmm, u32 address,
	vtlb_ReadRegAllocCallback dest_reg_alloc, int* result_reg)
{
	if (!CanUseFullTLBDirectSegmentFastPath() || !IsFullTLBDirectSegmentAddress(address) ||
		(address & ((bits / 8) - 1)) != 0)
	{
		return false;
	}

	const u32 paddr = address & 0x1fffffff;
	const void* const pointer = vtlb_GetPhyPtr(paddr);
	if (!pointer)
		return false;

	EE::Profiler.EmitConstMem(address);
	if (!xmm)
	{
		const int reg = dest_reg_alloc ? dest_reg_alloc() : (_freeX86reg(eax), eax.GetId());
		const xRegister64 dst(reg);
		switch (bits)
		{
			case 8:
				sign ? xMOVSX(dst, ptr8[pointer]) : xMOVZX(xRegister32(dst), ptr8[pointer]);
				break;
			case 16:
				sign ? xMOVSX(dst, ptr16[pointer]) : xMOVZX(xRegister32(dst), ptr16[pointer]);
				break;
			case 32:
				sign ? xMOVSX(dst, ptr32[pointer]) : xMOV(xRegister32(dst), ptr32[pointer]);
				break;
			case 64:
				xMOV(dst, ptr64[pointer]);
				break;
			default:
				pxFailRel("Invalid Full TLB direct read size");
				break;
		}
		*result_reg = reg;
	}
	else
	{
		pxAssert(bits == 32);
		const int reg = dest_reg_alloc ? dest_reg_alloc() : (_freeXMMreg(0), 0);
		xMOVDZX(xRegisterSSE(reg), ptr32[pointer]);
		*result_reg = reg;
	}
	return true;
}

static bool TryDynGenFullTLBConstReadQuad(u32 address, vtlb_ReadRegAllocCallback dest_reg_alloc,
	int* result_reg)
{
	if (!CanUseFullTLBDirectSegmentFastPath() || !IsFullTLBDirectSegmentAddress(address) ||
		(address & 0xf) != 0)
	{
		return false;
	}

	const u32 paddr = address & 0x1fffffff;
	const void* const pointer = vtlb_GetPhyPtr(paddr);
	if (!pointer)
		return false;

	EE::Profiler.EmitConstMem(address);
	const int reg = dest_reg_alloc ? dest_reg_alloc() : (_freeXMMreg(0), 0);
	if (reg >= 0)
		xMOVAPS(xRegisterSSE(reg), ptr128[pointer]);
	*result_reg = reg;
	return true;
}

static bool TryDynGenFullTLBConstWrite(u32 bits, bool xmm, u32 address, int value_reg)
{
	if (!CanUseFullTLBDirectSegmentFastPath() || !IsFullTLBDirectSegmentAddress(address) ||
		(address & ((bits / 8) - 1)) != 0)
	{
		return false;
	}

	const u32 paddr = address & 0x1fffffff;
	if (paddr >= Ps2MemSize::ExposedRam)
		return false;

	EE::Profiler.EmitConstMem(address);
	void* const pointer = &eeMem->Main[paddr];
	if (!xmm)
	{
		switch (bits)
		{
			case 8:
				xMOV(ptr8[pointer], xRegister8(xRegister32(value_reg)));
				break;
			case 16:
				xMOV(ptr16[pointer], xRegister16(value_reg));
				break;
			case 32:
				xMOV(ptr32[pointer], xRegister32(value_reg));
				break;
			case 64:
				xMOV(ptr64[pointer], xRegister64(value_reg));
				break;
			default:
				pxFailRel("Invalid Full TLB direct write size");
				break;
		}
	}
	else if (bits == 32)
	{
		xMOVSS(ptr32[pointer], xRegisterSSE(value_reg));
	}
	else
	{
		pxAssert(bits == 128);
		xMOVAPS(ptr128[pointer], xRegisterSSE(value_reg));
	}
	xADD(ptr32[EEMemory::GetPhysicalWriteGenerationAddress(paddr)], 1);
	return true;
}

static int GetFullTLBOperandSizeIndex(u32 bits)
{
	switch (bits)
	{
		case 8:
			return 0;
		case 16:
			return 1;
		case 32:
			return 2;
		case 64:
			return 3;
		case 128:
			return 4;
		default:
			pxFailRel("Invalid Full TLB access size");
			return 0;
	}
}

static void PreserveFullTLBXMMForCall(int xmm_reg)
{
	if (xmm_reg < 0 || !xRegisterSSE::IsCallerSaved(xmm_reg))
		return;
	xSUB(rsp, 16);
	xMOVAPS(ptr128[rsp], xRegisterSSE(xmm_reg));
}

static void RestoreFullTLBXMMFromCall(int xmm_reg)
{
	if (xmm_reg < 0 || !xRegisterSSE::IsCallerSaved(xmm_reg))
		return;
	xMOVAPS(xRegisterSSE(xmm_reg), ptr128[rsp]);
	xADD(rsp, 16);
}

static void DynGenFullTLBCheckAlignment(const xRegister32& original_address,
	EEMmu::AccessType access_type, u32 alignment_mask, int preserved_xmm_reg = -1)
{
	if (alignment_mask == 0)
		return;

	xTEST(original_address, alignment_mask);
	xForwardJZ32 aligned;
	xMOV(arg1regd, original_address);
	xMOV(arg2regd, static_cast<u32>(access_type));
	PreserveFullTLBXMMForCall(preserved_xmm_reg);
	recPrepareFullTLBAccessContext();
	xFastCall(reinterpret_cast<const void*>(EEMemory::RaiseRecompilerAddressError));
	RestoreFullTLBXMMFromCall(preserved_xmm_reg);
	recFinishFullTLBAccessContext();
	aligned.SetTarget();
}

// Returns a packed {attributes, translated 4K page} value in RAX and, when the
// page is directly accessible, its host base in arg4reg. Cache hits execute
// entirely in generated code; only a miss enters the MMU resolver.
static void DynGenFullTLBTranslate(const xRegister32& original_address, EEMmu::AccessType access_type,
	u32 alignment_mask, int preserved_xmm_reg = -1)
{
	DynGenFullTLBCheckAlignment(original_address, access_type, alignment_mask, preserved_xmm_reg);
	_freeX86reg(arg4reg.GetId());

	std::optional<xForwardJNE32> translated_segment_miss;
	std::optional<xForwardJump32> translated_segment_ready;
	if (CanUseFullTLBDirectSegmentFastPath())
	{
		xMOV(eax, original_address);
		xAND(eax, CanUseFullTLBKseg0FastPath() ? 0xc0000000 : 0xe0000000);
		xCMP(eax, CanUseFullTLBKseg0FastPath() ? 0x80000000 : 0xa0000000);
		translated_segment_miss.emplace();
		xMOV(eax, original_address);
		xAND(eax, 0x1ffff000);
		xXOR(arg4reg, arg4reg);
		// Most Linux kernel accesses target main RAM through KSEG0/KSEG1. Publish the
		// host page here so the read/write path can skip a second physical-map lookup.
		// Extra-memory mode changes flush recompiled blocks, but needs proper testing.
		xCMP(eax, Ps2MemSize::ExposedRam);
		xForwardJAE32 direct_segment_not_ram;
		xLoadFarAddr(arg4reg, eeMem->Main);
		xADD(arg4reg, rax);
		direct_segment_not_ram.SetTarget();
		translated_segment_ready.emplace();
		translated_segment_miss->SetTarget();
	}

	xMOV(eax, original_address);
	xSHR(eax, VTLB_PAGE_BITS);
	xMOV(arg1regd, eax);
	xAND(eax, EEMemory::RECOMPILER_TRANSLATION_CACHE_SIZE - 1);
	xSHL(eax, 5);
	_freeX86reg(arg3reg.GetId());
	EEMemory::RecompilerJitTranslationCacheEntry* const cache =
		EEMemory::GetRecompilerJitTranslationCacheBase(access_type);
	const xAddressVoid cache_entry = xComplexAddress(arg3reg, cache, rax);
	xCMP(ptr32[cache_entry + static_cast<sptr>(offsetof(EEMemory::RecompilerJitTranslationCacheEntry, virtual_page))],
		arg1regd);
	xForwardJNE32 cache_miss_page;
	// The key comes from the block-entry guard rather than an immediate, so a block compiled
	// under one ASID stays correct when the guard lets it run under another.
	xMOV(arg2regd, ptr32[&EEMemory::g_recompilerJitContextKeys[0]]);
	if (!EEMmu::IsUserMode(cpuRegs.CP0.n.Status.val))
	{
		// User mode faults on every address above 0x80000000, and those entries carry the
		// high-segment marker, so the kuseg key alone is exact there.
		xTEST(original_address, original_address);
		xForwardJNS8 kuseg_key;
		xMOV(arg2regd, ptr32[&EEMemory::g_recompilerJitContextKeys[1]]);
		kuseg_key.SetTarget();
	}
	xCMP(ptr32[cache_entry + offsetof(EEMemory::RecompilerJitTranslationCacheEntry, context_key)], arg2regd);
	xForwardJNE32 cache_miss_context;
	xMOV(arg1regd, ptr32[cache_entry + offsetof(EEMemory::RecompilerJitTranslationCacheEntry, tlb_entry_index)]);
	xCMP(arg1regd, EEMemory::RECOMPILER_TRANSLATION_NO_TLB_ENTRY);
	xForwardJE8 cache_generation_matches;
	xMOV(xRegister32(arg4reg.GetId()),
		ptr32[xComplexAddress(arg2reg, const_cast<u32*>(EEMmu::GetTLBEntryGenerationBase()), arg1reg * 4)]);
	xCMP(xRegister32(arg4reg.GetId()),
		ptr32[cache_entry + offsetof(EEMemory::RecompilerJitTranslationCacheEntry, translation_generation)]);
	xForwardJNE32 cache_miss_generation;
	cache_generation_matches.SetTarget();
	xMOV(arg4reg, ptr64[cache_entry + offsetof(EEMemory::RecompilerJitTranslationCacheEntry, host_page)]);
	xMOV(rax, ptr64[cache_entry + offsetof(EEMemory::RecompilerJitTranslationCacheEntry, translation)]);
	xForwardJump32 translation_ready;

	cache_miss_page.SetTarget();
	cache_miss_generation.SetTarget();
	cache_miss_context.SetTarget();
	xMOV(arg1regd, original_address);
	xMOV(arg2regd, static_cast<u32>(access_type));
	PreserveFullTLBXMMForCall(preserved_xmm_reg);
	recPrepareFullTLBAccessContext();
	xFastCall(reinterpret_cast<const void*>(EEMemory::ResolveRecompilerJitTranslation));
	RestoreFullTLBXMMFromCall(preserved_xmm_reg);
	recFinishFullTLBAccessContext();
	// The resolver publishes host_page before virtual_page. Recompute the direct-mapped
	// entry after the call because all argument registers are caller-saved.
	xMOV(xRegister32(arg4reg.GetId()), original_address);
	xSHR(xRegister32(arg4reg.GetId()), VTLB_PAGE_BITS);
	xAND(xRegister32(arg4reg.GetId()), EEMemory::RECOMPILER_TRANSLATION_CACHE_SIZE - 1);
	xSHL(xRegister32(arg4reg.GetId()), 5);
	xMOV(arg4reg,
		ptr64[xComplexAddress(arg3reg, cache, arg4reg) +
			  offsetof(EEMemory::RecompilerJitTranslationCacheEntry, host_page)]);
	translation_ready.SetTarget();
	if (translated_segment_ready.has_value())
		translated_segment_ready->SetTarget();
}

template <typename FastPath, typename SlowPath>
static void DynGenFullTLBKsegFastmemPath(const xRegister32& original_address,
	EEMmu::AccessType access_type, u32 alignment_mask, int preserved_xmm_reg,
	const FastPath& fast_path, const SlowPath& slow_path)
{
	if (!EmuConfig.Cpu.IsFullTLBKsegFastmemEnabled() || !CanUseFullTLBDirectSegmentFastPath() ||
		!CanUseFullTLBInlineMemoryPath())
	{
		slow_path();
		return;
	}

	// KSEG0 and KSEG1 share the same physical 512 MiB direct window. When cache
	// emulation requires KSEG0's cache mode, restrict this shortcut to KSEG1.
	xMOV(eax, original_address);
	xAND(eax, CanUseFullTLBKseg0FastPath() ? 0xc0000000 : 0xe0000000);
	xCMP(eax, CanUseFullTLBKseg0FastPath() ? 0x80000000 : 0xa0000000);
	xForwardJNE32 segment_miss;

	xMOV(eax, original_address);
	xAND(eax, 0x1fffffff);
	xCMP(eax, Ps2MemSize::ExposedRam);
	xForwardJAE32 ram_miss;

	DynGenFullTLBCheckAlignment(original_address, access_type, alignment_mask, preserved_xmm_reg);
	fast_path();
	xForwardJump32 done;

	segment_miss.SetTarget();
	ram_miss.SetTarget();
	slow_path();
	done.SetTarget();
}

static void DynGenFullTLBPhysicalRead(u32 bits, bool sign, const xRegister32& original_address)
{
	// RAX holds packed translation here. Test attributes before replacing the virtual address.
	xMOV(arg3reg, rax);
	xSHR(arg3reg, 32);
	xTEST(xRegister32(arg3reg.GetId()), EEMemory::RECOMPILER_TRANSLATION_NO_ACCESS);
	xForwardJNZ32 no_access;
	std::optional<xForwardJE32> cached_access;
	std::optional<xForwardJump32> cached_done;
	if (CHECK_CACHE)
	{
		xMOV(arg1regd, xRegister32(arg3reg.GetId()));
		xAND(arg1regd, EEMemory::RECOMPILER_TRANSLATION_CACHE_MODE_MASK);
		xCMP(arg1regd, 3U << EEMemory::RECOMPILER_TRANSLATION_CACHE_MODE_SHIFT);
		cached_access.emplace();
	}
	xTEST(arg4reg, arg4reg);
	xForwardJZ32 host_page_miss;
	xMOV(arg1regd, original_address);
	xAND(arg1regd, VTLB_PAGE_MASK);
	xADD(arg1reg, arg4reg);
	vtlb_private::DynGen_DirectRead(bits, sign);
	xForwardJump32 host_page_done;
	host_page_miss.SetTarget();
	xTEST(xRegister32(arg3reg.GetId()), EEMemory::RECOMPILER_TRANSLATION_SCRATCHPAD);
	xForwardJNZ32 scratchpad;

	xMOV(arg1regd, original_address);
	xAND(arg1regd, VTLB_PAGE_MASK);
	xMOV(original_address, eax);
	xADD(original_address, arg1regd);
	xMOV(eax, original_address);
	xSHR(eax, VTLB_PAGE_BITS);
	xMOV(rax, ptrNative[xComplexAddress(arg3reg, vtlbdata.pmap, rax * wordsize)]);
	xTEST(rax, rax);
	xForwardJS32 physical_handler;
	xMOV(arg1regd, original_address);
	xAND(arg1regd, VTLB_PAGE_MASK);
	xADD(arg1reg, rax);
	vtlb_private::DynGen_DirectRead(bits, sign);
	xForwardJump32 physical_direct_done;

	physical_handler.SetTarget();
	xMOV(arg1regd, original_address);
	xADD(arg1reg, rax);
	xFastCall(GetIndirectDispatcherPtr(0, GetFullTLBOperandSizeIndex(bits), sign && bits < 64));
	xForwardJump32 physical_handler_done;

	scratchpad.SetTarget();
	xMOV(arg1regd, original_address);
	xAND(arg1regd, VTLB_PAGE_MASK);
	xMOV(original_address, eax);
	xADD(original_address, arg1regd);
	xAND(original_address, 0x3fff);
	xLoadFarAddr(rax, eeMem->Scratch);
	xMOV(arg1regd, original_address);
	xADD(arg1reg, rax);
	vtlb_private::DynGen_DirectRead(bits, sign);
	xForwardJump32 scratchpad_done;

	if (CHECK_CACHE)
	{
		cached_access->SetTarget();
		xMOV(arg1regd, original_address);
		recPrepareFullTLBAccessContext();
		xFastCall(GetFullTLBReadFunction(bits));
		recFinishFullTLBAccessContext();
		cached_done.emplace();
	}

	no_access.SetTarget();
	if (bits == 128)
		xPXOR(xmm0, xmm0);
	else
		xXOR(rax, rax);
	physical_direct_done.SetTarget();
	physical_handler_done.SetTarget();
	scratchpad_done.SetTarget();
	host_page_done.SetTarget();
	if (cached_done.has_value())
		cached_done->SetTarget();
}

static int DynGenFullTLBReadNonQuad(u32 bits, bool sign, bool xmm, int addr_reg,
	vtlb_ReadRegAllocCallback dest_reg_alloc, const u32* addr_const)
{
	if (!addr_const && CanUseFullTLBSpeculativeFastmemRead())
	{
		pxAssert(addr_reg == arg1regd.GetId());
		EE::Profiler.EmitMem(addr_reg);
		return DynGenFullTLBSpeculativeFastmemReadNonQuad(bits, sign, xmm, addr_reg, dest_reg_alloc);
	}

	const u32 load_signature = cpuRegs.code & 0xffff0000U;
	const bool trace_gp_related_load = EmuConfig.Cpu.EnableFullTLBDiagnosticTrace && bits == 32 && sign &&
	                                   (load_signature == 0x8f990000U || // lw t9, imm(gp)
										   load_signature == 0x8fbc0000U); // lw gp, imm(sp)
	const int original_reg = _allocX86reg(X86TYPE_TEMP, 0, MODE_CALLEESAVED);
	const xRegister32 original_address(original_reg);
	if (!addr_const)
	{
		pxAssert(addr_reg == arg1regd.GetId());
		xMOV(original_address, arg1regd);
	}
	iFlushCall(FLUSH_FULLVTLB);
	if (addr_const)
	{
		EE::Profiler.EmitConstMem(*addr_const);
		xMOV(original_address, *addr_const);
	}
	else
	{
		EE::Profiler.EmitMem(addr_reg);
	}
	if (trace_gp_related_load)
		xMOV(ptr32[EEMemory::GetFullTLBDiagnosticReadVAddrAddress()], original_address);

	DynGenFullTLBKsegFastmemPath(original_address, EEMmu::AccessType::Load, (bits / 8) - 1, -1,
		[&]() {
			xMOV(arg1regd, original_address);
			xADD(arg1reg, RFASTMEMBASE);
			vtlb_private::DynGen_DirectRead(bits, sign);
		},
		[&]() {
			if (CanUseFullTLBInlineMemoryPath())
			{
				DynGenFullTLBTranslate(original_address, EEMmu::AccessType::Load, (bits / 8) - 1);
				DynGenFullTLBPhysicalRead(bits, sign, original_address);
			}
			else
			{
				xMOV(arg1regd, original_address);
				recPrepareFullTLBAccessContext();
				xFastCall(GetFullTLBReadFunction(bits));
				recFinishFullTLBAccessContext();
			}
		});
	if (trace_gp_related_load)
	{
		xMOV(arg1reg, rax);
		xMOV(arg2regd, pc - 4);
		xMOV(xRegister32(arg3reg.GetId()), cpuRegs.code);
		xFastCall(reinterpret_cast<const void*>(EEMemory::RecordFullTLBDiagnosticReadResult));
	}
	_freeX86reg(original_reg);
	EmitFullTLBReadResultExtension(bits, sign);

	if (!xmm)
	{
		const int reg = dest_reg_alloc ? dest_reg_alloc() : (_freeX86reg(eax), eax.GetId());
		xMOV(xRegister64(reg), rax);
		return reg;
	}

	pxAssert(bits == 32);
	const int reg = dest_reg_alloc ? dest_reg_alloc() : (_freeXMMreg(0), 0);
	xMOVDZX(xRegisterSSE(reg), eax);
	return reg;
}

static int DynGenFullTLBReadQuad(int addr_reg, vtlb_ReadRegAllocCallback dest_reg_alloc,
	const u32* addr_const)
{
	const int original_reg = _allocX86reg(X86TYPE_TEMP, 0, MODE_CALLEESAVED);
	const xRegister32 original_address(original_reg);
	if (!addr_const)
	{
		pxAssert(addr_reg == arg1regd.GetId());
		xMOV(original_address, arg1regd);
	}
	iFlushCall(FLUSH_FULLVTLB);
	if (addr_const)
	{
		EE::Profiler.EmitConstMem(*addr_const);
		xMOV(original_address, *addr_const);
	}
	else
	{
		EE::Profiler.EmitMem(addr_reg);
	}

	DynGenFullTLBKsegFastmemPath(original_address, EEMmu::AccessType::Load, 0xf, -1,
		[&]() {
			xMOV(arg1regd, original_address);
			xADD(arg1reg, RFASTMEMBASE);
			vtlb_private::DynGen_DirectRead(128, false);
		},
		[&]() {
			if (CanUseFullTLBInlineMemoryPath())
			{
				DynGenFullTLBTranslate(original_address, EEMmu::AccessType::Load, 0xf);
				DynGenFullTLBPhysicalRead(128, false, original_address);
			}
			else
			{
				xMOV(arg1regd, original_address);
				recPrepareFullTLBAccessContext();
				xFastCall(GetFullTLBReadFunction(128));
				recFinishFullTLBAccessContext();
			}
		});
	_freeX86reg(original_reg);
	const int reg = dest_reg_alloc ? dest_reg_alloc() : (_freeXMMreg(0), 0);
	if (reg >= 0)
		xMOVAPS(xRegisterSSE(reg), xmm0);
	return reg;
}

static void DynGenFullTLBPrepareWriteValue(u32 bits, bool xmm, int saved_value_reg)
{
	if (!xmm)
	{
		xMOV(arg2reg, xRegister64(saved_value_reg));
	}
	else if (bits == 32)
	{
		xMOVD(arg2regd, xRegisterSSE(saved_value_reg));
	}
	else
	{
		pxAssert(bits == 128);
		const xRegisterSSE argreg(xRegisterSSE::GetArgRegister(1, 0));
		if (argreg.GetId() != saved_value_reg)
		{
			_freeXMMreg(argreg.GetId());
			xMOVAPS(argreg, xRegisterSSE(saved_value_reg));
		}
	}
}

alignas(32) static constexpr u32 s_full_tlb_merge_left_masks32[4] = {
	0xffffff00, 0xffff0000, 0xff000000, 0};
alignas(32) static constexpr u32 s_full_tlb_merge_right_masks32[4] = {
	0, 0xff, 0xffff, 0xffffff};
alignas(64) static constexpr u64 s_full_tlb_merge_left_masks64[8] = {
	0xffffffffffffff00ULL, 0xffffffffffff0000ULL, 0xffffffffff000000ULL,
	0xffffffff00000000ULL, 0xffffff0000000000ULL, 0xffff000000000000ULL,
	0xff00000000000000ULL, 0};
alignas(64) static constexpr u64 s_full_tlb_merge_right_masks64[8] = {
	0, 0xff, 0xffff, 0xffffff, 0xffffffffULL, 0xffffffffffULL,
	0xffffffffffffULL, 0xffffffffffffffULL};

static void DynGenFullTLBMergeValue(u32 bits, bool left, const xRegister32& index,
	const xRegister64& saved_value)
{
	pxAssert(bits == 32 || bits == 64);
	if (bits == 32)
	{
		const u32* masks = left ? s_full_tlb_merge_left_masks32 : s_full_tlb_merge_right_masks32;
		xAND(eax, ptr32[xComplexAddress(arg3reg, const_cast<u32*>(masks), xAddressReg(index) * 4)]);
		xMOV(arg2regd, xRegister32(saved_value));
	}
	else
	{
		const u64* masks = left ? s_full_tlb_merge_left_masks64 : s_full_tlb_merge_right_masks64;
		xAND(rax, ptr64[xComplexAddress(arg3reg, const_cast<u64*>(masks), xAddressReg(index) * 8)]);
		xMOV(arg2reg, saved_value);
	}

	xMOV(ecx, index);
	if (left)
	{
		xXOR(ecx, (bits / 8) - 1);
		xSHL(ecx, 3);
		bits == 32 ? xSHR(arg2regd, cl) : xSHR(arg2reg, cl);
	}
	else
	{
		xSHL(ecx, 3);
		bits == 32 ? xSHL(arg2regd, cl) : xSHL(arg2reg, cl);
	}
	bits == 32 ? xOR(arg2regd, eax) : xOR(arg2reg, rax);
}

static void DynGenFullTLBMergePhysicalWrite(u32 bits, bool left, const xRegister32& original_address,
	const xRegister32& merge_index, const xRegister64& saved_value, const void* cache_function)
{
	xMOV(arg3reg, rax);
	xSHR(arg3reg, 32);
	xTEST(xRegister32(arg3reg.GetId()), EEMemory::RECOMPILER_TRANSLATION_NO_ACCESS);
	xForwardJNZ32 no_access_done;
	std::optional<xForwardJE32> cached_access;
	if (CHECK_CACHE)
	{
		xMOV(arg1regd, xRegister32(arg3reg.GetId()));
		xAND(arg1regd, EEMemory::RECOMPILER_TRANSLATION_CACHE_MODE_MASK);
		xCMP(arg1regd, 3U << EEMemory::RECOMPILER_TRANSLATION_CACHE_MODE_SHIFT);
		cached_access.emplace();
	}
	xTEST(xRegister32(arg3reg.GetId()), EEMemory::RECOMPILER_TRANSLATION_SCRATCHPAD);
	xForwardJNZ32 scratchpad;

	// The virtual address was aligned before translation, so the packed page plus
	// its low 12 bits is also the aligned physical RMW address.
	xMOV(arg1regd, original_address);
	xAND(arg1regd, VTLB_PAGE_MASK);
	xMOV(original_address, eax);
	xADD(original_address, arg1regd);
	xMOV(eax, original_address);
	xSHR(eax, VTLB_PAGE_BITS);
	xMOV(rax, ptrNative[xComplexAddress(arg3reg, vtlbdata.pmap, rax * wordsize)]);
	xTEST(rax, rax);
	xForwardJS32 physical_handler;
	xMOV(arg1regd, original_address);
	xAND(arg1regd, VTLB_PAGE_MASK);
	xADD(arg1reg, rax);
	vtlb_private::DynGen_DirectRead(bits, false);
	DynGenFullTLBMergeValue(bits, left, merge_index, saved_value);
	xMOV(eax, original_address);
	xSHR(eax, VTLB_PAGE_BITS);
	xADD(ptr32[xComplexAddress(arg3reg, EEMemory::GetPhysicalWriteGenerationBase(), rax * 4)], 1);
	xMOV(eax, original_address);
	xSHR(eax, VTLB_PAGE_BITS);
	xMOV(rax, ptrNative[xComplexAddress(arg3reg, vtlbdata.pmap, rax * wordsize)]);
	xMOV(arg1regd, original_address);
	xAND(arg1regd, VTLB_PAGE_MASK);
	xADD(arg1reg, rax);
	vtlb_private::DynGen_DirectWrite(bits);
	xForwardJump32 physical_direct_done;

	physical_handler.SetTarget();
	xMOV(arg1regd, original_address);
	xADD(arg1reg, rax);
	xFastCall(GetIndirectDispatcherPtr(0, GetFullTLBOperandSizeIndex(bits), 0));
	DynGenFullTLBMergeValue(bits, left, merge_index, saved_value);
	xMOV(eax, original_address);
	xSHR(eax, VTLB_PAGE_BITS);
	xADD(ptr32[xComplexAddress(arg3reg, EEMemory::GetPhysicalWriteGenerationBase(), rax * 4)], 1);
	xMOV(eax, original_address);
	xSHR(eax, VTLB_PAGE_BITS);
	xMOV(rax, ptrNative[xComplexAddress(arg3reg, vtlbdata.pmap, rax * wordsize)]);
	xMOV(arg1regd, original_address);
	xADD(arg1reg, rax);
	xFastCall(GetIndirectDispatcherPtr(1, GetFullTLBOperandSizeIndex(bits), 0));
	xForwardJump32 physical_handler_done;

	scratchpad.SetTarget();
	xMOV(arg1regd, original_address);
	xAND(arg1regd, VTLB_PAGE_MASK);
	xMOV(original_address, eax);
	xADD(original_address, arg1regd);
	xAND(original_address, 0x3fff);
	xLoadFarAddr(rax, eeMem->Scratch);
	xMOV(arg1regd, original_address);
	xADD(arg1reg, rax);
	vtlb_private::DynGen_DirectRead(bits, false);
	DynGenFullTLBMergeValue(bits, left, merge_index, saved_value);
	xADD(ptr32[EEMemory::GetScratchpadWriteGenerationAddress()], 1);
	xLoadFarAddr(rax, eeMem->Scratch);
	xMOV(arg1regd, original_address);
	xADD(arg1reg, rax);
	vtlb_private::DynGen_DirectWrite(bits);
	xForwardJump32 scratchpad_done;

	if (CHECK_CACHE)
	{
		cached_access->SetTarget();
		xMOV(arg1regd, original_address);
		xADD(arg1regd, merge_index);
		xMOV(arg2reg, saved_value);
		recPrepareFullTLBAccessContext();
		xFastCall(cache_function);
		recFinishFullTLBAccessContext();
	}
	no_access_done.SetTarget();
	physical_direct_done.SetTarget();
	physical_handler_done.SetTarget();
	scratchpad_done.SetTarget();
}

static void DynGenFullTLBMergeWrite(u32 bits, bool left, int addr_reg, int value_reg,
	const u32* addr_const, const void* cache_function)
{
	const u32 address_mask = (bits / 8) - 1;
	const int original_reg = _allocX86reg(X86TYPE_TEMP, 0, MODE_CALLEESAVED);
	const int index_reg = _allocX86reg(X86TYPE_TEMP, 0, MODE_CALLEESAVED);
	const int saved_value_reg = _allocX86reg(X86TYPE_TEMP, 0, MODE_CALLEESAVED);
	const xRegister32 original_address(original_reg);
	const xRegister32 merge_index(index_reg);
	const xRegister64 saved_value(saved_value_reg);
	xMOV(saved_value, xRegister64(value_reg));
	if (!addr_const)
	{
		pxAssert(addr_reg == arg1regd.GetId());
		xMOV(original_address, arg1regd);
		xMOV(merge_index, arg1regd);
	}
	iFlushCall(FLUSH_FULLVTLB);
	if (addr_const)
	{
		EE::Profiler.EmitConstMem(*addr_const);
		xMOV(original_address, *addr_const);
		xMOV(merge_index, *addr_const);
	}
	else
	{
		EE::Profiler.EmitMem(addr_reg);
	}
	xAND(merge_index, address_mask);
	xAND(original_address, ~address_mask);
	if (CanUseFullTLBInlineMemoryPath())
	{
		DynGenFullTLBTranslate(original_address, EEMmu::AccessType::Store, 0);
		DynGenFullTLBMergePhysicalWrite(
			bits, left, original_address, merge_index, saved_value, cache_function);
	}
	else
	{
		xMOV(arg1regd, original_address);
		xADD(arg1regd, merge_index);
		xMOV(arg2reg, saved_value);
		recPrepareFullTLBAccessContext();
		xFastCall(cache_function);
		recFinishFullTLBAccessContext();
	}
	_freeX86reg(saved_value_reg);
	_freeX86reg(index_reg);
	_freeX86reg(original_reg);
}

static void DynGenFullTLBPhysicalWrite(u32 bits, bool xmm, const xRegister32& original_address,
	int saved_value_reg, const void* cache_function)
{
	xMOV(arg3reg, rax);
	xSHR(arg3reg, 32);
	xTEST(xRegister32(arg3reg.GetId()), EEMemory::RECOMPILER_TRANSLATION_NO_ACCESS);
	xForwardJNZ32 no_access_done;
	std::optional<xForwardJE32> cached_access;
	if (CHECK_CACHE)
	{
		xMOV(arg1regd, xRegister32(arg3reg.GetId()));
		xAND(arg1regd, EEMemory::RECOMPILER_TRANSLATION_CACHE_MODE_MASK);
		xCMP(arg1regd, 3U << EEMemory::RECOMPILER_TRANSLATION_CACHE_MODE_SHIFT);
		cached_access.emplace();
	}
	xTEST(arg4reg, arg4reg);
	xForwardJZ32 host_page_miss;
	xTEST(xRegister32(arg3reg.GetId()), EEMemory::RECOMPILER_TRANSLATION_SCRATCHPAD);
	xForwardJNZ32 host_scratchpad;
	xSHR(eax, VTLB_PAGE_BITS);
	xADD(ptr32[xComplexAddress(arg3reg, EEMemory::GetPhysicalWriteGenerationBase(), rax * 4)], 1);
	xForwardJump32 host_generation_done;
	host_scratchpad.SetTarget();
	xADD(ptr32[EEMemory::GetScratchpadWriteGenerationAddress()], 1);
	host_generation_done.SetTarget();
	xMOV(arg1regd, original_address);
	xAND(arg1regd, VTLB_PAGE_MASK);
	xADD(arg1reg, arg4reg);
	DynGenFullTLBPrepareWriteValue(bits, xmm, saved_value_reg);
	vtlb_private::DynGen_DirectWrite(bits);
	xForwardJump32 host_page_done;
	host_page_miss.SetTarget();
	xTEST(xRegister32(arg3reg.GetId()), EEMemory::RECOMPILER_TRANSLATION_SCRATCHPAD);
	xForwardJNZ32 scratchpad;

	xMOV(arg1regd, original_address);
	xAND(arg1regd, VTLB_PAGE_MASK);
	xMOV(original_address, eax);
	xADD(original_address, arg1regd);
	xMOV(eax, original_address);
	xSHR(eax, VTLB_PAGE_BITS);
	xADD(ptr32[xComplexAddress(arg3reg, EEMemory::GetPhysicalWriteGenerationBase(), rax * 4)], 1);
	xMOV(eax, original_address);
	xSHR(eax, VTLB_PAGE_BITS);
	xMOV(rax, ptrNative[xComplexAddress(arg3reg, vtlbdata.pmap, rax * wordsize)]);
	xTEST(rax, rax);
	xForwardJS32 physical_handler;
	xMOV(arg1regd, original_address);
	xAND(arg1regd, VTLB_PAGE_MASK);
	xADD(arg1reg, rax);
	DynGenFullTLBPrepareWriteValue(bits, xmm, saved_value_reg);
	vtlb_private::DynGen_DirectWrite(bits);
	xForwardJump32 physical_direct_done;

	physical_handler.SetTarget();
	xMOV(arg1regd, original_address);
	xADD(arg1reg, rax);
	DynGenFullTLBPrepareWriteValue(bits, xmm, saved_value_reg);
	xFastCall(GetIndirectDispatcherPtr(1, GetFullTLBOperandSizeIndex(bits), 0));
	xForwardJump32 physical_handler_done;

	scratchpad.SetTarget();
	xMOV(arg1regd, original_address);
	xAND(arg1regd, VTLB_PAGE_MASK);
	xMOV(original_address, eax);
	xADD(original_address, arg1regd);
	xAND(original_address, 0x3fff);
	xADD(ptr32[EEMemory::GetScratchpadWriteGenerationAddress()], 1);
	xLoadFarAddr(rax, eeMem->Scratch);
	xMOV(arg1regd, original_address);
	xADD(arg1reg, rax);
	DynGenFullTLBPrepareWriteValue(bits, xmm, saved_value_reg);
	vtlb_private::DynGen_DirectWrite(bits);
	xForwardJump32 scratchpad_done;

	if (CHECK_CACHE)
	{
		cached_access->SetTarget();
		xMOV(arg1regd, original_address);
		DynGenFullTLBPrepareWriteValue(bits, xmm, saved_value_reg);
		recPrepareFullTLBAccessContext();
		xFastCall(cache_function);
		recFinishFullTLBAccessContext();
	}
	no_access_done.SetTarget();
	physical_direct_done.SetTarget();
	physical_handler_done.SetTarget();
	scratchpad_done.SetTarget();
	host_page_done.SetTarget();
}

static void DynGenFullTLBWrite(u32 bits, bool xmm, int addr_reg, int value_reg,
	const u32* addr_const)
{
	const int original_reg = _allocX86reg(X86TYPE_TEMP, 0, MODE_CALLEESAVED);
	const xRegister32 original_address(original_reg);
	if (!addr_const)
	{
		pxAssert(addr_reg == arg1regd.GetId());
		xMOV(original_address, arg1regd);
	}
	int saved_value_reg;
	if (!xmm)
	{
		saved_value_reg = _allocX86reg(X86TYPE_TEMP, 0, MODE_CALLEESAVED);
		xMOV(xRegister64(saved_value_reg), xRegister64(value_reg));
	}
	iFlushCall(FLUSH_FULLVTLB);
	if (xmm)
	{
		saved_value_reg = _allocTempXMMreg(XMMT_INT);
		xMOVAPS(xRegisterSSE(saved_value_reg), xRegisterSSE(value_reg));
	}
	if (addr_const)
	{
		EE::Profiler.EmitConstMem(*addr_const);
		xMOV(original_address, *addr_const);
	}
	else
	{
		EE::Profiler.EmitMem(addr_reg);
	}

	DynGenFullTLBKsegFastmemPath(original_address, EEMmu::AccessType::Store, (bits / 8) - 1,
		xmm ? saved_value_reg : -1,
		[&]() {
			xMOV(eax, original_address);
			xAND(eax, 0x1fffffff);
			xSHR(eax, VTLB_PAGE_BITS);
			xADD(ptr32[xComplexAddress(arg3reg, EEMemory::GetPhysicalWriteGenerationBase(), rax * 4)], 1);
			xMOV(arg1regd, original_address);
			xADD(arg1reg, RFASTMEMBASE);
			DynGenFullTLBPrepareWriteValue(bits, xmm, saved_value_reg);
			vtlb_private::DynGen_DirectWrite(bits);
		},
		[&]() {
			if (CanUseFullTLBInlineMemoryPath())
			{
				DynGenFullTLBTranslate(original_address, EEMmu::AccessType::Store, (bits / 8) - 1,
					xmm ? saved_value_reg : -1);
				DynGenFullTLBPhysicalWrite(
					bits, xmm, original_address, saved_value_reg, GetFullTLBWriteFunction(bits));
			}
			else
			{
				xMOV(arg1regd, original_address);
				DynGenFullTLBPrepareWriteValue(bits, xmm, saved_value_reg);
				recPrepareFullTLBAccessContext();
				xFastCall(GetFullTLBWriteFunction(bits));
				recFinishFullTLBAccessContext();
			}
		});
	if (xmm)
		_freeXMMreg(saved_value_reg);
	else
		_freeX86reg(saved_value_reg);
	_freeX86reg(original_reg);
}

/*
	// Pseudo-Code For the following Dynarec Implementations -->

	u32 vmv = vmap[addr>>VTLB_PAGE_BITS].raw();
	sptr ppf=addr+vmv;
	if (!(ppf<0))
	{
		data[0]=*reinterpret_cast<DataType*>(ppf);
		if (DataSize==128)
			data[1]=*reinterpret_cast<DataType*>(ppf+8);
		return 0;
	}
	else
	{
		//has to: translate, find function, call function
		u32 hand=(u8)vmv;
		u32 paddr=(ppf-hand) << 1;
		//Console.WriteLn("Translated 0x%08X to 0x%08X",params addr,paddr);
		return reinterpret_cast<TemplateHelper<DataSize,false>::HandlerType*>(RWFT[TemplateHelper<DataSize,false>::sidx][0][hand])(paddr,data);
	}

	// And in ASM it looks something like this -->

	mov eax,ecx;
	shr eax,VTLB_PAGE_BITS;
	mov rax,[rax*wordsize+vmap];
	add rcx,rax;
	js _fullread;

	//these are wrong order, just an example ...
	mov [rax],ecx;
	mov ecx,[rdx];
	mov [rax+4],ecx;
	mov ecx,[rdx+4];
	mov [rax+4+4],ecx;
	mov ecx,[rdx+4+4];
	mov [rax+4+4+4+4],ecx;
	mov ecx,[rdx+4+4+4+4];
	///....

	jmp cont;
	_fullread:
	movzx eax,al;
	sub   ecx,eax;
	call [eax+stuff];
	cont:
	........

*/

#ifdef LOG_STORES
static std::FILE* logfile;
static bool CheckLogFile()
{
	if (!logfile)
		logfile = std::fopen("C:\\Dumps\\comp\\memlog.bad.txt", "wb");
	return (logfile != nullptr);
}

static void LogWrite(u32 addr, u64 val)
{
	if (!CheckLogFile())
		return;

	std::fprintf(logfile, "%08X @ %u: %llx\n", addr, cpuRegs.cycle, val);
	std::fflush(logfile);
}

static void __vectorcall LogWriteQuad(u32 addr, __m128i val)
{
	if (!CheckLogFile())
		return;

	std::fprintf(logfile, "%08X @ %u: %llx %llx\n", addr, cpuRegs.cycle, val.m128i_u64[0], val.m128i_u64[1]);
	std::fflush(logfile);
}
#endif

namespace vtlb_private
{
	// ------------------------------------------------------------------------
	// Prepares eax and ecx for Direct or Indirect operations.
	//
	static void DynGen_PrepRegs(int addr_reg, int value_reg, u32 sz, bool xmm)
	{
		_freeX86reg(arg1regd);
		EE::Profiler.EmitMem(addr_reg);
		xMOV(arg1regd, xRegister32(addr_reg));

		if (value_reg >= 0)
		{
			if (sz == 128)
			{
				pxAssert(xmm);
				_freeXMMreg(xRegisterSSE::GetArgRegister(1, 0).GetId());
				xMOVAPS(xRegisterSSE::GetArgRegister(1, 0), xRegisterSSE::GetInstance(value_reg));
			}
			else if (xmm)
			{
				// 32bit xmms are passed in GPRs
				pxAssert(sz == 32);
				_freeX86reg(arg2regd);
				xMOVD(arg2regd, xRegisterSSE(value_reg));
			}
			else
			{
				_freeX86reg(arg2regd);
				xMOV(arg2reg, xRegister64(value_reg));
			}
		}

		xMOV(eax, arg1regd);
		xSHR(eax, VTLB_PAGE_BITS);
		xMOV(rax, ptrNative[xComplexAddress(arg3reg, vtlbdata.vmap, rax * wordsize)]);
		xADD(arg1reg, rax);
	}

	// ------------------------------------------------------------------------
	static void DynGen_DirectRead(u32 bits, bool sign)
	{
		pxAssert(bits == 8 || bits == 16 || bits == 32 || bits == 64 || bits == 128);

		switch (bits)
		{
			case 8:
				if (sign)
					xMOVSX(rax, ptr8[arg1reg]);
				else
					xMOVZX(rax, ptr8[arg1reg]);
				break;

			case 16:
				if (sign)
					xMOVSX(rax, ptr16[arg1reg]);
				else
					xMOVZX(rax, ptr16[arg1reg]);
				break;

			case 32:
				if (sign)
					xMOVSX(rax, ptr32[arg1reg]);
				else
					xMOV(eax, ptr32[arg1reg]);
				break;

			case 64:
				xMOV(rax, ptr64[arg1reg]);
				break;

			case 128:
				xMOVAPS(xmm0, ptr128[arg1reg]);
				break;

				jNO_DEFAULT
		}
	}

	// ------------------------------------------------------------------------
	static void DynGen_DirectWrite(u32 bits)
	{
		switch (bits)
		{
			case 8:
				xMOV(ptr[arg1reg], xRegister8(arg2regd));
				break;

			case 16:
				xMOV(ptr[arg1reg], xRegister16(arg2regd));
				break;

			case 32:
				xMOV(ptr[arg1reg], arg2regd);
				break;

			case 64:
				xMOV(ptr[arg1reg], arg2reg);
				break;

			case 128:
				xMOVAPS(ptr[arg1reg], xRegisterSSE::GetArgRegister(1, 0));
				break;
		}
	}
} // namespace vtlb_private

static constexpr u32 INDIRECT_DISPATCHER_SIZE = 32;
static constexpr u32 INDIRECT_DISPATCHERS_SIZE = 2 * 5 * 2 * INDIRECT_DISPATCHER_SIZE;
static u8* m_IndirectDispatchers = nullptr;

// ------------------------------------------------------------------------
// mode        - 0 for read, 1 for write!
// operandsize - 0 thru 4 represents 8, 16, 32, 64, and 128 bits.
//
static u8* GetIndirectDispatcherPtr(int mode, int operandsize, int sign = 0)
{
	pxAssert(mode || operandsize >= 3 ? !sign : true);

	return &m_IndirectDispatchers[(mode * (8 * INDIRECT_DISPATCHER_SIZE)) + (sign * 5 * INDIRECT_DISPATCHER_SIZE) +
								  (operandsize * INDIRECT_DISPATCHER_SIZE)];
}

// ------------------------------------------------------------------------
// Generates a JS instruction that targets the appropriate templated instance of
// the vtlb Indirect Dispatcher.
//

template <typename GenDirectFn>
static void DynGen_HandlerTest(const GenDirectFn& gen_direct, int mode, int bits, bool sign = false)
{
	int szidx = 0;
	switch (bits)
	{
		case 8:
			szidx = 0;
			break;
		case 16:
			szidx = 1;
			break;
		case 32:
			szidx = 2;
			break;
		case 64:
			szidx = 3;
			break;
		case 128:
			szidx = 4;
			break;
			jNO_DEFAULT;
	}
	xForwardJS8 to_handler;
	gen_direct();
	xForwardJump8 done;
	to_handler.SetTarget();
	xFastCall(GetIndirectDispatcherPtr(mode, szidx, sign));
	done.SetTarget();
}

// ------------------------------------------------------------------------
// Generates the various instances of the indirect dispatchers
// In: arg1reg: vtlb entry, arg2reg: data ptr (if mode >= 64)
// Out: eax: result (if mode < 64)
static void DynGen_IndirectTlbDispatcher(int mode, int bits, bool sign)
{
	// fixup stack
#ifdef _WIN32
	xSUB(rsp, 32 + 8);
#else
	xSUB(rsp, 8);
#endif

	xMOVZX(eax, al);
	if (wordsize != 8)
		xSUB(arg1regd, 0x80000000);
	xSUB(arg1regd, eax);

	// jump to the indirect handler, which is a C++ function.
	// [ecx is address, edx is data]
	sptr table = (sptr)vtlbdata.RWFT[bits][mode];
	if (table == (s32)table)
	{
		xFastCall(ptrNative[(rax * wordsize) + table], arg1reg, arg2reg);
	}
	else
	{
		xLEA(arg3reg, ptr[(void*)table]);
		xFastCall(ptrNative[(rax * wordsize) + arg3reg], arg1reg, arg2reg);
	}

	if (!mode)
	{
		if (bits == 0)
		{
			if (sign)
				xMOVSX(rax, al);
			else
				xMOVZX(rax, al);
		}
		else if (bits == 1)
		{
			if (sign)
				xMOVSX(rax, ax);
			else
				xMOVZX(rax, ax);
		}
		else if (bits == 2)
		{
			if (sign)
				xCDQE();
		}
	}

#ifdef _WIN32
	xADD(rsp, 32 + 8);
#else
	xADD(rsp, 8);
#endif

	xRET();
}

// One-time initialization procedure.  Multiple subsequent calls during the lifespan of the
// process will be ignored.
//
void vtlb_DynGenDispatchers()
{
	m_IndirectDispatchers = xGetAlignedCallTarget();

	// clear the buffer to 0xcc (easier debugging).
	std::memset(m_IndirectDispatchers, 0xcc, INDIRECT_DISPATCHERS_SIZE);

	for (int mode = 0; mode < 2; ++mode)
	{
		for (int bits = 0; bits < 5; ++bits)
		{
			for (int sign = 0; sign < (!mode && bits < 3 ? 2 : 1); sign++)
			{
				xSetPtr(GetIndirectDispatcherPtr(mode, bits, !!sign));
				xSetTextPtr(R5900_TEXTPTR);

				DynGen_IndirectTlbDispatcher(mode, bits, !!sign);
			}
		}
	}

	Perf::any.Register(m_IndirectDispatchers, INDIRECT_DISPATCHERS_SIZE, "TLB Dispatcher");

	xSetPtr(m_IndirectDispatchers + INDIRECT_DISPATCHERS_SIZE);
}

//////////////////////////////////////////////////////////////////////////////////////////
//                            Dynarec Load Implementations
// ------------------------------------------------------------------------
// Recompiled input registers:
//   ecx - source address to read from
//   Returns read value in eax.
int vtlb_DynGenReadNonQuad(u32 bits, bool sign, bool xmm, int addr_reg, vtlb_ReadRegAllocCallback dest_reg_alloc)
{
	pxAssume(bits <= 64);
	if (EmuConfig.Cpu.EnableExperimentalEETLB)
		return DynGenFullTLBReadNonQuad(bits, sign, xmm, addr_reg, dest_reg_alloc, nullptr);

	int x86_dest_reg;
	if (!CHECK_FASTMEM || vtlb_IsFaultingPC(pc))
	{
		iFlushCall(FLUSH_FULLVTLB);

		DynGen_PrepRegs(addr_reg, -1, bits, xmm);
		DynGen_HandlerTest([bits, sign]() { DynGen_DirectRead(bits, sign); }, 0, bits, sign && bits < 64);

		if (!xmm)
		{
			x86_dest_reg = dest_reg_alloc ? dest_reg_alloc() : (_freeX86reg(eax), eax.GetId());
			xMOV(xRegister64(x86_dest_reg), rax);
		}
		else
		{
			// we shouldn't be loading any FPRs which aren't 32bit..
			// we use MOVD here despite it being floating-point data, because we're going int->float reinterpret.
			pxAssert(bits == 32);
			x86_dest_reg = dest_reg_alloc ? dest_reg_alloc() : (_freeXMMreg(0), 0);
			xMOVDZX(xRegisterSSE(x86_dest_reg), eax);
		}

		return x86_dest_reg;
	}

	const u8* codeStart;
	const xAddressReg x86addr(addr_reg);
	if (!xmm)
	{
		x86_dest_reg = dest_reg_alloc ? dest_reg_alloc() : (_freeX86reg(eax), eax.GetId());
		codeStart = x86Ptr;
		const xRegister64 x86reg(x86_dest_reg);
		switch (bits)
		{
			case 8:
				sign ? xMOVSX(x86reg, ptr8[RFASTMEMBASE + x86addr]) : xMOVZX(xRegister32(x86reg), ptr8[RFASTMEMBASE + x86addr]);
				break;
			case 16:
				sign ? xMOVSX(x86reg, ptr16[RFASTMEMBASE + x86addr]) : xMOVZX(xRegister32(x86reg), ptr16[RFASTMEMBASE + x86addr]);
				break;
			case 32:
				sign ? xMOVSX(x86reg, ptr32[RFASTMEMBASE + x86addr]) : xMOV(xRegister32(x86reg), ptr32[RFASTMEMBASE + x86addr]);
				break;
			case 64:
				xMOV(x86reg, ptr64[RFASTMEMBASE + x86addr]);
				break;

				jNO_DEFAULT
		}
	}
	else
	{
		pxAssert(bits == 32);
		x86_dest_reg = dest_reg_alloc ? dest_reg_alloc() : (_freeXMMreg(0), 0);
		codeStart = x86Ptr;
		const xRegisterSSE xmmreg(x86_dest_reg);
		xMOVSSZX(xmmreg, ptr32[RFASTMEMBASE + x86addr]);
	}

	const u32 padding = LOADSTORE_PADDING - std::min<u32>(static_cast<u32>(x86Ptr - codeStart), 5);
	for (u32 i = 0; i < padding; i++)
		xNOP();

	vtlb_AddLoadStoreInfo((uptr)codeStart, static_cast<u32>(x86Ptr - codeStart),
		pc, GetAllocatedGPRBitmask(), GetAllocatedXMMBitmask(),
		static_cast<u8>(addr_reg), static_cast<u8>(x86_dest_reg),
		static_cast<u8>(bits), sign, true, xmm);

	return x86_dest_reg;
}

// ------------------------------------------------------------------------
// Recompiled input registers:
//   ecx - source address to read from
//   Returns read value in eax.
//
// TLB lookup is performed in const, with the assumption that the COP0/TLB will clear the
// recompiler if the TLB is changed.
//
int vtlb_DynGenReadNonQuad_Const(u32 bits, bool sign, bool xmm, u32 addr_const, vtlb_ReadRegAllocCallback dest_reg_alloc)
{
	if (EmuConfig.Cpu.EnableExperimentalEETLB)
	{
		int result_reg;
		if (TryDynGenFullTLBConstReadNonQuad(bits, sign, xmm, addr_const, dest_reg_alloc, &result_reg))
			return result_reg;
		return DynGenFullTLBReadNonQuad(bits, sign, xmm, -1, dest_reg_alloc, &addr_const);
	}

	EE::Profiler.EmitConstMem(addr_const);

	int x86_dest_reg;
	auto vmv = vtlbdata.vmap[addr_const >> VTLB_PAGE_BITS];
	if (!vmv.isHandler(addr_const))
	{
		auto ppf = vmv.assumePtr(addr_const);
		if (!xmm)
		{
			x86_dest_reg = dest_reg_alloc ? dest_reg_alloc() : (_freeX86reg(eax), eax.GetId());
			switch (bits)
			{
				case 8:
					sign ? xMOVSX(xRegister64(x86_dest_reg), ptr8[(u8*)ppf]) : xMOVZX(xRegister32(x86_dest_reg), ptr8[(u8*)ppf]);
					break;

				case 16:
					sign ? xMOVSX(xRegister64(x86_dest_reg), ptr16[(u16*)ppf]) : xMOVZX(xRegister32(x86_dest_reg), ptr16[(u16*)ppf]);
					break;

				case 32:
					sign ? xMOVSX(xRegister64(x86_dest_reg), ptr32[(u32*)ppf]) : xMOV(xRegister32(x86_dest_reg), ptr32[(u32*)ppf]);
					break;

				case 64:
					xMOV(xRegister64(x86_dest_reg), ptr64[(u64*)ppf]);
					break;
			}
		}
		else
		{
			x86_dest_reg = dest_reg_alloc ? dest_reg_alloc() : (_freeXMMreg(0), 0);
			xMOVSSZX(xRegisterSSE(x86_dest_reg), ptr32[(float*)ppf]);
		}
	}
	else
	{
		// has to: translate, find function, call function
		u32 paddr = vmv.assumeHandlerGetPAddr(addr_const);

		int szidx = 0;
		switch (bits)
		{
			case 8:
				szidx = 0;
				break;
			case 16:
				szidx = 1;
				break;
			case 32:
				szidx = 2;
				break;
			case 64:
				szidx = 3;
				break;
		}

		// Shortcut for the INTC_STAT register, which many games like to spin on heavily.
		if ((bits == 32) && !EmuConfig.Speedhacks.IntcStat && (paddr == INTC_STAT))
		{
			x86_dest_reg = dest_reg_alloc ? dest_reg_alloc() : (_freeX86reg(eax), eax.GetId());
			if (!xmm)
			{
				if (sign)
					xMOVSX(xRegister64(x86_dest_reg), ptr32[&psHu32(INTC_STAT)]);
				else
					xMOV(xRegister32(x86_dest_reg), ptr32[&psHu32(INTC_STAT)]);
			}
			else
			{
				xMOVDZX(xRegisterSSE(x86_dest_reg), ptr32[&psHu32(INTC_STAT)]);
			}
		}
		else
		{
			iFlushCall(FLUSH_FULLVTLB);
			xFastCall(vmv.assumeHandlerGetRaw(szidx, false), paddr);

			if (!xmm)
			{
				x86_dest_reg = dest_reg_alloc ? dest_reg_alloc() : (_freeX86reg(eax), eax.GetId());
				switch (bits)
				{
						// save REX prefix by using 32bit dest for zext
					case 8:
						sign ? xMOVSX(xRegister64(x86_dest_reg), al) : xMOVZX(xRegister32(x86_dest_reg), al);
						break;

					case 16:
						sign ? xMOVSX(xRegister64(x86_dest_reg), ax) : xMOVZX(xRegister32(x86_dest_reg), ax);
						break;

					case 32:
						sign ? xMOVSX(xRegister64(x86_dest_reg), eax) : xMOV(xRegister32(x86_dest_reg), eax);
						break;

					case 64:
						xMOV(xRegister64(x86_dest_reg), rax);
						break;
				}
			}
			else
			{
				x86_dest_reg = dest_reg_alloc ? dest_reg_alloc() : (_freeXMMreg(0), 0);
				xMOVDZX(xRegisterSSE(x86_dest_reg), eax);
			}
		}
	}

	return x86_dest_reg;
}

int vtlb_DynGenReadQuad(u32 bits, int addr_reg, vtlb_ReadRegAllocCallback dest_reg_alloc)
{
	pxAssume(bits == 128);
	if (EmuConfig.Cpu.EnableExperimentalEETLB)
		return DynGenFullTLBReadQuad(addr_reg, dest_reg_alloc, nullptr);

	if (!CHECK_FASTMEM || vtlb_IsFaultingPC(pc))
	{
		iFlushCall(FLUSH_FULLVTLB);

		DynGen_PrepRegs(arg1regd.GetId(), -1, bits, true);
		DynGen_HandlerTest([bits]() { DynGen_DirectRead(bits, false); }, 0, bits);

		const int reg = dest_reg_alloc ? dest_reg_alloc() : (_freeXMMreg(0), 0); // Handler returns in xmm0
		if (reg >= 0)
			xMOVAPS(xRegisterSSE(reg), xmm0);

		return reg;
	}

	const int reg = dest_reg_alloc ? dest_reg_alloc() : (_freeXMMreg(0), 0); // Handler returns in xmm0
	const u8* codeStart = x86Ptr;

	xMOVAPS(xRegisterSSE(reg), ptr128[RFASTMEMBASE + arg1reg]);

	const u32 padding = LOADSTORE_PADDING - std::min<u32>(static_cast<u32>(x86Ptr - codeStart), 5);
	for (u32 i = 0; i < padding; i++)
		xNOP();

	vtlb_AddLoadStoreInfo((uptr)codeStart, static_cast<u32>(x86Ptr - codeStart),
		pc, GetAllocatedGPRBitmask(), GetAllocatedXMMBitmask(),
		static_cast<u8>(arg1reg.GetId()), static_cast<u8>(reg),
		static_cast<u8>(bits), false, true, true);

	return reg;
}


// ------------------------------------------------------------------------
// TLB lookup is performed in const, with the assumption that the COP0/TLB will clear the
// recompiler if the TLB is changed.
int vtlb_DynGenReadQuad_Const(u32 bits, u32 addr_const, vtlb_ReadRegAllocCallback dest_reg_alloc)
{
	pxAssert(bits == 128);
	if (EmuConfig.Cpu.EnableExperimentalEETLB)
	{
		int result_reg;
		if (TryDynGenFullTLBConstReadQuad(addr_const, dest_reg_alloc, &result_reg))
			return result_reg;
		return DynGenFullTLBReadQuad(-1, dest_reg_alloc, &addr_const);
	}

	EE::Profiler.EmitConstMem(addr_const);

	int reg;
	auto vmv = vtlbdata.vmap[addr_const >> VTLB_PAGE_BITS];
	if (!vmv.isHandler(addr_const))
	{
		void* ppf = reinterpret_cast<void*>(vmv.assumePtr(addr_const));
		reg = dest_reg_alloc ? dest_reg_alloc() : (_freeXMMreg(0), 0);
		if (reg >= 0)
			xMOVAPS(xRegisterSSE(reg), ptr128[ppf]);
	}
	else
	{
		// has to: translate, find function, call function
		u32 paddr = vmv.assumeHandlerGetPAddr(addr_const);

		const int szidx = 4;
		iFlushCall(FLUSH_FULLVTLB);
		xFastCall(vmv.assumeHandlerGetRaw(szidx, 0), paddr);

		reg = dest_reg_alloc ? dest_reg_alloc() : (_freeXMMreg(0), 0);
		xMOVAPS(xRegisterSSE(reg), xmm0);
	}

	return reg;
}

//////////////////////////////////////////////////////////////////////////////////////////
//                            Dynarec Store Implementations

void vtlb_DynGenWrite(u32 sz, bool xmm, int addr_reg, int value_reg)
{
	if (EmuConfig.Cpu.EnableExperimentalEETLB)
	{
		DynGenFullTLBWrite(sz, xmm, addr_reg, value_reg, nullptr);
		return;
	}
#ifdef LOG_STORES
	{
		xSUB(rsp, 16 * 16);
		for (u32 i = 0; i < 16; i++)
			xMOVAPS(ptr[rsp + i * 16], xRegisterSSE::GetInstance(i));
		for (const auto& reg : {rbx, rcx, rdx, rsi, rdi, r8, r9, r10, r11, r12, r13, r14, r15, rbp})
			xPUSH(reg);

		xPUSH(xRegister64(addr_reg));
		xPUSH(xRegister64(value_reg));
		xPUSH(arg1reg);
		xPUSH(arg2reg);
		xMOV(arg1regd, xRegister32(addr_reg));
		if (xmm)
		{
			xSUB(rsp, 32 + 32);
			xMOVAPS(ptr[rsp + 32], xRegisterSSE::GetInstance(value_reg));
			xMOVAPS(ptr[rsp + 48], xRegisterSSE::GetArgRegister(1, 0));
			if (sz < 128)
				xPSHUF.D(xRegisterSSE::GetArgRegister(1, 0), xRegisterSSE::GetInstance(value_reg), 0);
			else
				xMOVAPS(xRegisterSSE::GetArgRegister(1, 0), xRegisterSSE::GetInstance(value_reg));
			xFastCall((void*)LogWriteQuad);
			xMOVAPS(xRegisterSSE::GetArgRegister(1, 0), ptr[rsp + 48]);
			xMOVAPS(xRegisterSSE::GetInstance(value_reg), ptr[rsp + 32]);
			xADD(rsp, 32 + 32);
		}
		else
		{
			xMOV(arg2reg, xRegister64(value_reg));
			if (sz == 8)
				xAND(arg2regd, 0xFF);
			else if (sz == 16)
				xAND(arg2regd, 0xFFFF);
			else if (sz == 32)
				xAND(arg2regd, -1);
			xSUB(rsp, 32);
			xFastCall((void*)LogWrite);
			xADD(rsp, 32);
		}
		xPOP(arg2reg);
		xPOP(arg1reg);
		xPOP(xRegister64(value_reg));
		xPOP(xRegister64(addr_reg));

		for (const auto& reg : {rbp, r15, r14, r13, r12, r11, r10, r9, r8, rdi, rsi, rdx, rcx, rbx})
			xPOP(reg);

		for (u32 i = 0; i < 16; i++)
			xMOVAPS(xRegisterSSE::GetInstance(i), ptr[rsp + i * 16]);
		xADD(rsp, 16 * 16);
	}
#endif

	if (!CHECK_FASTMEM || vtlb_IsFaultingPC(pc))
	{
		iFlushCall(FLUSH_FULLVTLB);

		DynGen_PrepRegs(addr_reg, value_reg, sz, xmm);
		DynGen_HandlerTest([sz]() { DynGen_DirectWrite(sz); }, 1, sz);
		return;
	}

	const u8* codeStart = x86Ptr;

	const xAddressReg vaddr_reg(addr_reg);
	if (!xmm)
	{
		switch (sz)
		{
			case 8:
				xMOV(ptr8[RFASTMEMBASE + vaddr_reg], xRegister8(xRegister32(value_reg)));
				break;
			case 16:
				xMOV(ptr16[RFASTMEMBASE + vaddr_reg], xRegister16(value_reg));
				break;
			case 32:
				xMOV(ptr32[RFASTMEMBASE + vaddr_reg], xRegister32(value_reg));
				break;
			case 64:
				xMOV(ptr64[RFASTMEMBASE + vaddr_reg], xRegister64(value_reg));
				break;

				jNO_DEFAULT
		}
	}
	else
	{
		pxAssert(sz == 32 || sz == 128);
		switch (sz)
		{
			case 32:
				xMOVSS(ptr32[RFASTMEMBASE + vaddr_reg], xRegisterSSE(value_reg));
				break;
			case 128:
				xMOVAPS(ptr128[RFASTMEMBASE + vaddr_reg], xRegisterSSE(value_reg));
				break;

				jNO_DEFAULT
		}
	}

	const u32 padding = LOADSTORE_PADDING - std::min<u32>(static_cast<u32>(x86Ptr - codeStart), 5);
	for (u32 i = 0; i < padding; i++)
		xNOP();

	vtlb_AddLoadStoreInfo((uptr)codeStart, static_cast<u32>(x86Ptr - codeStart),
		pc, GetAllocatedGPRBitmask(), GetAllocatedXMMBitmask(),
		static_cast<u8>(addr_reg), static_cast<u8>(value_reg),
		static_cast<u8>(sz), false, false, xmm);
}


// ------------------------------------------------------------------------
// Generates code for a store instruction, where the address is a known constant.
// TLB lookup is performed in const, with the assumption that the COP0/TLB will clear the
// recompiler if the TLB is changed.
void vtlb_DynGenWrite_Const(u32 bits, bool xmm, u32 addr_const, int value_reg)
{
	if (EmuConfig.Cpu.EnableExperimentalEETLB)
	{
		if (TryDynGenFullTLBConstWrite(bits, xmm, addr_const, value_reg))
			return;
		DynGenFullTLBWrite(bits, xmm, -1, value_reg, &addr_const);
		return;
	}

	EE::Profiler.EmitConstMem(addr_const);

#ifdef LOG_STORES
	{
		xSUB(rsp, 16 * 16);
		for (u32 i = 0; i < 16; i++)
			xMOVAPS(ptr[rsp + i * 16], xRegisterSSE::GetInstance(i));
		for (const auto& reg : {rbx, rcx, rdx, rsi, rdi, r8, r9, r10, r11, r12, r13, r14, r15, rbp})
			xPUSH(reg);

		xPUSH(xRegister64(value_reg));
		xPUSH(xRegister64(value_reg));
		xPUSH(arg1reg);
		xPUSH(arg2reg);
		xMOV(arg1reg, addr_const);
		if (xmm)
		{
			xSUB(rsp, 32 + 32);
			xMOVAPS(ptr[rsp + 32], xRegisterSSE::GetInstance(value_reg));
			xMOVAPS(ptr[rsp + 48], xRegisterSSE::GetArgRegister(1, 0));
			if (bits < 128)
				xPSHUF.D(xRegisterSSE::GetArgRegister(1, 0), xRegisterSSE::GetInstance(value_reg), 0);
			else
				xMOVAPS(xRegisterSSE::GetArgRegister(1, 0), xRegisterSSE::GetInstance(value_reg));
			xFastCall((void*)LogWriteQuad);
			xMOVAPS(xRegisterSSE::GetArgRegister(1, 0), ptr[rsp + 48]);
			xMOVAPS(xRegisterSSE::GetInstance(value_reg), ptr[rsp + 32]);
			xADD(rsp, 32 + 32);
		}
		else
		{
			xMOV(arg2reg, xRegister64(value_reg));
			if (bits == 8)
				xAND(arg2regd, 0xFF);
			else if (bits == 16)
				xAND(arg2regd, 0xFFFF);
			else if (bits == 32)
				xAND(arg2regd, -1);
			xSUB(rsp, 32);
			xFastCall((void*)LogWrite);
			xADD(rsp, 32);
		}
		xPOP(arg2reg);
		xPOP(arg1reg);
		xPOP(xRegister64(value_reg));
		xPOP(xRegister64(value_reg));

		for (const auto& reg : {rbp, r15, r14, r13, r12, r11, r10, r9, r8, rdi, rsi, rdx, rcx, rbx})
			xPOP(reg);

		for (u32 i = 0; i < 16; i++)
			xMOVAPS(xRegisterSSE::GetInstance(i), ptr[rsp + i * 16]);
		xADD(rsp, 16 * 16);
	}
#endif

	auto vmv = vtlbdata.vmap[addr_const >> VTLB_PAGE_BITS];
	if (!vmv.isHandler(addr_const))
	{
		auto ppf = vmv.assumePtr(addr_const);
		if (!xmm)
		{
			switch (bits)
			{
				case 8:
					xMOV(ptr[(void*)ppf], xRegister8(xRegister32(value_reg)));
					break;

				case 16:
					xMOV(ptr[(void*)ppf], xRegister16(value_reg));
					break;

				case 32:
					xMOV(ptr[(void*)ppf], xRegister32(value_reg));
					break;

				case 64:
					xMOV(ptr64[(void*)ppf], xRegister64(value_reg));
					break;

					jNO_DEFAULT
			}
		}
		else
		{
			switch (bits)
			{
				case 32:
					xMOVSS(ptr[(void*)ppf], xRegisterSSE(value_reg));
					break;

				case 128:
					xMOVAPS(ptr128[(void*)ppf], xRegisterSSE(value_reg));
					break;

					jNO_DEFAULT
			}
		}
	}
	else
	{
		// has to: translate, find function, call function
		u32 paddr = vmv.assumeHandlerGetPAddr(addr_const);

		int szidx = 0;
		switch (bits)
		{
			case 8:
				szidx = 0;
				break;
			case 16:
				szidx = 1;
				break;
			case 32:
				szidx = 2;
				break;
			case 64:
				szidx = 3;
				break;
			case 128:
				szidx = 4;
				break;
		}

		iFlushCall(FLUSH_FULLVTLB);

		_freeX86reg(arg1regd);
		xMOV(arg1regd, paddr);
		if (bits == 128)
		{
			pxAssert(xmm);
			const xRegisterSSE argreg(xRegisterSSE::GetArgRegister(1, 0));
			_freeXMMreg(argreg.GetId());
			xMOVAPS(argreg, xRegisterSSE(value_reg));
		}
		else if (xmm)
		{
			pxAssert(bits == 32);
			_freeX86reg(arg2regd);
			xMOVD(arg2regd, xRegisterSSE(value_reg));
		}
		else
		{
			_freeX86reg(arg2regd);
			xMOV(arg2reg, xRegister64(value_reg));
		}

		xFastCall(vmv.assumeHandlerGetRaw(szidx, true));
	}
}

void vtlb_DynGenFullTLBMergeWrite(u32 bits, bool left, int addr_reg, int value_reg)
{
	pxAssert(EmuConfig.Cpu.EnableExperimentalEETLB);
	const void* function = nullptr;
	if (bits == 32)
		function = left ? reinterpret_cast<const void*>(EEMemory::RecompilerWriteLeft32) :
		                  reinterpret_cast<const void*>(EEMemory::RecompilerWriteRight32);
	else
	{
		pxAssert(bits == 64);
		function = left ? reinterpret_cast<const void*>(EEMemory::RecompilerWriteLeft64) :
		                  reinterpret_cast<const void*>(EEMemory::RecompilerWriteRight64);
	}
	DynGenFullTLBMergeWrite(bits, left, addr_reg, value_reg, nullptr, function);
}

void vtlb_DynGenFullTLBMergeWrite_Const(u32 bits, bool left, u32 addr_const, int value_reg)
{
	pxAssert(EmuConfig.Cpu.EnableExperimentalEETLB);
	const void* function = nullptr;
	if (bits == 32)
		function = left ? reinterpret_cast<const void*>(EEMemory::RecompilerWriteLeft32) :
		                  reinterpret_cast<const void*>(EEMemory::RecompilerWriteRight32);
	else
	{
		pxAssert(bits == 64);
		function = left ? reinterpret_cast<const void*>(EEMemory::RecompilerWriteLeft64) :
		                  reinterpret_cast<const void*>(EEMemory::RecompilerWriteRight64);
	}
	DynGenFullTLBMergeWrite(bits, left, -1, value_reg, &addr_const, function);
}

static void DynGenFullTLBCacheTranslate(int addr_reg, const u32* addr_const)
{
	const int original_reg = _allocX86reg(X86TYPE_TEMP, 0, MODE_CALLEESAVED);
	const xRegister32 original_address(original_reg);
	if (!addr_const)
	{
		pxAssert(addr_reg == arg1regd.GetId());
		xMOV(original_address, arg1regd);
	}
	iFlushCall(FLUSH_FULLVTLB);
	if (addr_const)
	{
		EE::Profiler.EmitConstMem(*addr_const);
		xMOV(original_address, *addr_const);
	}
	else
	{
		EE::Profiler.EmitMem(addr_reg);
	}
	DynGenFullTLBTranslate(original_address, EEMmu::AccessType::Cache, 0);
	_freeX86reg(original_reg);
}

void vtlb_DynGenFullTLBCacheTranslate(int addr_reg)
{
	pxAssert(EmuConfig.Cpu.EnableExperimentalEETLB);
	DynGenFullTLBCacheTranslate(addr_reg, nullptr);
}

void vtlb_DynGenFullTLBCacheTranslate_Const(u32 addr_const)
{
	pxAssert(EmuConfig.Cpu.EnableExperimentalEETLB);
	DynGenFullTLBCacheTranslate(-1, &addr_const);
}

//////////////////////////////////////////////////////////////////////////////////////////
//							Extra Implementations

//   ecx - virtual address
//   Returns physical address in eax.
//   Clobbers edx
void vtlb_DynV2P()
{
	xMOV(eax, ecx);
	xAND(ecx, VTLB_PAGE_MASK); // vaddr & VTLB_PAGE_MASK

	xSHR(eax, VTLB_PAGE_BITS);
	xMOV(eax, ptr[xComplexAddress(rdx, vtlbdata.ppmap, rax * 4)]); // vtlbdata.ppmap[vaddr >> VTLB_PAGE_BITS];

	xOR(eax, ecx);
}

void vtlb_DynPatchLoadStore(uptr code_address, u32 code_size, uptr slow_path)
{
	x86Ptr = reinterpret_cast<u8*>(code_address);
	xJMP(reinterpret_cast<const void*>(slow_path));

	pxAssertRel(static_cast<u32>(reinterpret_cast<uptr>(x86Ptr) - code_address) <= code_size,
		"Overflowed when patching Full TLB fastmem access");
	for (u32 i = static_cast<u32>(reinterpret_cast<uptr>(x86Ptr) - code_address); i < code_size; i++)
		xNOP();
}

void vtlb_DynBackpatchLoadStore(uptr code_address, u32 code_size, u32 guest_pc, u32 guest_addr,
	u32 gpr_bitmask, u32 fpr_bitmask, u8 address_register, u8 data_register,
	u8 size_in_bits, bool is_signed, bool is_load, bool is_xmm)
{
	static constexpr u32 GPR_SIZE = 8;
	static constexpr u32 XMM_SIZE = 16;

	// on win32, we need to reserve an additional 32 bytes shadow space when calling out to C
#ifdef _WIN32
	static constexpr u32 SHADOW_SIZE = 32;
#else
	static constexpr u32 SHADOW_SIZE = 0;
#endif

#if 0
	DevCon.WriteLn("Backpatching %s at %p[%u] (pc %08X vaddr %08X): Bitmask %08X %08X Addr %u Data %u Size %u Flags %02X %02X",
		is_load ? "load" : "store", (void*)code_address, code_size, guest_pc, guest_addr, gpr_bitmask, fpr_bitmask,
		address_register, data_register, size_in_bits, is_signed, is_load);
#endif

	u8* thunk = recBeginThunk();

	// save regs
	u32 num_gprs = 0;
	u32 num_fprs = 0;

	const u32 arg1id = static_cast<u32>(arg1reg.GetId());
	const u32 arg2id = static_cast<u32>(arg2reg.GetId());
	const u32 arg3id = static_cast<u32>(arg3reg.GetId());

	for (u32 i = 0; i < iREGCNT_GPR; i++)
	{
		if ((gpr_bitmask & (1u << i)) && (i == arg1id || i == arg2id || xRegisterBase::IsCallerSaved(i)) && (!is_load || is_xmm || data_register != i))
			num_gprs++;
	}
	for (u32 i = 0; i < iREGCNT_XMM_EVEX; i++)
	{
		if (fpr_bitmask & (1u << i) && xRegisterSSE::IsCallerSaved(i) && (!is_load || !is_xmm || data_register != i))
			num_fprs++;
	}

	const u32 stack_size = (((num_gprs + 1) & ~1u) * GPR_SIZE) + (num_fprs * XMM_SIZE) + SHADOW_SIZE;

	if (stack_size > 0)
	{
		xSUB(rsp, stack_size);

		u32 stack_offset = SHADOW_SIZE;
		for (u32 i = 0; i < iREGCNT_XMM_EVEX; i++)
		{
			if (fpr_bitmask & (1u << i) && xRegisterSSE::IsCallerSaved(i) && (!is_load || !is_xmm || data_register != i))
			{
				if (i >= iREGCNT_XMM)
					xVMOVDQA32(ptr128[rsp + stack_offset], xRegisterSSE(i));
				else
					xMOVAPS(ptr128[rsp + stack_offset], xRegisterSSE(i));
				stack_offset += XMM_SIZE;
			}
		}

		for (u32 i = 0; i < iREGCNT_GPR; i++)
		{
			if ((gpr_bitmask & (1u << i)) && (i == arg1id || i == arg2id || i == arg3id || xRegisterBase::IsCallerSaved(i)) && (!is_load || is_xmm || data_register != i))
			{
				xMOV(ptr64[rsp + stack_offset], xRegister64(i));
				stack_offset += GPR_SIZE;
			}
		}
	}

	if (is_load)
	{
		DynGen_PrepRegs(address_register, -1, size_in_bits, is_xmm);
		DynGen_HandlerTest([size_in_bits, is_signed]() { DynGen_DirectRead(size_in_bits, is_signed); }, 0, size_in_bits, is_signed && size_in_bits <= 32);

		if (size_in_bits == 128)
		{
			if (data_register != xmm0.GetId())
				xMOVAPS(xRegisterSSE(data_register), xmm0);
		}
		else
		{
			if (is_xmm)
			{
				xMOVDZX(xRegisterSSE(data_register), rax);
			}
			else
			{
				if (data_register != eax.GetId())
					xMOV(xRegister64(data_register), rax);
			}
		}
	}
	else
	{
		if (address_register != arg1reg.GetId())
			xMOV(arg1regd, xRegister32(address_register));

		if (size_in_bits == 128)
		{
			const xRegisterSSE argreg(xRegisterSSE::GetArgRegister(1, 0));
			if (data_register != argreg.GetId())
				xMOVAPS(argreg, xRegisterSSE(data_register));
		}
		else
		{
			if (is_xmm)
			{
				xMOVD(arg2reg, xRegisterSSE(data_register));
			}
			else
			{
				if (data_register != arg2reg.GetId())
					xMOV(arg2reg, xRegister64(data_register));
			}
		}

		DynGen_PrepRegs(address_register, data_register, size_in_bits, is_xmm);
		DynGen_HandlerTest([size_in_bits]() { DynGen_DirectWrite(size_in_bits); }, 1, size_in_bits);
	}

	// restore regs
	if (stack_size > 0)
	{
		u32 stack_offset = SHADOW_SIZE;
		for (u32 i = 0; i < iREGCNT_XMM_EVEX; i++)
		{
			if (fpr_bitmask & (1u << i) && xRegisterSSE::IsCallerSaved(i) && (!is_load || !is_xmm || data_register != i))
			{
				if (i >= iREGCNT_XMM)
					xVMOVDQA32(xRegisterSSE(i), ptr128[rsp + stack_offset]);
				else
					xMOVAPS(xRegisterSSE(i), ptr128[rsp + stack_offset]);
				stack_offset += XMM_SIZE;
			}
		}

		for (u32 i = 0; i < iREGCNT_GPR; i++)
		{
			if ((gpr_bitmask & (1u << i)) && (i == arg1id || i == arg2id || i == arg3id || xRegisterBase::IsCallerSaved(i)) && (!is_load || is_xmm || data_register != i))
			{
				xMOV(xRegister64(i), ptr64[rsp + stack_offset]);
				stack_offset += GPR_SIZE;
			}
		}

		xADD(rsp, stack_size);
	}

	xJMP((void*)(code_address + code_size));

	recEndThunk();

	// backpatch to a jump to the slowmem handler
	x86Ptr = (u8*)code_address;
	xJMP(thunk);

	// fill the rest of it with nops, if any
	pxAssertRel(static_cast<u32>((uptr)x86Ptr - code_address) <= code_size, "Overflowed when backpatching");
	for (u32 i = static_cast<u32>((uptr)x86Ptr - code_address); i < code_size; i++)
		xNOP();
}
