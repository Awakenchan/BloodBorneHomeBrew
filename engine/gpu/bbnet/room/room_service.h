// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace BbRoom {

struct Vitals {
    int slot = 0;
    std::string name;
    std::int32_t hp = 0, hp_max = 0, stamina = 0, stamina_max = 0;
    std::uint16_t revives = 0;
    float pos[3] = {};
    std::uint32_t map = 0;
    std::int64_t age_ms = 0;
};

void SendVitals(const Vitals& own);
std::vector<Vitals> Party(int max_age_ms);
void SendTravel(std::uint32_t lamp);
bool TakeTravel(std::uint32_t& lamp);
void SendProgress(const std::vector<std::uint32_t>& flags);
std::vector<std::uint32_t> TakeProgress();

struct Dungeon {
    std::uint64_t channel = 0;
    std::array<std::uint32_t, 43> record{};
    std::vector<std::uint32_t> flags;
};
void PutDungeonForm(std::uint64_t channel, const std::vector<std::uint8_t>& form);
void SendDungeon(const Dungeon& d);
bool TakeDungeon(Dungeon& out);
void SendChalices(const std::vector<std::uint32_t>& goods);
bool TakeChalices(std::vector<std::uint32_t>& out);
int HostSlot();
void SendPortrait(const std::vector<std::uint8_t>& packed);
std::vector<std::pair<int, std::vector<std::uint8_t>>> TakePortraits();

bool Active();
bool IsHost();
std::string PlayerName();
void Start(int game_p2p_port);
void SetGamePort(int port);
bool Call(const std::string& path, const nlohmann::json& body, nlohmann::json& reply, std::string& error,
          int timeout_ms);

} // namespace BbRoom
