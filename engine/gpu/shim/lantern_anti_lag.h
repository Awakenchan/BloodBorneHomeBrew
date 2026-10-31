// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <vulkan/vulkan_core.h>

namespace LanternAntiLag {

bool Allowed();
void Start(VkDevice device, PFN_vkGetDeviceProcAddr resolve);
void BeforePresent();
void Shutdown();

} // namespace LanternAntiLag
