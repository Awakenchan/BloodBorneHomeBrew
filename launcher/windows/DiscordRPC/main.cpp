// SPDX-FileCopyrightText: Copyright 2026 @erezeth
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Bloodborne PC (bbport) - Discord Rich Presence companion.
// READ-ONLY: this tool never writes to the game's memory.

#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <thread>
#include <vector>
#include <algorithm>
#include <map>
#include "json.hpp"

namespace fs = std::filesystem;
using json = nlohmann::json;
using clk = std::chrono::steady_clock;

constexpr uintptr_t GAME_DATA_MAN = 0x553B130;
constexpr uintptr_t MAP_ID_ADDR = 0x0553B148;    
constexpr uintptr_t BOSS_GAUGE_ADDR = 0x05562878; 
constexpr int PGD_HP = 0x14, PGD_MAXHP = 0x18;
constexpr int PGD_INSIGHT = 0x84, PGD_LEVEL = 0x90, PGD_ECHOES = 0x94;
constexpr int PGD_READ_SIZE = 0x98;

struct AttrDef { const char* name; int off; };
constexpr AttrDef PGD_ATTR_DEFS[6] = {{"VIT", 0x40}, {"END", 0x48}, {"STR", 0x58}, {"SKL", 0x60}, {"BLT", 0x68}, {"ARC", 0x70}};
struct TitleDef { int level; const char* name; };
constexpr TitleDef HUNTER_TITLES[] = {{1, "Fledgling Hunter"}, {15, "Blooded Hunter"}, {40, "Seasoned Hunter"}, {80, "Veteran Hunter"}, {120, "Dreaming Hunter"}, {180, "Hunter of Hunters"}};

constexpr std::array<std::pair<int32_t, const char*>, 21> BbscPlaceNames{{
    {2100, "Hunter's Dream"}, {2110, "Abandoned Old Workshop"}, {2200, "Hemwick Charnel Lane"},
    {2300, "Old Yharnam"}, {2400, "Cathedral Ward"}, {2410, "Central Yharnam"},
    {2411, "Iosefka's Clinic"}, {2420, "Upper Cathedral Ward"}, {2421, "Healing Church Workshop"},
    {2422, "Altar of Despair"}, {2500, "Forsaken Cainhurst Castle"}, {2600, "Nightmare of Mensis"},
    {2700, "Forbidden Woods"}, {2800, "Yahar'gul, Unseen Village"}, {3200, "Byrgenwerth"},
    {3201, "Lecture Building"}, {3202, "Moonside Lake"}, {3300, "Nightmare Frontier"},
    {3400, "Hunter's Nightmare"}, {3500, "Research Hall"}, {3600, "Fishing Hamlet"},
}};

