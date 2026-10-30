// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>

namespace BbEngine::GxTrace {
struct Seen {
    std::uint64_t programs[6];
    std::uint64_t index_base;
    std::uint64_t color[8];
    std::uint64_t depth;
    std::uint32_t index_count;
    std::uint32_t index_type;
    std::uint32_t primitive;
    std::uint32_t indexed;
    float viewport[4];
    const std::uint32_t* user_data[5];
};

bool Install(std::uint8_t* image, std::size_t size);
bool On();
void Stamp(std::uint64_t record_address, std::uint32_t count);
void Check(std::uint64_t record_address, const Seen& seen);
const void* TakeSample();
enum ResourceKind { ResourceImage, ResourceBuffer, ResourceSampler };
void NoteResource(const void* sample, ResourceKind kind, const void* sharp, bool wide);
}
