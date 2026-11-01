// SPDX-License-Identifier: GPL-3.0-or-later
#include "../gpu/shadps4/video_core/renderer_vulkan/vk_ggx_patch.h"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace {
std::vector<uint32_t> Read(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    assert(file);
    const auto size = file.tellg();
    assert(size > 0 && size % 4 == 0);
    std::vector<uint32_t> words(size_t(size) / 4);
    file.seekg(0);
    file.read(reinterpret_cast<char*>(words.data()), size);
    assert(file);
    return words;
}
}

int main(int argc, char** argv) {
    using namespace Vulkan::Ggx;
    assert(argc == 1 || argc == 4);
    std::vector<uint32_t> malformed{0x07230203, 0x10600, 0, 10, 0, 0};
    const auto original = malformed;
    assert(Patch(DirectionalHash, true, false, malformed) == Result::Disabled);
    assert(Patch(DirectionalHash, false, true, malformed) == Result::Unrelated);
    assert(Patch(0x1234, true, true, malformed) == Result::Unrelated);
    assert(Patch(DirectionalHash, true, true, malformed) == Result::Unsupported);
    assert(malformed == original);
    if (argc == 4) {
        const std::filesystem::path output = argv[3];
        assert(!std::filesystem::exists(output));
        std::filesystem::create_directories(output);
        for (int i = 1; i <= 2; ++i) {
            const auto source = Read(argv[i]);
            auto words = source;
            assert(Patch(DirectionalHash, true, false, words) == Result::Disabled && words == source);
            auto altered = source;
            altered.back() ^= 1;
            const auto bad = altered;
            assert(Patch(DirectionalHash, true, true, altered) == Result::Unsupported && altered == bad);
            assert(Patch(DirectionalHash, true, true, words) == Result::Applied);
            auto second = source;
            assert(Patch(DirectionalHash, true, true, second) == Result::Applied && second == words);
            const auto patched = words;
            assert(Patch(DirectionalHash, true, true, words) == Result::Unsupported && words == patched);
            const auto path = output / (i == 1 ? "legacy.spv" : "current.spv");
            std::ofstream file(path, std::ios::binary);
            file.write(reinterpret_cast<const char*>(words.data()), std::streamsize(words.size() * 4));
            file.close();
            assert(file);
        }
    }
    std::cout << "GGX binary gates and deterministic transformations passed\n";
}
