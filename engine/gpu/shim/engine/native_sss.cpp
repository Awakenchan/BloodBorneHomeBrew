// SPDX-License-Identifier: GPL-3.0-or-later
#include "native_sss.h"
#include "engine_state.h"
#include "bbport_settings.h"
#include <array>
#include <atomic>
#include <cstring>
#include <mutex>

namespace BbEngine::Sss {
namespace {
constexpr std::size_t kRender = 0x229e990, kDisable = 0x54b0870;
constexpr std::size_t kVtable = 0x53b8450, kEnable = 0x2975;
constexpr std::array<std::size_t, 5> kResources = {0x200, 0x2a8, 0x6c8, 0x6d8, 0x6e0};
constexpr std::uint8_t kEntry[] = {
    0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53,
    0x48, 0x81, 0xec, 0xa8, 0x0e, 0x00, 0x00};
std::uint64_t g_image = 0;
std::atomic<Status> g_status{Status::Unsupported};
std::mutex g_mutex;
struct Owner {
    std::uint64_t context = 0;
    std::array<std::uint64_t, kResources.size()> resources{};
    std::uint8_t enabled = 0;
};
std::array<Owner, 16> g_owners{};
bool g_disable_owned = false;
std::uint8_t g_disable_baseline = 0;

template <typename T> bool Read(std::uint64_t at, T& value) {
    return at && host_read_safe(reinterpret_cast<const void*>(at), &value, sizeof value);
}
void Write(std::uint64_t at, std::uint8_t value) {
    std::memcpy(reinterpret_cast<void*>(at), &value, sizeof value);
}
void Publish(Status status) {
    if (g_status.exchange(status, std::memory_order_relaxed) == status) return;
    const char* text = status == Status::Active ? "active: native material profiles and diffuse blur" :
                       status == Status::Off ? "off: original native switches restored" :
                       status == Status::Waiting ? "waiting: no verified allocated SSS view" :
                       "unavailable: executable or native SSS state not verified";
    host_log("native skin SSS: %s", text);
}
bool Snapshot(std::uint64_t context, Owner& owner) {
    std::uint64_t vtable = 0, flags = 0;
    if (!Read(context, vtable) || vtable != g_image + kVtable ||
        !Read(context + 0x48, flags) || !(flags & 0x400000) ||
        !Read(context + kEnable, owner.enabled) || owner.enabled > 1)
        return false;
    for (std::size_t i = 0; i < kResources.size(); ++i) {
        std::uint64_t resource_vtable = 0;
        if (!Read(context + kResources[i], owner.resources[i]) ||
            !Read(owner.resources[i], resource_vtable) || !resource_vtable)
            return false;
    }
    owner.context = context;
    return true;
}
bool Same(const Owner& owner, Owner& current) {
    return Snapshot(owner.context, current) && current.resources == owner.resources;
}
void Restore() {
    for (auto& owner : g_owners) {
        if (!owner.context) continue;
        Owner current;
        if (Same(owner, current)) {
            if (current.enabled == 1) Write(owner.context + kEnable, owner.enabled);
        } else {
            host_log("native skin SSS: view replaced; old switches not accessed");
        }
        owner = {};
    }
    if (g_disable_owned) {
        std::uint8_t value = 0;
        if (Read(g_image + kDisable, value) && value == 0)
            Write(g_image + kDisable, g_disable_baseline);
        else
            host_log("native skin SSS: force-disable changed externally; preserving it");
        g_disable_owned = false;
    }
}
}

void BeforeRender(std::uint64_t context) {
    if (!g_image) return;
    std::scoped_lock lock(g_mutex);
    if (!BbSettings::Get().graphics_native_sss.load(std::memory_order_relaxed)) {
        Restore();
        Publish(Status::Off);
        return;
    }
    Owner view;
    // R9 is this render's view, not the manager's primary (often non-deferred) view.
    if (!Snapshot(context, view)) {
        if (!g_disable_owned) Publish(Status::Waiting);
        return;
    }
    Owner* slot = nullptr;
    for (auto& owner : g_owners) {
        if (owner.context && owner.context != view.context) {
            Owner current;
            if (!Same(owner, current)) {
                host_log("native skin SSS: retired view released");
                owner = {};
            }
        }
        if (owner.context == view.context) {
            if (owner.resources != view.resources) owner = view;
            slot = &owner;
            break;
        }
        if (!owner.context && !slot) slot = &owner;
    }
    std::uint8_t disabled = 0;
    if (!slot || !Read(g_image + kDisable, disabled) || disabled > 1) {
        Restore();
        Publish(Status::Unsupported);
        return;
    }
    if (!slot->context) *slot = view;
    if (!g_disable_owned) {
        g_disable_baseline = disabled;
        g_disable_owned = true;
    }
    Write(g_image + kDisable, 0);
    Write(view.context + kEnable, 1);
    Publish(Status::Active);
}

bool Install(std::uint8_t* image, std::size_t size) {
    if (!image || size <= kDisable ||
        std::memcmp(image + kRender, kEntry, sizeof kEntry)) {
        host_log("native skin SSS unavailable: shared native render entry not verified");
        return false;
    }
    g_image = reinterpret_cast<std::uint64_t>(image);
    Publish(Status::Off);
    return true;
}
void Tick() {
    if (!g_image || BbSettings::Get().graphics_native_sss.load(std::memory_order_relaxed)) return;
    std::scoped_lock lock(g_mutex);
    Restore();
    Publish(Status::Off);
}
Status CurrentStatus() { return g_status.load(std::memory_order_relaxed); }
}
