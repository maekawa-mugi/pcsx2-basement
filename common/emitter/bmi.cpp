// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "common/emitter/internal.h"

namespace x86Emitter
{
	const xImplBMI_RVM xMULX = {SIMDInstructionInfo(0xF6).m0f38().dstw().pf2()};
	const xImplBMI_RVM xPDEP = {SIMDInstructionInfo(0xF5).m0f38().dstw().pf2()};
	const xImplBMI_RVM xPEXT = {SIMDInstructionInfo(0xF5).m0f38().dstw().pf3()};
	const xImplBMI_RVM xANDN_S = {SIMDInstructionInfo(0xF2).m0f38().dstw()};
	// BMI2 flagless shifts. Operand order follows the RVM encoding: (dst, count -> VEX.vvvv, source -> ModRM.rm).
	const xImplBMI_RVM xSHLX = {SIMDInstructionInfo(0xF7).m0f38().dstw().p66()};
	const xImplBMI_RVM xSARX = {SIMDInstructionInfo(0xF7).m0f38().dstw().pf3()};
	const xImplBMI_RVM xSHRX = {SIMDInstructionInfo(0xF7).m0f38().dstw().pf2()};

	void xImplBMI_RVM::operator()(const xRegisterInt& to, const xRegisterInt& from1, const xRegisterInt& from2) const
	{
		EmitVEX(info, to, from1.GetId(), from2);
	}
	void xImplBMI_RVM::operator()(const xRegisterInt& to, const xRegisterInt& from1, const xIndirectVoid& from2) const
	{
		EmitVEX(info, to, from1.GetId(), from2);
	}
} // namespace x86Emitter
