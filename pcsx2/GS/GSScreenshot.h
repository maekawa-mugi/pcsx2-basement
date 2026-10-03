// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

#include <string>
#include <string_view>
#include <vector>

const char* GSGetScreenshotSuffix();
std::string GSGetScreenshotFilename(std::string_view base_path);
void GSCompressAndWriteScreenshot(std::string filename, u32 width, u32 height, std::vector<u32> pixels);
void GSJoinSnapshotThreads();
