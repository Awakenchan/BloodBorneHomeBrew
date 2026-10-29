// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace BbRoom {

struct Vitals {
    int slot = 0;
    std::string name;
    std::int32_t hp = 0, hp_max = 0, stamina = 0, stamina_max = 0;
    std::uint16_t revives = 0;
    float pos[3] = {};
    std::int64_t age_ms = 0;
};

void SendVitals(const Vitals& own);
std::vector<Vitals> Party(int max_age_ms);

bool Active();
bool IsHost();
std::string PlayerName();
void Start(int game_p2p_port);
void SetGamePort(int port);
bool Call(const std::string& path, const nlohmann::json& body, nlohmann::json& reply, std::string& error,
          int timeout_ms);

} // namespace BbRoom
