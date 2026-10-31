// SPDX-License-Identifier: GPL-3.0-or-later
#include "../gpu/shim/engine/auto_exposure.h"
#include "../gpu/shim/engine/engine_hooks.h"
#include "../gpu/shim/engine/engine_state.h"
#include "../gpu/shim/bbport_platform.h"
#include "../gpu/shim/bbport_settings.h"
#include <array>
#include <cassert>
#include <chrono>
#include <cstring>
#include <thread>
#include <vector>

namespace {
std::vector<std::uint8_t> image(0x5540800);
std::array<std::uint8_t, 0xc00> object{};
std::array<std::uint8_t, 0x68> record{};
std::array<std::uint8_t, 0x510> resource{}, replacement{};
std::array<BbEngine::Callback, 2> callbacks{};
std::size_t prepared = 0;
bool committed = false, loading = false, cinematic = false, gameplay = true;
std::uint64_t generation = 0;
std::uint32_t area = 30;

template <typename T> void Put(void* memory, std::size_t offset, T value) {
    std::memcpy(static_cast<std::uint8_t*>(memory) + offset, &value, sizeof value);
}
template <typename T> T Get(const void* memory, std::size_t offset) {
    T value;
    std::memcpy(&value, static_cast<const std::uint8_t*>(memory) + offset, sizeof value);
    return value;
}
struct Frame {
    // The real stub stores XMM0 152 bytes below the saved integer argument array.
    std::array<std::uint64_t, 26> words{};
    std::uint64_t* Saved() { return words.data() + 19; }
    Frame(std::uint32_t argument) {
        Saved()[5] = reinterpret_cast<std::uint64_t>(object.data());
        Saved()[4] = argument;
        const float baseline = 3;
        std::memcpy(words.data(), &baseline, sizeof baseline);
    }
    float Exposure() { return Get<float>(words.data(), 0); }
};
Frame Enable(std::uint32_t index = 0, bool authored = false) {
    Frame frame(index);
    frame.Saved()[6] = reinterpret_cast<std::uint64_t>(image.data()) + 0x21d7056;
    frame.Saved()[3] = authored;
    const auto before = frame.words;
    assert(callbacks[0](0, frame.Saved()) == 0);
    for (std::size_t i = 0; i < frame.words.size(); ++i)
        if (i != 22) assert(frame.words[i] == before[i]);
    return frame;
}
float Tone(int index = 0, float baseline = 3) {
    Put(resource.data(), 0x50, index);
    Frame frame(2);
    frame.Saved()[6] = reinterpret_cast<std::uint64_t>(image.data()) + 0x21d7168;
    Put(frame.words.data(), 0, baseline);
    const auto before = frame.words;
    assert(callbacks[1](0, frame.Saved()) == 0);
    for (std::size_t i = 1; i < frame.words.size(); ++i)
        assert(frame.words[i] == before[i]);
    return frame.Exposure();
}
void Neutral() {
    for (int i = 0; i < 21; ++i) {
        Enable();
        assert(Tone(i % 17) == 3);
    }
    for (int i = 0; i < 7; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        Enable();
        assert(Tone((21 + i) % 17) == 3);
    }
    assert(BbEngine::Exposure::CurrentStatus() == BbEngine::Exposure::Status::Active);
}
}

