// SPDX-License-Identifier: GPL-3.0-or-later
#include "gx_records.h"
#include "engine_hooks.h"
#include "engine_state.h"
#include "gx_objects.h"
#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <span>

namespace BbEngine::GxRecords {
namespace {
using u32 = std::uint32_t;
using u64 = std::uint64_t;

constexpr std::size_t kDraws[4] = {0x21696c0, 0x2169780, 0x2169840, 0x2169900};
constexpr std::uint8_t kPushes[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x50};
constexpr std::uint8_t kPushesWide[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41,
                                        0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xec, 0x18};

constexpr std::size_t kLayout = 0, kTopology = 0x8, kRaster = 0x240, kBlend = 0x4d0, kDepthState = 0x4d8;
constexpr std::size_t kTargets = 0x4e0, kDepthTarget = 0x520, kAccess = 0x528;
constexpr std::size_t kVertexBlock = 0x598, kHullBlock = 0xa90, kDomainBlock = 0xf88, kGeometryBlock = 0x1480, kPixelBlock = 0x1978;
constexpr std::size_t kBlockSamplers = 0, kBlockViews = 0x80, kBlockBuffers = 0x480, kBlockShader = 0x4f0;
constexpr std::size_t kBlockBytes = kBlockShader;
constexpr std::size_t kShaderInputs = 0xe0, kShaderInputCount = 0xe8;
constexpr u32 kMaxInputs = 256;

constexpr u32 kSlotBits = 14;
struct Slot {
    std::atomic<u64> address{0}, mixed{0};
};
std::unique_ptr<Slot[]> g_slots;
std::atomic<bool> g_on{false}, g_checked{false};

struct Current {
    bool set = false;
    u32 count = 0;
    u64 record = 0;
};
thread_local Current g_current;

template <typename T> T Peek(u64 address) {
    T value{};
    std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof value);
    return value;
}

u64 Mix(u64 hash, u64 value) {
    hash = (hash ^ value) * 0x100000001B3ull;
    return hash ^ (hash >> 29);
}

Slot& SlotFor(u64 address) {
    return g_slots[((address >> 2) * 0x9E3779B97F4A7C15ull) >> (64 - kSlotBits)];
}

u64 MixStage(u64 hash, u64 pending, std::size_t block) {
    const u64 at = pending + block;
    const u64 shader = Peek<u64>(at + kBlockShader);
    hash = Mix(hash, shader);
    if (!shader) return hash;
    const u64 inputs = Peek<u64>(shader + kShaderInputs);
    const u32 count = Peek<u32>(shader + kShaderInputCount);
    if (!inputs || count > kMaxInputs) {
        for (std::size_t offset = 0; offset < kBlockBytes; offset += 8) hash = Mix(hash, Peek<u64>(at + offset));
        return hash;
    }
    bool whole = false;
    for (u32 i = 0; i < count && !whole; ++i) {
        const u32 word = Peek<u32>(inputs + u64(i) * 4);
        const u32 kind = word & 0xff, slot = (word >> 16) & 0xff;
        switch (kind) {
        case 0:
        case 1:
            hash = Mix(hash, Peek<u64>(at + kBlockViews + 8 * u64(slot & 0x7f)));
            break;
        case 2:
        case 3:
            hash = Mix(hash, Peek<u64>(pending + kAccess + 8 * u64(slot & 7)));
            break;
        case 4:
            hash = Mix(hash, Peek<u64>(at + kBlockSamplers + 8 * u64(slot & 0xf)));
            break;
        case 5:
            hash = Mix(hash, Peek<u64>(at + kBlockBuffers + 8 * u64(slot & 0xf)));
            break;
        default:
            whole = kind >= 8;
            break;
        }
    }
    if (whole) {
        for (std::size_t offset = 0; offset < kBlockBytes; offset += 8) hash = Mix(hash, Peek<u64>(at + offset));
    }
    return hash;
}

u64 RecordOf(u64 pending) {
    if (Peek<u64>(pending + kHullBlock + kBlockShader) || Peek<u64>(pending + kDomainBlock + kBlockShader) ||
        Peek<u64>(pending + kGeometryBlock + kBlockShader)) {
        return 0;
    }
    u64 hash = Mix(0xCBF29CE484222325ull, GxObjects::Epoch());
    for (const std::size_t state : {kLayout, kTopology, kRaster, kBlend, kDepthState, kDepthTarget}) hash = Mix(hash, Peek<u64>(pending + state));
    for (int i = 0; i < 8; ++i) hash = Mix(hash, Peek<u64>(pending + kTargets + 8 * i));
    hash = MixStage(hash, pending, kVertexBlock);
    hash = MixStage(hash, pending, kPixelBlock);
    return hash ? hash : 1;
}

std::int64_t __attribute__((sysv_abi)) OnDraw(std::uint64_t, const std::uint64_t* saved) {
    Current& current = g_current;
    current.record = RecordOf(saved[4]);
    current.count = static_cast<u32>(saved[3]);
    current.set = current.record != 0;
    return 0;
}
}

bool On() {
    return g_on.load(std::memory_order_relaxed);
}

bool Checked() {
    return g_checked.load(std::memory_order_relaxed);
}

void Stamp(std::uint64_t record_address, std::uint32_t count) {
    Current& current = g_current;
    Slot& slot = SlotFor(record_address);
    if (!current.set || current.count != count) {
        if (slot.address.load(std::memory_order_relaxed) == record_address) slot.address.store(0, std::memory_order_relaxed);
        return;
    }
    current.set = false;
    slot.mixed.store(current.record ^ record_address, std::memory_order_relaxed);
    slot.address.store(record_address, std::memory_order_release);
}

std::uint64_t Take(std::uint64_t record_address) {
    Slot& slot = SlotFor(record_address);
    if (slot.address.load(std::memory_order_acquire) != record_address) return 0;
    const u64 record = slot.mixed.load(std::memory_order_relaxed) ^ record_address;
    slot.address.store(0, std::memory_order_relaxed);
    return record;
}

bool Install(std::uint8_t* image, std::size_t size) {
    const char* on = std::getenv("LANTERN_GX_RECORDS");
    if (on && on[0] != '1' && on[0] != '2') return false;
    const bool compare = on && on[0] == '2';
    if (!GxObjects::On()) {
        host_log("GX records off: they need the object table");
        return false;
    }
    std::array<Hook, 4> hooks;
    bool ready = true;
    for (int i = 0; i < 4 && ready; ++i) {
        const std::span<const std::uint8_t> entry = i == 3 ? std::span<const std::uint8_t>(kPushesWide) : std::span<const std::uint8_t>(kPushes);
        ready = Prepare(hooks[i], image, size, kDraws[i], entry, OnDraw);
    }
    if (!ready) {
        for (Hook& hook : hooks) Discard(hook);
        host_log("GX records off: GX's draw calls differ from 1.09, or the trace holds them");
        return false;
    }
    g_slots = std::make_unique<Slot[]>(std::size_t(1) << kSlotBits);
    g_checked.store(compare, std::memory_order_relaxed);
    g_on.store(true, std::memory_order_relaxed);
    Commit(std::span<Hook>(hooks));
    host_log(compare ? "GX records: on, each pipeline compared with the registers' (LANTERN_GX_RECORDS=2)" : "GX records: on (LANTERN_GX_RECORDS=0 turns them off)");
    return true;
}
}
