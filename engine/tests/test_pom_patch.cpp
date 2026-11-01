// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/vk_pom_patch.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>

namespace {
std::vector<uint32_t> Read(const char* path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    assert(file && file.tellg() >= 20 && file.tellg() % 4 == 0);
    std::vector<uint32_t> words(size_t(file.tellg()) / 4);
    file.seekg(0);
    file.read(reinterpret_cast<char*>(words.data()), std::streamsize(words.size() * 4));
    assert(file && words[0] == 0x07230203);
    return words;
}
size_t Definition(const std::vector<uint32_t>& words, uint32_t opcode, uint32_t id) {
    for (size_t i = 5; i < words.size(); i += words[i] >> 16) {
        assert((words[i] >> 16) && (words[i] >> 16) <= words.size() - i);
        if ((words[i] & 0xffff) == opcode && words[i + 2] == id) return i;
    }
    assert(false);
    return 0;
}
double Refine(double previous_layer, double previous_height, double layer, double height) {
    const double before = previous_layer - previous_height;
    const double after = layer - height;
    return 1 - (layer * before - previous_layer * after) / (before - after);
}
void BracketMath() {
    const double step = 1.0 / 64, jitter = 1.0 / 128;
    const double layer = 1 - jitter - step, height = 1 - step;
    assert(!std::isfinite(Refine(1 - jitter, 1, layer, height)));
    const double corrected = Refine(1, 1, layer, height);
    assert(std::isfinite(corrected) && corrected == 0);
    for (double offset : {0.0, 0.25, 0.999}) {
        const double first_layer = 1 - (1 + offset) * step;
        for (double first_height : {0.0, 0.25, 0.75, 1.0}) {
            if (first_height <= first_layer) continue;
            const double hit = Refine(1, 1, first_layer, first_height);
            assert(std::isfinite(hit) && hit >= 0 && hit <= 1);
        }
    }
    // Subsequent crossings retain the actual sampled predecessor, including zero jitter.
    const double later = Refine(0.8, 0.7, 0.6, 0.75);
    assert(std::isfinite(later) && later > 0.2 && later < 0.4);
}
void CheckFixture(const Vulkan::Pom::PomShader& shader, const char* path,
                  const std::filesystem::path& output) {
    using namespace Vulkan::Pom;
    const auto original = Read(path);
    assert(original.size() == shader.words);
    auto ignored = original;
    PatchPom(shader.hash, false, ignored);
    assert(ignored == original);
    PatchPom(0, true, ignored);
    assert(ignored == original);
    auto changed = original;
    changed.back() ^= 1;
    ignored = changed;
    PatchPom(shader.hash, true, ignored);
    assert(ignored == changed);
    changed = original;
    changed.pop_back();
    ignored = changed;
    PatchPom(shader.hash, true, ignored);
    assert(ignored == changed);
    auto patched = original;
    PatchPom(shader.hash, true, patched);
    assert(patched != original);
    const size_t old_cast = Definition(original, kOpBitcast, shader.upper_layer_cast);
    const size_t cast = Definition(patched, kOpBitcast, shader.upper_layer_cast);
    const size_t select = Definition(patched, kOpSelect, patched[cast + 3]);
    const size_t previous_layer = Definition(original, kOpFAdd, original[old_cast + 3]);
    assert(select + 6 == cast && patched[select + 1] == original[previous_layer + 1]);
    const size_t compare = Definition(patched, kOpIEqual, patched[select + 3]);
    assert(compare + 5 == select && patched[compare + 3] == shader.iteration_phi);
    const size_t zero = Definition(patched, kOpConstant, patched[compare + 4]);
    assert(patched[zero + 3] == 0);
    const size_t iteration = Definition(patched, kOpPhi, shader.iteration_phi);
    assert(patched[zero + 1] == patched[iteration + 1]);
    const size_t one = Definition(patched, kOpConstant, patched[select + 4]);
    assert(patched[one + 1] == patched[select + 1]);
    assert(std::bit_cast<float>(patched[one + 3]) == 1.0f);
    assert(patched[select + 5] == original[old_cast + 3]);
    const auto once = patched;
    PatchPom(shader.hash, true, patched);
    assert(patched == once);
    std::ofstream file(output, std::ios::binary);
    file.write(reinterpret_cast<const char*>(patched.data()), std::streamsize(patched.size() * 4));
    file.close();
    assert(file);
    std::printf("POM %08x: checksum gate, first bracket and idempotence passed\n", unsigned(shader.hash));
}
} // namespace

int main(int argc, char** argv) {
    BracketMath();
    if (argc == 1) {
        std::puts("POM first/later bracket math passed; local native fixtures not supplied");
        return 0;
    }
    assert(argc == 5);
    const std::filesystem::path directory(argv[4]);
    assert(std::filesystem::create_directory(directory));
    for (size_t i = 0; i < Vulkan::Pom::kShaders.size(); ++i)
        CheckFixture(Vulkan::Pom::kShaders[i], argv[i + 1],
                     directory / (std::to_string(i) + ".spv"));
}