constexpr std::array<std::pair<int32_t, const char*>, 87> BbscBossNames{{
    {1000, "Madman's Escort"}, {1010, "Forgotten Madman"}, {6400, "Old Hunter Henryk"},
    {6500, "Valtr, Master of the League"}, {6505, "Valtr, Beast Eater"}, {6530, "Yamamura the Wanderer"},
    {6540, "Younger Madaras Twin"}, {6550, "Mensis Scholar Damian"}, {6600, "Henriett"},
    {6640, "Defector Antal"}, {7040, "Tomb Prospector Olek"}, {7045, "Tomb Prospector Gremia"},
    {7060, "Beastclaw Jozef"}, {7160, "Queen Killer"}, {7300, "Vitus"}, {7310, "Madman Wallar"},
    {101000, "Degenerate Beast"}, {106000, "Brainsucker"}, {127000, "Maneater Boar"},
    {209000, "Blood-starved Beast"}, {209010, "Blood-starved Beast"}, {210000, "The Witch of Hemwick"},
    {212010, "Shadow of Yharnam"}, {212020, "Shadow of Yharnam"}, {212030, "Shadow of Yharnam"},
    {216000, "Keeper of the Old Lords"}, {218000, "Loran Silverbeast"}, {232000, "Martyr Logarius"},
    {251000, "Ebrietas, Daughter of the Cosmos"}, {251010, "Ebrietas, Daughter of the Cosmos"},
    {257000, "Celestial Emissary"}, {257010, "Celestial Emissary"}, {271000, "Father Gascoigne"},
    {272000, "Father Gascoigne"}, {304000, "Merciless Watcher"}, {304001, "Watcher Chieftain"},
    {304002, "Merciless Watcher"}, {304003, "Merciless Watcher"}, {305000, "Pthumerian Descendant"},
    {305001, "Pthumerian Descendant"}, {305002, "Pthumerian Elder"}, {306000, "Pthumerian Bride"},
    {306001, "Yharnam, Pthumerian Queen"}, {313000, "Undead Giant"}, {403000, "Living Failures"},
    {450000, "Laurence, the First Vicar"}, {451000, "Ludwig the Accursed"}, {451005, "Ludwig, the Holy Blade"},
    {452000, "Lady Maria of the Astral Clocktower"}, {453000, "Orphan of Kos"}, {454000, "Orphan of Kos"},
    {500000, "Cleric Beast"}, {501000, "Watchdog of the Old Lords"}, {502000, "Vicar Amelia"},
    {504000, "Loran Darkbeast"}, {504010, "Abhorrent Beast"}, {507000, "The One Reborn"},
    {508000, "Darkbeast Paarl"}, {508010, "Loran Darkbeast"}, {509000, "Bloodletting Beast"},
    {509010, "Bloodletting Beast"}, {510000, "Rom, the Vacuous Spider"}, {510010, "Rom, the Vacuous Spider"},
    {511000, "Great One Beast"}, {512000, "Amygdala"}, {512010, "Amygdala"}, {540000, "Moon Presence"},
    {551000, "Mergo's Wet Nurse"}, {551010, "Mergo's Wet Nurse"}, {750000, "Beast-possessed Soul"},
    {802000, "Slug Princess"}, {803000, "Heroine Doll"}, {804000, "Gehrman, the First Hunter"},
    {805000, "Gehrman, the First Hunter"}, {899000, "Micolash, Host of the Nightmare"},
    {899010, "Vileblood Hunter Alfred"}, {900020, "The Vileblood"}, {900030, "The Executioners"},
    {900040, "Hunter of Hunters"}, {901040, "Nameless Tomb Prospector"}, {901060, "Izzy's Admirer"},
    {901070, "Nameless Madman"}, {901110, "Bone Ash Hunter Carla"}, {901150, "Drifting Vileblood Leo"},
    {901160, "Ardeo Man"}, {901170, "Defeated Hunter"}, {901180, "Choir Intelligencer Edgar"},
}};

template <size_t N>
const char* FindBbscName(const std::array<std::pair<int32_t, const char*>, N>& names, int32_t id) {
    for (const auto& [key, text] : names) { if (key == id) return text; }
    return nullptr;
}

static std::string GetAreaName(uint32_t map_id) {
    if (map_id == 0) return "";
    int32_t area = (map_id >> 24) & 0xFF, block = (map_id >> 16) & 0xFF;
    if (area > 40 || block > 20) return ""; 
    if (area == 29) return "Chalice Dungeon";
    if (const char* name = FindBbscName(BbscPlaceNames, area * 100 + block * 10)) return name;
    if (const char* name = FindBbscName(BbscPlaceNames, area * 100)) return name;
    char buf[32]; std::snprintf(buf, sizeof(buf), "m%02d_%02d", area, block); return buf;
}

static fs::path g_exe_dir;
static std::string hex(uintptr_t v) { char b[32]; std::snprintf(b, sizeof(b), "0x%llx", (unsigned long long)v); return b; }

static void log(const std::string& msg) {
    char ts[16]; std::time_t t = std::time(nullptr); std::tm tb{}; localtime_s(&tb, &t);
    std::strftime(ts, sizeof(ts), "%H:%M:%S", &tb);
    std::string line = std::string("[") + ts + "] " + msg;
    std::ofstream f(g_exe_dir / "rpc.log", std::ios::app); if (f) f << line << "\n";
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (out && out != INVALID_HANDLE_VALUE) { DWORD w = 0; std::string cl = line + "\n"; WriteFile(out, cl.data(), (DWORD)cl.size(), &w, nullptr); }
}

