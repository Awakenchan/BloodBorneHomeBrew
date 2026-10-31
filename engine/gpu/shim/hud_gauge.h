// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <vector>

namespace BbHudGauge {
struct Piece {
    int x = 0, y = 0, w = 0, h = 0;
};
struct Art {
    int width = 0, height = 0;
    std::vector<std::uint32_t> pixels;
    Piece frame, health, trail, stamina, portrait, filigree, rust, teal, header;
};

void Start(bool sharp = false);
const Art* Ready();

bool InflateDcx(const std::vector<std::uint8_t>& file, std::vector<std::uint8_t>& out);
void DecodeBc7Block(const std::uint8_t* block, std::uint32_t out[16]);
int TileIndex(int x, int y);
}
