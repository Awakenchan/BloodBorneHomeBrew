// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace BbFaceShot {
constexpr int kSize = 128;

void Tick(bool preview_shown);
bool Pending();
bool Due(float rect[4]);
void Deliver(const std::uint8_t* pixels, std::uint32_t width, std::uint32_t height, std::uint32_t stride, bool bgra);

bool JustTaken();
std::uint32_t OwnVersion();
bool OwnRgba(std::vector<std::uint32_t>& rgba, std::uint32_t& version);
bool OwnPacked(std::vector<std::uint8_t>& out, std::uint32_t& version);
void Receive(const std::string& name, const std::vector<std::uint8_t>& packed);
bool Get(const std::string& name, std::vector<std::uint32_t>& rgba, std::uint32_t& version);
}