namespace BbSettings {
Values& Get() { static Values values; return values; }
}
namespace BbPlatform {
bool ReadProcessMemory(const void* p, void* out, std::size_t size) {
    const auto at = reinterpret_cast<std::uintptr_t>(p);
    const auto contains = [&](const auto& buffer) {
        const auto start = reinterpret_cast<std::uintptr_t>(buffer.data());
        return at >= start && size <= buffer.size() && at - start <= buffer.size() - size;
    };
    if (!contains(image) && !contains(object) && !contains(record) &&
        !contains(resource) && !contains(replacement)) return false;
    std::memcpy(out, p, size);
    return true;
}
}
namespace BbEngine {
void host_log(const char*, ...) {}
bool world_player_block(std::uint32_t* block) { *block = area; return gameplay; }
bool Prepare(Hook& hook, std::uint8_t* bytes, std::size_t size, std::size_t offset,
             std::span<const std::uint8_t> expected, Callback callback) {
    if (!bytes || offset > size || expected.size() > size - offset) return false;
    assert(prepared < callbacks.size());
    callbacks[prepared++] = callback;
    hook.site = bytes + offset;
    return true;
}
void Discard(Hook& hook) { hook = {}; }
void Commit(std::span<Hook> hooks) { assert(hooks.size() == 2); committed = true; }
namespace Loading {
bool ScreenUp() { return loading; }
bool CinematicPlaying() { return cinematic; }
std::uint64_t Generation() { return generation; }
}
}

