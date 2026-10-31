// SPDX-License-Identifier: GPL-3.0-or-later
#include "online_fixes.h"
#include "engine_hooks.h"
#include "shared_progress.h"
#include "boss_rewards.h"
#include "party_death.h"
#include "kept_travel.h"
#include "bbport_platform.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <vector>
#include "../../bbnet/room/room_service.h"
#include "../face_shot.h"
#include "../voice.h"
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
    const char* off_switch = nullptr;
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
    {"the bell's X is gone in every area and on every floor",
     {{0x191a8c3, {0x7d, 0x04, 0x88, 0xc1, 0xeb, 0x02}, {0xb1, 0x01, 0x90, 0x90, 0xeb, 0x02}},
      {0x1506a2c, {0x45, 0x20, 0xfc}, {0x41, 0xb4, 0x01}}},
     "BB_BELL_NO_X"},
    {"bells after the area boss is dead",
     {{0x18749e8, {0x0f, 0x85, 0x48, 0x01, 0x00, 0x00}, {0x90, 0x90, 0x90, 0x90, 0x90, 0x90}},
      {0x18749f0, {0x0f, 0x88, 0x40, 0x01, 0x00, 0x00}, {0x90, 0x90, 0x90, 0x90, 0x90, 0x90}},
      {0x14b714a, {0x0f, 0x85, 0xc0, 0x03, 0x00, 0x00}, {0x90, 0x90, 0x90, 0x90, 0x90, 0x90}},
      {0x1874b79, {0x74, 0x0d}, {0xeb, 0x0d}}}},
    {"the party stays after a boss",
     {{0x13858f5, {0xe8, 0xa6, 0xae, 0xb4, 0x00}, {0x31, 0xc0, 0x90, 0x90, 0x90}},
      {0x1385a95, {0xe8, 0x06, 0xad, 0xb4, 0x00}, {0x31, 0xc0, 0x90, 0x90, 0x90}},
      {0x13856f4, {0xe8, 0x37, 0x87, 0x04, 0x00}, {0x0f, 0x1f, 0x44, 0x00, 0x00}},
      {0x13859df, {0xe9, 0x4c, 0x84, 0x04, 0x00}, {0xc3, 0x90, 0x90, 0x90, 0x90}}}},
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
        if (const char* off = rule.off_switch ? std::getenv(rule.off_switch) : nullptr; off && std::strcmp(off, "0") == 0) continue;
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
constexpr auto kRebuildDelay = std::chrono::milliseconds(750);
constexpr std::size_t kCameraChrSite = 0x193be2a;
constexpr std::uint8_t kCameraChrBytes[] = {0x48, 0x8b, 0x00, 0x48, 0x8b, 0x50, 0x28, 0x8b,
                                            0x85, 0xa0, 0xfe, 0xff, 0xff, 0x0f, 0xb6, 0xc8};
constexpr std::size_t kMapList = 0x553b148, kLampPoint = 0x1528, kRespawnMark = 0x1520, kHomeMap = 0x14f0, kLoadMap = 0xc;
constexpr std::uint32_t kPlayerStart = 4, kRejoinMark = 1;
constexpr std::chrono::milliseconds kFightGone{1500}, kWipeAgree{1500}, kWipeWait{8000}, kSeenGrace{3000}, kWipeFresh{30000}, kRejoinHold{20000};
constexpr auto kCardLinger = std::chrono::seconds(30);
constexpr auto kCardGone = std::chrono::seconds(5);
const bool g_linger_cards = [] {
    const char* v = std::getenv("BB_PARTY_CARD_LINGER");
    return v && std::strcmp(v, "1") == 0;
}();
constexpr auto kShowFor = std::chrono::seconds(30);
constexpr std::size_t kLampWarp = 0x13cdf30;
constexpr std::uint8_t kLampWarpEntry[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56,
                                           0x53, 0x48, 0x83, 0xec, 0x38, 0x89, 0xfb};
constexpr std::size_t kLampGate = 0x12f831c;
constexpr std::uint8_t kLampGateBytes[] = {0x85, 0xc9, 0x0f, 0x9f, 0xc1, 0x08, 0xc1};
constexpr std::uint8_t kLampGateOpen[] = {0x31, 0xc9, 0x90, 0x90, 0x90, 0x90, 0x90};
constexpr std::size_t kSendHomeCall = 0x138a51c;
constexpr std::uint8_t kSendHomeCallBytes[] = {0xe8, 0x0f, 0x39, 0x04, 0x00};
constexpr std::size_t kLuaDispatch = 0x1339870;
constexpr std::uint8_t kLuaDispatchEntry[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53,
                                              0x48, 0x81, 0xec, 0xd8, 0x05, 0x00, 0x00, 0x49, 0x89, 0xf4, 0x48, 0x89, 0xfb};
constexpr std::size_t kLuaEventMan = 0x553b0c8;
constexpr std::size_t kSessionStop = 0x1ed07a0;
constexpr std::uint8_t kSessionStopEntry[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41,
                                              0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xec, 0x38};
constexpr std::size_t kStageEndStopCall = 0x19471b1;
constexpr std::uint8_t kStageEndStopCallBytes[] = {0xe8, 0xea, 0x95, 0x58, 0x00};
constexpr std::size_t kDestroyTasks = 0x1e54830;
constexpr std::uint8_t kDestroyTasksEntry[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56,
                                               0x41, 0x55, 0x41, 0x54, 0x53, 0x50};
constexpr std::size_t kStageEndTasksCall = 0x1947207;
constexpr std::uint8_t kStageEndTasksCallBytes[] = {0xe8, 0x24, 0xd6, 0x50, 0x00};
constexpr std::size_t kFlagMode = 0x13bb590;
constexpr std::uint8_t kFlagModeEntry[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54,
                                           0x53, 0x48, 0x81, 0xec, 0x88, 0x00, 0x00, 0x00, 0x41, 0x89, 0xf7};
constexpr std::size_t kFlagSnapshot = 0x13be3c0;
constexpr std::uint8_t kFlagSnapshotEntry[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41,
                                               0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xec, 0x78};
constexpr std::size_t kFlagApply = 0x13beca0;
constexpr std::uint8_t kFlagApplyEntry[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54,
                                            0x53, 0x48, 0x81, 0xec, 0xf8, 0x06, 0x00, 0x00};
constexpr std::size_t kForgetHostChrs = 0x156e950;
constexpr std::uint8_t kForgetHostChrsEntry[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x56, 0x53, 0x48, 0x8b, 0x1d};
constexpr std::size_t kForgetHostDecals = 0x156ea40;
constexpr std::uint8_t kForgetHostDecalsEntry[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x56, 0x53, 0x48, 0x8b, 0x1d};
constexpr std::size_t kWarpParamFind = 0x1f26b40;
constexpr std::uint8_t kWarpParamFindEntry[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x54, 0x53, 0x41, 0x89, 0xf4};
constexpr std::size_t kWarpParamRow = 0x5320b40;
constexpr std::size_t kReloadRules = 0x553d6d0;
constexpr std::size_t kReloadPhase = 0x84, kReloadWatchdog = 0x108;
constexpr int kPhaseSummoned = 3, kPhaseInWorld = 4;
constexpr std::size_t kGuestType = 0x8c, kSlotRole = 0x5d8, kPlayerMap = 0x3f8;
constexpr std::size_t kTransitionAsked = 8, kTransitionMap = 0xc, kTransitionSummonMap = 0x14c4, kTransitionJoined = 0x1521,
                      kTransitionOwnWorld = 0x1592;
constexpr std::size_t kNowLoading = 0x556286b;
constexpr char kHostBell[] = "OnEvent_Call_SOS";
constexpr char kGuestBell[] = "OnEvent_SendSoulSign_NormalCoop";
constexpr auto kSettle = std::chrono::seconds(3);
constexpr auto kRingEvery = std::chrono::seconds(10);
constexpr auto kRejoinFor = std::chrono::seconds(120);
constexpr auto kDreamWait = std::chrono::minutes(15);
constexpr std::uint32_t kDreamArea = 21;

bool DreamLamp(std::uint32_t lamp) {
    return lamp / 100000 == kDreamArea;
}

struct Follow {
    std::uint32_t lamp = 0;
    bool replayed = false;
    bool replaying = false;
    bool left = false;
    bool kept = false;
    std::chrono::steady_clock::time_point at, settled, rang;
};
Follow g_follow;
std::atomic<bool> g_bells_off{false};
std::atomic<bool> g_auto_bell{false};
struct Lead {
    bool on = false;
    bool dream = false;
    bool kept = false;
    int expected = 0;
    std::chrono::steady_clock::duration hold = kSettle * 2;
    std::chrono::steady_clock::time_point at, settled, rang;
};
Lead g_lead;
std::atomic<bool> g_lamp_travel{false};
std::atomic<bool> g_keep_stage_end{false};
std::atomic<bool> g_keep_parts{false};
KeptTravel::Load<std::chrono::steady_clock> g_kept;
std::mutex g_kept_mu;
std::vector<std::uint8_t> g_kept_flags;
std::int32_t g_kept_role = 0;
std::uint8_t g_kept_joined = 0;
constexpr std::size_t kTalkTest = 0x203ad60;
constexpr std::uint8_t kTalkTestEntry[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55,
                                           0x41, 0x54, 0x53, 0x48, 0x81, 0xec, 0x68, 0x03, 0x00, 0x00};
constexpr int kTestIsClient = 19, kTestMultiplayer = 52;
constexpr std::size_t kEmevdDispatch = 0x17b93a0;
constexpr std::uint8_t kEmevdDispatchEntry[] = {0x55, 0x48, 0x89, 0xe5, 0x53, 0x50, 0x48, 0x8b,
                                                0x86, 0xb0, 0x00, 0x00, 0x00, 0x8b, 0x08};
constexpr std::int32_t kLampEvent = 7200;
constexpr std::int32_t kSceneBank = 2002, kScenePlay = 3;
std::atomic<int> g_role{-1};
std::atomic<std::uint32_t> g_lamp_talk{0};
std::atomic<std::int64_t> g_own_lamp_ms{0};
std::atomic<std::uint32_t> g_solo_travel{0};
std::atomic<std::uint32_t> g_lamp_ask{0};
std::atomic<int> g_lamp_answer{0};
constexpr int kGuestHpEffect = 9006;
constexpr std::int64_t kOwnLampMs = 120000;
std::atomic<bool> g_solo_lamp{false};
std::atomic<bool> g_wall_kept_down{false};

constexpr std::size_t kFlagMan = 0x553b100;
constexpr std::size_t kGroupKey = 0x13bc710;
constexpr std::uint8_t kGroupKeyEntry[] = {0x89, 0xd0, 0x09, 0xf0, 0x74, 0x6e};
constexpr std::size_t kSaveRequest = 0x1336da0;
constexpr std::uint8_t kSaveRequestEntry[] = {0x55, 0x48, 0x89, 0xe5, 0x48, 0x8d, 0x05};
constexpr std::uint32_t kFlagStride = 125;
constexpr std::uint32_t kFlagClear = 0x80000000u;
struct FlagRange {
    std::uint32_t first, last, kind;
};
constexpr FlagRange kProgressFlags[] = {
#include "progress_flags.inc"
};
constexpr std::uint32_t kKeepSet = 0;
constexpr auto kProgressScan = std::chrono::seconds(1);
constexpr auto kCatchUpEvery = std::chrono::seconds(15);
constexpr auto kProgressKeep = std::chrono::minutes(10);
constexpr auto kPartyRecent = std::chrono::seconds(60);
constexpr auto kPortraitEvery = std::chrono::seconds(20);
constexpr int kPortraitNameAge = 60000;
constexpr auto kSaveAfter = std::chrono::seconds(3);
constexpr auto kSaveGap = std::chrono::seconds(15);
std::atomic<bool> g_progress{false};
constexpr auto kPresentGrace = std::chrono::seconds(10);
SharedProgress::Presence<std::chrono::steady_clock> g_present{kPresentGrace};
constexpr std::uint32_t kDungeonFlag = 2;
constexpr std::size_t kGetFlagValue = 0x13cfd80;
constexpr std::uint8_t kGetFlagValueEntry[] = {0x55, 0x41, 0x57, 0x41, 0x56, 0x53, 0x41, 0x89,
                                               0xd0, 0x44, 0x8b, 0x4f, 0x1c, 0x31, 0xd2};
constexpr std::size_t kSetFlagValue = 0x13d0060;
constexpr std::uint8_t kSetFlagValueEntry[] = {0x55, 0x41, 0x57, 0x41, 0x56, 0x53, 0x41, 0x89, 0xc8, 0x41, 0x89, 0xd1};
constexpr std::size_t kSetEventFlag = 0x13cfcc0;
constexpr std::uint8_t kSetEventFlagEntry[] = {0x41, 0x89, 0xd0, 0x8b, 0x4f, 0x1c, 0x31, 0xd2};
constexpr std::size_t kInventoryFind = 0x14d9e80;
constexpr std::uint8_t kInventoryFindEntry[] = {0x53, 0x81, 0xe6, 0x00, 0x00, 0x00, 0xf0};
constexpr std::size_t kGiveItem = 0x131cb70;
constexpr std::uint8_t kGiveItemEntry[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54,
                                           0x53, 0x48, 0x83, 0xec, 0x68, 0x89, 0xcb, 0x41, 0x89, 0xd6};
constexpr std::size_t kAwardLot = 0x17ddc50;
constexpr std::uint8_t kAwardLotEntry[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54,
                                           0x53, 0x48, 0x81, 0xec, 0xa8, 0x01, 0x00, 0x00, 0x41, 0x89, 0xd6};
constexpr std::size_t kEventAwardCall = 0x17c1806;
constexpr std::uint8_t kEventAwardCallBytes[] = {0xe8, 0x45, 0xc4, 0x01, 0x00};
constexpr std::int64_t kRewardWindowMs = 60000;
constexpr std::size_t kItemMan = 0x553d6e0;
constexpr auto kBossLotKeep = std::chrono::minutes(10);
std::mutex g_boss_lots_mu;
std::vector<std::int32_t> g_boss_lots_out;
std::vector<std::int32_t> g_boss_trophies_out;
constexpr std::size_t kTrophyMan = 0x55404e8;
BossRewards::Given<std::chrono::steady_clock> g_boss_lots_given{kBossLotKeep};
constexpr std::size_t kMpMessage = 0x1bff910;
constexpr std::uint8_t kMpMessageEntry[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55,
                                            0x41, 0x54, 0x53, 0x48, 0x81, 0xec, 0xc8, 0x00, 0x00, 0x00};
