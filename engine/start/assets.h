// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "common.h"

namespace start {

std::string VerifyExecutable(const fs::path& elf);
fs::path PrepareMirror(const fs::path& game, const fs::path& elf, const fs::path& cache);
fs::path PrepareMenuAssets(const fs::path& game, const fs::path& elf, const fs::path& cache, const fs::path& tool,
                           bool rebirth, bool plate_roles = false);

std::vector<std::pair<std::string, fs::path>> PrepareEffectFixes(const fs::path& game, const fs::path& cache,
                                                                 std::vector<std::string>& notes);

int RunProgram(const fs::path& program, const std::vector<std::wstring>& arguments, std::string* output = nullptr,
               void* log = nullptr);
std::wstring CommandLine(const fs::path& program, const std::vector<std::wstring>& arguments);

}
