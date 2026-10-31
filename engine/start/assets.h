// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "common.h"

namespace start {

std::string VerifyExecutable(const fs::path& elf);
fs::path PrepareMirror(const fs::path& game, const fs::path& elf, const fs::path& cache);
fs::path PrepareMenuAssets(const fs::path& game, const fs::path& elf, const fs::path& cache, const fs::path& tool,
                           bool rebirth, const std::optional<std::string>& host_title = std::nullopt,
                           const std::optional<std::string>& guest_title = std::nullopt);

std::vector<std::pair<std::string, fs::path>> PrepareEffectFixes(const fs::path& game, const fs::path& cache,
                                                                 std::vector<std::string>& notes);

fs::path PrepareLamp2Lamp(const fs::path& game, const std::optional<fs::path>& update, const fs::path& patch,
                          const fs::path& cache);

int RunProgram(const fs::path& program, const std::vector<std::wstring>& arguments, std::string* output = nullptr,
               void* log = nullptr);
std::wstring CommandLine(const fs::path& program, const std::vector<std::wstring>& arguments);

}
