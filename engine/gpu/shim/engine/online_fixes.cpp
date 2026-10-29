// SPDX-License-Identifier: GPL-3.0-or-later
#include "online_fixes.h"
#include "engine_hooks.h"
#include "bbport_platform.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <map>
#include <mutex>
#include <vector>
#include "../../bbnet/room/room_service.h"
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

namespace BbEngine::OnlineFixes {
namespace {
constexpr std::size_t kStep = 0x1c43ee0;
constexpr std::uint8_t kStepEntry[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56,
                                       0x41, 0x54, 0x53, 0x48, 0x83, 0xec, 0x40};
constexpr std::size_t kIdLoad = 0x1c43f00;
constexpr std::uint8_t kIdLoadHead[] = {0x4c, 0x8d, 0x35};
constexpr std::uint8_t kIdLoadTail[] = {0x49, 0x8b, 0x06,
                                        0x48, 0x8b, 0x40, 0x08,
                                        0x48, 0x8b, 0x80, 0x90, 0x06, 0x00, 0x00,
                                        0x48, 0x85, 0xc0};
constexpr std::uint64_t kNoId = 0x8000000000000000ull;

constexpr std::size_t kAgeCheck = 0x18729ed;
constexpr std::uint8_t kAgeCheckHead[] = {0xc5, 0xf8, 0x2e, 0x05};
constexpr float kGameTimeout = 180.0f;
constexpr long kLongestTimeout = 600;

std::uint64_t g_game_data_man = 0;
std::atomic<unsigned> g_cleared{0};
std::atomic<bool> g_told_id{false};

__attribute__((format(printf, 1, 2))) void Say(const char* format, ...) {
    std::va_list args;
    va_start(args, format);
    std::printf("Engine: online: ");
    std::vprintf(format, args);
    std::putchar('\n');
    va_end(args);
}

bool Matches(const std::uint8_t* image, std::size_t size, std::size_t at, const std::uint8_t* bytes, std::size_t n) {
    return at <= size && n <= size - at && std::memcmp(image + at, bytes, n) == 0;
}

std::size_t RipTarget(const std::uint8_t* image, std::size_t disp, std::size_t end) {
    std::int32_t d = 0;
    std::memcpy(&d, image + disp, sizeof d);
    return std::size_t(std::int64_t(end) + d);
}

template <typename T> bool Peek(std::uint64_t at, T* out) {
    return at && BbPlatform::ReadProcessMemory(reinterpret_cast<const void*>(at), out, sizeof(T));
}

std::int64_t __attribute__((sysv_abi)) BeforeCharacterStep(std::uint64_t, const std::uint64_t*) {
    std::uint64_t man = 0, player = 0, id = 0;
    if (!Peek(g_game_data_man, &man) || !Peek(man + 8, &player) || !Peek(player + 0x690, &id)) return 0;
    if (id == kNoId) {
        *reinterpret_cast<std::uint64_t*>(player + 0x690) = 0;
        if (g_cleared.fetch_add(1) < 4) Say("this character was left without an id by the server; asking again");
    } else if (id && !g_told_id.exchange(true)) {
        Say("character id %llu", static_cast<unsigned long long>(id));
    }
    return 0;
}

bool FixCharacterId(std::uint8_t* image, std::size_t size) {
    const std::size_t tail = kIdLoad + sizeof kIdLoadHead + 4;
    if (!Matches(image, size, kIdLoad, kIdLoadHead, sizeof kIdLoadHead) ||
        !Matches(image, size, tail, kIdLoadTail, sizeof kIdLoadTail)) {
        Say("character id fix off: the login's character step is not 1.09's");
        return false;
    }
    const std::size_t slot = RipTarget(image, kIdLoad + sizeof kIdLoadHead, tail);
    if (slot >= size) return false;
    Hook hook;
    if (!Prepare(hook, image, size, kStep, kStepEntry, BeforeCharacterStep)) {
        Say("character id fix off: the login's character step is not 1.09's");
        return false;
    }
    g_game_data_man = reinterpret_cast<std::uint64_t>(image + slot);
    Commit(std::span(&hook, 1));
    return true;
}

struct Site {
    std::size_t at;
    std::initializer_list<std::uint8_t> original;
    std::initializer_list<std::uint8_t> patched;
};

struct Rule {
    const char* name;
    std::initializer_list<Site> sites;
};

const Rule kBellRules[] = {
    {"bells anywhere",
     {{0x157f8c8, {0x0f, 0x87, 0x90, 0x00, 0x00, 0x00}, {0x0f, 0x8f, 0x90, 0x00, 0x00, 0x00}},
      {0x157f960, {0x34, 0x01}, {0x31, 0xc0}},
      {0x157f6d1, {0x88, 0xc3}, {0xb1, 0x01}},
      {0x157f6d3, {0x4c, 0x89, 0xf7, 0xe8, 0xf5, 0x9f, 0x34, 0x00}, {0xe9, 0xe9, 0x09, 0x00, 0x00, 0x90, 0x90, 0x90}},
      {0x15068bb, {0x77, 0x4a}, {0x7f, 0x4a}},
      {0x15068fc, {0x88, 0xc3, 0x80, 0xf3, 0x01}, {0xb3, 0x01, 0x90, 0x90, 0x90}},
      {0x191a8c3, {0x18, 0xc9, 0x20, 0xc1, 0xeb, 0x02}, {0x7d, 0x04, 0x88, 0xc1, 0xeb, 0x02}},
      {0x18700d3,
       {0x84, 0xc0, 0x41, 0xbd, 0xff, 0xff, 0xff, 0xff, 0x44, 0x0f, 0x44, 0xeb, 0x41, 0xc1, 0xed, 0x1f},
       {0x45, 0x31, 0xed, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90}}}},
    {"bells after the area boss is dead",
     {{0x18749e8, {0x0f, 0x85, 0x48, 0x01, 0x00, 0x00}, {0x90, 0x90, 0x90, 0x90, 0x90, 0x90}},
      {0x18749f0, {0x0f, 0x88, 0x40, 0x01, 0x00, 0x00}, {0x90, 0x90, 0x90, 0x90, 0x90, 0x90}},
      {0x14b714a, {0x0f, 0x85, 0xc0, 0x03, 0x00, 0x00}, {0x90, 0x90, 0x90, 0x90, 0x90, 0x90}},
      {0x1874b79, {0x74, 0x0d}, {0xeb, 0x0d}}}},
    {"the party stays after a boss",
     {{0x13858f5, {0xe8, 0xa6, 0xae, 0xb4, 0x00}, {0x31, 0xc0, 0x90, 0x90, 0x90}},
      {0x1385a95, {0xe8, 0x06, 0xad, 0xb4, 0x00}, {0x31, 0xc0, 0x90, 0x90, 0x90}}}},
    {"faster summons and joins",
     {{0x196d3a8, {0xc7, 0x05, 0x1e, 0x77, 0xc0, 0x03, 0x00, 0x00, 0xa0, 0x40}, {0xc7, 0x05, 0x1e, 0x77, 0xc0, 0x03, 0xcd, 0xcc, 0xcc, 0x3d}},
      {0x196d3b2, {0xc7, 0x05, 0x1c, 0x77, 0xc0, 0x03, 0x00, 0x00, 0xa0, 0x40}, {0xc7, 0x05, 0x1c, 0x77, 0xc0, 0x03, 0x00, 0x00, 0x80, 0x3f}},
      {0x196d3c6, {0xc7, 0x05, 0x10, 0x77, 0xc0, 0x03, 0x00, 0x00, 0xa0, 0x40}, {0xc7, 0x05, 0x10, 0x77, 0xc0, 0x03, 0x00, 0x00, 0x80, 0x3f}},
      {0x196d3d0, {0xc7, 0x05, 0x0e, 0x77, 0xc0, 0x03, 0x00, 0x00, 0x00, 0x40}, {0xc7, 0x05, 0x0e, 0x77, 0xc0, 0x03, 0x00, 0x00, 0x00, 0x3f}},
      {0x14b80c4, {0x41, 0xc7, 0x45, 0x1c, 0x00, 0x00, 0x40, 0x40}, {0x41, 0xc7, 0x45, 0x1c, 0x00, 0x00, 0x00, 0x3f}},
      {0x14b4975, {0x41, 0xc7, 0x44, 0x24, 0x18, 0x01, 0x00, 0x05, 0x00}, {0x41, 0xc7, 0x44, 0x24, 0x18, 0x01, 0x00, 0x10, 0x00}},
      {0x14bb067, {0xc7, 0x43, 0x18, 0x01, 0x00, 0x05, 0x00}, {0xc7, 0x43, 0x18, 0x01, 0x00, 0x10, 0x00}},
      {0xcdb2e8, {0xc7, 0x84, 0x24, 0x88, 0x00, 0x00, 0x00, 0xb8, 0x0b, 0x00, 0x00}, {0xc7, 0x84, 0x24, 0x88, 0x00, 0x00, 0x00, 0x2c, 0x01, 0x00, 0x00}}}},
    {"bell signs go up 2 s after the bell",
     {{0x18a9264, {0xc7, 0x47, 0x28, 0x00, 0x00, 0xa0, 0x40}, {0xc7, 0x47, 0x28, 0x00, 0x00, 0x00, 0x40}},
      {0x18a9324, {0xc7, 0x47, 0x28, 0x00, 0x00, 0xa0, 0x40}, {0xc7, 0x47, 0x28, 0x00, 0x00, 0x00, 0x40}},
      {0x189a095, {0xc7, 0x43, 0x28, 0x00, 0x00, 0xa0, 0x40}, {0xc7, 0x43, 0x28, 0x00, 0x00, 0x00, 0x40}},
      {0x189a1a5, {0xc7, 0x43, 0x28, 0x00, 0x00, 0xa0, 0x40}, {0xc7, 0x43, 0x28, 0x00, 0x00, 0x00, 0x40}},
      {0x189a2b5, {0xc7, 0x43, 0x28, 0x00, 0x00, 0xa0, 0x40}, {0xc7, 0x43, 0x28, 0x00, 0x00, 0x00, 0x40}},
      {0x189a3c5, {0xc7, 0x43, 0x28, 0x00, 0x00, 0xa0, 0x40}, {0xc7, 0x43, 0x28, 0x00, 0x00, 0x00, 0x40}},
      {0x189a4d5, {0xc7, 0x43, 0x28, 0x00, 0x00, 0xa0, 0x40}, {0xc7, 0x43, 0x28, 0x00, 0x00, 0x00, 0x40}},
      {0x189a5e5, {0xc7, 0x43, 0x28, 0x00, 0x00, 0xa0, 0x40}, {0xc7, 0x43, 0x28, 0x00, 0x00, 0x00, 0x40}},
      {0x189a6f5, {0xc7, 0x43, 0x28, 0x00, 0x00, 0xa0, 0x40}, {0xc7, 0x43, 0x28, 0x00, 0x00, 0x00, 0x40}},
      {0x189a805, {0xc7, 0x43, 0x28, 0x00, 0x00, 0xa0, 0x40}, {0xc7, 0x43, 0x28, 0x00, 0x00, 0x00, 0x40}},
      {0x189a915, {0xc7, 0x43, 0x28, 0x00, 0x00, 0xa0, 0x40}, {0xc7, 0x43, 0x28, 0x00, 0x00, 0x00, 0x40}},
      {0x189aa25, {0xc7, 0x43, 0x28, 0x00, 0x00, 0xa0, 0x40}, {0xc7, 0x43, 0x28, 0x00, 0x00, 0x00, 0x40}}}},
    {"multiplayer messages hold 1.5 s",
     {{0x1bfface, {0x41, 0xc7, 0x45, 0x00, 0x00, 0x00, 0xa0, 0x40}, {0x41, 0xc7, 0x45, 0x00, 0x00, 0x00, 0xc0, 0x3f}}}},
    {"bells without Insight",
     {{0x157fa5c, {0x0f, 0x9f, 0xc0}, {0x30, 0xc0, 0x90}},
      {0x18c92bb, {0x41, 0x0f, 0xb6, 0x47, 0x38, 0xf7, 0xd8}, {0x31, 0xc0, 0x0f, 0x1f, 0x44, 0x00, 0x00}}}},
};

bool BellRules(std::uint8_t* image, std::size_t size) {
    bool any = false;
    for (const Rule& rule : kBellRules) {
        const bool all = std::all_of(rule.sites.begin(), rule.sites.end(), [&](const Site& site) {
            return Matches(image, size, site.at, site.original.begin(), site.original.size());
        });
        if (!all) {
            Say("seamless: %s left alone: the game's code there is not 1.09's", rule.name);
            continue;
        }
        bool written = true;
        for (const Site& site : rule.sites) written = WriteCode(image + site.at, site.patched.begin(), site.patched.size()) && written;
        Say("seamless: %s%s", rule.name, written ? "" : " (not every site could be written)");
        any = any || written;
    }
    return any;
}

constexpr std::size_t kSlotSummary = 0x186fe40;
constexpr std::uint8_t kSlotSummaryEntry[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55,
                                              0x41, 0x54, 0x53, 0x48, 0x81, 0xec, 0x88, 0x00, 0x00, 0x00};
constexpr std::size_t kKindCount = 0x18796c0;
constexpr std::uint8_t kKindCountEntry[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56};
constexpr std::size_t kCoopCount = 0x15bdc20;
constexpr std::uint8_t kCoopCountEntry[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56,
                                            0x41, 0x55, 0x41, 0x54, 0x53, 0x50};
constexpr std::size_t kBossCountCall = 0x17bf187;
constexpr std::uint8_t kBossCountCallBytes[] = {0xe8, 0x94, 0xea, 0xdf, 0xff};
constexpr std::size_t kDopingBranch = 0x138bcbf;
constexpr std::uint8_t kDopingOriginal[] = {0x83, 0xf8, 0x02, 0x75, 0x32};
constexpr std::uint8_t kDopingPatched[] = {0x83, 0xf8, 0x02, 0x7c, 0x32};
constexpr int kCoopKinds[] = {7, 1, 0x15, 0x16, 0x17};
constexpr int kMaxCooperators = 3;

std::uint8_t* g_image = nullptr;
thread_local bool g_in_summary = false;
thread_local bool g_in_coop_count = false;

std::int64_t __attribute__((sysv_abi)) OnSlotSummary(std::uint64_t, const std::uint64_t* saved) {
    if (g_in_summary) return 0;
    const std::uint64_t out = saved[5], selector = saved[4], debug = saved[3];
    g_in_summary = true;
    const std::int64_t result = Call(reinterpret_cast<std::uint64_t>(g_image + kSlotSummary), out, selector, debug);
    g_in_summary = false;
    std::int32_t status = 0;
    if (out && selector && Peek(out + 4, &status) && status == 2) {
        int cooperators = 0;
        for (const int kind : kCoopKinds)
            cooperators += static_cast<std::int32_t>(Call(reinterpret_cast<std::uint64_t>(g_image + kKindCount), selector, kind));
        if (cooperators < kMaxCooperators) *reinterpret_cast<std::int32_t*>(out + 4) = 0;
    }
    const_cast<std::uint64_t*>(saved)[-1] = static_cast<std::uint64_t>(result);
    return 1;
}

std::int64_t __attribute__((sysv_abi)) OnCoopCount(std::uint64_t, const std::uint64_t* saved) {
    if (g_in_coop_count || saved[6] != reinterpret_cast<std::uint64_t>(g_image + kBossCountCall + sizeof kBossCountCallBytes))
        return 0;
    g_in_coop_count = true;
    const std::int64_t count = static_cast<std::int32_t>(
        Call(reinterpret_cast<std::uint64_t>(g_image + kCoopCount), saved[5], saved[4], saved[3]));
    g_in_coop_count = false;
    const_cast<std::uint64_t*>(saved)[-1] = static_cast<std::uint64_t>(std::min<std::int64_t>(count, 2));
    return 1;
}

bool FourPlayers(std::uint8_t* image, std::size_t size) {
    if (!Matches(image, size, kKindCount, kKindCountEntry, sizeof kKindCountEntry) ||
        !Matches(image, size, kBossCountCall, kBossCountCallBytes, sizeof kBossCountCallBytes) ||
        !Matches(image, size, kDopingBranch, kDopingOriginal, sizeof kDopingOriginal)) {
        Say("seamless: 4 players in one world left off: the game's code there is not 1.09's");
        return false;
    }
    g_image = image;
    Hook hooks[2];
    if (!Prepare(hooks[0], image, size, kSlotSummary, kSlotSummaryEntry, OnSlotSummary) ||
        !Prepare(hooks[1], image, size, kCoopCount, kCoopCountEntry, OnCoopCount)) {
        Discard(hooks[0]);
        Discard(hooks[1]);
        Say("seamless: 4 players in one world left off: the game's code there is not 1.09's");
        return false;
    }
    Commit(std::span(hooks, 2));
    WriteCode(image + kDopingBranch, kDopingPatched, sizeof kDopingPatched);
    Say("seamless: up to 4 players in one world (host and 3 cooperators); bosses scale as for 2 cooperators beyond 2");
    return true;
}

constexpr std::size_t kDeathLoad = 0x13cde30;
constexpr std::uint8_t kDeathLoadEntry[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56,
                                            0x41, 0x55, 0x41, 0x54, 0x53, 0x50};
constexpr std::size_t kHostDead = 0x1381870;
constexpr std::uint8_t kHostDeadEntry[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56,
                                           0x53, 0x50, 0x49, 0x89, 0xf6, 0x49, 0x89, 0xff};
constexpr std::size_t kOwnDeathCall = 0x1381ede;
constexpr std::uint8_t kOwnDeathCallBytes[] = {0xe8, 0x4d, 0xbf, 0x04, 0x00};
constexpr std::size_t kPhantomDeathCall = 0x1382663;
constexpr std::uint8_t kPhantomDeathCallBytes[] = {0xe8, 0xc8, 0xb7, 0x04, 0x00};
constexpr std::size_t kRevive = 0x13cbad0;
constexpr std::uint8_t kReviveEntry[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41,
                                         0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xec, 0x48};
constexpr std::size_t kSetFlag = 0x132e5a0;
constexpr std::uint8_t kSetFlagEntry[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x56, 0x53};
constexpr std::size_t kSetHitFilter = 0x1812400;
constexpr std::uint8_t kSetHitFilterEntry[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56};
constexpr std::size_t kReleaseChrSlot = 0x15bc080;
constexpr std::uint8_t kReleaseChrSlotEntry[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x54, 0x53};
constexpr std::size_t kRemoveChr = 0x19186e0;
constexpr std::uint8_t kRemoveChrEntry[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x53, 0x50};
constexpr std::size_t kIdEquals = 0xca1c70;
constexpr std::uint8_t kIdEqualsEntry[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55};
const Site kDeathSites[] = {
    {0x13cbd0f, {0xe8, 0x8c, 0x4a, 0xb0, 0x00}, {0x31, 0xc0, 0x90, 0x90, 0x90}},
    {0x18c4d7d, {0xe8, 0x2e, 0x00, 0x59, 0x00}, {0x0f, 0x1f, 0x44, 0x00, 0x00}},
};

constexpr std::size_t kSessionMan = 0x5540290;
constexpr std::size_t kWorldChrMan = 0x553e878;
constexpr std::size_t kMultiPlayMan = 0x5540230;
constexpr std::size_t kTransition = 0x5556678;
constexpr std::size_t kEventMan = 0x553b108;
constexpr int kRoleHost = 3, kRoleClient = 6;
constexpr std::uint32_t kLocalPlayer = 10000;
constexpr std::uint32_t kDeathScriptFlag = 4000;
constexpr std::uint32_t kDeadHitLayer = 0x36, kAliveHitLayer = 0x08;
constexpr std::size_t kTaskStep = 0x50, kTaskNext = 0x54, kTaskPeer = 0xc8, kTaskMode = 0xe8, kTaskFlags = 0xed;
constexpr int kStepCreate = 4;
constexpr std::size_t kWarp = 0x13cb4f0;
constexpr std::uint8_t kWarpEntry[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41,
                                       0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xec, 0x28};
constexpr std::size_t kGauge = 0x5562878;
constexpr std::size_t kBossId = 0x1658, kBossTarget = 0x165c, kBossStride = 0x14;
constexpr int kBossSlots = 3;
constexpr float kFallDrop = 20.0f;
constexpr double kFallSettle = 1.0;
constexpr int kVitalsFresh = 5000;
constexpr float kSameWorld = 8.0f;
constexpr float kRebuildNear = 12.0f;
constexpr auto kVitalsEvery = std::chrono::milliseconds(250);
constexpr auto kVitalsHeartbeat = std::chrono::milliseconds(2000);

std::uint16_t g_revives = 0;
std::atomic<float> g_camera_yaw{0.0f};
std::atomic<bool> g_camera_known{false};
std::mutex g_rows_mu;
std::vector<PartyRow> g_rows;

std::atomic<bool> g_death_rules{false};

template <typename T> T Field(std::uint64_t base, std::size_t at, T fallback = T{}) {
    T value{};
    return base && Peek(base + at, &value) ? value : fallback;
}

std::uint64_t Global(std::size_t at) {
    return Field<std::uint64_t>(reinterpret_cast<std::uint64_t>(g_image), at);
}

std::uint64_t Fn(std::size_t at) {
    return reinterpret_cast<std::uint64_t>(g_image + at);
}

int Role() {
    return Field<std::int32_t>(Global(kSessionMan), 0x124, -1);
}

std::int32_t Hp(std::uint64_t chr) {
    return Field<std::int32_t>(Field<std::uint64_t>(Field<std::uint64_t>(chr, 0x3b0), 0x20), 0xf8, -1);
}

std::uint64_t LocalPlayer() {
    return Field<std::uint64_t>(Global(kWorldChrMan), 0x60);
}

void WakeHitBodies(std::uint64_t chr) {
    const std::uint64_t bodies = Field<std::uint64_t>(Field<std::uint64_t>(chr, 0x58), 0x20);
    const std::uint32_t filter = Field<std::uint32_t>(bodies, 0xa8);
    if (bodies && (filter & 0x3f) == kDeadHitLayer) Call(Fn(kSetHitFilter), bodies, (filter & ~0x3fu) | kAliveHitLayer);
}

void RearmDeathScript() {
    const std::uint64_t request = Field<std::uint64_t>(Global(kEventMan), 0x10);
    if (request && Field<std::int32_t>(request, 8) > 0) *reinterpret_cast<std::int32_t*>(request + 8) = 0;
    Call(Fn(kSetFlag), 0, kDeathScriptFlag, 0);
}

bool BossUp() {
    const std::uint64_t gauge = Global(kGauge);
    if (gauge < 0x10000) return false;
    for (int i = 0; i < kBossSlots; ++i) {
        if (Field<std::int32_t>(gauge, kBossTarget + i * kBossStride, -1) != -1 &&
            Field<std::int32_t>(gauge, kBossId + i * kBossStride, -1) > 0)
            return true;
    }
    return false;
}

template <typename F> void ForEachPlayer(F&& f) {
    const std::uint64_t world = Global(kWorldChrMan);
    const int count = Field<std::int32_t>(world, 0x30);
    const std::uint64_t entries = Field<std::uint64_t>(world, 0x38);
    if (!world || !entries || count <= 0 || count > 8) return;
    for (int i = 0; i < count; ++i) {
        if (const std::uint64_t chr = Field<std::uint64_t>(entries + std::uint64_t(i) * 0x38, 0)) f(world, chr);
    }
}

bool Position(std::uint64_t chr, float out[3]) {
    const std::uint64_t physics =
        Field<std::uint64_t>(Field<std::uint64_t>(Field<std::uint64_t>(Field<std::uint64_t>(chr, 0x58), 8), 0x3b0), 0x68);
    return physics && Peek(physics + 0x1e0, reinterpret_cast<float(*)[3]>(out));
}

float Facing(std::uint64_t chr) {
    const std::uint64_t physics =
        Field<std::uint64_t>(Field<std::uint64_t>(Field<std::uint64_t>(Field<std::uint64_t>(chr, 0x58), 8), 0x3b0), 0x68);
    return Field<float>(physics, 0x1d4, 0.0f);
}

float Distance2(const float a[3], const float b[3]) {
    const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return dx * dx + dy * dy + dz * dz;
}

std::uint64_t RemoteNear(const float at[3], float within) {
    const std::uint64_t local = LocalPlayer();
    std::uint64_t best = 0;
    float best_d = within * within;
    ForEachPlayer([&](std::uint64_t, std::uint64_t chr) {
        float p[3];
        if (chr == local || !Position(chr, p)) return;
        const float d = Distance2(p, at);
        if (d < best_d) {
            best_d = d;
            best = chr;
        }
    });
    return best;
}

std::vector<BbRoom::Vitals> InWorld() {
    std::vector<BbRoom::Vitals> out;
    for (BbRoom::Vitals& v : BbRoom::Party(kVitalsFresh)) {
        if (RemoteNear(v.pos, kSameWorld)) out.push_back(std::move(v));
    }
    return out;
}

bool AnyoneElseUp(std::uint64_t) {
    for (const BbRoom::Vitals& v : InWorld()) {
        if (v.hp > 0) return true;
    }
    return false;
}

bool SafeSpot(std::uint64_t chr, float pos[3], float rot[3]) {
    const std::uint64_t spot = Field<std::uint64_t>(chr, 0x400);
    return spot && Peek(spot + 0x30, reinterpret_cast<float(*)[3]>(pos)) && Peek(spot + 0x3c, reinterpret_cast<float(*)[3]>(rot));
}

void MoveBodyToSafeSpot(std::uint64_t chr, const float spot[3], const float facing[3]) {
    alignas(16) float pos[4] = {spot[0], spot[1], spot[2], 1.0f};
    alignas(16) float rot[4] = {facing[0], facing[1], facing[2], 0.0f};
    if (const std::uint64_t transition = Global(kTransition)) {
        std::memcpy(reinterpret_cast<void*>(transition + 0x1450), pos, sizeof pos);
        std::memcpy(reinterpret_cast<void*>(transition + 0x1460), rot, sizeof rot);
    }
    Call(Fn(kWarp), kLocalPlayer, pos, rot, -1);
    Call(Field<std::uint64_t>(Field<std::uint64_t>(chr, 0), 0x5f8), chr, pos, rot, 1);
}

bool ReviveHere(const char* who) {
    const std::uint64_t player = LocalPlayer();
    if (!player) return false;
    Call(Fn(kRevive), kLocalPlayer, 0);
    if (Hp(player) <= 0) {
        Say("seamless: %s death: the game could not stand this hunter up again; the death load runs", who);
        return false;
    }
    WakeHitBodies(player);
    if (const std::uint64_t transition = Global(kTransition)) {
        *reinterpret_cast<std::uint8_t*>(transition + 0x1522) = 0;
        *reinterpret_cast<std::uint8_t*>(transition + 0x1523) = 0;
    }
    RearmDeathScript();
    ++g_revives;
    Say("seamless: %s death: up again on the last safe ground, still in the party (HP %d)", who, Hp(player));
    return true;
}

struct PendingRevive {
    bool on = false;
    bool host = false;
    bool waited = false;
    bool moved = false;
    std::uint64_t player = 0;
    std::chrono::steady_clock::time_point moved_at;
};
PendingRevive g_pending;

std::int64_t __attribute__((sysv_abi)) OnDeathLoad(std::uint64_t, const std::uint64_t* saved) {
    const std::uint64_t from = saved[6];
    const int role = Role();
    const bool own = from == Fn(kOwnDeathCall + sizeof kOwnDeathCallBytes) && (role == kRoleHost || role == kRoleClient);
    const bool phantom = from == Fn(kPhantomDeathCall + sizeof kPhantomDeathCallBytes) && role == kRoleClient;
    if (!own && !phantom) return 0;
    g_pending = {};
    g_pending.on = true;
    g_pending.host = role == kRoleHost;
    g_pending.player = LocalPlayer();
    Say("seamless: %s death: no death load, the hunter stays in the party", g_pending.host ? "host" : "guest");
    const_cast<std::uint64_t*>(saved)[-1] = 0;
    return 1;
}

void PendingTick() {
    if (!g_pending.on) return;
    const char* who = g_pending.host ? "host" : "guest";
    const std::uint64_t player = LocalPlayer();
    if (!player || player != g_pending.player || Hp(player) > 0) {
        g_pending.on = false;
        return;
    }
    const int role = Role();
    const bool party = role == kRoleHost || role == kRoleClient;
    if (party && BossUp() && AnyoneElseUp(player)) {
        if (!g_pending.waited) Say("seamless: %s down in a boss fight: waiting for the boss to die or the party to fall", who);
        g_pending.waited = true;
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    float at[3], spot[3], facing[3];
    if (!g_pending.moved && Position(player, at) && SafeSpot(player, spot, facing) && std::isfinite(spot[1]) &&
        (!std::isfinite(at[1]) || at[1] < spot[1] - kFallDrop)) {
        MoveBodyToSafeSpot(player, spot, facing);
        g_pending.moved = true;
        g_pending.moved_at = now;
        Say("seamless: %s fell %.0f m: the body is put on its last safe ground before it gets up", who,
            std::isfinite(at[1]) ? double(spot[1] - at[1]) : 0.0);
        return;
    }
    if (g_pending.moved && now - g_pending.moved_at < std::chrono::duration<double>(kFallSettle)) return;
    g_pending.on = false;
    if (g_pending.waited) Say("seamless: %s gets up in the arena (%s)", who, BossUp() ? "the whole party fell" : "the boss is dead");
    if (!ReviveHere(who)) Call(Fn(kDeathLoad));
}

std::int64_t __attribute__((sysv_abi)) OnHostDead(std::uint64_t, const std::uint64_t*) {
    if (Role() != kRoleClient) return 0;
    Say("seamless: the host died; this guest stays in its world");
    return 1;
}


void RebuildRemote(std::uint64_t world, std::uint64_t chr) {
    const std::uint64_t transition = Global(kTransition);
    Call(Fn(kReleaseChrSlot), Field<std::uint64_t>(transition, 0x16f8), Field<std::uint32_t>(chr, 8));
    Call(Fn(kRemoveChr), world, chr);
    const std::uint64_t man = Global(kMultiPlayMan);
    const std::uint64_t begin = Field<std::uint64_t>(man, 0x10), end = Field<std::uint64_t>(man, 0x18);
    if (!begin || end < begin || end - begin > 64 * 8) return;
    const int count = Field<std::int32_t>(world, 0x30);
    const std::uint64_t entries = Field<std::uint64_t>(world, 0x38);
    for (std::uint64_t at = begin; at < end; at += 8) {
        const std::uint64_t task = Field<std::uint64_t>(at, 0);
        const int mode = Field<std::int32_t>(task, kTaskMode, -1);
        const std::uint8_t flags = Field<std::uint8_t>(task, kTaskFlags);
        if (!task || (mode != 1 && mode != 2) || Field<std::int32_t>(task, kTaskStep, -1) < kStepCreate || (flags & 2))
            continue;
        bool has_chr = false;
        for (int i = 0; i < count && i < 8 && !has_chr; ++i) {
            const std::uint64_t other = Field<std::uint64_t>(entries + std::uint64_t(i) * 0x38, 0);
            has_chr = other && Call(Fn(kIdEquals), other + 0x3d8, task + kTaskPeer) & 0xff;
        }
        if (has_chr) continue;
        *reinterpret_cast<std::uint8_t*>(task + kTaskFlags) = std::uint8_t(flags | 1);
        *reinterpret_cast<std::int32_t*>(task + kTaskNext) = kStepCreate;
    }
    Say("seamless: a party member got up again elsewhere; its body here is built again");
}

bool DeathRules(std::uint8_t* image, std::size_t size) {
    const bool sites = std::all_of(std::begin(kDeathSites), std::end(kDeathSites), [&](const Site& site) {
        return Matches(image, size, site.at, site.original.begin(), site.original.size());
    });
    if (!sites || !Matches(image, size, kOwnDeathCall, kOwnDeathCallBytes, sizeof kOwnDeathCallBytes) ||
        !Matches(image, size, kPhantomDeathCall, kPhantomDeathCallBytes, sizeof kPhantomDeathCallBytes) ||
        !Matches(image, size, kRevive, kReviveEntry, sizeof kReviveEntry) ||
        !Matches(image, size, kWarp, kWarpEntry, sizeof kWarpEntry) ||
        !Matches(image, size, kSetFlag, kSetFlagEntry, sizeof kSetFlagEntry) ||
        !Matches(image, size, kSetHitFilter, kSetHitFilterEntry, sizeof kSetHitFilterEntry) ||
        !Matches(image, size, kReleaseChrSlot, kReleaseChrSlotEntry, sizeof kReleaseChrSlotEntry) ||
        !Matches(image, size, kRemoveChr, kRemoveChrEntry, sizeof kRemoveChrEntry) ||
        !Matches(image, size, kIdEquals, kIdEqualsEntry, sizeof kIdEqualsEntry)) {
        Say("seamless: deaths keep the party left off: the game's code there is not 1.09's");
        return false;
    }
    g_image = image;
    Hook hooks[2];
    if (!Prepare(hooks[0], image, size, kDeathLoad, kDeathLoadEntry, OnDeathLoad) ||
        !Prepare(hooks[1], image, size, kHostDead, kHostDeadEntry, OnHostDead)) {
        Discard(hooks[0]);
        Discard(hooks[1]);
        Say("seamless: deaths keep the party left off: no room for the death hooks");
        return false;
    }
    Commit(std::span(hooks, 2));
    for (const Site& site : kDeathSites) WriteCode(image + site.at, site.patched.begin(), site.patched.size());
    g_death_rules = true;
    Say("seamless: deaths keep the party: a fallen hunter gets up on its last safe ground, nobody is sent home");
    return true;
}

bool SetSignTimeout(std::uint8_t* image, std::size_t size, long seconds) {
    const std::size_t end = kAgeCheck + sizeof kAgeCheckHead + 4;
    if (!Matches(image, size, kAgeCheck, kAgeCheckHead, sizeof kAgeCheckHead)) {
        Say("sign timeout left alone: the net update's age check is not 1.09's");
        return false;
    }
    const std::size_t limit = RipTarget(image, kAgeCheck + sizeof kAgeCheckHead, end);
    float now = 0;
    if (limit > size - sizeof now) return false;
    std::memcpy(&now, image + limit, sizeof now);
    if (now != kGameTimeout) {
        Say("sign timeout left alone: its limit is %.1f, not 180", now);
        return false;
    }
    const float wanted = float(std::min(seconds, kLongestTimeout));
    std::memcpy(image + limit, &wanted, sizeof wanted);
    Say("a summon sign entry times out after %.0f s (the game's 180)", wanted);
    return true;
}
}

bool Install(std::uint8_t* image, std::size_t size) {
    const char* online = std::getenv("BB_ONLINE");
    if (!online || std::strcmp(online, "1") != 0) return false;
    bool any = FixCharacterId(image, size);
    if (const char* backend = std::getenv("BB_NET_BACKEND"); backend && std::strcmp(backend, "room") == 0) {
        g_image = image;
        any = BellRules(image, size) || any;
        any = FourPlayers(image, size) || any;
        any = DeathRules(image, size) || any;
    }
    const char* timeout = std::getenv("BB_SIGN_TIMEOUT");
    const long seconds = timeout ? std::strtol(timeout, nullptr, 10) : 0;
    if (seconds > 0 && seconds != 180) any = SetSignTimeout(image, size, seconds) || any;
    return any;
}

void SendOwnVitals(std::uint64_t local) {
    static BbRoom::Vitals last;
    static std::chrono::steady_clock::time_point sent_at;
    const std::uint64_t data = Field<std::uint64_t>(Field<std::uint64_t>(local, 0x3b0), 0x20);
    BbRoom::Vitals own;
    own.name = BbRoom::PlayerName();
    own.hp = Field<std::int32_t>(data, 0xf8);
    own.hp_max = Field<std::int32_t>(data, 0xfc);
    own.stamina = Field<std::int32_t>(data, 0x134);
    own.stamina_max = Field<std::int32_t>(data, 0x138);
    own.revives = g_revives;
    Position(local, own.pos);
    const auto now = std::chrono::steady_clock::now();
    const bool changed = own.hp != last.hp || own.hp_max != last.hp_max || own.stamina != last.stamina ||
                         own.stamina_max != last.stamina_max || own.revives != last.revives ||
                         Distance2(own.pos, last.pos) > 1.0f;
    if (now - sent_at < (changed ? kVitalsEvery : kVitalsHeartbeat)) return;
    BbRoom::SendVitals(own);
    last = own;
    sent_at = now;
}

void RebuildRevived(std::uint64_t world, const std::vector<BbRoom::Vitals>& party) {
    static std::map<int, std::pair<std::uint16_t, std::array<float, 3>>> seen;
    std::map<int, std::pair<std::uint16_t, std::array<float, 3>>> next;
    for (const BbRoom::Vitals& v : party) {
        const auto was = seen.find(v.slot);
        if (was != seen.end() && v.revives != was->second.first) {
            if (const std::uint64_t corpse = RemoteNear(was->second.second.data(), kRebuildNear))
                RebuildRemote(world, corpse);
        }
        next[v.slot] = {v.revives, {v.pos[0], v.pos[1], v.pos[2]}};
    }
    seen.swap(next);
}

void PublishRows(std::uint64_t local, const std::vector<BbRoom::Vitals>& party) {
    float own[3] = {};
    const bool placed = local && Position(local, own);
    const float view = g_camera_known.load() ? g_camera_yaw.load() : Facing(local);
    std::vector<PartyRow> rows;
    for (const BbRoom::Vitals& v : party) {
        PartyRow row;
        row.name = v.name;
        row.health = v.hp_max > 0 ? std::clamp(float(v.hp) / float(v.hp_max), 0.0f, 1.0f) : 0.0f;
        row.stamina = v.stamina_max > 0 ? std::clamp(float(v.stamina) / float(v.stamina_max), 0.0f, 1.0f) : 0.0f;
        row.down = v.hp <= 0;
        if (placed && RemoteNear(v.pos, kSameWorld)) {
            const float dx = v.pos[0] - own[0], dz = v.pos[2] - own[2];
            row.distance = std::sqrt(Distance2(v.pos, own));
            row.bearing = std::atan2(dx, dz) - view;
        }
        rows.push_back(std::move(row));
    }
    std::lock_guard lock(g_rows_mu);
    g_rows.swap(rows);
}

void Tick() {
    if (!g_image || !BbRoom::Active()) return;
    if (g_death_rules.load()) PendingTick();
    const int role = Role();
    const std::uint64_t world = Global(kWorldChrMan);
    const std::uint64_t local = Field<std::uint64_t>(world, 0x60);
    if (!local) {
        std::lock_guard lock(g_rows_mu);
        g_rows.clear();
        return;
    }
    SendOwnVitals(local);
    const std::vector<BbRoom::Vitals> party = BbRoom::Party(kVitalsFresh);
    if (g_death_rules.load() && (role == kRoleHost || role == kRoleClient)) RebuildRevived(world, party);
    PublishRows(local, party);
}

std::vector<PartyRow> PartyRows() {
    std::lock_guard lock(g_rows_mu);
    return g_rows;
}

void SetCameraYaw(float yaw) {
    g_camera_yaw = yaw;
    g_camera_known = true;
}
}
