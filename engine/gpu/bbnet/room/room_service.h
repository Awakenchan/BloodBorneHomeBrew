// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>

#include <nlohmann/json.hpp>

namespace BbRoom {

bool Active();
bool IsHost();
std::string PlayerName();
void Start(int game_p2p_port);
void SetGamePort(int port);
bool Call(const std::string& path, const nlohmann::json& body, nlohmann::json& reply, std::string& error,
          int timeout_ms);

} // namespace BbRoom