static void fatal(const std::string& msg) { log("[ERROR] " + msg); if (!std::getenv("BB_GAME_PID")) MessageBoxA(nullptr, msg.c_str(), "BloodborneRPC", MB_ICONERROR | MB_OK); }
static fs::path exe_directory() { wchar_t buf[MAX_PATH]; DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH); if (n == 0) return fs::current_path(); try { return fs::path(std::wstring(buf, n)).parent_path(); } catch (...) { return fs::current_path(); } }

struct Config {
    std::string client_id; std::string process_name = "bb-probe.exe"; std::vector<uintptr_t> image_bases = {0x800000000};
    int attach_delay = 5, update_interval = 15; double poll_interval = 1.0;
    std::string large_image = "bloodborne_logo", small_image; json buttons = json::array(); bool offline_only = true;
    bool show_titles = true, show_echoes = true, show_insight = true, show_level_up = true, show_session_levels = true, show_attributes_tooltip = true;
};

static Config load_config(const fs::path& path) {
    Config c; if (!fs::exists(path)) return c;
    try {
        std::ifstream f(path); json j; f >> j;
        if (j.contains("client_id")) c.client_id = j["client_id"];
        if (j.contains("process_name")) c.process_name = j["process_name"];
        if (j.contains("image_base_candidates") && j["image_base_candidates"].is_array()) {
            c.image_bases.clear(); for (const auto& v : j["image_base_candidates"]) { if (v.is_string()) c.image_bases.push_back(std::strtoull(v.get<std::string>().c_str(), nullptr, 0)); }
        }
        if (j.contains("attach_delay")) c.attach_delay = j["attach_delay"];
        if (j.contains("large_image")) c.large_image = j["large_image"];
        if (j.contains("offline_only")) c.offline_only = j["offline_only"];
        if (j.contains("show")) {
            auto& s = j["show"];
            if (s.contains("titles")) c.show_titles = s["titles"]; if (s.contains("echoes")) c.show_echoes = s["echoes"];
            if (s.contains("insight")) c.show_insight = s["insight"]; if (s.contains("attributes_tooltip")) c.show_attributes_tooltip = s["attributes_tooltip"];
        }
    } catch (...) {} return c;
}

class Process {
    HANDLE h_ = nullptr;
public:
    Process() = default; Process(const Process&) = delete; Process& operator=(const Process&) = delete;
    Process(Process&& o) noexcept : h_(o.h_) { o.h_ = nullptr; }
    Process& operator=(Process&& o) noexcept { if (this != &o) { close(); h_ = o.h_; o.h_ = nullptr; } return *this; }
    ~Process() { close(); }
    bool open(DWORD pid) { close(); h_ = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, pid); return h_ != nullptr; }
    void close() { if (h_) { CloseHandle(h_); h_ = nullptr; } }
    bool read(uintptr_t addr, void* buf, size_t size) const { if (!h_) return false; SIZE_T got = 0; return ReadProcessMemory(h_, (LPCVOID)addr, buf, size, &got) && got == size; }
    template <typename T> bool read(uintptr_t addr, T& out) const { return read(addr, &out, sizeof(T)); }
};

static std::vector<DWORD> find_processes(const std::string& name) {
    std::vector<DWORD> pids; HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return pids;
    PROCESSENTRY32W pe{}; pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) { do { if (_wcsicmp(pe.szExeFile, std::wstring(name.begin(), name.end()).c_str()) == 0) pids.push_back(pe.th32ProcessID); } while (Process32NextW(snap, &pe)); }
    CloseHandle(snap); return pids;
}

static bool pid_alive(DWORD pid, const std::string& name_hint = "") {
    if (!pid) return false; HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return false; DWORD ec = 0; bool alive = GetExitCodeProcess(h, &ec) && ec == STILL_ACTIVE;
    if (alive && !name_hint.empty()) { wchar_t buf[MAX_PATH]; DWORD s = MAX_PATH; if (QueryFullProcessImageNameW(h, 0, buf, &s)) { fs::path p(std::wstring(buf, s)); if (_wcsicmp(p.filename().c_str(), std::wstring(name_hint.begin(), name_hint.end()).c_str()) != 0) alive = false; } }
    CloseHandle(h); return alive;
}

static uintptr_t find_base(const Process& p, const std::vector<uintptr_t>& bases) {
    for (auto b : bases) { uint8_t tmp[8]; if (p.read(b + GAME_DATA_MAN, tmp, sizeof(tmp))) return b; } return 0;
}

