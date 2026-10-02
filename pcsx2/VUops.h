// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once
#include "VU.h"
#include "VUflags.h"

struct _VURegsNum {
	u8 pipe; // if 0xff, COP2
	u8 VFwrite;
	u8 VFwxyzw;
	u8 VFr0xyzw;
	u8 VFr1xyzw;
	u8 VFread0;
	u8 VFread1;
	u32 VIwrite;
	u32 VIread;
	int cycles;
};

using FnPtr_VuVoid = void (*)();
using FnPtr_VuRegsN = void(*)(_VURegsNum *VUregsn);

enum class VuUpperFmacSoftOp : u32
{
	ADD,
	ADDi,
	ADDq,
	ADDx,
	ADDy,
	ADDz,
	ADDw,
	SUB,
	SUBi,
	SUBq,
	SUBx,
	SUBy,
	SUBz,
	SUBw,
	MUL,
	MULi,
	MULq,
	MULx,
	MULy,
	MULz,
	MULw,
	ADDA,
	ADDAi,
	ADDAq,
	ADDAx,
	ADDAy,
	ADDAz,
	ADDAw,
	SUBA,
	SUBAi,
	SUBAq,
	SUBAx,
	SUBAy,
	SUBAz,
	SUBAw,
	MULA,
	MULAi,
	MULAq,
	MULAx,
	MULAy,
	MULAz,
	MULAw,
	MADD,
	MADDi,
	MADDq,
	MADDx,
	MADDy,
	MADDz,
	MADDw,
	MADDA,
	MADDAi,
	MADDAq,
	MADDAx,
	MADDAy,
	MADDAz,
	MADDAw,
	MSUB,
	MSUBi,
	MSUBq,
	MSUBx,
	MSUBy,
	MSUBz,
	MSUBw,
	MSUBA,
	MSUBAi,
	MSUBAq,
	MSUBAx,
	MSUBAy,
	MSUBAz,
	MSUBAw,
};

alignas(16) extern const FnPtr_VuVoid VU0_LOWER_OPCODE[128];
alignas(16) extern const FnPtr_VuVoid VU0_UPPER_OPCODE[64];
alignas(16) extern const FnPtr_VuRegsN VU0regs_LOWER_OPCODE[128];
alignas(16) extern const FnPtr_VuRegsN VU0regs_UPPER_OPCODE[64];

