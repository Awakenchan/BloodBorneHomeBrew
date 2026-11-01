// SPDX-License-Identifier: GPL-3.0-or-later
#include "../gpu/shim/engine/native_sss.h"
#include "../gpu/shim/bbport_settings.h"
#include <array>
#include <cassert>
#include <cstring>
#include <vector>

namespace {
std::vector<std::uint8_t> image(0x54b0900);
std::array<std::uint8_t, 0x2980> view{}, replacement{}, unavailable{};
std::array<std::uint64_t, 6> resources{1, 2, 3, 4, 5, 6};
bool readable = true;
constexpr std::size_t enable = 0x2975, disable = 0x54b0870;
constexpr std::array<std::size_t, 5> offsets = {0x200, 0x2a8, 0x6c8, 0x6d8, 0x6e0};
template <typename T> void Put(void* memory, std::size_t offset, T value) {
    std::memcpy(static_cast<std::uint8_t*>(memory) + offset, &value, sizeof value);
}
void Init(auto& buffer, bool enabled = false) {
    buffer.fill(0);
    Put(buffer.data(), 0, reinterpret_cast<std::uint64_t>(image.data()) + 0x53b8450);
    Put(buffer.data(), 0x48, std::uint64_t(0x400000));
    buffer[enable] = enabled;
    for (std::size_t i = 0; i < offsets.size(); ++i)
        Put(buffer.data(), offsets[i], reinterpret_cast<std::uint64_t>(&resources[i]));
}
void Render(auto& buffer) {
    BbEngine::Sss::BeforeRender(reinterpret_cast<std::uint64_t>(buffer.data()));
}
}
namespace BbSettings {
Values& Get() { static Values values; return values; }
}
namespace BbEngine {
void host_log(const char*, ...) {}
bool host_read_safe(const void* p, void* out, std::size_t size) {
    const auto at = reinterpret_cast<std::uintptr_t>(p);
    const auto contains = [&](const auto& buffer) {
        const auto begin = reinterpret_cast<std::uintptr_t>(buffer.data());
        const auto bytes = sizeof(typename std::decay_t<decltype(buffer)>::value_type) * buffer.size();
        return at >= begin && size <= bytes && at - begin <= bytes - size;
    };
    if (!contains(image) && !contains(resources) && !contains(replacement) &&
        !contains(unavailable) && !(readable && contains(view))) return false;
    std::memcpy(out, p, size);
    return true;
}
}
int main() {
    using namespace BbEngine::Sss;
    assert(!Install(image.data(), 0x100));
    assert(!Install(image.data(), image.size()));
    constexpr std::uint8_t entry[] = {
        0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53,
        0x48, 0x81, 0xec, 0xa8, 0x0e, 0x00, 0x00};
    std::memcpy(image.data() + 0x229e990, entry, sizeof entry);
    image[disable] = 1;
    Init(view);
    Init(replacement, true);
    Init(unavailable);
    Put(unavailable.data(), 0x6c8, std::uint64_t(0));
    assert(Install(image.data(), image.size()));
    const auto original = view;
    Render(view);
    assert(view == original && image[disable] == 1 && CurrentStatus() == Status::Off);
    BbSettings::Get().graphics_native_sss = true;
    Render(unavailable);
    assert(CurrentStatus() == Status::Waiting && image[disable] == 1);
    Render(view);
    assert(CurrentStatus() == Status::Active && view[enable] == 1 && image[disable] == 0);
    auto expected = original;
    expected[enable] = 1;
    assert(view == expected);
    Render(view);
    Render(replacement);
    BbSettings::Get().graphics_native_sss = false;
    Tick();
    assert(view == original && replacement[enable] == 1 && image[disable] == 1);
    assert(CurrentStatus() == Status::Off);
    BbSettings::Get().graphics_native_sss = true;
    Render(view);
    image[disable] = 1; // A native/external override must not be undone on disable.
    view[enable] = 0;
    BbSettings::Get().graphics_native_sss = false;
    Tick();
    assert(view == original && image[disable] == 1);
    BbSettings::Get().graphics_native_sss = true;
    Render(view);
    Put(view.data(), 0x6c8, reinterpret_cast<std::uint64_t>(&resources[5]));
    BbSettings::Get().graphics_native_sss = false;
    Tick();
    assert(view[enable] == 1 && image[disable] == 1); // Replaced resources are not ours.
    Init(view);
    BbSettings::Get().graphics_native_sss = true;
    Render(view);
    readable = false;
    BbSettings::Get().graphics_native_sss = false;
    Tick();
    assert(image[disable] == 1);
    readable = true;
    Init(view);
    image[disable] = 0; // An already-enabled native force gate stays enabled afterward.
    BbSettings::Get().graphics_native_sss = true;
    Render(view);
    BbSettings::Get().graphics_native_sss = false;
    Render(view);
    assert(view[enable] == 0 && image[disable] == 0);
}
