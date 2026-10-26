// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>

namespace Vulkan {

// RADV may advertise UINT32_MAX in X. That limit alone does not reject the game's
// multi-billion particle counts; keep the safeguard within the portable dispatch range.
constexpr std::array<uint32_t, 3> IndirectDispatchLimits(std::array<uint32_t, 3> device_limits) {
    for (auto& count : device_limits) {
        count = std::min(count, 65535u);
    }
    return device_limits;
}

} // namespace Vulkan