// FUNGSI BARU: Baca Rantai Pointer Map
static uint32_t ReadCurrentMapId(const Process& p, uintptr_t base) {
    uint64_t map_list = 0;
    if (!p.read(base + MAP_ID_ADDR, map_list) || !map_list || map_list < 0x10000) return 0;
    
    int32_t index = 0; 
    if (!p.read(map_list + 0x20, index)) return 0;
    
    uint64_t map_array = 0; 
    if (!p.read(map_list + 0x10, map_array) || !map_array || map_array < 0x10000) return 0;
    
    int32_t count = 0; 
    if (!p.read(map_array + 0x18, count) || count <= 0 || index < 0 || index >= count) return 0;
    
    uint64_t entries = 0; 
    if (!p.read(map_array + 0x20, entries) || !entries || entries < 0x10000) return 0;
    
    uint64_t entry = entries + (index * 0xA0);
    uint32_t packed_map = 0;
    if (!p.read(entry + 0x08, packed_map)) return 0;
    
    return packed_map;
}

struct Stats { int level, insight, echoes, hp, maxhp; uint32_t map_id; std::string area_name; std::string boss_name; std::array<int, 6> attrs; };

static std::string GetBossName(const Process& p, uintptr_t base) {
    uint64_t gauge = 0; if (!p.read(base + BOSS_GAUGE_ADDR, gauge) || !gauge || gauge < 0x10000) return "";
    for (int i = 0; i < 3; ++i) {
        int32_t handle = -1; if (p.read(gauge + 0x165C + i * 0x14, handle) && handle != -1) {
            int32_t boss_id = -1; if (p.read(gauge + 0x1658 + i * 0x14, boss_id)) { if (const char* name = FindBbscName(BbscBossNames, boss_id)) return name; return "Unknown Boss"; }
        }
    } return "";
}

static std::optional<Stats> read_stats(const Process& p, uintptr_t base) {
    uint64_t gdm = 0; if (!p.read(base + GAME_DATA_MAN, gdm) || !gdm || gdm < 0x10000) return std::nullopt;
    uint64_t pgd = 0; if (!p.read(gdm + 8, pgd) || !pgd || pgd < 0x10000) return std::nullopt;
    uint8_t blk[PGD_READ_SIZE]; if (!p.read(pgd, blk, sizeof(blk))) return std::nullopt;
    auto i32 = [&](int off) { int32_t v; std::memcpy(&v, blk + off, 4); return v; };
    
    Stats s{};
    for (int i = 0; i < 6; ++i) s.attrs[i] = i32(PGD_ATTR_DEFS[i].off);
    for (int a : s.attrs) if (a < 1 || a > 99) return std::nullopt;
    
    s.level = i32(PGD_LEVEL); int sum = 0; for (int a : s.attrs) sum += a; if (s.level != sum - 50) return std::nullopt;
    s.insight = i32(PGD_INSIGHT); s.echoes = i32(PGD_ECHOES); s.hp = i32(PGD_HP); s.maxhp = i32(PGD_MAXHP);
    
    if (s.insight < 0 || s.insight > 999 || s.echoes < 0 || s.echoes > 999999999 || s.maxhp <= 0 || s.maxhp >= 100000 || s.hp < 0 || s.hp > s.maxhp) return std::nullopt;

    s.map_id = ReadCurrentMapId(p, base);
    if (s.map_id != 0) {
        s.area_name = GetAreaName(s.map_id);
    }
    
    s.boss_name = GetBossName(p, base); return s;
}

class Session {
    static constexpr int LEVEL_UP_SECONDS = 120; int last_level_ = -1, known_level_ = -1, levels_gained_ = 0, level_up_to_ = 0; clk::time_point level_up_until_{};
public:
    void update(const std::optional<Stats>& stats, clk::time_point now) {
        if (!stats) { last_level_ = -1; return; }
        int level = stats->level;
        if (last_level_ < 0 && known_level_ >= 0) { int diff = level - known_level_; if (diff < 0 || diff > 5) { levels_gained_ = 0; level_up_until_ = {}; } }
        known_level_ = level;
        if (last_level_ >= 0 && level > last_level_ && level - last_level_ <= 5) { levels_gained_ += level - last_level_; level_up_to_ = level; level_up_until_ = now + std::chrono::seconds(LEVEL_UP_SECONDS); }
        last_level_ = level;
    }
    int levels_gained() const { return levels_gained_; } bool level_up_active(clk::time_point now) const { return now < level_up_until_; } int level_up_to() const { return level_up_to_; }
};

