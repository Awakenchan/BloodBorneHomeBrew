// SPDX-License-Identifier: GPL-3.0-or-later
#include "rebirth.h"
#include "engine_state.h"
#include "rebirth_cost.h"
#include "../../../tools/engine/rebirth_script.h"
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

extern "C" int sys_file_translate(const char*, char*, std::size_t);
extern "C" int sys_file_mount(const char*, const char*);
extern "C" void sys_file_unmount(const char*);

namespace BbEngine::Rebirth {
namespace {
constexpr char kAltarScripts[] = "dvdroot_ps4/script/talk/m24_02_00_00.talkesdbnd.dcx";
constexpr std::uint32_t kAltarMap = 0x18020000;
constexpr std::size_t kApply = 0x1c13d00;
constexpr std::uint8_t kApplyEntry[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41,
                                        0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xec, 0x28};
constexpr std::size_t kWorldChrMan = 0x553e878;
constexpr std::uint32_t kOriginRows = 3000, kPriceRow = 200;
constexpr std::size_t kOriginLevel = 0xc0, kOriginStats[6] = {0xc2, 0xc3, 0xc5, 0xc6, 0xc7, 0xc8};
constexpr std::size_t kCurve = 0x3c;
constexpr const char* kStatNames[6] = {"vitality", "endurance", "strength", "skill", "bloodtinge", "arcane"};
constexpr int kMenuPatience = 120;

std::uint64_t g_image = 0;
int g_watch = -1;
bool g_on = false;
bool g_away = true;
std::optional<Hunter> g_before;
std::uint64_t g_menu = 0;
int g_menu_frames = 0;
int g_complaints = 0;

std::uint64_t Follow(std::uint64_t at) {
    std::uint64_t v = 0;
    return at && host_read_safe(reinterpret_cast<const void*>(at), &v, sizeof v) ? v : 0;
}

bool Flag(std::uint32_t id) {
    bool v = false;
    return event_flag_get(id, &v) && v;
}

std::optional<Hunter> Now() {
    Hunter h;
    if (!player_stat_get("level", &h.level) || !player_stat_get("echoes", &h.echoes)) return std::nullopt;
    for (int i = 0; i < 6; ++i)
        if (!player_stat_get(kStatNames[i], &h.stats[i])) return std::nullopt;
    return h;
}

void Become(const Hunter& h) {
    alignas(8) std::uint32_t functor[9]{};
    functor[2] = static_cast<std::uint32_t>(h.level);
    for (int i = 0; i < 6; ++i) functor[3 + i] = static_cast<std::uint32_t>(h.stats[i]);
    Call(g_image + kApply, functor);
    const std::uint64_t chr = Follow(Follow(g_image + kWorldChrMan) + 0x60);
    const std::uint64_t points = Follow(Follow(chr + 0x3b0) + 0x20);
    std::int32_t most = 0;
    if (points && host_read_safe(reinterpret_cast<const void*>(points + 0xfc), &most, sizeof most) && most > 0) {
        std::memcpy(reinterpret_cast<void*>(points + 0xf8), &most, sizeof most);
        player_stat_set("hp", most);
    }
    player_stat_set("echoes", h.echoes);
}

bool Reborn() {
    const auto now = Now();
    int origin = -1;
    if (!now || !player_origin(&origin)) {
        host_log("rebirth: the hunter could not be read");
        return false;
    }
    std::size_t row_size = 0, curve_size = 0;
    const auto* row = static_cast<const std::uint8_t*>(params_row("CharaInitParam", kOriginRows + origin, &row_size));
    const auto* graph = static_cast<const std::uint8_t*>(params_row("CalcCorrectGraph", kPriceRow, &curve_size));
    if (!row || row_size < kOriginStats[5] + 1 || !graph || curve_size < kCurve + sizeof(LevelCurve)) {
        host_log("rebirth: origin %d's starting hunter or the level-up price is not loaded", origin);
        return false;
    }
    Hunter start;
    std::int16_t level = 0;
    std::memcpy(&level, row + kOriginLevel, sizeof level);
    start.level = level;
    for (int i = 0; i < 6; ++i) start.stats[i] = row[kOriginStats[i]];
    LevelCurve curve;
    std::memcpy(&curve, graph + kCurve, sizeof curve);
    const auto refund = Refund(*now, start, curve);
    if (!refund) {
        host_log("rebirth: refused (level %lld, %lld echoes held: the refund would not fit or the hunter is not one the game makes)",
                 static_cast<long long>(now->level), static_cast<long long>(now->echoes));
        return false;
    }
    start.echoes = now->echoes + *refund;
    g_before = now;
    Become(start);
    host_log("rebirth: level %lld back to origin %d's level %lld, %lld blood echoes given back (%lld held)",
             static_cast<long long>(now->level), origin, static_cast<long long>(start.level),
             static_cast<long long>(*refund), static_cast<long long>(start.echoes));
    return true;
}

void Undo() {
    if (!g_before) {
        host_log("rebirth: nothing to undo");
        return;
    }
    Become(*g_before);
    host_log("rebirth: undone, level %lld with %lld blood echoes again", static_cast<long long>(g_before->level),
             static_cast<long long>(g_before->echoes));
    g_before.reset();
}

void LetGoOfMenu() {
    if (g_menu) menu_step_release(g_menu);
    g_menu = 0;
}
}

bool Install(std::uint8_t* image, std::size_t size) {
    const char* asset = std::getenv("BB_REBIRTH_ASSET");
    const char* game = std::getenv("BB_GAME_DIR");
    if (!asset || !*asset || !game || !*game || !std::getenv("BB_PC_MENU_ASSETS")) return false;
    if (size < kApply + sizeof kApplyEntry || std::memcmp(image + kApply, kApplyEntry, sizeof kApplyEntry) != 0) {
        host_log("rebirth off: the level-up's apply is not 1.09's");
        return false;
    }
    namespace fs = std::filesystem;
    const std::string guest = std::string("/app0/") + kAltarScripts;
    char resolved[512]{};
    std::error_code error;
    const char* merged = std::getenv("BB_GAME_MERGED");
    const bool games = !sys_file_translate(guest.c_str(), resolved, sizeof resolved) &&
                       (fs::equivalent(resolved, fs::path(game) / kAltarScripts, error) ||
                        (merged && *merged && fs::equivalent(resolved, fs::path(merged) / kAltarScripts, error)));
    if (!games) {
        host_log("rebirth off: a mod replaces the altar's talk scripts");
        return false;
    }
    if (!fs::is_regular_file(asset, error) || sys_file_mount(guest.c_str(), asset)) return false;
    g_image = reinterpret_cast<std::uint64_t>(image);
    g_watch = menu_steps_watch(7, u"LevelUp");
    if (g_watch < 0) {
        sys_file_unmount(guest.c_str());
        return false;
    }
    g_on = true;
    host_log("rebirth: the Altar of Despair offers Rebirth (with the Yharnam Stone), its refund and an undo");
    return true;
}

void Tick() {
    if (!g_on) return;
    const std::uint64_t made = menu_steps_take(g_watch);
    std::uint32_t map = 0;
    if (!world_player_block(&map)) {
        g_away = true;
        return;
    }
    using namespace rebirth;
    if (g_away) {
        g_away = false;
        for (const std::uint32_t id : {kReqReset, kReqUndo, kFail, kMenu}) event_flag_set(id, false);
        g_before.reset();
        LetGoOfMenu();
        return;
    }
    if (Flag(kReqReset)) {
        if (map != kAltarMap || !Reborn()) event_flag_set(kFail, true);
        event_flag_set(kReqReset, false);
    }
    if (Flag(kReqUndo)) {
        Undo();
        event_flag_set(kReqUndo, false);
    }
    if (!Flag(kMenu)) {
        LetGoOfMenu();
        return;
    }
    if (!g_menu && menu_step_is(made, 7, u"LevelUp")) {
        menu_step_hold(made);
        g_menu = made;
        g_menu_frames = 0;
    }
    if (g_menu && *menu_step_count(g_menu) == 1) {
        LetGoOfMenu();
        event_flag_set(kMenu, false);
    } else if (!g_menu && ++g_menu_frames > kMenuPatience) {
        g_menu_frames = 0;
        event_flag_set(kMenu, false);
        if (g_complaints++ < 4) host_log("rebirth: the level-up menu did not open");
    }
}
}