alignas(16) extern const FnPtr_VuVoid VU1_LOWER_OPCODE[128];
alignas(16) extern const FnPtr_VuVoid VU1_UPPER_OPCODE[64];
alignas(16) extern const FnPtr_VuRegsN VU1regs_LOWER_OPCODE[128];
alignas(16) extern const FnPtr_VuRegsN VU1regs_UPPER_OPCODE[64];
extern void _vuClearFMAC(VURegs * VU);
extern void _vuTestPipes(VURegs * VU);
extern void _vuTestUpperStalls(VURegs * VU, _VURegsNum *VUregsn);
extern void _vuTestLowerStalls(VURegs * VU, _VURegsNum *VUregsn);
extern void _vuAddUpperStalls(VURegs * VU, _VURegsNum *VUregsn);
extern void _vuAddLowerStalls(VURegs * VU, _VURegsNum *VUregsn);
extern void _vuXGKICKTransfer(s32 cycles, bool flush);
extern void vuUpperFmacSoftHelper(VURegs* VU, VuUpperFmacSoftOp op);
extern void vuUpperFmacSoftNativeFixup(VURegs* VU, VuUpperFmacSoftOp op);
extern void vuUpperFmacSoftNativeBridge(VURegs* VU, VuUpperFmacSoftOp op);
extern void vuUpperFmacSoftAddFull(VURegs* VU);
extern void vuUpperFmacSoftAddMasked(VURegs* VU);
extern void vuUpperFmacSoftAddI(VURegs* VU);
extern void vuUpperFmacSoftAddQ(VURegs* VU);
extern void vuUpperFmacSoftSubFull(VURegs* VU);
extern void vuUpperFmacSoftSubMasked(VURegs* VU);
extern void vuUpperFmacSoftSubI(VURegs* VU);
extern void vuUpperFmacSoftSubQ(VURegs* VU);
extern void vuUpperFmacSoftMulFull(VURegs* VU);
extern void vuUpperFmacSoftMulMasked(VURegs* VU);
extern void vuUpperFmacSoftMulI(VURegs* VU);
extern void vuUpperFmacSoftMulQ(VURegs* VU);
extern void vuUpperFmacSoftAddX(VURegs* VU);
extern void vuUpperFmacSoftAddY(VURegs* VU);
extern void vuUpperFmacSoftAddZ(VURegs* VU);
extern void vuUpperFmacSoftAddW(VURegs* VU);
extern void vuUpperFmacSoftSubX(VURegs* VU);
extern void vuUpperFmacSoftSubY(VURegs* VU);
extern void vuUpperFmacSoftSubZ(VURegs* VU);
extern void vuUpperFmacSoftSubW(VURegs* VU);
extern void vuUpperFmacSoftMulX(VURegs* VU);
extern void vuUpperFmacSoftMulY(VURegs* VU);
extern void vuUpperFmacSoftMulZ(VURegs* VU);
extern void vuUpperFmacSoftMulW(VURegs* VU);
extern void vuUpperFmacSoftAddAccFull(VURegs* VU);
extern void vuUpperFmacSoftAddAccI(VURegs* VU);
extern void vuUpperFmacSoftAddAccQ(VURegs* VU);
extern void vuUpperFmacSoftSubAccFull(VURegs* VU);
extern void vuUpperFmacSoftSubAccI(VURegs* VU);
extern void vuUpperFmacSoftSubAccQ(VURegs* VU);
extern void vuUpperFmacSoftMulAccFull(VURegs* VU);
extern void vuUpperFmacSoftMulAccI(VURegs* VU);
extern void vuUpperFmacSoftMulAccQ(VURegs* VU);
extern void vuUpperFmacSoftAddAccX(VURegs* VU);
extern void vuUpperFmacSoftAddAccY(VURegs* VU);
extern void vuUpperFmacSoftAddAccZ(VURegs* VU);
extern void vuUpperFmacSoftAddAccW(VURegs* VU);
extern void vuUpperFmacSoftSubAccX(VURegs* VU);
extern void vuUpperFmacSoftSubAccY(VURegs* VU);
extern void vuUpperFmacSoftSubAccZ(VURegs* VU);
extern void vuUpperFmacSoftSubAccW(VURegs* VU);
extern void vuUpperFmacSoftMulAccX(VURegs* VU);
extern void vuUpperFmacSoftMulAccY(VURegs* VU);
extern void vuUpperFmacSoftMulAccZ(VURegs* VU);
extern void vuUpperFmacSoftMulAccW(VURegs* VU);
extern void vuUpperFmacSoftMaddFull(VURegs* VU);
extern void vuUpperFmacSoftMaddI(VURegs* VU);
extern void vuUpperFmacSoftMaddQ(VURegs* VU);
extern void vuUpperFmacSoftMaddX(VURegs* VU);
extern void vuUpperFmacSoftMaddY(VURegs* VU);
extern void vuUpperFmacSoftMaddZ(VURegs* VU);
extern void vuUpperFmacSoftMaddW(VURegs* VU);
extern void vuUpperFmacSoftMaddaFull(VURegs* VU);
extern void vuUpperFmacSoftMaddaI(VURegs* VU);
extern void vuUpperFmacSoftMaddaQ(VURegs* VU);
extern void vuUpperFmacSoftMaddaX(VURegs* VU);
extern void vuUpperFmacSoftMaddaY(VURegs* VU);
extern void vuUpperFmacSoftMaddaZ(VURegs* VU);
extern void vuUpperFmacSoftMaddaW(VURegs* VU);
extern void vuUpperFmacSoftMsubFull(VURegs* VU);
extern void vuUpperFmacSoftMsubI(VURegs* VU);
extern void vuUpperFmacSoftMsubQ(VURegs* VU);
extern void vuUpperFmacSoftMsubX(VURegs* VU);
extern void vuUpperFmacSoftMsubY(VURegs* VU);
extern void vuUpperFmacSoftMsubZ(VURegs* VU);
extern void vuUpperFmacSoftMsubW(VURegs* VU);
extern void vuUpperFmacSoftMsubaFull(VURegs* VU);
extern void vuUpperFmacSoftMsubaI(VURegs* VU);
extern void vuUpperFmacSoftMsubaQ(VURegs* VU);
extern void vuUpperFmacSoftMsubaX(VURegs* VU);
extern void vuUpperFmacSoftMsubaY(VURegs* VU);
extern void vuUpperFmacSoftMsubaZ(VURegs* VU);
extern void vuUpperFmacSoftMsubaW(VURegs* VU);
extern void vuLowerDivSoftHelper(VURegs* VU, u32 op);
extern void vuLowerDivSoftDivHelper(VURegs* VU);
extern void vuLowerDivSoftSqrtHelper(VURegs* VU);
extern void vuLowerDivSoftRsqrtHelper(VURegs* VU);