class DiscordPipe {
    HANDLE pipe_ = INVALID_HANDLE_VALUE;
public:
    ~DiscordPipe() { close(); } void close() { if (pipe_ != INVALID_HANDLE_VALUE) { CloseHandle(pipe_); pipe_ = INVALID_HANDLE_VALUE; } } bool connected() const { return pipe_ != INVALID_HANDLE_VALUE; }
    bool connect_pipe(const std::string& client_id) {
        close(); for (int i = 0; i < 10; ++i) { std::wstring name = L"\\\\.\\pipe\\discord-ipc-" + std::to_wstring(i); pipe_ = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr); if (pipe_ != INVALID_HANDLE_VALUE) break; }
        if (pipe_ == INVALID_HANDLE_VALUE) return false; std::string hs = std::string("{\"v\":1,\"client_id\":\"") + client_id + "\"}";
        if (!send(0, hs)) { close(); return false; } return drain(true);
    }
    bool send(uint32_t opcode, const std::string& payload) {
        if (pipe_ == INVALID_HANDLE_VALUE) return false; std::string frame(8, '\0'); std::memcpy(&frame[0], &opcode, 4);
        uint32_t len = (uint32_t)payload.size(); std::memcpy(&frame[4], &len, 4); frame += payload; DWORD written = 0;
        return WriteFile(pipe_, frame.data(), (DWORD)frame.size(), &written, nullptr) && written == frame.size();
    }
    bool drain(bool wait) {
        for (;;) {
            DWORD avail = 0; if (!PeekNamedPipe(pipe_, nullptr, 0, nullptr, &avail, nullptr)) { close(); return false; }
            if (avail < 8 && !wait) return true; uint32_t hdr[2] = {0, 0}; DWORD got = 0;
            if (!ReadFile(pipe_, hdr, 8, &got, nullptr) || got != 8) { close(); return false; } std::string body(hdr[1], '\0');
            if (hdr[1] > 0 && (!ReadFile(pipe_, body.data(), hdr[1], &got, nullptr) || got != hdr[1])) { close(); return false; }
            if (hdr[0] == 2) { close(); return false; } wait = false;
        }
    }
};

static std::string format_thousands(int n) {
    std::string s = std::to_string(n), out; int c = 0;
    for (int i = (int)s.size() - 1; i >= 0; --i) { if (c > 0 && c % 3 == 0) out = ',' + out; out = s[i] + out; ++c; } return out;
}

