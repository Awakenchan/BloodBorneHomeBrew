// SPDX-License-Identifier: GPL-3.0-or-later
#include "auto_exposure.h"
#include "engine_hooks.h"
#include "engine_state.h"
#include "loading.h"
#include "bbport_platform.h"
#include "bbport_settings.h"
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>

namespace BbEngine::Exposure {
namespace {
constexpr std::size_t kEnable = 0xbd4fc0, kTone = 0xbd4d40;
constexpr std::size_t kEnableCall = 0x21d7051, kToneCall = 0x21d7163;
constexpr std::array<std::uint8_t, 5> CallBytes(std::size_t site, std::size_t target) {
    const auto relative = std::uint32_t(target - (site + 5));
    return {0xe8, std::uint8_t(relative), std::uint8_t(relative >> 8),
            std::uint8_t(relative >> 16), std::uint8_t(relative >> 24)};
}
constexpr auto kEnableCaller = CallBytes(kEnableCall, 0xbb31c0);
constexpr auto kToneCaller = CallBytes(kToneCall, 0xbb3550);
constexpr std::uint8_t kEnableEntry[] = {
    0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x50};
constexpr std::uint8_t kToneEntry[] = {
    0x55, 0x48, 0x89, 0xe5, 0x41, 0x56, 0x53, 0x48, 0x83, 0xec, 0x10,
    0xc5, 0xfa, 0x11, 0x5d, 0xe8};
constexpr std::size_t kObject = 0x5465ed0, kPost = 0x55406e0;
constexpr std::array<std::size_t, 12> kOwned = {
    4, 0x24, 0x28, 0x2c, 0x30, 0x34, 0x38, 0x3c, 0x40, 0x44, 0x4c, 0x54};

std::uint64_t g_image = 0;
std::atomic<Status> g_status{Status::Unsupported};
std::atomic<float> g_stops{0};
std::atomic<unsigned> g_recalibrate{0};
unsigned g_seen_recalibrate = 0;
std::uint64_t g_seen_generation = 0;
std::mutex g_mutex;
Policy g_policy;
bool g_meter_requested = false;
float g_authored_baseline = 0;
bool g_post_owned = false;
float g_post_baseline = 0, g_post_applied = 0;

struct Native {
    std::uint64_t object = 0, record = 0, resource = 0;
    std::uint32_t area = 0;
    float dt = 0;
};
Native g_owner;
std::array<float, kOwned.size()> g_saved{};

template <typename T> bool Read(std::uint64_t at, T& out) {
    return at && BbPlatform::ReadProcessMemory(reinterpret_cast<const void*>(at), &out, sizeof out);
}
template <typename T> void Write(std::uint64_t at, T value) {
    std::memcpy(reinterpret_cast<void*>(at), &value, sizeof value);
}
double Now() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
void Publish(Status status, float stops = 0) {
    g_stops.store(stops, std::memory_order_relaxed);
    if (g_status.exchange(status, std::memory_order_relaxed) == status) return;
    const char* text = "off";
    switch (status) {
    case Status::Waiting: text = "manual exposure while loading, in menus or cinematics"; break;
    case Status::Calibrating: text = "learning neutral view (manual exposure)"; break;
    case Status::Active: text = "active, bounded to -1/+0.5 stops"; break;
    case Status::Invalid: text = "manual exposure: invalid or unavailable native meter"; break;
    case Status::Unsupported: text = "unavailable on this executable or tone curve"; break;
    case Status::Off: break;
    }
    host_log("auto exposure: %s", text);
}
void Restore() {
    if (!g_owner.record) return;
    std::uint64_t object = 0, record = 0, resource = 0;
    if (Read(g_image + kObject, object) && object == g_owner.object &&
        Read(object + 0x208, record) && record == g_owner.record &&
        Read(record + 0x60, resource) && resource == g_owner.resource) {
        for (std::size_t i = 0; i < kOwned.size(); ++i)
            Write(record + kOwned[i], g_saved[i]);
    } else {
        host_log("auto exposure: native resource replaced; old record not accessed");
    }
    g_owner = {};
}
void RestorePost() {
    float current = 0;
    if (g_post_owned && Read(g_image + kPost + 0x98, current) && current == g_post_applied)
        Write(g_image + kPost + 0x98, g_post_baseline);
    g_post_owned = false;
}
void Stop(Status status) {
    RestorePost();
    Restore();
    g_policy.Reset();
    g_meter_requested = false;
    Publish(status);
}
bool Context(std::uint64_t object, Native& native) {
    std::uint64_t current = 0;
    int count = 0, active = -1;
    native.object = object;
    return object && Read(g_image + kObject, current) && object == current &&
           Read(object + 0x320, count) && count == 1 &&
           Read(object + 0x200, active) && active == 0 &&
           Read(object + 0x208, native.record) && native.record &&
           Read(native.record + 0x60, native.resource) && native.resource &&
           Read(object + 0x6ac, native.dt);
}
bool Gameplay(std::uint32_t& area) {
    return !Loading::ScreenUp() && !Loading::CinematicPlaying() && world_player_block(&area);
}

std::int64_t __attribute__((sysv_abi)) BeforeEnable(std::uint64_t, const std::uint64_t* saved) {
    if (saved[6] != g_image + kEnableCall + 5) return 0;
    std::scoped_lock lock(g_mutex);
    RestorePost();
    int authored = 0;
    if (!BbSettings::Get().graphics_auto_exposure.load(std::memory_order_relaxed)) {
        Stop(Status::Off);
        return 0;
    }
    // Never override a mod/GPARAM that already selects the game's adaptation branch.
    if (!Read(g_image + kPost + 0x50, authored) || authored || (saved[3] & 0xff) ||
        (std::uint32_t(saved[4]) != 0 && std::uint32_t(saved[4]) != UINT32_MAX)) {
        Stop(Status::Unsupported);
        return 0;
    }
    Native native;
    if (!Gameplay(native.area)) {
        Stop(Status::Waiting);
        return 0;
    }
    if (!Context(saved[5], native)) {
        Stop(Status::Invalid);
        return 0;
    }
    const auto generation = Loading::Generation();
    if (generation != g_seen_generation) {
        Stop(Status::Waiting);
        g_seen_generation = generation;
    }
    if (!Read(g_image + kPost + 0x98, g_authored_baseline) ||
        !std::isfinite(g_authored_baseline) || g_authored_baseline <= 0) {
        Stop(Status::Invalid);
        return 0;
    }
    // The original setter still performs its resource-present check. Only metering
    // is enabled here; the authored branch still supplies the neutral baseline.
    const_cast<std::uint64_t*>(saved)[3] = 1;
    g_meter_requested = true;
    return 0;
}

std::int64_t __attribute__((sysv_abi)) BeforeTone(std::uint64_t, const std::uint64_t* saved) {
    // YEBIS also calls this setter internally, including while metering. Only
    // the game's post-application call supplies an authored exposure baseline.
    if (saved[6] != g_image + kToneCall + 5) return 0;
    std::scoped_lock lock(g_mutex);
    float baseline = 0;
    std::memcpy(&baseline, SavedXmm(saved, 0), sizeof baseline);
    // Render jobs copy the post block before the enable call restores our
    // writeback. Their XMM argument can therefore contain an older adapted value.
    // Use the restored authored field captured at enable, not the job snapshot.
    if (g_meter_requested) {
        baseline = g_authored_baseline;
        std::memcpy(const_cast<std::uint8_t*>(SavedXmm(saved, 0)), &baseline, sizeof baseline);
    }
    if (!BbSettings::Get().graphics_auto_exposure.load(std::memory_order_relaxed)) {
        Stop(Status::Off);
        return 0;
    }
    if (!g_meter_requested)
        return 0;
    Native native;
    float adapted = 0, luminance = 0;
    int index = 0, curve = 0, authored = 0;
    if (!Gameplay(native.area)) {
        Stop(Status::Waiting);
        return 0;
    }
    if (!Context(saved[5], native) ||
        !Read(g_image + kPost + 0x50, authored) || authored) {
        Stop(Status::Invalid);
        return 0;
    }
    if (saved[4] != 2 || !Read(native.resource + 0x500, curve) || (curve != 10 && curve != 2)) {
        Stop(Status::Unsupported);
        return 0;
    }
    if (g_owner.object != native.object || g_owner.record != native.record ||
        g_owner.resource != native.resource) {
        Restore();
        g_policy.Reset();
        for (std::size_t i = 0; i < kOwned.size(); ++i) {
            if (!Read(native.record + kOwned[i], g_saved[i])) {
                Stop(Status::Invalid);
                return 0;
            }
        }
        g_owner = native;
    }
    if (!Read(native.resource + 0x50, index) ||
        !Read(native.resource + 0x5c, luminance) || !Read(native.record + 0x54, adapted)) {
        Stop(Status::Invalid);
        return 0;
    }
    const unsigned recalibrate = g_recalibrate.load(std::memory_order_relaxed);
    const auto result = g_policy.Update(
        {Now(), native.resource, native.area, index, baseline, luminance, adapted, native.dt, curve == 10},
        recalibrate != g_seen_recalibrate);
    g_seen_recalibrate = recalibrate;
    if (result.status == Status::Invalid) {
        Stop(Status::Invalid);
        return 0;
    }
    Write(native.record + 4, result.gray);
    Write(native.record + 0x24, result.minimum);
    Write(native.record + 0x28, result.maximum);
    Write(native.record + 0x2c, baseline);
    Write(native.record + 0x30, result.response.sensitivity);
    Write(native.record + 0x34, 1.0f);
    Write(native.record + 0x38, result.response.speed);
    Write(native.record + 0x3c, result.response.sensitivity);
    Write(native.record + 0x40, 1.0f);
    Write(native.record + 0x44, result.response.speed);
    if (result.reset_output || result.status != Status::Active) {
        Write(native.record + 0x4c, 0.0f);
        Write(native.record + 0x54, baseline);
    }
    std::memcpy(const_cast<std::uint8_t*>(SavedXmm(saved, 0)), &result.exposure, sizeof(float));
    // Match the native adapted branch's post writeback, not just its tone-setter
    // argument. Other render consumers must see the same applied exposure.
    g_post_baseline = baseline;
    g_post_applied = result.exposure;
    Write(g_image + kPost + 0x98, result.exposure);
    g_post_owned = true;
    Publish(result.status, std::log2(result.exposure / baseline));
    return 0;
}
}

bool Install(std::uint8_t* image, std::size_t size) {
    std::array<Hook, 2> hooks;
    if (!image || size < kPost + 0x100 ||
        std::memcmp(image + kEnableCall, kEnableCaller.data(), kEnableCaller.size()) ||
        std::memcmp(image + kToneCall, kToneCaller.data(), kToneCaller.size()) ||
        !Prepare(hooks[0], image, size, kEnable, kEnableEntry, BeforeEnable) ||
        !Prepare(hooks[1], image, size, kTone, kToneEntry, BeforeTone)) {
        for (auto& hook : hooks) Discard(hook);
        host_log("auto exposure unavailable: native hooks could not be verified/prepared");
        return false;
    }
    g_image = reinterpret_cast<std::uint64_t>(image);
    Commit(hooks);
    Publish(Status::Off);
    host_log("auto exposure ready (opt-in): native meter, native bounded response and tonemapper");
    return true;
}
void Recalibrate() { g_recalibrate.fetch_add(1, std::memory_order_relaxed); }
Status CurrentStatus() { return g_status.load(std::memory_order_relaxed); }
float CurrentStops() { return g_stops.load(std::memory_order_relaxed); }
}
