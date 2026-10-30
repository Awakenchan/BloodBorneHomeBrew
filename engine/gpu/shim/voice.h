// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <string>
#include <vector>

namespace BbVoice {
struct Talker {
    int slot = 0;
    std::string name;
    bool talking = false;
    bool muted = false;
};

void Start();
bool Available();
bool Speaking();
std::vector<Talker> Talkers();
bool TalkingName(const std::string& name);
void SetMuted(int slot, bool muted);
}