static json build_presence(const std::optional<Stats>& stats, Session& session, const Config& cfg, int64_t session_start_s, clk::time_point now) {
    json data; data["large_image"] = cfg.large_image; data["start"] = session_start_s;
    if (!cfg.small_image.empty()) data["small_image"] = cfg.small_image;
    if (cfg.buttons.is_array() && !cfg.buttons.empty()) { json btns = json::array(); int n = 0; for (const auto& b : cfg.buttons) { if (n++ >= 2) break; btns.push_back(b); } data["buttons"] = std::move(btns); }
    if (!stats) { data["details"] = "Wandering the menus"; data["state"] = "Waiting for a hunter..."; data["large_text"] = "Bloodborne PC"; return data; }
    
    const int level = stats->level; std::string title = "Hunter"; 
    if (cfg.show_titles) { title = HUNTER_TITLES[0].name; for (const auto& t : HUNTER_TITLES) if (level >= t.level) title = t.name; }
    
    if (cfg.show_level_up && session.level_up_active(now)) { char b[64]; std::snprintf(b, sizeof(b), "\u2726 Ascended to Level %d", session.level_up_to()); data["details"] = b; }
    else if (!stats->boss_name.empty()) { data["details"] = "Boss Fight: " + stats->boss_name; }
    else if (!stats->area_name.empty()) { data["details"] = "In " + stats->area_name; }
    else { char b[128]; std::snprintf(b, sizeof(b), "%s \u2022 Lv %d", title.c_str(), level); data["details"] = b; }
    
    std::vector<std::string> parts;
    if (cfg.show_echoes) { char b[128]; std::snprintf(b, sizeof(b), "%s Echoes", format_thousands(stats->echoes).c_str()); parts.push_back(b); }
    if (cfg.show_insight) { char b[64]; std::snprintf(b, sizeof(b), "Insight %d", stats->insight); parts.push_back(b); }
    if (cfg.show_session_levels && session.levels_gained() > 0) { int g = session.levels_gained(); char b[64]; std::snprintf(b, sizeof(b), "+%d level%s", g, g == 1 ? "" : "s"); parts.push_back(b); }
    
    std::string s; for (size_t i = 0; i < parts.size(); ++i) { if (i) s += " \u2022 "; s += parts[i]; } data["state"] = s;
    
    if (cfg.show_attributes_tooltip) { std::string attrs; for (int i = 0; i < 6; ++i) { if (i) attrs += " "; attrs += PGD_ATTR_DEFS[i].name; attrs += " "; attrs += std::to_string(stats->attrs[i]); } char b[256]; std::snprintf(b, sizeof(b), "%s \u2022 Max HP %d", attrs.c_str(), stats->maxhp); data["large_text"] = b; } else { data["large_text"] = "Bloodborne PC"; }
    for (auto it = data.begin(); it != data.end(); ++it) if (it->is_string() && it->get<std::string>().size() > 128) *it = it->get<std::string>().substr(0, 128); return data;
}

static json to_discord_activity(const json& flat) {
    json a = json::object(); if (flat.contains("details")) a["details"] = flat["details"]; if (flat.contains("state")) a["state"] = flat["state"]; if (flat.contains("start")) a["timestamps"] = {{"start", (int64_t)flat["start"] * 1000}};
    json assets = json::object(); if (flat.contains("large_image")) assets["large_image"] = flat["large_image"]; if (flat.contains("large_text")) assets["large_text"] = flat["large_text"]; if (!assets.empty()) a["assets"] = std::move(assets); return a;
}

static std::string play_mode() {
    const char* pid = std::getenv("BB_GAME_PID"); if (!pid || !*pid) return ""; for (const char* c = pid; *c; ++c) if (!std::isdigit((unsigned char)*c)) return "";
    if (auto coop = std::getenv("BB_COOP"); coop && std::string(coop) == "1") return "coop"; if (auto online = std::getenv("BB_ONLINE"); online && std::string(online) == "1") { auto backend = std::getenv("BB_NET_BACKEND"); return (backend && std::string(backend) == "thd") ? "thd" : "online"; } return "offline";
}

static bool should_skip(const Config& cfg, const std::string& mode, std::string& reason) { if (cfg.offline_only && !mode.empty() && mode != "offline") { reason = "Play mode is " + mode + ", which has its own Rich Presence."; return true; } return false; }

static void probe_mode(const Config& cfg) {
    auto pids = find_processes(cfg.process_name);
    if (pids.empty()) { log("Process " + cfg.process_name + " is not running."); return; }
    log("Found " + std::to_string(pids.size()) + " process(es) named " + cfg.process_name);
    for (DWORD pid : pids) {
        Process p; if (!p.open(pid)) { log("PID " + std::to_string(pid) + ": Access denied."); continue; }
        uintptr_t base = find_base(p, cfg.image_bases);
        if (!base) { log("PID " + std::to_string(pid) + ": Base not found."); continue; }
        log("PID " + std::to_string(pid) + ": Base found at " + hex(base) + " (MATCH!)");
        
        auto stats = read_stats(p, base);
        if (!stats) { log("  -> Stats: null (Menus)."); continue; }
        log("  -> Level: " + std::to_string(stats->level) + " | Echoes: " + std::to_string(stats->echoes));
        log("  -> Map ID: " + hex(stats->map_id) + " (" + stats->area_name + ")");
        log("  -> Boss: " + (stats->boss_name.empty() ? "None" : stats->boss_name));
    }
}