int main() {
    using namespace BbEngine::Exposure;
    assert(!Install(image.data(), 0x100));
    assert(!committed && prepared == 0);
    const auto call = [](std::size_t site, std::size_t target) {
        image[site] = 0xe8;
        Put(image.data(), site + 1, std::uint32_t(target - (site + 5)));
    };
    call(0x21d7051, 0xbb31c0);
    assert(!Install(image.data(), image.size()) && prepared == 0);
    call(0x21d7163, 0xbb3550);
    Put(image.data(), 0x5465ed0, reinterpret_cast<std::uint64_t>(object.data()));
    Put(image.data(), 0x5540778, 3.0f);
    Put(object.data(), 0x208, reinterpret_cast<std::uint64_t>(record.data()));
    Put(object.data(), 0x320, 1);
    Put(object.data(), 0x6ac, 1.0f / 60);
    Put(record.data(), 0x60, reinterpret_cast<std::uint64_t>(resource.data()));
    for (auto offset : {4, 0x24, 0x28, 0x2c, 0x30, 0x34, 0x38, 0x3c, 0x40, 0x44})
        Put(record.data(), offset, 0.1f + float(offset) / 100);
    Put(record.data(), 0x54, 3.0f);
    Put(resource.data(), 0x500, 10);
    Put(resource.data(), 0x5c, 0.016f);
    const auto original = record;
    assert(Install(image.data(), image.size()) && committed);
    assert(Enable().Saved()[3] == 0);
    assert(Tone() == 3 && record == original && CurrentStatus() == Status::Off);

    BbSettings::Get().graphics_auto_exposure = true;
    assert(Enable(UINT32_MAX).Saved()[3] == 1); // SysV esi=-1 is zero-extended.
    assert(Tone() == 3 && CurrentStatus() == Status::Calibrating);
    assert(Get<float>(record.data(), 0x24) == 3 && Get<float>(record.data(), 0x28) == 3);
    Neutral();
    Put(record.data(), 0x54, 100.0f);
    assert(std::abs(std::log2(Tone(12) / 3) - 0.5f) < 1e-6);
    assert(Get<float>(image.data(), 0x5540778) == Get<float>(record.data(), 0x28));
    const float applied = Get<float>(image.data(), 0x5540778);
    const float learned_gray = Get<float>(record.data(), 4);
    assert(Tone(12, applied) == applied);
    assert(BbEngine::Exposure::CurrentStatus() == Status::Active);
    assert(Get<float>(record.data(), 4) == learned_gray);
    for (const auto caller : {0xbb6ab1, 0xbd1b89, 0xbd1c93}) {
        Frame internal(2);
        internal.Saved()[6] = reinterpret_cast<std::uint64_t>(image.data()) + caller;
        Put(internal.words.data(), 0, 4.0f);
        const auto registers = internal.words;
        const auto native_record = record;
        const auto post = Get<float>(image.data(), 0x5540778);
        callbacks[0](0, internal.Saved());
        callbacks[1](0, internal.Saved());
        assert(internal.words == registers && record == native_record);
        assert(Get<float>(image.data(), 0x5540778) == post);
        assert(CurrentStatus() == Status::Active);
    }
    Enable();
    assert(Get<float>(image.data(), 0x5540778) == 3);
    assert(Tone(12, applied) == applied); // Job snapshot predates the enable restore.
    assert(CurrentStatus() == Status::Active && Get<float>(record.data(), 4) == learned_gray);
    for (int i = 0; i < 1000; ++i) {
        const float stale_snapshot = Get<float>(image.data(), 0x5540778);
        Enable();
        Put(record.data(), 0x54, 2.2f + float(i % 20) / 10);
        Tone(i % 17, stale_snapshot);
        assert(CurrentStatus() == Status::Active);
        assert(Get<float>(record.data(), 4) == learned_gray);
        assert(Get<float>(record.data(), 0x24) == 1.5f);
    }
    Put(record.data(), 0x54, 0.01f);
    assert(std::abs(std::log2(Tone(13) / 3) + 1.0f) < 1e-6);
    const float gray = Get<float>(record.data(), 4);
    Put(resource.data(), 0x5c, 0.128f);
    Tone(14);
    assert(Get<float>(record.data(), 4) == gray);

    BbSettings::Get().graphics_auto_exposure = false;
    assert(Tone() == 3 && record == original && CurrentStatus() == Status::Off);
    assert(Get<float>(image.data(), 0x5540778) == 3);
    assert(Enable().Saved()[3] == 0);
    BbSettings::Get().graphics_auto_exposure = true;
    assert(Enable(0, true).Saved()[3] == 1 && record == original);
    assert(CurrentStatus() == Status::Unsupported);
    assert(Enable(2).Saved()[3] == 0 && record == original);
    Put(image.data(), 0x5540730, 1);
    assert(Enable().Saved()[3] == 0 && record == original);
    Put(image.data(), 0x5540730, 0);
    loading = true;
    assert(Enable().Saved()[3] == 0 && CurrentStatus() == Status::Waiting);
    loading = false;
    cinematic = true;
    assert(Enable().Saved()[3] == 0 && record == original);
    cinematic = false;
    gameplay = false;
    assert(Enable().Saved()[3] == 0);
    gameplay = true;
    Put(record.data(), 0x60, std::uint64_t(0));
    assert(Enable().Saved()[3] == 0 && CurrentStatus() == Status::Invalid);
    Put(record.data(), 0x60, reinterpret_cast<std::uint64_t>(resource.data()));
    Put(resource.data(), 0x5c, 0.016f);
    Enable();
    Tone();
    Neutral();
    ++generation; // A complete quick load can occur between native callbacks.
    Enable();
    assert(Tone() == 3 && CurrentStatus() == Status::Calibrating);
    Put(image.data(), 0x5540778, 2.5f); // A newer area-authored value is not ours to restore.
    Enable();
    assert(Get<float>(image.data(), 0x5540778) == 2.5f);
    assert(Tone(1, 3) == 2.5f); // A real new authored value wins over an older job.
    assert(Get<float>(record.data(), 0x24) == 2.5f);
    Put(image.data(), 0x5540778, 3.0f);
    Enable();
    Recalibrate();
    Enable();
    assert(Tone() == 3 && CurrentStatus() == Status::Calibrating);
    Put(resource.data(), 0x500, 9);
    assert(Tone() == 3 && CurrentStatus() == Status::Unsupported && record == original);
    Put(resource.data(), 0x500, 10);
    Enable();
    Tone();
    const auto modified = record;
    Put(record.data(), 0x60, reinterpret_cast<std::uint64_t>(replacement.data()));
    BbSettings::Get().graphics_auto_exposure = false;
    Tone();
    auto expected = modified;
    Put(expected.data(), 0x60, reinterpret_cast<std::uint64_t>(replacement.data()));
    assert(record == expected); // Never restore fields into a replaced native allocation.
}
