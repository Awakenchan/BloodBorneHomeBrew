// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace BbEngine::OnlineFixes {
struct PartyRow {
    std::string name;
    float health = 0, stamina = 0;
    float distance = -1, bearing = 0;
    bool down = false;
    bool dev = false;
};

bool Install(std::uint8_t* image, std::size_t size);
void Tick();
std::vector<PartyRow> PartyRows();
void SetCameraYaw(float yaw);
void StartSummon();
void StopSummon();
void LeaveHost();
bool Summoning();
int PartyRole();
void AnswerLampAsk(bool yes);
std::string LampPlace(std::uint32_t lamp);
bool Spectating();
void SpectateNext();
bool SpectatedName(std::string& name, int& choices);
}
