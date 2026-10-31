// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>

namespace BbEngine::Loading {
void Install(std::uint8_t* image);
void Tick(std::uint64_t pacer);
bool ScreenUp();
bool CinematicPlaying();
std::uint64_t Generation();
}
