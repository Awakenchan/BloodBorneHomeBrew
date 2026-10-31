// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "common.h"

namespace start {

fs::path BuildModsOverlay(const fs::path& game, const fs::path& out, const fs::path& mods_dir, const fs::path& config,
                          bool enabled, const std::vector<fs::path>& first_layers = {});
std::optional<fs::path> GameUpdateFolder(const fs::path& game);
void RemoveOverlay(const fs::path& path);

}
