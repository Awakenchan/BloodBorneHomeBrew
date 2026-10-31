// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "exposure_policy.h"
#include <cstddef>
#include <cstdint>

namespace BbEngine::Exposure {
bool Install(std::uint8_t* image, std::size_t size);
void Recalibrate();
Status CurrentStatus();
float CurrentStops();
}
