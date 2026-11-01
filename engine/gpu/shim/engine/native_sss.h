// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>

namespace BbEngine::Sss {
enum class Status { Unsupported, Off, Waiting, Active };
bool Install(std::uint8_t* image, std::size_t size);
void BeforeRender(std::uint64_t context);
void Tick();
Status CurrentStatus();
}
