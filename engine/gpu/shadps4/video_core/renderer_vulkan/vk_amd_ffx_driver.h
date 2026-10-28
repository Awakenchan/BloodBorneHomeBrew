// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <string>

namespace Vulkan::AmdFfxDriver {
struct Functions {
    void* create = nullptr;
    void* destroy = nullptr;
    void* configure = nullptr;
    void* query = nullptr;
    void* dispatch = nullptr;
    std::uint64_t version_id = 0;
    std::string version_name;
    std::string path;
};

std::string ProviderPath();
bool Load(std::uint64_t context_type, const void* backend, Functions& out, std::string& why);
}