static void run(const Config& cfg) {
    if (cfg.client_id.empty() || !std::all_of(cfg.client_id.begin(), cfg.client_id.end(), ::isdigit)) { fatal("Config Error: client_id missing."); return; }
    const bool companion = [] { const char* pid = std::getenv("BB_GAME_PID"); return pid && *pid && std::all_of(pid, pid + std::strlen(pid), ::isdigit); }();
    const DWORD companion_pid = companion ? (DWORD)std::strtoul(std::getenv("BB_GAME_PID"), nullptr, 10) : 0;
    const std::string mode = play_mode(); std::string reason; if (should_skip(cfg, mode, reason)) { log("Skipping: " + reason); return; }
    if (companion) log("Companion Mode: Watching game PID " + std::to_string(companion_pid)); else log("Standalone Mode: Waiting for " + cfg.process_name + " ...");
    const auto poll = std::chrono::milliseconds((int)(std::max(0.25, std::min(5.0, cfg.poll_interval)) * 1000));
    const int update_interval = std::max(15, cfg.update_interval); const std::string check_name = companion ? "" : cfg.process_name;

    while (true) {
        DWORD pid = companion_pid;
        if (!companion) { auto pids = find_processes(cfg.process_name); pid = 0; for (DWORD p : pids) { Process tp; if (tp.open(p) && find_base(tp, cfg.image_bases)) { pid = p; break; } } }
        if (companion && !pid_alive(pid)) { log("Game process closed, exiting."); return; }
        if (!pid) { std::this_thread::sleep_for(std::chrono::seconds(3)); continue; }
        std::this_thread::sleep_for(std::chrono::seconds(cfg.attach_delay));
        if (companion && !pid_alive(pid)) { log("Game closed before attachment, exiting."); return; }
        Process proc; if (!proc.open(pid)) { log("Failed to open process (Try running as Admin). Retrying..."); std::this_thread::sleep_for(std::chrono::seconds(10)); continue; }
        uintptr_t base = find_base(proc, cfg.image_bases); if (!base) { log("Game memory not ready. Retrying..."); std::this_thread::sleep_for(std::chrono::seconds(5)); continue; }
        DiscordPipe discord; while (pid_alive(pid, check_name)) { if (discord.connect_pipe(cfg.client_id)) break; log("Failed to connect to Discord. Retrying in 5s..."); std::this_thread::sleep_for(std::chrono::seconds(5)); }
        log("Attached to PID " + std::to_string(pid) + " (Base: " + hex(base) + ")");
        const int64_t start_s = (int64_t)std::time(nullptr); Session session; json last_payload; auto last_sent = clk::time_point{};
        while (pid_alive(pid, check_name)) {
            if (!discord.connected()) { if (!discord.connect_pipe(cfg.client_id)) { std::this_thread::sleep_for(std::chrono::seconds(5)); continue; } }
            discord.drain(false); if (!discord.connected()) continue;
            auto stats = read_stats(proc, base); session.update(stats, clk::now()); json payload = build_presence(stats, session, cfg, start_s, clk::now()); auto now = clk::now();
            if (payload != last_payload && (now - last_sent) >= std::chrono::seconds(update_interval)) {
                json cmd = { {"cmd", "SET_ACTIVITY"}, {"args", {{"pid", (int)pid}, {"activity", to_discord_activity(payload)}}}, {"nonce", std::to_string((uint64_t)now.time_since_epoch().count())} };
                if (discord.send(1, cmd.dump())) { last_payload = payload; last_sent = now; log("Presence Updated: " + payload.value("details", std::string()) + " | " + payload.value("state", std::string())); } else discord.close();
            }
            std::this_thread::sleep_for(poll);
        }
        if (discord.connected()) { json cmd = {{"cmd", "SET_ACTIVITY"}, {"args", {{"pid", (int)pid}}}, {"nonce", "0"}}; discord.send(1, cmd.dump()); }
        log("Game closed. Cleaning up presence."); if (companion) return; log("Waiting for game again...");
    }
}

static bool has_flag(const wchar_t* flag) { return wcsstr(GetCommandLineW(), flag) != nullptr; }

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    g_exe_dir = exe_directory(); const Config cfg = load_config(g_exe_dir / "config.json");
    if (has_flag(L"--probe")) { probe_mode(cfg); return 0; } run(cfg); return 0;
}
