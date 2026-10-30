// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include "common/types.h"

namespace Libraries::VideoOut {

constexpr std::size_t MaxDisplayBuffers = 16;

enum class PixelFormat : u32 {
    Unknown,
    A8R8G8B8Srgb = 0x80000000,
    A8B8G8R8Srgb = 0x80002200,
    A2R10G10B10 = 0x88060000,
    A2R10G10B10Srgb = 0x88000000,
    A2R10G10B10Bt2020Pq = 0x88740000,
    A16R16G16B16Float = 0xC1060000,
};

enum class TilingMode : s32 {
    Tile = 0,
    Linear = 1,
};

struct BufferAttribute {
    PixelFormat pixel_format;
    TilingMode tiling_mode;
    s32 aspect_ratio;
    u32 width;
    u32 height;
    u32 pitch_in_pixel;
    u32 option;
    u32 reserved0;
    u64 reserved1;
};

struct BufferAttributeGroup {
    bool is_occupied;
    BufferAttribute attrib;
};

} // namespace Libraries::VideoOut