constexpr std::size_t kMpMessagePending = 0x55813d8;
std::atomic<int> g_quieted{0};
std::atomic<std::int64_t> g_fight_seen_ms{0};
std::atomic<bool> g_guest_rewards{false};
std::atomic<std::int64_t> g_fight_start_ms{0};
constexpr std::size_t kMapLoadSlotCall = 0x192876a;
constexpr std::uint8_t kMapLoadSlotCallBytes[] = {0xe8, 0x11, 0x76, 0xaa, 0xff};
constexpr std::size_t kNetChannelSlotCall = 0x1941b05;
constexpr std::uint8_t kNetChannelSlotCallBytes[] = {0xe8, 0x76, 0xe2, 0xa8, 0xff};
constexpr std::size_t kNetChannelObjCall = 0x1937e7b;
constexpr std::uint8_t kNetChannelObjCallBytes[] = {0xe8, 0x00, 0x7f, 0xa9, 0xff};
constexpr std::size_t kHolygrail = 0x553e9e8;
constexpr std::size_t kGameDataMan = 0x553b130;
constexpr std::size_t kInventory = 0x328;
constexpr std::uint32_t kGoods = 0x40000000u;
constexpr std::uint32_t kSlotFlag = 9020;
constexpr int kSlots = 7;
constexpr std::uint32_t kRecordBase = 94000000, kRecordStride = 100000;
constexpr int kRecordWords = 43;
constexpr int kFormWords = 0x7c / 4;
constexpr std::uint32_t kDungeonMaps = 0x1d000000u;
constexpr std::uint32_t kDungeonArea = 29;
constexpr std::uint64_t kNoDungeon = 1ull << 63;
constexpr int kChaliceWait = 9900;
constexpr std::uint32_t kChaliceGoods[] = {6109, 6209, 6219, 6309, 6319, 6409, 6429, 6509, 6529, 6539};
constexpr auto kDungeonEvery = std::chrono::seconds(2);
constexpr auto kDungeonRing = std::chrono::minutes(3);
constexpr auto kChaliceEvery = std::chrono::seconds(10);
constexpr auto kChaliceResend = std::chrono::seconds(30);
std::atomic<bool> g_chalice{false};
std::atomic<bool> g_join_dungeon{false};
std::atomic<bool> g_joined_read{false};
std::chrono::steady_clock::time_point g_save_due;
constexpr auto kVitalsEvery = std::chrono::milliseconds(500);
constexpr auto kVitalsHeartbeat = std::chrono::milliseconds(2000);

std::uint16_t g_revives = 0;
std::atomic<float> g_alive_draw{1.0f};
constexpr std::size_t kBodyRevive = 0x1900710;
constexpr std::uint8_t kBodyReviveEntry[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x56, 0x53, 0x48, 0x89, 0xfb};
constexpr std::size_t kTeamResolve = 0x18c2a00;
constexpr std::uint8_t kTeamResolveEntry[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x56, 0x53, 0x49, 0x89, 0xfe};
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

