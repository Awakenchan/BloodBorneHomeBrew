// SPDX-License-Identifier: GPL-3.0-or-later
#include "loading.h"
#include "engine_hooks.h"
#include "bbport_platform.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace BbEngine::Loading {
namespace {
constexpr std::size_t kLoadAsked = 0x556286b;
constexpr std::size_t kNowLoadingHelper = 0x553e8b8;
constexpr std::size_t kWorldShownA = 0x50, kWorldShownB = 0x51;
constexpr std::size_t kRemo = 0x5540058, kRemoManager = 8, kRemoPlaying = 0x168;
constexpr std::size_t kPacerMode = 0xc, kPacerVblanks = 0x10, kPacerTarget = 0x18;

constexpr std::size_t kTargetStores[] = {0x2034840, 0x203485a, 0x203487e, 0x20348b3};
constexpr std::uint8_t kTargetStore[] = {0x41, 0xc7, 0x44, 0x24, 0x18};
constexpr float kLoadingTarget = 1.0f / 30.0f;

using Clock = std::chrono::steady_clock;
std::uint8_t* g_image = nullptr;
std::atomic<bool> g_up{false};
std::atomic<std::uint64_t> g_generation{0};
bool g_cinematic = false;
Clock::time_point g_started, g_load_began;
std::uint64_t g_frames = 0;
bool g_world_seen = false;
bool g_fast_allowed = false, g_fast_set = false, g_fast_this_load = false;
float g_game_targets[4]{};
float g_target_before = 0;

template <typename T> bool Peek(std::uint64_t at, T* out) {
    return at && BbPlatform::ReadProcessMemory(reinterpret_cast<const void*>(at), out, sizeof(T));
}
std::uint64_t Follow(std::uint64_t at) {
    std::uint64_t v = 0;
    return Peek(at, &v) ? v : 0;
}
std::uint64_t Image() { return reinterpret_cast<std::uint64_t>(g_image); }

bool CutscenePlaying() {
    const std::uint64_t manager = Follow(Follow(Image() + kRemo) + kRemoManager);
    std::uint32_t state = 0;
    return manager && Peek(manager + kRemoPlaying, &state) && (state & 1);
}

void SetLoadingPace(bool on) {
    if (on == g_fast_set || !g_fast_allowed) return;
    for (int i = 0; i < 4; ++i) {
        std::uint8_t* imm = g_image + kTargetStores[i] + sizeof kTargetStore;
        if (on) std::memcpy(&g_game_targets[i], imm, sizeof(float));
        const float value = on ? kLoadingTarget : g_game_targets[i];
        WriteCode(imm, &value, sizeof value);
    }
    g_fast_set = on;
    g_fast_this_load |= on;
}

struct Pace {
    std::uint32_t mode = 0, vblanks = 0;
    float target = 0;
};
Pace PaceOf(std::uint64_t pacer) {
    Pace p;
    Peek(pacer + kPacerMode, &p.mode);
    Peek(pacer + kPacerVblanks, &p.vblanks);
    Peek(pacer + kPacerTarget, &p.target);
    return p;
}
}

void Install(std::uint8_t* image) {
    g_image = image;
    g_started = Clock::now();
    const char* fast = std::getenv("BB_FAST_LOADING");
    const char* fps = std::getenv("BB_FPS");
    if (!fast || std::strcmp(fast, "1") != 0) return;
    if (fps && !std::strcmp(fps, "30")) {
        std::printf("Loading: fast loading not needed at 30 FPS\n");
        return;
    }
    for (const std::size_t at : kTargetStores) {
        if (std::memcmp(image + at, kTargetStore, sizeof kTargetStore) != 0) {
            std::printf("Loading: fast loading off: the frame pacer's code is not 1.09's\n");
            return;
        }
    }
    g_fast_allowed = true;
    std::printf("Loading: fast loading on (loading screens paced at 30 FPS)\n");
}

bool ScreenUp() { return g_up.load(std::memory_order_relaxed); }
bool CinematicPlaying() { return g_image && CutscenePlaying(); }
std::uint64_t Generation() { return g_generation.load(std::memory_order_relaxed); }

void Tick(std::uint64_t pacer) {
    if (!g_image) return;
    std::uint8_t asked = 0, shown_a = 0, shown_b = 0;
    Peek(Image() + kLoadAsked, &asked);
    const std::uint64_t helper = Follow(Image() + kNowLoadingHelper);
    const bool world = helper && ((Peek(helper + kWorldShownA, &shown_a) && shown_a) ||
                                  (Peek(helper + kWorldShownB, &shown_b) && shown_b));
    const bool loading = asked && !world;
    const auto now = Clock::now();
    ++g_frames;
    const bool was = g_up.exchange(loading, std::memory_order_relaxed);
    const bool cinematic = CutscenePlaying();
    if ((loading && !was) || cinematic != g_cinematic)
        g_generation.fetch_add(1, std::memory_order_relaxed);
    g_cinematic = cinematic;
    if (g_fast_allowed) SetLoadingPace(loading && !CutscenePlaying());

    if (loading && !was) {
        g_load_began = now;
        g_frames = 0;
        const Pace p = PaceOf(pacer);
        if (p.target != kLoadingTarget) g_target_before = p.target;
        std::printf("Loading: begins; pacer mode %u, %u vblank(s) a frame, target %.1f ms\n", p.mode, p.vblanks,
                    p.target * 1000.0f);
    } else if (!loading && was) {
        const double seconds = std::chrono::duration<double>(now - g_load_began).count();
        const bool cutscene = CutscenePlaying();
        float live = 0;
        if (g_fast_this_load && !cutscene && g_target_before > 0 && Peek(pacer + kPacerTarget, &live) &&
            live == kLoadingTarget) {
            *reinterpret_cast<volatile float*>(pacer + kPacerTarget) = g_target_before;
            std::printf("Loading: the frame pace back to %.1f ms (the loading pace stayed set)\n",
                        g_target_before * 1000.0f);
        }
        g_fast_this_load = false;
        const Pace p = PaceOf(pacer);
        std::printf("Loading: %.2f s, %llu frames (%.1f ms each)%s; pacer mode %u, %u vblank(s), target %.1f ms\n",
                    seconds, static_cast<unsigned long long>(g_frames),
                    g_frames ? seconds * 1000.0 / double(g_frames) : 0.0, cutscene ? ", a cutscene follows" : "",
                    p.mode, p.vblanks, p.target * 1000.0f);
    }
    if (world && !g_world_seen) {
        g_world_seen = true;
        std::printf("Loading: the first in-game frame, %.1f s after the game started\n",
                    std::chrono::duration<double>(now - g_started).count());
    }
}
}
