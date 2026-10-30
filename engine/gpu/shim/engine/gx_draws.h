// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>

namespace BbEngine::GxDraws {
constexpr std::uint32_t kBindTag = 0xbb5e0000u;
bool On();
void Report();
bool Install(std::uint8_t* image, std::size_t size);
}
