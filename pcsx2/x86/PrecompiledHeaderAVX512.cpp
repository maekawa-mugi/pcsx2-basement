// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// Creates the AVX-512 precompiled header used by InterpreterAVX512.cpp only. Clang rejects a PCH
// built for a different target CPU, so this file is compiled with the same instruction set as its
// user (see pcsx2.vcxproj). Needs proper testing on the MSVC (non-Clang) configurations.
#include "PrecompiledHeader.h"