void RestorePhantomBody(std::uint64_t player) {
    const std::int32_t type = Field<std::int32_t>(player, 0x78, -1);
    const std::int32_t team = Field<std::int32_t>(player, 0x88, -1);
    Call(Fn(kBodyRevive), player);
    if (type >= 0) *reinterpret_cast<std::int32_t*>(player + 0x78) = type;
    if (Field<std::int32_t>(player, 0x88, -1) != team) Call(Fn(kTeamResolve), player);
    const float alive = g_alive_draw.load();
    if (Field<float>(player, 0x208) < alive) *reinterpret_cast<float*>(player + 0x208) = alive;
    Say("seamless: guest body back to its phantom form (type %d, team %d -> %d, draw %.2f)", type, team,
        Field<std::int32_t>(player, 0x88, -1), double(Field<float>(player, 0x208)));
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
    if (Role() == kRoleClient) RestorePhantomBody(player);
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
PartyDeath::FightWatch<std::chrono::steady_clock> g_fight{kFightGone};
PartyDeath::Held<std::chrono::steady_clock> g_all_down;
std::chrono::steady_clock::time_point g_wipe_at{};
std::mutex g_view_mu;
PartyDeath::Spectate g_view;
std::string g_view_name;
int g_view_choices = 0;
std::atomic<std::uint64_t> g_view_chr{0};
std::atomic<bool> g_spectating{false};
std::atomic<bool> g_view_next{false};
bool g_camera_hook = false;

bool IsPlayer(std::uint64_t chr);
bool Loading();

std::int64_t __attribute__((sysv_abi)) OnCameraChr(std::uint64_t, const std::uint64_t* saved) {
    const std::uint64_t chr = g_view_chr.load(std::memory_order_relaxed);
    if (chr && IsPlayer(chr)) const_cast<std::uint64_t*>(saved)[4] = chr;
    return 0;
}

void SetView(std::uint64_t chr) {
    const std::uint64_t was = g_view_chr.exchange(chr, std::memory_order_relaxed);
    if (was && was != chr && IsPlayer(was)) *reinterpret_cast<std::uint8_t*>(was + 0x1e2) &= 0xf7;
}

void ClearSpectate() {
    SetView(0);
    g_spectating = false;
    g_view_next = false;
    std::lock_guard lock(g_view_mu);
    g_view.Clear();
    g_view_name.clear();
    g_view_choices = 0;
}

std::uint64_t LivingBodyNear(const float at[3]) {
    const std::uint64_t local = LocalPlayer();
    std::uint64_t best = 0;
    float best_d = kSameWorld * kSameWorld;
    ForEachPlayer([&](std::uint64_t, std::uint64_t chr) {
        float p[3];
        if (chr == local || Hp(chr) == 0 || !Position(chr, p)) return;
        const float d = Distance2(p, at);
        if (d < best_d) {
            best_d = d;
            best = chr;
        }
    });
    return best;
}

std::vector<PartyDeath::Living> LivingHere(std::chrono::steady_clock::time_point now) {
    static std::map<int, std::chrono::steady_clock::time_point> near;
    std::vector<PartyDeath::Living> out;
    for (const BbRoom::Vitals& v : BbRoom::Party(kVitalsFresh)) {
        if (v.hp <= 0) continue;
        if (LivingBodyNear(v.pos)) near[v.slot] = now;
        const auto seen = near.find(v.slot);
        if (seen != near.end() && now - seen->second <= kSeenGrace) out.push_back({v.slot, v.name});
    }
    return out;
}

void SpectateTick(const std::vector<PartyDeath::Living>& living) {
    static std::uint64_t bound = 0;
    static int bound_slot = -1;
    int slot = -1, was = -1;
    std::string name;
    {
        std::lock_guard lock(g_view_mu);
        was = g_view.Slot();
        slot = g_view.Update(living, g_view_next.exchange(false));
        name = g_view.Name();
        g_view_name = name;
        g_view_choices = g_view.Count();
    }
    g_spectating = true;
    if (slot != was && slot >= 0) Say("seamless: spectating %s", name.c_str());
    if (!g_camera_hook) return;
    std::uint64_t body = 0;
    for (const BbRoom::Vitals& v : BbRoom::Party(kVitalsFresh)) {
        if (v.slot != slot) continue;
        float p[3];
        if (slot == bound_slot && bound && IsPlayer(bound) && Hp(bound) != 0 && Position(bound, p) &&
            Distance2(p, v.pos) < kSameWorld * kSameWorld)
            body = bound;
        else
            body = LivingBodyNear(v.pos);
    }
    if (body) {
        bound = body;
        bound_slot = slot;
    }
    SetView(body);
}

bool LampSpot(float pos[3], float rot[3]) {
    const auto point = Field<std::int32_t>(Global(kTransition), kLampPoint, -1);
    if (point < 0) return false;
    const std::uint64_t list = Field<std::uint64_t>(Global(kMapList), 0x10);
    const int maps = Field<std::int32_t>(list, 0x18);
    const std::uint64_t entries = Field<std::uint64_t>(list, 0x20);
    for (int m = 0; m < maps && m < 16 && entries; ++m) {
        const std::uint64_t msb = Field<std::uint64_t>(entries + std::uint64_t(m) * 0xa0, 0x38);
        const int count = Field<std::int32_t>(msb, 0x2b8);
        const std::uint64_t parts = Field<std::uint64_t>(msb, 0x2c0);
        const bool old = Field<std::uint32_t>(Field<std::uint64_t>(msb, 0x88), 0, 3) < 3;
        for (int i = 0; i < count && i < 8192 && parts; ++i) {
            const std::uint64_t part = Field<std::uint64_t>(parts + std::uint64_t(i) * 8, 0);
            if (!part || Field<std::uint32_t>(part, old ? 0x08 : 0x14) != kPlayerStart) continue;
            if (Field<std::int32_t>(Field<std::uint64_t>(part, old ? 0x68 : 0xb0), 0, -1) != point) continue;
            return Peek(part + (old ? 0x20 : 0x28), reinterpret_cast<float(*)[3]>(pos)) &&
                   Peek(part + (old ? 0x2c : 0x34), reinterpret_cast<float(*)[3]>(rot));
        }
    }
    return false;
}

void PlaceBody(std::uint64_t chr, const float spot[3], const float facing[3]) {
    alignas(16) float pos[4] = {spot[0], spot[1], spot[2], 1.0f};
    alignas(16) float rot[4] = {facing[0], facing[1], facing[2], 0.0f};
    if (const std::uint64_t transition = Global(kTransition)) {
        std::memcpy(reinterpret_cast<void*>(transition + 0x1450), pos, sizeof pos);
        std::memcpy(reinterpret_cast<void*>(transition + 0x1460), rot, sizeof rot);
    }
    Call(Field<std::uint64_t>(Field<std::uint64_t>(chr, 0), 0x5f8), chr, pos, rot, 1);
}

std::int64_t __attribute__((sysv_abi)) OnDeathLoad(std::uint64_t, const std::uint64_t* saved) {
    const std::uint64_t from = saved[6];
    const int role = Role();
    const bool own = from == Fn(kOwnDeathCall + sizeof kOwnDeathCallBytes) && (role == kRoleHost || role == kRoleClient);
    const bool phantom = from == Fn(kPhantomDeathCall + sizeof kPhantomDeathCallBytes) && role == kRoleClient;
    if (from == Fn(kSendHomeCall + sizeof kSendHomeCallBytes) && g_lamp_travel.load()) {
        std::uint32_t lamp = 0;
        if (BbRoom::TakeTravel(lamp) && lamp / 100000 != kDungeonArea) g_follow.lamp = lamp;
        if (g_follow.lamp && !g_follow.replayed) {
            g_follow.replayed = true;
            g_follow.at = std::chrono::steady_clock::now();
            Say("seamless: the host's room closed for its travel: following to lamp %u instead of going home", g_follow.lamp);
            Call(Fn(kLampWarp), g_follow.lamp);
            const_cast<std::uint64_t*>(saved)[-1] = 0;
            return 1;
        }
        return 0;
    }
    if (!own && !phantom) return 0;
    g_pending = {};
    g_pending.on = true;
    g_pending.host = role == kRoleHost;
    g_pending.player = LocalPlayer();
    g_fight.Begin(std::chrono::steady_clock::now());
    g_all_down = {};
    Say("seamless: %s death: no death load yet, the hunter stays in the party", g_pending.host ? "host" : "guest");
    const_cast<std::uint64_t*>(saved)[-1] = 0;
    return 1;
}

void RejoinAfterLoad(std::chrono::steady_clock::time_point now) {
    g_follow = {};
    g_follow.lamp = kRejoinMark;
    g_follow.replayed = true;
    g_follow.at = now;
}

void GuestLoadHome(std::chrono::steady_clock::time_point now) {
    if (const std::uint64_t transition = Global(kTransition)) {
        const auto home = Field<std::uint32_t>(transition, kHomeMap);
        *reinterpret_cast<std::uint8_t*>(transition + kRespawnMark) = 1;
        if (home && home != 0xffffffffu) *reinterpret_cast<std::uint32_t*>(transition + kLoadMap) = home;
    }
    RejoinAfterLoad(now);
    Call(Fn(kDeathLoad));
}

void ExpectBack(std::chrono::steady_clock::time_point now) {
    const int here = std::max(static_cast<int>(InWorld().size()), static_cast<int>(g_present.Here(now).size()));
    if (g_lead.on) {
        g_lead.expected = std::max(g_lead.expected, here);
        g_lead.hold = std::max<std::chrono::steady_clock::duration>(g_lead.hold, kRejoinHold);
        return;
    }
    g_lead = {};
    g_lead.on = here > 0;
    if (g_lead.on) g_bells_off = false;
    g_lead.expected = here;
    g_lead.at = now;
    g_lead.hold = kRejoinHold;
}

void PartyWipe(bool host, std::chrono::steady_clock::time_point now) {
    Say("seamless: the whole party is down: everyone goes back to the lamp");
    if (host) {
        BbRoom::SendPartyCall(BbRoom::PartyCall::Wipe);
        ExpectBack(now);
        Call(Fn(kDeathLoad));
    } else {
        GuestLoadHome(now);
    }
}

void BackToLamp(const char* who, bool guest, std::uint64_t player, std::chrono::steady_clock::time_point now) {
    float spot[3], facing[3];
    const bool lamp = LampSpot(spot, facing);
    if (!lamp && guest) {
        Say("seamless: guest death: its lamp is not in this map: it loads at its own lamp and is summoned back, the party stays");
        BbRoom::SendPartyCall(BbRoom::PartyCall::Rejoin, BbRoom::HostSlot());
        GuestLoadHome(now);
        return;
    }
    if (!ReviveHere(who)) {
        Call(Fn(kDeathLoad));
        return;
    }
    if (lamp) {
        PlaceBody(player, spot, facing);
        Say("seamless: %s back at its lamp with no load, the party stays", who);
    } else {
        Say("seamless: host death: its lamp is not in this map, so it gets up on its last safe ground (a load would close the room)");
    }
}

void PartyCallsTick(int role) {
    const auto now = std::chrono::steady_clock::now();
    for (const auto& [sender, call] : BbRoom::TakePartyCalls()) {
        if (call == BbRoom::PartyCall::Wipe && sender == BbRoom::HostSlot() && role == kRoleClient) {
            g_wipe_at = now;
            RejoinAfterLoad(now);
        } else if (call == BbRoom::PartyCall::Rejoin && role == kRoleHost) {
            ExpectBack(now);
            Say("seamless: a guest is reloading at its lamp: ringing the Beckoning Bell for it");
        }
    }
}

bool PartyDeathRules() {
    static const bool on = [] {
        const char* v = std::getenv("BB_SEAMLESS_DEATHS");
        return v && std::strcmp(v, "1") == 0;
    }();
    return on;
}

bool AnyoneElseUp() {
    for (const BbRoom::Vitals& v : InWorld()) {
        if (v.hp > 0) return true;
    }
    return false;
}

void StandUpTick() {
    if (!g_pending.on) return;
    const char* who = g_pending.host ? "host" : "guest";
    const std::uint64_t player = LocalPlayer();
    if (!player || player != g_pending.player || Hp(player) > 0) {
        g_pending.on = false;
        return;
    }
    const int role = Role();
    const bool party = role == kRoleHost || role == kRoleClient;
    if (party && BossUp() && AnyoneElseUp()) {
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

void PendingTick() {
    if (!PartyDeathRules()) {
        StandUpTick();
        return;
    }
    if (!g_pending.on) {
        if (g_spectating.load()) ClearSpectate();
        return;
    }
    const char* who = g_pending.host ? "host" : "guest";
    const std::uint64_t player = LocalPlayer();
    if (!player || player != g_pending.player || Hp(player) > 0) {
        g_pending.on = false;
        ClearSpectate();
        return;
    }
    if (Loading()) return;
    const auto now = std::chrono::steady_clock::now();
    const int role = Role();
    const bool host = role == kRoleHost;
    float at[3], spot[3], facing[3];
    if (!g_pending.moved && Position(player, at) && SafeSpot(player, spot, facing) && std::isfinite(spot[1]) &&
        (!std::isfinite(at[1]) || at[1] < spot[1] - kFallDrop)) {
        MoveBodyToSafeSpot(player, spot, facing);
        g_pending.moved = true;
        g_pending.moved_at = now;
        Say("seamless: %s fell %.0f m: the body is put on its last safe ground", who,
            std::isfinite(at[1]) ? double(spot[1] - at[1]) : 0.0);
        return;
    }
    const std::vector<PartyDeath::Living> living = LivingHere(now);
    PartyDeath::Situation s;
    s.in_party = host || role == kRoleClient;
    s.boss_up = g_fight.Up(now);
    s.wipe_called = g_wipe_at != std::chrono::steady_clock::time_point{} && now - g_wipe_at < kWipeFresh;
    s.anyone_else_up = !living.empty();
    s.all_down_held = g_all_down.Update(living.empty(), now, host ? kWipeAgree : kWipeWait);
    const PartyDeath::Move move = PartyDeath::Decide(s);
    if (move == PartyDeath::Move::Stay) {
        if (!g_pending.waited) Say("seamless: %s down in a boss fight: spectating until the boss dies or the whole party falls", who);
        g_pending.waited = true;
        SpectateTick(living);
        return;
    }
    if (g_pending.moved && now - g_pending.moved_at < std::chrono::duration<double>(kFallSettle)) return;
    g_pending.on = false;
    g_wipe_at = {};
    ClearSpectate();
    switch (move) {
    case PartyDeath::Move::PartyWipe:
        PartyWipe(host, now);
        break;
    case PartyDeath::Move::ToLamp:
        BackToLamp(who, role == kRoleClient, player, now);
        break;
    default:
        Call(Fn(kDeathLoad));
        break;
    }
}

std::int64_t __attribute__((sysv_abi)) OnHostDead(std::uint64_t, const std::uint64_t*) {
    if (Role() != kRoleClient) return 0;
    Say("seamless: the host died; this guest stays in its world");
    return 1;
}


std::map<std::uint64_t, std::chrono::steady_clock::time_point> g_rebuilding;

void ShowRebuilt() {
    if (g_rebuilding.empty()) return;
    const auto now = std::chrono::steady_clock::now();
    for (auto it = g_rebuilding.begin(); it != g_rebuilding.end();) {
        const std::uint64_t task = it->first;
        std::uint64_t body = 0;
        ForEachPlayer([&](std::uint64_t, std::uint64_t chr) {
            if (!body && chr != LocalPlayer() && (Call(Fn(kIdEquals), chr + 0x3d8, task + kTaskPeer) & 0xff)) body = chr;
        });
        if (!body) {
            it = now - it->second > kShowFor ? g_rebuilding.erase(it) : std::next(it);
            continue;
        }
        const std::uint64_t table = Field<std::uint64_t>(body, 0);
        const std::uint64_t presenting = Field<std::uint64_t>(table, 0x1b8), show = Field<std::uint64_t>(table, 0x610);
        if (presenting && show && !(Call(presenting, body) & 0xff)) {
            *reinterpret_cast<std::uint8_t*>(body + 0x1e2) |= 0x10;
            Call(show, body, 1);
            Say("seamless: the rebuilt body is shown again");
        }
        it = g_rebuilding.erase(it);
    }
}

bool IsPlayer(std::uint64_t chr) {
    bool found = false;
    ForEachPlayer([&](std::uint64_t, std::uint64_t c) { found = found || c == chr; });
    return chr && found;
}

int RewindBodiless(std::uint64_t world, bool rewind) {
    const std::uint64_t man = Global(kMultiPlayMan);
    const std::uint64_t begin = Field<std::uint64_t>(man, 0x10), end = Field<std::uint64_t>(man, 0x18);
    if (!begin || end < begin || end - begin > 64 * 8) return 0;
    const int count = Field<std::int32_t>(world, 0x30);
    const std::uint64_t entries = Field<std::uint64_t>(world, 0x38);
    int bodiless = 0;
    for (std::uint64_t at = begin; at < end; at += 8) {
        const std::uint64_t task = Field<std::uint64_t>(at, 0);
        const int mode = Field<std::int32_t>(task, kTaskMode, -1);
        const std::uint8_t flags = Field<std::uint8_t>(task, kTaskFlags);
        const int step = Field<std::int32_t>(task, kTaskStep, -1);
        if (!task) continue;
        if ((mode != 1 && mode != 2) || step < kStepCreate || (flags & 2)) {
            if (rewind) Say("seamless:   task mode %d step %d flags %#x left alone", mode, step, unsigned(flags));
            continue;
        }
        bool has_chr = false;
        for (int i = 0; i < count && i < 8 && !has_chr; ++i) {
            const std::uint64_t other = Field<std::uint64_t>(entries + std::uint64_t(i) * 0x38, 0);
            has_chr = other && Call(Fn(kIdEquals), other + 0x3d8, task + kTaskPeer) & 0xff;
        }
        if (has_chr) continue;
        ++bodiless;
        if (!rewind) continue;
        *reinterpret_cast<std::uint8_t*>(task + kTaskFlags) = std::uint8_t(flags | 1);
        *reinterpret_cast<std::int32_t*>(task + kTaskNext) = kStepCreate;
        g_rebuilding[task] = std::chrono::steady_clock::now();
        Say("seamless:   task mode %d step %d flags %#x has no body: built again", mode, step, unsigned(flags));
    }
    return bodiless;
}

void RebuildRemote(std::uint64_t world, std::uint64_t chr, const std::string& who) {
    if (chr && IsPlayer(chr) && chr != LocalPlayer()) {
        const std::uint64_t transition = Global(kTransition);
        Call(Fn(kReleaseChrSlot), Field<std::uint64_t>(transition, 0x16f8), Field<std::uint32_t>(chr, 8));
        Call(Fn(kRemoveChr), world, chr);
        Say("seamless: %s got up again: its old body here removed", who.c_str());
    } else {
        Say("seamless: %s got up again: no old body of it here", who.c_str());
    }
    RewindBodiless(world, true);
}

std::int64_t SteadyNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool LampsKeepWorld() {
    static const bool keep = [] {
        const char* v = std::getenv("BB_GUEST_LAMP_MOVES_HOST");
        return !(v && std::strcmp(v, "1") == 0);
    }();
    return keep;
}

bool LampsKeepSession() {
    return g_keep_parts.load();
}

std::uint32_t LampMap(std::uint32_t lamp) {
    std::uint64_t lookup[3] = {Fn(kWarpParamRow), 0xffffffffull, 0};
    Call(Fn(kWarpParamFind), lookup, lamp);
    const auto* row = reinterpret_cast<const std::uint16_t*>(lookup[2]);
    return row ? std::uint32_t(row[0]) << 24 | std::uint32_t(row[1] & 0xff) << 16 : 0;
}

std::vector<std::uint8_t> WorldFlagsAt(std::uint32_t lamp) {
    const std::uint64_t man = Global(kFlagMan), player = LocalPlayer();
    const std::uint32_t map = LampMap(lamp);
    if (!man || !player || !map) return {};
    const auto size = static_cast<std::int32_t>(Call(Fn(kFlagSnapshot), man, 0));
    if (size <= 0 || std::size_t(size) > KeptTravel::kFlagsLargest) return {};
    std::vector<std::uint8_t> out(static_cast<std::size_t>(size));
    auto* at = reinterpret_cast<std::uint32_t*>(player + kPlayerMap);
    const std::uint32_t was = *at;
    *at = map;
    Call(Fn(kFlagSnapshot), man, out.data());
    *at = was;
    return KeptTravel::ValidFlags(out.data(), out.size()) ? out : std::vector<std::uint8_t>{};
}

std::int64_t __attribute__((sysv_abi)) OnSessionStop(std::uint64_t, const std::uint64_t* saved) {
    if (!g_keep_stage_end.load(std::memory_order_relaxed) || saved[6] != Fn(kStageEndStopCall + sizeof kStageEndStopCallBytes)) return 0;
    Say("seamless: the stage ended, the session stays for the party's lamp travel");
    const_cast<std::uint64_t*>(saved)[-1] = 0;
    return 1;
}

std::int64_t __attribute__((sysv_abi)) OnDestroyTasks(std::uint64_t, const std::uint64_t* saved) {
    if (!g_keep_stage_end.load(std::memory_order_relaxed) || saved[6] != Fn(kStageEndTasksCall + sizeof kStageEndTasksCallBytes)) return 0;
    const_cast<std::uint64_t*>(saved)[-1] = 0;
    return 1;
}

std::int64_t __attribute__((sysv_abi)) OnFlagMode(std::uint64_t, const std::uint64_t* saved) {
    if (saved[4] != 1) return 0;
    std::vector<std::uint8_t> flags;
    {
        std::lock_guard lock(g_kept_mu);
        flags.swap(g_kept_flags);
    }
    if (flags.empty()) return 0;
    const std::uint64_t man = saved[5];
    if (Field<std::int32_t>(man, 0x80) == 1) Call(Fn(kFlagMode), man, 0);
    Call(Fn(kFlagApply), man, flags.data());
    if (const std::uint64_t slot = Field<std::uint64_t>(Global(kGameDataMan), 8))
        *reinterpret_cast<std::int32_t*>(slot + kSlotRole) = g_kept_role;
    Say("seamless: the host's world flags for this map are in place (%zu bytes)", flags.size());
    return 0;
}

bool KeepPartyThroughLoad(std::uint32_t lamp, std::vector<std::uint8_t>& flags) {
    if (!LampsKeepSession() || !KeptTravel::CanKeep(lamp)) return false;
    flags = WorldFlagsAt(lamp);
    if (flags.empty()) return false;
    g_keep_stage_end = true;
    g_kept.Begin(std::chrono::steady_clock::now());
    return true;
}

bool FollowKeepingSession(std::uint32_t lamp, std::vector<std::uint8_t>& flags, bool again) {
    const std::uint64_t transition = Global(kTransition), rules = Global(kReloadRules), data = Global(kGameDataMan);
    const std::uint64_t slot = Field<std::uint64_t>(data, 8), local = LocalPlayer();
    if (!LampsKeepSession() || !KeptTravel::CanKeep(lamp) || !KeptTravel::ValidFlags(flags.data(), flags.size()) || !transition ||
        !rules || !slot || !local || Hp(local) <= 0)
        return false;
    {
        std::lock_guard lock(g_kept_mu);
        g_kept_flags = std::move(flags);
    }
    g_kept_role = Field<std::int32_t>(slot, kSlotRole);
    if (!again) g_kept_joined = Field<std::uint8_t>(transition, kTransitionJoined);
    *reinterpret_cast<std::uint8_t*>(transition + kTransitionJoined) = 0;
    *reinterpret_cast<std::int32_t*>(data + kGuestType) = Field<std::int32_t>(local, 0x78);
    Call(Fn(kForgetHostChrs), 0);
    Call(Fn(kForgetHostDecals), 0);
    *reinterpret_cast<std::int32_t*>(rules + kReloadPhase) = kPhaseSummoned;
    *reinterpret_cast<float*>(rules + kReloadWatchdog) = 0.0f;
    const std::uint8_t own_world = Field<std::uint8_t>(transition, kTransitionOwnWorld);
    *reinterpret_cast<std::uint8_t*>(transition + kTransitionOwnWorld) = 1;
    g_follow.replaying = true;
    Call(Fn(kLampWarp), lamp);
    g_follow.replaying = false;
    *reinterpret_cast<std::uint8_t*>(transition + kTransitionOwnWorld) = own_world;
    *reinterpret_cast<std::int32_t*>(transition + kTransitionSummonMap) = Field<std::int32_t>(transition, kTransitionMap);
    g_follow.kept = true;
    g_kept.Begin(std::chrono::steady_clock::now());
    return true;
}

std::int64_t __attribute__((sysv_abi)) OnLampWarp(std::uint64_t, const std::uint64_t* saved) {
    if (g_follow.replaying) return 0;
    const int role = Role();
    const bool hosting = role == kRoleHost;
    const std::uint32_t lamp = static_cast<std::uint32_t>(saved[5]);
    if (LampsKeepWorld() && role == kRoleClient && lamp / 100000 != kDungeonArea) {
        if (LampsKeepSession() && KeptTravel::CanKeep(lamp)) g_lamp_ask.store(lamp, std::memory_order_relaxed);
        else g_solo_travel.store(lamp, std::memory_order_relaxed);
        const_cast<std::uint64_t*>(saved)[-1] = 0;
        return 1;
    }
    if (LampsKeepWorld() && hosting && SteadyNowMs() - g_own_lamp_ms.load(std::memory_order_relaxed) > kOwnLampMs) {
        Say("seamless: a lamp travel this host did not choose (a guest's lamp) was left out: the host's world stays put");
        const_cast<std::uint64_t*>(saved)[-1] = 0;
        return 1;
    }
    if (!hosting && !(g_lead.on && g_lead.dream)) return 0;
    const int expected = hosting ? static_cast<int>(InWorld().size()) : g_lead.expected;
    std::vector<std::uint8_t> flags;
    const bool kept = hosting && expected > 0 && KeepPartyThroughLoad(lamp, flags);
    BbRoom::SendTravel(lamp, flags);
    g_lead = {};
    g_lead.on = expected > 0;
    if (g_lead.on) g_bells_off = false;
    g_lead.dream = DreamLamp(lamp);
    g_lead.kept = kept;
    g_lead.at = std::chrono::steady_clock::now();
    g_lead.expected = expected;
    Say("seamless: host travels to lamp %u%s: %d party member(s) follow%s", lamp, g_lead.dream ? " (the Hunter's Dream)" : "",
        expected, kept ? ", the session stays" : "");
    return 0;
}

bool Loading() {
    return Field<std::uint8_t>(reinterpret_cast<std::uint64_t>(g_image), kNowLoading) != 0;
}

constexpr int kHostBellEffect = 9000, kGuestBellEffect = 9005;
constexpr std::size_t kRemoveEffect = 0x17e1d90;
constexpr std::uint8_t kRemoveEffectEntry[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x50};

void AddEffect(std::uint64_t chr, int id) {
    using AddFn = void(__attribute__((sysv_abi))*)(std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
                                                     std::uint64_t, std::uint64_t, float, float, float, float, float);
    const std::uint64_t fn = Field<std::uint64_t>(Field<std::uint64_t>(chr, 0), 0x3f0);
    if (fn) reinterpret_cast<AddFn>(fn)(chr, std::uint64_t(id), chr, 0, 1, 0, 0, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f);
}

std::uint64_t EffectNode(std::uint64_t chr, int id) {
    const std::uint64_t effects = Field<std::uint64_t>(chr, 0x1c8);
    int n = 0;
    for (std::uint64_t node = Field<std::uint64_t>(effects, 8); node && n < 256; node = Field<std::uint64_t>(node, 0x58), ++n)
        if (Field<std::int32_t>(node, 0x40) == id) return node;
    return 0;
}

bool RemoveEffect(std::uint64_t chr, int id) {
    const std::uint64_t node = EffectNode(chr, id);
    if (!node || std::memcmp(g_image + kRemoveEffect, kRemoveEffectEntry, sizeof kRemoveEffectEntry) != 0) return false;
    Call(Fn(kRemoveEffect), Field<std::uint64_t>(chr, 0x1c8), node, 1);
    return true;
}

bool RingBell(const char* event) {
    const std::uint64_t local = LocalPlayer();
    if (!local || Hp(local) <= 0) return false;
    const int effect = std::strcmp(event, kHostBell) == 0 ? kHostBellEffect : kGuestBellEffect;
    if (EffectNode(local, effect)) return true;
    AddEffect(local, effect);
    return EffectNode(local, effect) != 0;
}

bool SilenceBell(const char* event) {
    const std::uint64_t local = LocalPlayer();
    return local && RemoveEffect(local, std::strcmp(event, kHostBell) == 0 ? kHostBellEffect : kGuestBellEffect);
}

bool Settled(std::chrono::steady_clock::time_point& since, std::chrono::steady_clock::time_point now) {
    const std::uint64_t local = LocalPlayer();
    if (!local || Loading() || Hp(local) <= 0) {
        since = {};
        return false;
    }
    if (since == std::chrono::steady_clock::time_point{}) since = now;
    return now - since >= kSettle;
}

void SoloTravelTick(int role, std::chrono::steady_clock::time_point now) {
    static std::uint32_t lamp = 0;
    static std::chrono::steady_clock::time_point asked, settled;
    if (const std::uint32_t chosen = g_solo_travel.exchange(0, std::memory_order_relaxed)) {
        lamp = chosen;
        asked = now;
        Say("seamless: a guest's lamp to %u: leaving the host's world first, then travelling there alone", lamp);
        g_bells_off = true;
        if (!BbRoom::LeaveParty()) Say("seamless: the host did not answer the leave (an older beta?); use Return to your own world");
    }
    if (!lamp) return;
    if (now - asked > std::chrono::minutes(2)) {
        Say("seamless: the guest's lamp travel to %u gave up: still in the host's world", lamp);
        lamp = 0;
        return;
    }
    if (role == kRoleClient || !Settled(settled, now)) return;
    Say("seamless: back home: travelling to lamp %u", lamp);
    g_follow.replaying = true;
    Call(Fn(kLampWarp), lamp);
    g_follow.replaying = false;
    lamp = 0;
}

void LampAskTick(int role) {
    if (const std::uint32_t lamp = g_lamp_ask.exchange(0, std::memory_order_relaxed); lamp && role == kRoleClient) {
        Say("seamless: a guest's lamp to %u: the host is asked, nobody moves until it accepts", lamp);
        BbRoom::SendLampAsk(lamp);
    }
    const int answer = g_lamp_answer.exchange(0, std::memory_order_relaxed);
    int slot = 0;
    std::string who;
    std::uint32_t lamp = 0;
    if (!answer || !BbRoom::IsHost() || !BbRoom::PendingLampAsk(slot, who, lamp)) return;
    const std::uint64_t local = LocalPlayer();
    const bool can = answer == 1 && role == kRoleHost && local && Hp(local) > 0 && !Loading() && !BossUp() && !g_lead.on && LampMap(lamp);
    if (answer == 1 && !can) Say("seamless: %s's lamp to %u can't be taken right now: the party stays", who.c_str(), lamp);
    BbRoom::AnswerLampAsk(can);
    if (!can) return;
    Say("seamless: the host accepted %s's lamp to %u: the party travels", who.c_str(), lamp);
    g_own_lamp_ms.store(SteadyNowMs(), std::memory_order_relaxed);
    Call(Fn(kLampWarp), lamp);
}

std::uint32_t OwnMap() {
    return Field<std::uint32_t>(LocalPlayer(), 0x3f8);
}

std::uint32_t HostMap() {
    const int host = BbRoom::HostSlot();
    for (const BbRoom::Vitals& v : BbRoom::Party(kVitalsFresh))
        if (v.slot == host) return v.map;
    return 0;
}

void KeptEnd(int role, bool stayed) {
    g_keep_stage_end = false;
    {
        std::lock_guard lock(g_kept_mu);
        g_kept_flags.clear();
    }
    if (!stayed) g_bells_off = false;
    if (g_follow.kept) {
        const std::uint64_t transition = Global(kTransition), rules = Global(kReloadRules);
        if (transition) *reinterpret_cast<std::uint8_t*>(transition + kTransitionJoined) = g_kept_joined;
        if (rules && Field<std::int32_t>(rules, kReloadPhase) == kPhaseSummoned)
            *reinterpret_cast<std::int32_t*>(rules + kReloadPhase) = kPhaseInWorld;
        g_follow.kept = false;
        if (stayed) {
            g_follow = {};
        } else {
            g_follow.at = std::chrono::steady_clock::now();
            if (role == kRoleClient && !BbRoom::LeaveParty())
                Say("seamless: the host did not answer the leave (an older beta?); use Return to your own world");
        }
    }
    if (g_lead.kept) {
        g_lead.kept = false;
        g_lead.at = std::chrono::steady_clock::now();
        if (stayed) g_lead = {};
    }
    Say("seamless: %s", stayed ? "the party stayed together through the lamp travel"
                               : "the session could not be kept through the lamp travel: back to rejoining by bell");
}

void KeptTick(int role, std::chrono::steady_clock::time_point now) {
    if (!g_kept.Active()) return;
    const bool guest = g_follow.kept;
    const bool loading = Loading() || Field<std::uint8_t>(Global(kTransition), kTransitionAsked);
    if (guest && loading)
        if (const std::uint64_t rules = Global(kReloadRules)) *reinterpret_cast<float*>(rules + kReloadWatchdog) = 0.0f;
    const std::uint64_t world = Global(kWorldChrMan), local = LocalPlayer();
    const bool placed = local && !loading;
    const std::uint32_t mine = placed ? OwnMap() : 0;
    bool here = false;
    if (placed) {
        if (guest) {
            here = HostMap() == mine;
        } else {
            int back = 0;
            for (const BbRoom::Vitals& v : BbRoom::Party(kVitalsFresh))
                if (v.map == mine) ++back;
            here = back >= g_lead.expected;
        }
    }
    const int missing = placed ? RewindBodiless(world, false) : 0;
    const KeptTravel::Step step = g_kept.Tick(now, loading, placed, role == (guest ? kRoleClient : kRoleHost), here, missing);
    if (g_kept.LoadOver()) g_keep_stage_end = false;
    if (step == KeptTravel::Step::Rebuild) {
        Say("seamless: arrived with the session kept: building the party's bodies here (%d missing)", missing);
        RewindBodiless(world, true);
    } else if (step == KeptTravel::Step::Done) {
        KeptEnd(role, true);
    } else if (step == KeptTravel::Step::GiveUp) {
        KeptEnd(role, false);
    }
}

void TravelTick() {
    if (!g_lamp_travel.load()) return;
    const auto now = std::chrono::steady_clock::now();
    const int role = Role();
    static std::chrono::steady_clock::time_point last_client;
    if (role == kRoleClient) last_client = now;
    SoloTravelTick(role, now);
    LampAskTick(role);
    KeptTick(role, now);
    std::uint32_t lamp = 0;
    std::vector<std::uint8_t> flags;
    const bool following = role == kRoleClient || ((g_follow.replayed || now - last_client < std::chrono::seconds(60)) && !g_bells_off.load());
    if (following && !Loading() && LocalPlayer() && BbRoom::TakeTravel(lamp, &flags)) {
        if (lamp / 100000 == kDungeonArea) {
            Say("seamless: the host went to a chalice dungeon's lamp %u: this hunter is summoned into it instead", lamp);
            return;
        }
        const bool again = g_follow.kept;
        g_bells_off = false;
        g_follow = {};
        g_follow.lamp = lamp;
        g_follow.replayed = true;
        g_follow.at = now;
        if (role == kRoleClient && FollowKeepingSession(lamp, flags, again)) {
            Say("seamless: following the host to lamp %u with the session kept", lamp);
            return;
        }
        if (again) {
            if (const std::uint64_t transition = Global(kTransition))
                *reinterpret_cast<std::uint8_t*>(transition + kTransitionJoined) = g_kept_joined;
            g_kept = {};
        }
        Say("seamless: following the host to lamp %u", lamp);
        g_follow.replaying = true;
        Call(Fn(kLampWarp), lamp);
        g_follow.replaying = false;
        return;
    }
    if (g_follow.replayed && !g_follow.kept) {
        if (role != kRoleClient) g_follow.left = true;
        const bool dream = DreamLamp(g_follow.lamp);
        if (g_follow.left && role == kRoleClient) {
            Say("seamless: back in the host's world");
            g_follow = {};
        } else if (now - g_follow.at > (dream ? kDreamWait : kRejoinFor)) {
            Say("seamless: the host was not found again after its travel; ring your bell to rejoin");
            g_follow = {};
        } else if (!g_bells_off.load() && !dream && role == 0 && Settled(g_follow.settled, now) && now - g_follow.rang >= kRingEvery) {
            g_follow.rang = now;
            Say("seamless: arrived at the host's lamp: ringing the Small Resonant Bell (%s)",
                RingBell(kGuestBell) ? "rung" : "not rung");
        }
    }
    if (g_lead.on && !g_lead.kept) {
        const int back = static_cast<int>(InWorld().size());
        if (now - g_lead.at > (g_lead.dream ? kDreamWait : kRejoinFor)) {
            Say("seamless: %d of %d party member(s) back after the travel; ring the Beckoning Bell for the rest", back,
                g_lead.expected);
            g_lead = {};
        } else if (role == kRoleHost && back >= g_lead.expected && now - g_lead.at > g_lead.hold) {
            Say("seamless: the whole party is back after the travel");
            g_lead = {};
        } else if (!g_bells_off.load() && !g_lead.dream && (role == 0 || role == kRoleHost) && Settled(g_lead.settled, now) &&
                   now - g_lead.rang >= kRingEvery) {
            g_lead.rang = now;
            Say("seamless: arrived: ringing the Beckoning Bell for the party (%s)", RingBell(kHostBell) ? "rung" : "not rung");
        }
    }
}

void LampNotes() {
    static std::set<std::uint32_t> told;
    const std::uint32_t talk = g_lamp_talk.exchange(0, std::memory_order_relaxed);
    if (talk && told.insert(talk).second)
        Say("seamless: lamp t%u: its 'in multiplayer' check answered 'no', so the %s can use it", talk,
            Role() == kRoleClient ? "guest" : "host");
    if (g_solo_lamp.exchange(false, std::memory_order_relaxed))
        Say("seamless: a guest's lamp: the host is asked, or for the Dream this guest goes alone");
    static bool told_wall = false;
    if (g_wall_kept_down.exchange(false, std::memory_order_relaxed) && !told_wall) {
        told_wall = true;
        Say("seamless: a multiplayer fog wall stayed down (boss fogs are kept)");
    }
}

bool LampTalk(std::uint32_t talk) {
    const std::uint32_t map = talk / 1000;
    return map >= 210 && map <= 369 && map % 10 <= 2 && talk % 1000 <= 3;
}

bool HeadstoneTalk(std::uint32_t talk) {
    return talk >= 210000 && talk <= 210003;
}

std::int64_t __attribute__((sysv_abi)) OnTalkTest(std::uint64_t, const std::uint64_t* saved) {
    const std::uint64_t result = saved[5], env = saved[4], call = saved[3];
    const int role = g_role.load(std::memory_order_relaxed);
    if (!env || !call || !result || (role != kRoleHost && role != kRoleClient)) return 0;
    const std::uint32_t talk = *reinterpret_cast<const std::uint32_t*>(env + 0x10);
    if (!LampTalk(talk) || (role == kRoleClient && HeadstoneTalk(talk))) return 0;
    const std::uint64_t table = *reinterpret_cast<const std::uint64_t*>(call);
    const int test = static_cast<int>(Call(*reinterpret_cast<const std::uint64_t*>(table + 0x10), call));
    if (test != kTestMultiplayer && !(test == kTestIsClient && role == kRoleClient)) return 0;
    *reinterpret_cast<std::int32_t*>(result) = 0;
    *reinterpret_cast<std::int32_t*>(result + 8) = 2;
    g_lamp_talk.store(talk, std::memory_order_relaxed);
    if (role == kRoleHost) g_own_lamp_ms.store(SteadyNowMs(), std::memory_order_relaxed);
    const_cast<std::uint64_t*>(saved)[-1] = result;
    return 1;
}

struct CoopWall {
    std::int32_t event, wall_index, effect_index;
};
constexpr CoopWall kCoopWalls[] = {{7600, 6, 7}, {12907600, 6, 7}, {13400010, 7, 8}};

bool CoopWallStep(std::int32_t event_id, std::int32_t index, std::int32_t bank, std::int32_t id) {
    for (const CoopWall& w : kCoopWalls) {
        if (w.event != event_id) continue;
        return (bank == 2005 && id == 3 && index == w.wall_index) || (bank == 2006 && id == 2 && index == w.effect_index);
    }
    return false;
}

void NoteBossTrophy(std::uint64_t event, std::int32_t event_id) {
    if (!BossRewards::IsTrophyEvent(event_id) || g_role.load(std::memory_order_relaxed) != kRoleHost) return;
    const std::uint64_t instr = *reinterpret_cast<const std::uint64_t*>(event + 0xb0);
    if (!instr || *reinterpret_cast<const std::int32_t*>(instr) != 2003 || *reinterpret_cast<const std::int32_t*>(instr + 4) != 28) return;
    std::uint64_t args = *reinterpret_cast<const std::uint64_t*>(event + 0xb8);
    if (!args) {
        const std::uint64_t script = *reinterpret_cast<const std::uint64_t*>(*reinterpret_cast<const std::uint64_t*>(event + 0xa8) + 8);
        if (!script) return;
        args = script + *reinterpret_cast<const std::uint64_t*>(script + 0x78) + *reinterpret_cast<const std::uint64_t*>(instr + 0x10);
    }
    const std::int32_t trophy = *reinterpret_cast<const std::int32_t*>(args);
    std::lock_guard lock(g_boss_lots_mu);
    g_boss_trophies_out.push_back(trophy);
}

bool OwnSaveHasFlag(std::uint32_t id);

bool SkipSeenScene(std::uint64_t event, std::int32_t event_id) {
    const std::uint64_t instr = *reinterpret_cast<const std::uint64_t*>(event + 0xb0);
    if (!instr || *reinterpret_cast<const std::int32_t*>(instr) != kSceneBank || *reinterpret_cast<const std::int32_t*>(instr + 4) != kScenePlay ||
        g_role.load(std::memory_order_relaxed) != kRoleClient)
        return false;
    static const bool play = [] {
        const char* v = std::getenv("BB_GUEST_SCENES");
        return v && std::strcmp(v, "1") == 0;
    }();
    if (play || !OwnSaveHasFlag(KeptTravel::SceneSeenFlag(static_cast<std::uint32_t>(event_id)))) return false;
    static std::set<std::int32_t> told;
    if (told.insert(event_id).second) Say("seamless: event %d's scene: this hunter's own save has seen it, skipped for this hunter only", event_id);
    return true;
}

std::int64_t __attribute__((sysv_abi)) OnEmevdStep(std::uint64_t, const std::uint64_t* saved) {
    const std::uint64_t event = saved[4];
    if (!event) return 0;
    const std::int32_t event_id = *reinterpret_cast<const std::int32_t*>(event + 0x28);
    NoteBossTrophy(event, event_id);
    if (SkipSeenScene(event, event_id)) {
        const_cast<std::uint64_t*>(saved)[-1] = 1;
        return 1;
    }
    if (event_id != kLampEvent && event_id != kCoopWalls[0].event && event_id != kCoopWalls[1].event &&
        event_id != kCoopWalls[2].event)
        return 0;
    const std::uint64_t instr = *reinterpret_cast<const std::uint64_t*>(event + 0xb0);
    if (!instr) return 0;
    const std::int32_t index = *reinterpret_cast<const std::int32_t*>(event + 0xa0);
    const std::int32_t bank = *reinterpret_cast<const std::int32_t*>(instr), id = *reinterpret_cast<const std::int32_t*>(instr + 4);
    if (event_id == kLampEvent) {
        if (index != 0 || bank != 1003 || id != 6 || g_role.load(std::memory_order_relaxed) != kRoleClient) return 0;
        g_solo_lamp.store(true, std::memory_order_relaxed);
    } else if (CoopWallStep(event_id, index, bank, id)) {
        g_wall_kept_down.store(true, std::memory_order_relaxed);
    } else {
        return 0;
    }
    const_cast<std::uint64_t*>(saved)[-1] = 1;
    return 1;
}

bool LampTravel(std::uint8_t* image, std::size_t size) {
    if (!g_death_rules.load() || !Matches(image, size, kLampGate, kLampGateBytes, sizeof kLampGateBytes) ||
        !Matches(image, size, kSendHomeCall, kSendHomeCallBytes, sizeof kSendHomeCallBytes) ||
        !Matches(image, size, kLuaDispatch, kLuaDispatchEntry, sizeof kLuaDispatchEntry)) {
        Say("seamless: lamp travel with the party left off: the game's code there is not 1.09's");
        return false;
    }
    const char* keep_switch = std::getenv("BB_LAMP_KEEP_PARTY");
    const bool keep_parts = !(keep_switch && std::strcmp(keep_switch, "0") == 0) &&
                            Matches(image, size, kStageEndStopCall, kStageEndStopCallBytes, sizeof kStageEndStopCallBytes) &&
                            Matches(image, size, kStageEndTasksCall, kStageEndTasksCallBytes, sizeof kStageEndTasksCallBytes) &&
                            Matches(image, size, kFlagSnapshot, kFlagSnapshotEntry, sizeof kFlagSnapshotEntry) &&
                            Matches(image, size, kFlagApply, kFlagApplyEntry, sizeof kFlagApplyEntry) &&
                            Matches(image, size, kForgetHostChrs, kForgetHostChrsEntry, sizeof kForgetHostChrsEntry) &&
                            Matches(image, size, kForgetHostDecals, kForgetHostDecalsEntry, sizeof kForgetHostDecalsEntry) &&
                            Matches(image, size, kWarpParamFind, kWarpParamFindEntry, sizeof kWarpParamFindEntry);
    Hook hooks[6];
    if (!Prepare(hooks[0], image, size, kLampWarp, kLampWarpEntry, OnLampWarp) ||
        !Prepare(hooks[1], image, size, kTalkTest, kTalkTestEntry, OnTalkTest) ||
        !Prepare(hooks[2], image, size, kEmevdDispatch, kEmevdDispatchEntry, OnEmevdStep) ||
        (keep_parts && (!Prepare(hooks[3], image, size, kSessionStop, kSessionStopEntry, OnSessionStop) ||
                        !Prepare(hooks[4], image, size, kDestroyTasks, kDestroyTasksEntry, OnDestroyTasks) ||
                        !Prepare(hooks[5], image, size, kFlagMode, kFlagModeEntry, OnFlagMode)))) {
        for (Hook& hook : hooks) Discard(hook);
        Say("seamless: lamp travel with the party left off: no room for the lamp hooks");
        return false;
    }
    Commit(std::span(hooks, keep_parts ? 6 : 3));
    g_keep_parts = keep_parts;
    WriteCode(image + kLampGate, kLampGateOpen, sizeof kLampGateOpen);
    g_lamp_travel = true;
    Say("seamless: lamps work in a party: the host's lamp takes everyone along (%s), %s; no multiplayer fog walls outside boss fights",
        LampsKeepSession() ? "the session stays, the bells only if that fails" : "the bells ring by themselves",
        LampsKeepSession() ? "a guest's lamp asks the host" : "a guest's lamp takes only that guest");
    return true;
}

constexpr std::size_t kItemCheck = 0x157f200;
constexpr std::uint8_t kItemCheckEntry[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55,
                                            0x41, 0x54, 0x53, 0x48, 0x81, 0xec, 0xe8, 0x01, 0x00, 0x00};
constexpr std::size_t kGoodsRow = 0x1f1e3d0, kGoodsChrState = 0x1f1e910, kAreaCheck = 0x191a750, kGroundCheck = 0x18c96d0, kMenuLocks = 0x176b7e0;
constexpr std::size_t kItemTimers = 0x5540508, kRegion = 0x278, kOnlineOk = 0x1590;
constexpr std::int32_t kBellGoods[] = {120, 200, 201, 205, 206, 225, 226};
constexpr int kBellEffects[] = {9000, 9003, 9004, 9005, 9008, 9009};
thread_local bool g_in_item_check = false;
std::atomic<bool> g_bell_trace{false};
std::atomic<bool> g_bell_trace_ring{false};

std::string BellRefusals(std::int32_t item, std::uint64_t chr, std::uint64_t effects, int type) {
    std::uint64_t row[2] = {};
    Call(Fn(kGoodsRow), row, item);
    const std::uint64_t goods = row[1];
    if (!goods) return "no goods row";
    std::string out;
    const auto add = [&](const char* why) {
        out += out.empty() ? "" : ",";
        out += why;
    };
    const std::uint32_t bits = Field<std::uint32_t>(goods, 0x42);
    const std::uint8_t type_bit = type == 8   ? std::uint8_t(bits >> 0x11)
                                  : type == 2 ? std::uint8_t(bits >> 0x13)
                                  : type == 1 ? std::uint8_t(bits >> 0x12)
                                              : Field<std::uint8_t>(goods, 0x44);
    const std::uint64_t transition = Global(kTransition), timers = Global(kItemTimers);
    const int members = Field<std::int32_t>(Field<std::uint64_t>(transition, 0x16f8), 0x18);
    if (Field<std::uint32_t>(chr, 0x1e1) & 0x400) add("chr flag 0x400");
    if (!(type_bit & 1)) add("player type bit");
    if (!(bits & 0x100000) && members > 1) add("members>1");
    if ((bits & 0x200000) && !Field<std::uint8_t>(transition, kOnlineOk)) add("+0x1590 off");
    if (effects && !(Call(Field<std::uint64_t>(Field<std::uint64_t>(effects, 0), 0x10), effects, item) & 0xff)) add("effect list");
    if (Field<std::uint8_t>(goods, 0x3a) < 6 && (0x26u >> Field<std::uint8_t>(goods, 0x3a) & 1)) add("goods kind");
    if (Field<std::uint8_t>(goods, 0x3f) ? Field<std::uint8_t>(timers, 0x3ff8) != 0 : Field<float>(timers, 0x400c) > 0.0f) add("item timer");
    const std::int64_t state = Call(Field<std::uint64_t>(Field<std::uint64_t>(chr, 0), 0x598), chr);
    if ((Call(Fn(kGoodsChrState), row, state & 0xff) & 0xff) != 1) add("chr state");
    if (!(Call(Fn(kAreaCheck), Global(kWorldChrMan)) & 0xff)) add("area check");
    if (!(Call(Fn(kGroundCheck), chr) & 0xff)) add("ground check");
    return out.empty() ? "nothing seen" : out;
}

std::int64_t __attribute__((sysv_abi)) OnItemCheck(std::uint64_t, const std::uint64_t* saved) {
    if (g_in_item_check) return 0;
    g_in_item_check = true;
    const auto item = static_cast<std::int32_t>(saved[5]);
    const std::int64_t result = Call(Fn(kItemCheck), saved[5], saved[4], saved[3], saved[2], saved[1], saved[0], saved[7]);
    if (!(result & 0xff) && saved[4] == LocalPlayer() && std::find(std::begin(kBellGoods), std::end(kBellGoods), item) != std::end(kBellGoods)) {
        static std::mutex mu;
        static std::set<std::uint64_t> told;
        const std::uint64_t key = std::uint64_t(Field<std::uint32_t>(saved[4], kRegion)) << 32 | std::uint32_t(item);
        std::lock_guard lock(mu);
        if (told.insert(key).second)
            Say("bell trace: goods %d refused at region %u, map %08x, role %d: %s", item, Field<std::uint32_t>(saved[4], kRegion),
                Field<std::uint32_t>(saved[4], kPlayerMap), Role(),
                BellRefusals(item, saved[4], saved[3], static_cast<std::int32_t>(saved[2])).c_str());
    }
    g_in_item_check = false;
    const_cast<std::uint64_t*>(saved)[-1] = static_cast<std::uint64_t>(result);
    return 1;
}

void BellTraceTick() {
    if (!g_bell_trace.load(std::memory_order_relaxed)) return;
    const std::uint64_t local = LocalPlayer();
    if (!local || Loading() || Hp(local) <= 0) return;
    static std::string last;
    std::string now = "map " + std::to_string(Field<std::uint32_t>(local, kPlayerMap)) + " region " + std::to_string(Field<std::uint32_t>(local, kRegion)) +
                      " role " + std::to_string(Role());
    now += " area " + std::to_string(Call(Fn(kAreaCheck), Global(kWorldChrMan)) & 0xff);
    now += " ground " + std::to_string(Call(Fn(kGroundCheck), local) & 0xff);
    now += " menu locks " + std::to_string(Call(Fn(kMenuLocks)) & 0x7ffff77f);
    now += " effects";
    for (const int effect : kBellEffects)
        if (EffectNode(local, effect)) now += " " + std::to_string(effect);
    const std::uint64_t effects = Field<std::uint64_t>(local, 0x1c8);
    const int type = Field<std::int32_t>(local, 0x78);
    now += " usable";
    for (const std::int32_t item : kBellGoods) {
        g_in_item_check = true;
        const bool ok = Call(Fn(kItemCheck), item, local, effects, type, -1, -1, 0) & 0xff;
        g_in_item_check = false;
        now += " " + std::to_string(item) + "=" + (ok ? "yes" : "NO(" + BellRefusals(item, local, effects, type) + ")");
    }
    if (now != last) Say("bell trace: %s", now.c_str());
    last = std::move(now);
    static std::uint32_t rung_at = 0;
    static std::chrono::steady_clock::time_point settled;
    const std::uint32_t map = Field<std::uint32_t>(local, kPlayerMap);
    if (g_bell_trace_ring.load(std::memory_order_relaxed) && rung_at != map && Settled(settled, std::chrono::steady_clock::now())) {
        rung_at = map;
        Say("bell trace: Small Resonant Bell at map %08x, region %d: %s", map, Field<std::int32_t>(local, kRegion), RingBell(kGuestBell) ? "rung" : "not rung");
    }
}

void BellTrace(std::uint8_t* image, std::size_t size) {
    const char* on = std::getenv("BB_BELL_TRACE");
    if (!on || (std::strcmp(on, "1") != 0 && std::strcmp(on, "ring") != 0)) return;
    g_bell_trace_ring = std::strcmp(on, "ring") == 0;
    Hook hook;
    if (!Prepare(hook, image, size, kItemCheck, kItemCheckEntry, OnItemCheck)) {
        Say("bell trace off: the item check is not 1.09's");
        return;
    }
    Commit(std::span(&hook, 1));
    g_bell_trace = true;
    Say("bell trace on");
}

bool DeathRules(std::uint8_t* image, std::size_t size) {
    const bool sites = std::all_of(std::begin(kDeathSites), std::end(kDeathSites), [&](const Site& site) {
        return Matches(image, size, site.at, site.original.begin(), site.original.size());
    });
    if (!sites || !Matches(image, size, kOwnDeathCall, kOwnDeathCallBytes, sizeof kOwnDeathCallBytes) ||
        !Matches(image, size, kPhantomDeathCall, kPhantomDeathCallBytes, sizeof kPhantomDeathCallBytes) ||
        !Matches(image, size, kRevive, kReviveEntry, sizeof kReviveEntry) ||
        !Matches(image, size, kWarp, kWarpEntry, sizeof kWarpEntry) ||
        !Matches(image, size, kBodyRevive, kBodyReviveEntry, sizeof kBodyReviveEntry) ||
        !Matches(image, size, kTeamResolve, kTeamResolveEntry, sizeof kTeamResolveEntry) ||
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
    if (!PartyDeathRules()) {
        Say("seamless: deaths keep the party: a fallen hunter gets up on its last safe ground, nobody is sent home");
        return true;
    }
    Say("seamless: deaths keep the party: outside a boss only the fallen hunter goes back to its lamp; in a boss fight the dead spectate until the boss dies or the whole party falls");
    Hook camera;
    if (Prepare(camera, image, size, kCameraChrSite, kCameraChrBytes, OnCameraChr)) {
        Commit(std::span(&camera, 1));
        g_camera_hook = true;
    } else {
        Say("seamless: the spectator camera is off: the game's follow camera call is not 1.09's");
    }
    return true;
}

bool SharedProgress(std::uint8_t* image, std::size_t size) {
    if (!Matches(image, size, kGroupKey, kGroupKeyEntry, sizeof kGroupKeyEntry)) {
        Say("seamless: shared progress left off: the game's flag code is not 1.09's");
        return false;
    }
    g_progress = true;
    Say("seamless: shared progress: bosses, lamps, doors, shortcuts, story and NPC quest steps of the host's world go into every guest's own save");
    return true;
}

std::int64_t __attribute__((sysv_abi)) OnFlagValue(std::uint64_t, const std::uint64_t* saved) {
    const std::uint64_t from = saved[6];
    if (from != Fn(kMapLoadSlotCall + 5) && from != Fn(kNetChannelSlotCall + 5) && from != Fn(kNetChannelObjCall + 5)) return 0;
    if (!g_join_dungeon.load(std::memory_order_relaxed) || saved[4] != kSlotFlag) return 0;
    const auto map = static_cast<std::uint32_t>(Call(Fn(kGetFlagValue), saved[5], kRecordBase + 96, 32));
    if ((map & 0xff000000u) != kDungeonMaps) return 0;
    g_joined_read.store(true, std::memory_order_relaxed);
    const_cast<std::uint64_t*>(saved)[-1] = 1u << (kSlots - 1);
    return 1;
}

bool ChaliceParty(std::uint8_t* image, std::size_t size) {
    const bool code = Matches(image, size, kSetFlagValue, kSetFlagValueEntry, sizeof kSetFlagValueEntry) &&
                      Matches(image, size, kSetEventFlag, kSetEventFlagEntry, sizeof kSetEventFlagEntry) &&
                      Matches(image, size, kInventoryFind, kInventoryFindEntry, sizeof kInventoryFindEntry) &&
                      Matches(image, size, kGiveItem, kGiveItemEntry, sizeof kGiveItemEntry) &&
                      Matches(image, size, kMapLoadSlotCall, kMapLoadSlotCallBytes, sizeof kMapLoadSlotCallBytes) &&
                      Matches(image, size, kNetChannelSlotCall, kNetChannelSlotCallBytes, sizeof kNetChannelSlotCallBytes) &&
                      Matches(image, size, kNetChannelObjCall, kNetChannelObjCallBytes, sizeof kNetChannelObjCallBytes);
    Hook hook;
    if (!g_progress.load() || !g_lamp_travel.load() || !code || !Prepare(hook, image, size, kGetFlagValue, kGetFlagValueEntry, OnFlagValue)) {
        Say("seamless: chalice dungeons with the party left off: the game's code there is not 1.09's");
        return false;
    }
    Commit(std::span(&hook, 1));
    g_chalice = true;
    Say("seamless: chalice dungeons with the party: guests are summoned into the host's dungeon, its progress and the "
        "chalices go to everyone");
    return true;
}

std::int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void NoteBossFight(int role) {
    if (role != kRoleClient || !BossUp()) return;
    const std::int64_t now = NowMs();
    if (now - g_fight_seen_ms.load(std::memory_order_relaxed) > kRewardWindowMs) g_fight_start_ms.store(now, std::memory_order_relaxed);
    g_fight_seen_ms.store(now, std::memory_order_relaxed);
}

std::int64_t __attribute__((sysv_abi)) OnAwardLot(std::uint64_t, const std::uint64_t* saved) {
    const auto lot = static_cast<std::int32_t>(saved[4]);
    const bool host_only = (saved[3] & 0xff) == 1;
    const int role = g_role.load(std::memory_order_relaxed);
    if (saved[6] != Fn(kEventAwardCall + sizeof kEventAwardCallBytes) || lot <= 0) return 0;
    if (host_only) {
        if (!BossRewards::IsBossLot(lot)) return 0;
        if (role == kRoleHost) {
            std::lock_guard lock(g_boss_lots_mu);
            g_boss_lots_out.push_back(lot);
        }
        return 0;
    }
    if (role != kRoleClient) return 0;
    static std::mutex mu;
    static std::set<std::int32_t> given;
    static std::int64_t window = 0;
    const std::int64_t seen = g_fight_seen_ms.load(std::memory_order_relaxed);
    const std::int64_t start = g_fight_start_ms.load(std::memory_order_relaxed);
    const bool fought = seen && NowMs() - seen <= kRewardWindowMs;
    bool first = false;
    {
        std::lock_guard lock(mu);
        if (window != start) {
            window = start;
            given.clear();
        }
        first = fought && given.insert(lot).second;
    }
    if (first) {
        Say("seamless: item lot %d given to this guest: its reward from the boss it fought here", lot);
        return 0;
    }
    Say("seamless: item lot %d held back: %s", lot,
        fought ? "already given for this boss" : "no boss fought in the host's world since this hunter arrived (an earlier kill)");
    const_cast<std::uint64_t*>(saved)[-1] = 0;
    return 1;
}

std::int64_t __attribute__((sysv_abi)) OnMpMessage(std::uint64_t, const std::uint64_t*) {
    std::uint8_t* pending = g_image + kMpMessagePending;
    if (*pending == 1) {
        *pending = 0;
        if (g_quieted.fetch_add(1) < 3) Say("seamless: a multiplayer message of the game left out (the co-op banner shows the summons)");
    }
    return 0;
}

bool QuietMessages(std::uint8_t* image, std::size_t size) {
    const char* keep = std::getenv("BB_GAME_MP_MESSAGES");
    if (keep && std::strcmp(keep, "1") == 0) return false;
    Hook hook;
    if (!Prepare(hook, image, size, kMpMessage, kMpMessageEntry, OnMpMessage)) {
        Say("seamless: the game's multiplayer messages kept: their code is not 1.09's");
        return false;
    }
    Commit(std::span(&hook, 1));
    Say("seamless: the game's multiplayer messages (bells, summons, joins) left out; the co-op banner shows them");
    return true;
}

bool GuestRewards(std::uint8_t* image, std::size_t size) {
    const char* all = std::getenv("BB_GUEST_REWARDS_ALL");
    if (all && std::strcmp(all, "1") == 0) {
        Say("seamless: guests get every boss reward the host's world plays out (BB_GUEST_REWARDS_ALL=1)");
        return false;
    }
    Hook hook;
    if (!Matches(image, size, kEventAwardCall, kEventAwardCallBytes, sizeof kEventAwardCallBytes) ||
        !Prepare(hook, image, size, kAwardLot, kAwardLotEntry, OnAwardLot)) {
        Say("seamless: boss rewards for guests left as the game has them: its item lot code is not 1.09's");
        return false;
    }
    Commit(std::span(&hook, 1));
    g_guest_rewards = true;
    Say("seamless: boss rewards: every hunter in the host's world gets a boss's drop once");
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
        BellTrace(image, size);
        any = FourPlayers(image, size) || any;
        any = DeathRules(image, size) || any;
        any = LampTravel(image, size) || any;
        any = SharedProgress(image, size) || any;
        any = ChaliceParty(image, size) || any;
        any = GuestRewards(image, size) || any;
        any = QuietMessages(image, size) || any;
        BbVoice::Start();
    }
    const char* timeout = std::getenv("BB_SIGN_TIMEOUT");
    const long seconds = timeout ? std::strtol(timeout, nullptr, 10) : 0;
    if (seconds > 0 && seconds != 180) any = SetSignTimeout(image, size, seconds) || any;
    return any;
}

std::uint64_t FlagBlock(std::uint32_t block) {
    const std::uint64_t man = Global(kFlagMan);
    const std::uint64_t pool = Field<std::uint64_t>(man, 0x28);
    if (!pool || Field<std::uint32_t>(man, 0x20) != kFlagStride) return 0;
    const std::uint32_t key = static_cast<std::uint32_t>(
        Call(Fn(kGroupKey), block / 10000, block / 100 % 100, block / 10 % 10, block % 10));
    if (key >= Field<std::uint32_t>(man, 0x24)) return 0;
    return pool + std::uint64_t(key) * kFlagStride;
}

class PoolReader {
public:
    bool On(std::uint32_t id, bool& known) {
        const std::uint32_t block = id / 1000;
        auto it = blocks_.find(block);
        if (it == blocks_.end()) {
            Entry e;
            const std::uint64_t at = FlagBlock(block);
            e.ok = at && Peek(at, &e.bytes);
            it = blocks_.emplace(block, e).first;
        }
        known = it->second.ok;
        const std::uint32_t bit = id % 1000;
        return known && (it->second.bytes[bit >> 3] & (0x80u >> (bit & 7)));
    }

private:
    struct Entry {
        bool ok = false;
        std::array<std::uint8_t, kFlagStride> bytes{};
    };
    std::map<std::uint32_t, Entry> blocks_;
};

bool WriteFlag(std::uint32_t id, bool on) {
    const std::uint64_t at = FlagBlock(id / 1000);
    if (!at) return false;
    const std::uint32_t bit = id % 1000;
    auto* byte = reinterpret_cast<std::uint8_t*>(at + (bit >> 3));
    const std::uint8_t mask = std::uint8_t(0x80u >> (bit & 7));
    const std::uint8_t was = *byte;
    *byte = on ? std::uint8_t(was | mask) : std::uint8_t(was & ~mask);
    return *byte != was;
}

std::uint32_t MapOf(std::uint32_t id) {
    return (id / 100000 % 100) << 24 | (id / 10000 % 10) << 16;
}

void RequestSave();

bool WholeProgress() {
    static const bool whole = [] {
        const char* v = std::getenv("BB_PROGRESS_WHOLE");
        return v && std::strcmp(v, "1") == 0;
    }();
    return whole;
}

std::set<std::string> PresentGuests(int role) {
    const auto now = std::chrono::steady_clock::now();
    if (role != kRoleHost) {
        g_present.Clear();
        return {};
    }
    if (!Loading() && LocalPlayer())
        for (const BbRoom::Vitals& v : InWorld()) g_present.Seen(v.name, now);
    return g_present.Here(now);
}

void PromptLockTick(int role) {
    constexpr std::size_t kPromptsOff = 0x8d, kHostDeadMark = 0x70, kGameMan = 0x5556678;
    constexpr std::uint32_t kClearFlags[] = {4047, 4000, 4092};
    constexpr auto kHold = std::chrono::seconds(10);
    static std::chrono::steady_clock::time_point since{};
    const std::uint64_t holder = Global(kLuaEventMan);
    const std::uint64_t lua = holder ? Field<std::uint64_t>(holder, 8) : 0;
    const std::uint64_t player = LocalPlayer();
    if (!lua || !Field<std::uint8_t>(lua, kPromptsOff) || (role != kRoleClient && role != kRoleHost) || Loading() || !player || Hp(player) <= 0) {
        since = {};
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (since == std::chrono::steady_clock::time_point{}) since = now;
    if (now - since < kHold) return;
    since = {};
    *reinterpret_cast<std::uint8_t*>(lua + kPromptsOff) = 0;
    if (role == kRoleClient) {
        for (const std::uint32_t flag : kClearFlags) Call(Fn(kSetFlag), 0, flag, 0);
        if (const std::uint64_t data = Global(kGameDataMan)) *reinterpret_cast<std::uint8_t*>(data + kHostDeadMark) = 0;
        if (const std::uint64_t game = Global(kGameMan)) *reinterpret_cast<std::uint8_t*>(game + kRespawnMark) = 0;
    }
    Say("seamless: ladders, levers, pickups and talk prompts back on: the game had turned them off for the load this hunter does not take");
}

void BossDropsTick(int role) {
    std::vector<std::int32_t> out, trophies;
    {
        std::lock_guard lock(g_boss_lots_mu);
        out.swap(g_boss_lots_out);
        trophies.swap(g_boss_trophies_out);
    }
    if (!out.empty() || !trophies.empty()) {
        const std::set<std::string> present = PresentGuests(role);
        for (const BbRoom::Vitals& v : BbRoom::Party(kVitalsFresh)) {
            if (!present.count(v.name)) continue;
            for (const std::int32_t lot : out) {
                BbRoom::SendBossLot(lot, v.slot);
                Say("seamless: boss drop %d sent to %s", lot, v.name.c_str());
            }
            for (const std::int32_t trophy : trophies) {
                BbRoom::SendBossTrophy(trophy, v.slot);
                Say("seamless: boss trophy %d sent to %s", trophy, v.name.c_str());
            }
        }
    }
    static std::vector<std::int32_t> waiting, waiting_trophies;
    const std::vector<std::int32_t> got = BbRoom::TakeBossLots();
    waiting.insert(waiting.end(), got.begin(), got.end());
    const std::vector<std::int32_t> got_trophies = BbRoom::TakeBossTrophies();
    waiting_trophies.insert(waiting_trophies.end(), got_trophies.begin(), got_trophies.end());
    if (role != kRoleClient || Loading() || !LocalPlayer()) return;
    for (const std::int32_t trophy : waiting_trophies) {
        const std::uint64_t holder = Global(kTrophyMan);
        const std::uint64_t man = holder ? Field<std::uint64_t>(holder, 8) : 0;
        if (!man) continue;
        std::int32_t id = trophy;
        Call(Field<std::uint64_t>(Field<std::uint64_t>(man, 0), 0x30), man, &id);
        Say("seamless: boss trophy %d unlocked on this guest", trophy);
    }
    waiting_trophies.clear();
    for (const std::int32_t lot : waiting) {
        if (!BossRewards::IsBossLot(lot)) continue;
        bool first = false;
        {
            std::lock_guard lock(g_boss_lots_mu);
            first = g_boss_lots_given.First(lot, std::chrono::steady_clock::now());
        }
        if (!first) continue;
        Say("seamless: boss drop %d given to this guest: the host's world beat the boss with it there", lot);
        Call(Fn(kAwardLot), Global(kItemMan), lot, 0);
    }
    waiting.clear();
}

void SendLedger(const SharedProgress::Ledger& ledger) {
    static std::map<std::string, std::size_t> told;
    for (const BbRoom::Vitals& v : BbRoom::Party(kVitalsFresh)) {
        const std::vector<std::uint32_t> mine = ledger.For(v.name);
        BbRoom::SendProgress(mine, v.slot);
        const auto was = told.find(v.name);
        if (was != told.end() && was->second == mine.size()) continue;
        told[v.name] = mine.size();
        Say("seamless: progress for %s: %zu flag(s) set while %s was in this world; the world's earlier progress stays out",
            v.name.c_str(), mine.size(), v.name.c_str());
    }
}

void HostProgressTick(int role) {
    static std::set<std::uint32_t> kept;
    static std::map<std::uint32_t, std::uint32_t> npc;
    static bool scanned = false;
    static std::chrono::steady_clock::time_point last, caught_up;
    const auto now = std::chrono::steady_clock::now();
    const std::set<std::string> present = PresentGuests(role);
    if (Loading() || !LocalPlayer() || now - last < kProgressScan) return;
    last = now;
    PoolReader pool;
    std::vector<std::uint32_t> fresh;
    int flags = 0, steps = 0;
    for (const FlagRange& r : kProgressFlags) {
        if (r.kind == kDungeonFlag) continue;
        if (r.kind == kKeepSet) {
            for (std::uint32_t id = r.first; id <= r.last; ++id) {
                bool known = false;
                if (pool.On(id, known) && kept.insert(id).second && scanned) {
                    fresh.push_back(id);
                    ++flags;
                }
            }
            continue;
        }
        std::uint32_t value = 0;
        bool known = true;
        for (std::uint32_t id = r.first; id <= r.last && known; ++id)
            if (pool.On(id, known)) value |= 1u << (id - r.first);
        if (!known) continue;
        const auto was = npc.find(r.first);
        if (was != npc.end() && was->second != value) {
            for (std::uint32_t id = r.first; id <= r.last; ++id)
                fresh.push_back(value & (1u << (id - r.first)) ? id : id | kFlagClear);
            ++steps;
        }
        npc[r.first] = value;
    }
    scanned = true;
    if (role != kRoleHost) return;
    if (WholeProgress()) {
        if (!fresh.empty()) {
            BbRoom::SendProgress(fresh);
            Say("seamless: progress shared with the party: %d new flag(s) (bosses, lamps, doors, story), %d NPC quest step(s)",
                flags, steps);
        }
        if (now - caught_up >= kCatchUpEvery && !kept.empty()) {
            caught_up = now;
            BbRoom::SendProgress(std::vector<std::uint32_t>(kept.begin(), kept.end()));
        }
        return;
    }
    static SharedProgress::Ledger ledger;
    if (!fresh.empty()) {
        for (const std::uint32_t flag : fresh) ledger.Record(flag, present);
        for (const BbRoom::Vitals& v : BbRoom::Party(kVitalsFresh))
            if (present.count(v.name)) BbRoom::SendProgress(fresh, v.slot);
        Say("seamless: progress shared with the %zu hunter(s) in this world: %d new flag(s) (bosses, lamps, doors, story), "
            "%d NPC quest step(s)",
            present.size(), flags, steps);
    }
    if (now - caught_up >= kCatchUpEvery && ledger.Size()) {
        caught_up = now;
        SendLedger(ledger);
    }
}

void GuestProgressTick(int role) {
    struct Pending {
        std::uint32_t flag;
        std::chrono::steady_clock::time_point at;
    };
    static std::vector<Pending> pending;
    static std::chrono::steady_clock::time_point in_party;
    const auto now = std::chrono::steady_clock::now();
    if (role == kRoleClient) in_party = now;
    const std::vector<std::uint32_t> got = BbRoom::TakeProgress();
    if (!got.empty() && in_party != std::chrono::steady_clock::time_point{} && now - in_party < kPartyRecent)
        for (const std::uint32_t flag : got) pending.push_back({flag, now});
    if (Loading() || !LocalPlayer()) return;
    if (!pending.empty()) {
        PoolReader own;
        for (const FlagRange& r : kProgressFlags) {
            if (r.kind == kKeepSet || r.kind == kDungeonFlag) continue;
            std::uint32_t host = 0, guest = 0;
            bool any = false, known = true;
            for (const Pending& p : pending) {
                const std::uint32_t id = p.flag & ~kFlagClear;
                if (id < r.first || id > r.last) continue;
                any = true;
                if (p.flag & kFlagClear) host &= ~(1u << (id - r.first));
                else host |= 1u << (id - r.first);
            }
            if (!any) continue;
            for (std::uint32_t id = r.first; id <= r.last && known; ++id)
                if (own.On(id, known)) guest |= 1u << (id - r.first);
            if (!known || SharedProgress::HostStepIsNewer(host, guest)) continue;
            std::erase_if(pending, [&](const Pending& p) {
                const std::uint32_t id = p.flag & ~kFlagClear;
                return id >= r.first && id <= r.last;
            });
        }
        const int mode = Field<std::int32_t>(Global(kFlagMan), 0x80, -1);
        const std::uint32_t here = Field<std::uint32_t>(LocalPlayer(), 0x3f8) & 0xffff0000u;
        int changed = 0, held = 0;
        for (auto it = pending.begin(); it != pending.end();) {
            const std::uint32_t id = it->flag & ~kFlagClear;
            if (mode != 1 && MapOf(id) == here) {
                ++held;
                it = now - it->at > kProgressKeep ? pending.erase(it) : std::next(it);
                continue;
            }
            if (WriteFlag(id, !(it->flag & kFlagClear))) ++changed;
            it = pending.erase(it);
        }
        if (changed) {
            Say("seamless: %d of the host's progress flag(s) written to this hunter's own save%s", changed,
                held ? " (this map's wait for the next load)" : "");
            RequestSave();
        }
    }
}

namespace {
bool OwnSaveHasFlag(std::uint32_t id) {
    PoolReader own;
    bool known = false;
    return own.On(id, known) && known;
}
}

void RequestSave() {
    g_save_due = std::chrono::steady_clock::now() + kSaveAfter;
}

void SaveTick() {
    static std::chrono::steady_clock::time_point saved_at;
    static bool can_save = true;
    const auto now = std::chrono::steady_clock::now();
    if (!can_save || g_save_due == std::chrono::steady_clock::time_point{} || now < g_save_due || now - saved_at < kSaveGap ||
        Loading() || !LocalPlayer() || !Field<std::uint64_t>(Global(kLuaEventMan), 0) || Hp(LocalPlayer()) <= 0)
        return;
    if (std::memcmp(g_image + kSaveRequest, kSaveRequestEntry, sizeof kSaveRequestEntry) != 0) {
        can_save = false;
        Say("seamless: the game's save request is not 1.09's: the shared progress is kept at the game's next save");
        return;
    }
    Call(Fn(kSaveRequest));
    saved_at = now;
    g_save_due = {};
    Say("seamless: the game saved the shared progress");
}

std::uint32_t FlagValue(std::uint32_t first, std::uint32_t bits) {
    const std::uint64_t man = Global(kFlagMan);
    return man ? static_cast<std::uint32_t>(Call(Fn(kGetFlagValue), man, first, bits)) : 0;
}

int CurrentSlot() {
    const std::uint32_t bits = FlagValue(kSlotFlag, kSlots);
    if (!bits || bits >= (1u << kSlots)) return -1;
    return kSlots - (32 - __builtin_clz(bits));
}

std::uint32_t RecordBase(int slot) {
    return kRecordBase + std::uint32_t(slot) * kRecordStride;
}

std::uint64_t SlotChannel(int slot) {
    return std::uint64_t(FlagValue(RecordBase(slot) + 64, 32)) << 32 | FlagValue(RecordBase(slot) + 32, 32);
}

bool SlotHoldsDungeon(int slot) {
    return (FlagValue(RecordBase(slot) + 96, 32) & 0xff000000u) == kDungeonMaps;
}

bool InDungeonMap(std::uint32_t map) {
    return (map & 0xff000000u) == kDungeonMaps;
}

std::vector<std::uint8_t> DungeonForm(int slot) {
    std::array<std::uint32_t, kFormWords> w{};
    w[0] = 4;
    w[1] = kFormWords * 4;
    const std::uint64_t grail = Global(kHolygrail);
    const std::uint64_t obj = Field<std::uint64_t>(Field<std::uint64_t>(grail, 8), 8 * std::uint64_t(slot));
    const std::uint64_t helper = obj > 0x10000 ? obj + 0x108 : 0;
    if (helper && Field<std::uint32_t>(helper, 0xe8)) {
        w[2] = Field<std::uint32_t>(helper, 0xe0);
        w[3] = Field<std::uint32_t>(helper, 0xe8);
        for (int i = 0; i < 18; ++i) w[4 + i] = Field<std::uint32_t>(helper, 0xf8 + 4 * i);
        for (int i = 0; i < 4; ++i) w[22 + i] = Field<std::uint32_t>(helper, 0x140 + 4 * i);
    } else {
        const std::uint32_t base = RecordBase(slot);
        w[2] = FlagValue(base + 96, 32);
        w[3] = FlagValue(base + 128, 32);
        for (int i = 0; i < 18; ++i) w[4 + i] = FlagValue(base + 160 + 32 * i, 32);
        for (int i = 0; i < 4; ++i) w[22 + i] = FlagValue(base + 736 + 32 * i, 32);
    }
    std::vector<std::uint8_t> out(kFormWords * 4);
    std::memcpy(out.data(), w.data(), out.size());
    return out;
}

bool HasItem(std::uint32_t type_bits, std::uint32_t id) {
    const std::uint64_t list = Field<std::uint64_t>(Global(kGameDataMan), 8);
    if (!list || !Field<std::uint64_t>(list + kInventory, 0x68) || !Field<std::uint64_t>(list + kInventory, 0x78)) return false;
    return static_cast<std::int32_t>(Call(Fn(kInventoryFind), list + kInventory, type_bits, id)) >= 0;
}

void ApplyEffect(std::uint64_t chr, int id) {
    using AddFn = void(__attribute__((sysv_abi))*)(std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
                                                     std::uint64_t, std::uint64_t, float, float, float, float, float);
    const std::uint64_t fn = Field<std::uint64_t>(Field<std::uint64_t>(chr, 0), 0x3f0);
    if (fn) reinterpret_cast<AddFn>(fn)(chr, std::uint64_t(id), chr, 0, 1, 0, 0, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f);
}

void ChaliceHostTick(int role, std::chrono::steady_clock::time_point now) {
    static std::chrono::steady_clock::time_point last, sent, entered, rang, settled;
    static std::uint64_t told = 0;
    static std::vector<std::uint32_t> last_flags;
    const std::uint32_t map = OwnMap();
    if (!InDungeonMap(map) || Loading()) {
        entered = {};
        g_auto_bell = false;
        return;
    }
    if (entered == std::chrono::steady_clock::time_point{}) {
        entered = now;
        g_bells_off = false;
    }
    if (now - last < kDungeonEvery) return;
    last = now;
    const int slot = CurrentSlot();
    if (slot < 0) return;
    const std::uint64_t channel = SlotChannel(slot);
    if (!channel || channel == kNoDungeon) return;
    BbRoom::PutDungeonForm(channel, DungeonForm(slot));
    if (told != channel) {
        told = channel;
        Say("seamless: in chalice dungeon %llu (slot %d): its layout is on the room's board for the party",
            static_cast<unsigned long long>(channel), slot);
    }
    if (role != kRoleHost && role != 0) return;
    const auto party = BbRoom::Party(kVitalsFresh);
    const bool calling = !g_bells_off.load() && !party.empty() && InWorld().size() < party.size() && now - entered < kDungeonRing;
    g_auto_bell = calling;
    if (calling && now - rang >= kRingEvery && Settled(settled, now)) {
        rang = now;
        Say("seamless: in the dungeon: ringing the Beckoning Bell for the party (%s)", RingBell(kHostBell) ? "rung" : "not rung");
    }
    if (role != kRoleHost) return;
    BbRoom::Dungeon d;
    d.channel = channel;
    for (int i = 0; i < kRecordWords; ++i) d.record[i] = FlagValue(RecordBase(slot) + 32 * std::uint32_t(i), 32);
    for (const FlagRange& r : kProgressFlags) {
        if (r.kind != kDungeonFlag) continue;
        for (std::uint32_t id = r.first; id <= r.last; ++id)
            if (FlagValue(id, 1)) d.flags.push_back(id);
    }
    if (WholeProgress()) {
        if (d.flags != last_flags || now - sent >= kCatchUpEvery) {
            sent = now;
            last_flags = d.flags;
            BbRoom::SendDungeon(d);
        }
        return;
    }
    static SharedProgress::Ledger ledger;
    static std::set<std::uint32_t> known;
    static std::uint64_t known_channel = 0;
    const std::set<std::string> present = PresentGuests(role);
    if (known_channel != channel) {
        known_channel = channel;
        known = std::set<std::uint32_t>(d.flags.begin(), d.flags.end());
        ledger.Clear();
        Say("seamless: dungeon %llu: %zu flag(s) of the host's earlier runs stay out of the guests' saves",
            static_cast<unsigned long long>(channel), known.size());
    }
    bool changed = false;
    for (const std::uint32_t id : d.flags)
        if (known.insert(id).second) {
            ledger.Record(id, present);
            changed = true;
        }
    if (!changed && now - sent < kCatchUpEvery) return;
    sent = now;
    for (const BbRoom::Vitals& v : BbRoom::Party(kVitalsFresh)) {
        BbRoom::Dungeon mine = d;
        mine.flags = ledger.For(v.name);
        BbRoom::SendDungeon(mine, v.slot);
    }
}

void ChaliceGuestTick(int role, std::chrono::steady_clock::time_point now) {
    static std::chrono::steady_clock::time_point rang, waiting_until, settled;
    static bool told_wait = false;
    static std::optional<BbRoom::Dungeon> carry;
    const std::uint32_t host_map = HostMap();
    const bool host_in = InDungeonMap(host_map);
    const std::uint64_t local = LocalPlayer();
    const bool off = g_bells_off.load();
    static bool was_off = false;
    if (off && !was_off && local) RemoveEffect(local, kChaliceWait);
    was_off = off;
    if (off) waiting_until = {};
    const bool joining = !off && host_in && role != kRoleClient && local && !InDungeonMap(OwnMap());
    g_auto_bell = joining;
    if (joining && Settled(settled, now) && now - rang >= kRingEvery) {
        rang = now;
        waiting_until = now + kDungeonRing;
        ApplyEffect(local, kChaliceWait);
        const bool ok = RingBell(kGuestBell);
        if (!told_wait) Say("seamless: the host is in a chalice dungeon: ringing to be summoned into it (%s)", ok ? "rung" : "not rung");
        told_wait = true;
    }
    if (!host_in) told_wait = false;
    g_join_dungeon.store(host_in && (role == kRoleClient || now < waiting_until), std::memory_order_relaxed);
    if (g_joined_read.exchange(false, std::memory_order_relaxed))
        Say("seamless: loading the host's dungeon (the joined slot 0), not this hunter's own selected one");
    BbRoom::Dungeon d;
    if (BbRoom::TakeDungeon(d) && role == kRoleClient && InDungeonMap(OwnMap()) && SlotChannel(0) == d.channel) {
        if (!carry || carry->channel != d.channel || carry->flags.size() != d.flags.size())
            Say("seamless: the host's dungeon progress noted (%zu flag(s)); it goes into this save on the way home", d.flags.size());
        carry = std::move(d);
    }
    if (!carry || Loading() || !local || Field<std::int32_t>(Global(kFlagMan), 0x80, -1) != 0) return;
    int slot = -1;
    for (int k = 1; k < kSlots && slot < 0; ++k)
        if (SlotHoldsDungeon(k) && SlotChannel(k) == carry->channel) slot = k;
    const bool known = slot >= 0;
    for (int k = 1; k < kSlots && slot < 0; ++k)
        if (!SlotHoldsDungeon(k)) slot = k;
    if (slot < 0) {
        Say("seamless: the host's dungeon was not added to the altar: all 6 dungeon slots are taken");
        carry.reset();
        return;
    }
    const std::uint64_t man = Global(kFlagMan);
    if (!known)
        for (int i = 0; i < kRecordWords; ++i)
            Call(Fn(kSetFlagValue), man, RecordBase(slot) + 32 * std::uint32_t(i), 32, carry->record[i]);
    int set = 0;
    for (const std::uint32_t id : carry->flags) {
        if (id / 100000 % 100 != kDungeonArea) continue;
        const std::uint32_t mine = id - kDungeonArea * 100000 + (40 + std::uint32_t(slot)) * 100000;
        if (FlagValue(mine, 1)) continue;
        Call(Fn(kSetEventFlag), man, mine, 1);
        ++set;
    }
    Say("seamless: the host's dungeon %s altar slot %d, %d of its lamp / shortcut / boss flag(s) set",
        known ? "progress added to this hunter's own copy in" : "added to this hunter's altar in", slot, set);
    carry.reset();
    RequestSave();
}

void ChaliceShareTick(std::chrono::steady_clock::time_point now) {
    static std::chrono::steady_clock::time_point last, sent;
    static std::vector<std::uint32_t> owned_last;
    static std::vector<std::uint32_t> wanted;
    std::vector<std::uint32_t> got;
    if (BbRoom::TakeChalices(got))
        for (const std::uint32_t id : got)
            if (std::find(std::begin(kChaliceGoods), std::end(kChaliceGoods), id) != std::end(kChaliceGoods) &&
                std::find(wanted.begin(), wanted.end(), id) == wanted.end())
                wanted.push_back(id);
    if (now - last < kChaliceEvery || Loading() || !LocalPlayer() || Field<std::int32_t>(Global(kFlagMan), 0x80, -1) != 0) return;
    last = now;
    std::vector<std::uint32_t> owned;
    for (const std::uint32_t id : kChaliceGoods)
        if (HasItem(kGoods, id)) owned.push_back(id);
    int given = 0;
    for (const std::uint32_t id : wanted) {
        if (std::find(owned.begin(), owned.end(), id) != owned.end()) continue;
        Call(Fn(kGiveItem), 0, kGoods, id, 1);
        owned.push_back(id);
        ++given;
    }
    wanted.clear();
    if (given) {
        Say("seamless: %d chalice(s) another hunter of the party holds given to this hunter", given);
        RequestSave();
    }
    if (owned != owned_last || now - sent >= kChaliceResend) {
        owned_last = owned;
        sent = now;
        BbRoom::SendChalices(owned);
    }
}

void ChaliceTick(int role) {
    if (!g_chalice.load()) return;
    const auto now = std::chrono::steady_clock::now();
    if (BbRoom::IsHost()) ChaliceHostTick(role, now);
    else ChaliceGuestTick(role, now);
    ChaliceShareTick(now);
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
    own.map = Field<std::uint32_t>(local, 0x3f8);
    Position(local, own.pos);
    const auto now = std::chrono::steady_clock::now();
    const bool changed = own.hp != last.hp || own.hp_max != last.hp_max || own.stamina != last.stamina ||
                         own.stamina_max != last.stamina_max || own.revives != last.revives || own.map != last.map ||
                         Distance2(own.pos, last.pos) > 1.0f;
    if (now - sent_at < (changed ? kVitalsEvery : kVitalsHeartbeat)) return;
    BbRoom::SendVitals(own);
    last = own;
    sent_at = now;
}

void RebuildRevived(std::uint64_t world, const std::vector<BbRoom::Vitals>& party) {
    struct Member {
        std::uint16_t revives = 0;
        std::uint64_t body = 0;
    };
    struct Due {
        std::chrono::steady_clock::time_point at;
        std::uint64_t body;
        std::string name;
    };
    static std::map<int, Member> members;
    static std::vector<Due> due;
    const auto now = std::chrono::steady_clock::now();
    std::map<int, Member> next;
    for (const BbRoom::Vitals& v : party) {
        Member m;
        const auto was = members.find(v.slot);
        if (was != members.end()) m = was->second;
        if (was != members.end() && v.revives != m.revives) {
            due.push_back({now + kRebuildDelay, m.body, v.name});
            m.body = 0;
        }
        m.revives = v.revives;
        if (v.hp > 0) {
            if (const std::uint64_t body = RemoteNear(v.pos, kSameWorld)) m.body = body;
        }
        next[v.slot] = m;
    }
    members.swap(next);
    for (auto it = due.begin(); it != due.end();) {
        if (now < it->at) {
            ++it;
            continue;
        }
        RebuildRemote(world, it->body, it->name);
        it = due.erase(it);
    }
}

void PublishRows(std::uint64_t local, const std::vector<BbRoom::Vitals>& party, int role) {
    float own[3] = {};
    const bool placed = local && Position(local, own);
    const float view = g_camera_known.load() ? g_camera_yaw.load() : Facing(local);
    static std::map<int, std::chrono::steady_clock::time_point> seen;
    const auto now = std::chrono::steady_clock::now();
    const bool moving = Loading() || !placed || g_lead.on || g_follow.lamp;
    const bool in_party = role == kRoleHost || role == kRoleClient;
    if (!in_party && !moving && !g_linger_cards) seen.clear();
    const auto linger = moving || g_linger_cards ? kCardLinger : kCardGone;
    std::vector<PartyRow> rows;
    for (const BbRoom::Vitals& v : party) {
        const bool here = placed && RemoteNear(v.pos, kSameWorld);
        if (here) seen[v.slot] = now;
        const auto was = seen.find(v.slot);
        if (!here && (was == seen.end() || now - was->second > linger)) continue;
        PartyRow row;
        row.name = v.name;
        row.dev = BbRoom::IsDevSlot(v.slot);
        row.health = v.hp_max > 0 ? std::clamp(float(v.hp) / float(v.hp_max), 0.0f, 1.0f) : 0.0f;
        row.stamina = v.stamina_max > 0 ? std::clamp(float(v.stamina) / float(v.stamina_max), 0.0f, 1.0f) : 0.0f;
        row.down = v.hp <= 0;
        if (here) {
            const float dx = v.pos[0] - own[0], dz = v.pos[2] - own[2];
            row.distance = std::sqrt(Distance2(v.pos, own));
            row.bearing = std::atan2(dx, dz) - view;
        }
        rows.push_back(std::move(row));
    }
    std::lock_guard lock(g_rows_mu);
    g_rows.swap(rows);
}

void PortraitTick() {
    static std::uint32_t sent_version = 0;
    static std::chrono::steady_clock::time_point sent;
    static std::map<int, std::vector<std::uint8_t>> unnamed;
    const auto now = std::chrono::steady_clock::now();
    std::vector<std::uint8_t> packed;
    std::uint32_t version = BbFaceShot::OwnVersion();
    if (version && (version != sent_version || now - sent >= kPortraitEvery) && BbFaceShot::OwnPacked(packed, version)) {
        BbRoom::SendPortrait(packed);
        sent_version = version;
        sent = now;
    }
    for (auto& [slot, bytes] : BbRoom::TakePortraits()) unnamed[slot] = std::move(bytes);
    if (unnamed.empty()) return;
    for (const BbRoom::Vitals& v : BbRoom::Party(kPortraitNameAge)) {
        const auto it = unnamed.find(v.slot);
        if (it == unnamed.end()) continue;
        BbFaceShot::Receive(v.name, it->second);
        unnamed.erase(it);
    }
}

void LogOwnState(int role) {
    static int last = -1, seen = -1;
    static std::chrono::steady_clock::time_point since;
    const auto now = std::chrono::steady_clock::now();
    const int place = role == kRoleClient ? 2 : role == kRoleHost ? 1 : 0;
    if (place != seen) {
        seen = place;
        since = now;
    }
    if (place == last || Loading() || now - since < std::chrono::seconds(5)) return;
    const std::uint64_t local = LocalPlayer();
    if (!local || Hp(local) <= 0) return;
    last = place;
    const std::uint64_t data = Field<std::uint64_t>(Field<std::uint64_t>(local, 0x3b0), 0x20);
    std::string ids;
    const std::uint64_t effects = Field<std::uint64_t>(local, 0x1c8);
    int n = 0;
    for (std::uint64_t node = Field<std::uint64_t>(effects, 8); node && n < 64; node = Field<std::uint64_t>(node, 0x58), ++n) {
        if (!ids.empty()) ids += ' ';
        ids += std::to_string(Field<std::int32_t>(node, 0x40));
    }
    Say("seamless: this hunter %s: HP max %d, stamina max %d, effects [%s]",
        role == kRoleClient ? "in the host's world" : role == kRoleHost ? "hosting" : "in its own world",
        Field<std::int32_t>(data, 0xfc), Field<std::int32_t>(data, 0x138), ids.c_str());
}

void GuestStatsTick(int role) {
    static const bool keep = [] {
        const char* v = std::getenv("BB_GUEST_PHANTOM_HP");
        return v && std::strcmp(v, "1") == 0;
    }();
    static int told = 0;
    if (keep || role != kRoleClient || Loading()) return;
    const std::uint64_t local = LocalPlayer();
    if (!local || !EffectNode(local, kGuestHpEffect)) return;
    if (RemoveEffect(local, kGuestHpEffect) && told++ < 3)
        Say("seamless: the guest's 70%% HP effect (9006) removed: this hunter keeps its own stats in the host's world");
}

void BringTick(int role) {
    if (!BbRoom::TakeBring()) return;
    const std::uint64_t local = LocalPlayer();
    if (role != kRoleClient || Loading() || !local || Hp(local) <= 0) {
        Say("seamless: the host called this hunter, but it is not standing in the host's world right now");
        return;
    }
    const int host = BbRoom::HostSlot();
    for (const BbRoom::Vitals& v : BbRoom::Party(kVitalsFresh)) {
        if (v.slot != host) continue;
        if ((v.map & 0xffff0000u) != (Field<std::uint32_t>(local, 0x3f8) & 0xffff0000u)) {
            Say("seamless: the host called this hunter from another area; use the host's lamp instead");
        BbRoom::NoteBringFar();
            return;
        }
        const float yaw = Facing(local);
        const float spot[3] = {v.pos[0] + 1.5f * std::sin(yaw), v.pos[1] + 0.5f, v.pos[2] + 1.5f * std::cos(yaw)};
        const float facing[3] = {0.0f, yaw, 0.0f};
        MoveBodyToSafeSpot(local, spot, facing);
        Say("seamless: brought to the host's side");
        return;
    }
    Say("seamless: the host called this hunter, but its position is not known yet");
}

std::atomic<bool> g_summon{false};
std::atomic<bool> g_silence{false};

void SummonTick(int role) {
    static std::chrono::steady_clock::time_point rang, settled;
    if (g_silence.exchange(false)) {
        const bool host = BbRoom::IsHost();
        Say("seamless: co-op panel: %s %s", host ? "Beckoning Bell" : "Small Resonant Bell",
            SilenceBell(host ? kHostBell : kGuestBell) ? "silenced" : "was not ringing");
    }
    if (!g_summon.load()) return;
    const auto now = std::chrono::steady_clock::now();
    if (BbRoom::IsHost()) {
        if (InWorld().size() >= 3) {
            g_summon = false;
            Say("seamless: the party is full (4 hunters): summoning stopped");
            return;
        }
        if (role != kRoleClient && Settled(settled, now) && now - rang >= kRingEvery) {
            rang = now;
            const bool ok = RingBell(kHostBell);
            static int told = 0;
            if (told++ < 12) Say("seamless: co-op panel: Beckoning Bell %s (role %d)", ok ? "rung" : "not rung", role);
        }
        return;
    }
    if (role == kRoleClient) {
        g_summon = false;
        return;
    }
    if (Settled(settled, now) && now - rang >= kRingEvery) {
        rang = now;
        const bool ok = RingBell(kGuestBell);
        static int told = 0;
        if (told++ < 12) Say("seamless: co-op panel: Small Resonant Bell %s (role %d)", ok ? "rung" : "not rung", role);
    }
}

void Tick() {
    if (!g_image || !BbRoom::Active()) return;
    PortraitTick();
    const int role = Role();
    g_role.store(role, std::memory_order_relaxed);
    NoteBossFight(role);
    if (g_death_rules.load(std::memory_order_relaxed)) g_fight.Update(BossUp(), std::chrono::steady_clock::now());
    if (g_guest_rewards.load(std::memory_order_relaxed)) BossDropsTick(role);
    if (g_death_rules.load(std::memory_order_relaxed)) PromptLockTick(role);
    if (BbRoom::TakeSentHome() && !g_follow.lamp) {
        g_bells_off = true;
        g_summon = false;
        g_silence = true;
        BbRoom::TakeDownSign();
        Say("seamless: sent home: no more signs until Send sign is pressed");
    }
    SummonTick(role);
    BellTraceTick();
    LogOwnState(role);
    BringTick(role);
    GuestStatsTick(role);
    if (g_death_rules.load()) {
        PartyCallsTick(role);
        PendingTick();
    }
    TravelTick();
    LampNotes();
    if (g_progress.load()) {
        if (BbRoom::IsHost()) HostProgressTick(role);
        else GuestProgressTick(role);
    }
    ChaliceTick(role);
    SaveTick();
    const std::uint64_t world = Global(kWorldChrMan);
    const std::uint64_t local = Field<std::uint64_t>(world, 0x60);
    if (!local) {
        std::lock_guard lock(g_rows_mu);
        g_rows.clear();
        return;
    }
    if (Hp(local) > 0) g_alive_draw = Field<float>(local, 0x208, 1.0f);
    SendOwnVitals(local);
    const std::vector<BbRoom::Vitals> party = BbRoom::Party(kVitalsFresh);
    if (g_death_rules.load() && (role == kRoleHost || role == kRoleClient)) {
        RebuildRevived(world, party);
        ShowRebuilt();
    }
    PublishRows(local, party, role);
}

std::vector<PartyRow> PartyRows() {
    std::lock_guard lock(g_rows_mu);
    return g_rows;
}

void SetCameraYaw(float yaw) {
    g_camera_yaw = yaw;
    g_camera_known = true;
}

void StartSummon() {
    g_bells_off = false;
    g_summon = true;
    Say("seamless: %s from the co-op panel", BbRoom::IsHost() ? "summoning the party" : "joining the host");
}

void AnswerLampAsk(bool yes) {
    g_lamp_answer = yes ? 1 : 2;
}

std::string LampPlace(std::uint32_t lamp) {
    return "lamp " + std::to_string(lamp);
}

void StopSummon() {
    g_bells_off = true;
    g_summon = false;
    g_silence = true;
    BbRoom::TakeDownSign();
}

void LeaveHost() {
    StopSummon();
    if (!BbRoom::LeaveParty()) Say("seamless: the host did not answer the leave");
}

bool Summoning() {
    return g_summon.load() || g_auto_bell.load();
}

bool Spectating() {
    return g_spectating.load(std::memory_order_relaxed);
}

void SpectateNext() {
    g_view_next = true;
}

bool SpectatedName(std::string& name, int& choices) {
    std::lock_guard lock(g_view_mu);
    name = g_view_name;
    choices = g_view_choices;
    return !name.empty();
}

int PartyRole() {
    return g_role.load(std::memory_order_relaxed);
}
}
