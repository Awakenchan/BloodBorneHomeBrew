// SPDX-License-Identifier: GPL-3.0-or-later
#include "online_fixes.h"
#include "engine_hooks.h"
#include "bbport_platform.h"
#include <algorithm>
#include <atomic>
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
    if (const char* backend = std::getenv("BB_NET_BACKEND"); backend && std::strcmp(backend, "room") == 0)
        any = BellRules(image, size) || any;
    const char* timeout = std::getenv("BB_SIGN_TIMEOUT");
    const long seconds = timeout ? std::strtol(timeout, nullptr, 10) : 0;
    if (seconds > 0 && seconds != 180) any = SetSignTimeout(image, size, seconds) || any;
    return any;
}
}
