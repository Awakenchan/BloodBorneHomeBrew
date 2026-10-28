// SPDX-License-Identifier: GPL-3.0-or-later
#include "game_status.h"
#include "engine_state.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>

namespace BbEngine::GameStatus {
namespace {
using Clock = std::chrono::steady_clock;

constexpr std::size_t kGaugeLoad = 0x131ef44;
constexpr std::uint8_t kGaugeLoadBytes[] = {0x4c, 0x8b, 0x3d, 0x2d, 0x39, 0x24, 0x04};
constexpr std::size_t kGaugeStore = 0x131ef59;
constexpr std::uint8_t kGaugeStoreBytes[] = {0x48, 0x63, 0xcb, 0x8b, 0x40, 0x08, 0x48, 0x8d, 0x0c,
                                             0x89, 0x41, 0x89, 0x84, 0x8f, 0x5c, 0x16, 0x00, 0x00};
constexpr std::size_t kGaugeSlot = 0x5562878;
constexpr std::size_t kBossId = 0x1658, kBossTarget = 0x165c, kBossStride = 0x14;
constexpr int kBossSlots = 3;
constexpr auto kPoll = std::chrono::seconds(1);
constexpr auto kMinGap = std::chrono::seconds(5);

bool g_on = false;
std::uint64_t g_gauge_slot = 0;
Clock::time_point g_next{}, g_said_at{};

struct Status {
    std::uint32_t map = 0;
    std::int32_t boss = -1;
    std::int64_t level = 0, echoes = 0, insight = 0;
    bool present = false;
    bool operator==(const Status&) const = default;
};
Status g_said{};

template <typename T> bool Peek(std::uint64_t at, T* out) {
    return at && host_read_safe(reinterpret_cast<const void*>(at), out, sizeof(T));
}

bool Matches(const std::uint8_t* image, std::size_t size, std::size_t at, std::span<const std::uint8_t> bytes) {
    return at <= size && bytes.size() <= size - at && std::memcmp(image + at, bytes.data(), bytes.size()) == 0;
}

std::int32_t Boss() {
    std::uint64_t gauge = 0;
    if (!Peek(g_gauge_slot, &gauge) || gauge < 0x10000) return -1;
    for (int i = 0; i < kBossSlots; ++i) {
        std::int32_t target = -1, id = -1;
        if (Peek(gauge + kBossTarget + i * kBossStride, &target) && target != -1 &&
            Peek(gauge + kBossId + i * kBossStride, &id) && id > 0)
            return id;
    }
    return -1;
}

Status Read() {
    Status s;
    std::uint32_t map = 0;
    if (!world_player_block(&map) || !player_stat_get("level", &s.level) || s.level < 1 || s.level > 999 ||
        !player_stat_get("echoes", &s.echoes) || !player_stat_get("insight", &s.insight))
        return s;
    s.present = true;
    s.map = map;
    s.boss = Boss();
    return s;
}

void Say(const Status& s, Clock::time_point now) {
    g_said = s;
    g_said_at = now;
    if (!s.present) {
        std::printf("Game status: none\n");
        return;
    }
    std::printf("Game status: map %08x, boss %d, level %lld, echoes %lld, insight %lld\n", s.map, s.boss,
                static_cast<long long>(s.level), static_cast<long long>(s.echoes), static_cast<long long>(s.insight));
}
}

bool Install(std::uint8_t* image, std::size_t size) {
    const char* on = std::getenv("LANTERN_GAME_STATUS");
    if (on && on[0] == '0') {
        host_log("Game status: off (LANTERN_GAME_STATUS=0)");
        return false;
    }
    if (!Matches(image, size, kGaugeLoad, kGaugeLoadBytes) || !Matches(image, size, kGaugeStore, kGaugeStoreBytes)) {
        host_log("Game status off: the boss gauge differs from 1.09");
        return false;
    }
    g_gauge_slot = reinterpret_cast<std::uint64_t>(image + kGaugeSlot);
    g_on = true;
    host_log("Game status: area, boss, level, Blood Echoes and Insight for the launcher's Discord status");
    return true;
}

void Tick() {
    if (!g_on) return;
    const auto now = Clock::now();
    if (now < g_next) return;
    g_next = now + kPoll;
    const Status s = Read();
    if (s == g_said) return;
    const bool place = s.present != g_said.present || s.map != g_said.map || s.boss != g_said.boss ||
                       s.level != g_said.level;
    if (place || now - g_said_at >= kMinGap) Say(s, now);
}
}
