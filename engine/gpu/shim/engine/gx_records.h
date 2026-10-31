// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>

namespace BbEngine::GxRecords {
bool Install(std::uint8_t* image, std::size_t size);
bool On();
bool Checked();
void Stamp(std::uint64_t record_address, std::uint32_t count);
std::uint64_t Take(std::uint64_t record_address);
}
