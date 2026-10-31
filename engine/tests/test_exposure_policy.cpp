// SPDX-License-Identifier: GPL-3.0-or-later
#include "gpu/shim/engine/exposure_policy.h"
#include <cassert>
#include <limits>

using namespace BbEngine::Exposure;

int main() {
    Policy policy;
    Policy::Input in{1, 0x10000, 30, 0, 3, 0.016f, 3, 1.0f / 60, true};
    auto step = [&](double elapsed = 1.0 / 30, bool fresh = true) {
        in.now += elapsed;
        if (fresh) in.index = (in.index + 1) % 17;
        return policy.Update(in);
    };
    auto ready = [&] {
        Policy::Result result;
        for (int i = 0; i < 100; ++i) result = step(0.1);
        assert(result.status == Status::Active);
        assert(std::abs(result.gray - -std::expm1(-0.016f * 3)) < 1e-6);
        return result;
    };
    assert(policy.Update(in).status == Status::Calibrating);
    for (int i = 0; i < 17; ++i) assert(step().status == Status::Calibrating);
    ready();
    in.adapted = 0.01f;
    auto low = step();
    assert(std::abs(std::log2(low.exposure / 3) + 1.0f) < 1e-6);
    in.adapted = 1.84f;
    assert(step().exposure == 1.84f);
    in.adapted = 100;
    auto high = step();
    assert(std::abs(std::log2(high.exposure / 3) - 0.5f) < 1e-6);
    in.luminance *= 8; // A sky-facing pan must not relearn the neutral reference.
    assert(step().gray == high.gray);
    in.luminance = 0.016f;
    in.adapted = 3;
    assert(policy.Update(in, true).status == Status::Calibrating);
    ready();
    ++in.area;
    assert(step().status == Status::Calibrating);
    ready();
    ++in.resource;
    assert(step().status == Status::Calibrating);
    ready();
    in.baseline = 2.5;
    auto reset = step();
    assert(reset.status == Status::Calibrating && reset.exposure == 2.5);
    in.baseline = in.adapted = 3;
    ready();
    assert(step(1).status == Status::Calibrating);
    ready();
    for (int i = 0; i < 60; ++i) reset = step(1.0 / 30, false);
    assert(reset.status == Status::Calibrating); // No fresh meter result: don't use stale exposure.
    ready();
    in.valid_sample = false;
    assert(step().status == Status::Invalid);
    in.valid_sample = true;
    ready();
    in.adapted = std::numeric_limits<float>::infinity();
    assert(step().status == Status::Invalid);
    in.adapted = 3;
    in.native_dt = 0;
    assert(step().status == Status::Invalid);
    in.native_dt = 1.0f / 60;
    in.baseline = std::numeric_limits<float>::quiet_NaN();
    assert(step().status == Status::Invalid);

    for (int fps : {30, 60, 120, 144}) {
        const auto response = Response(1.0 / fps, 1.0f / 60);
        const double alpha = 1 - std::pow(1 - response.sensitivity, (1.0 / 60) / 0.3);
        const double residual_after_second = std::pow(1 - alpha, fps);
        assert(std::abs(residual_after_second - std::pow(0.5, 1 / 0.6)) < 1e-6);
        assert(std::abs(response.speed * (1.0 / 60) * fps - 0.25) < 1e-6);
    }
}
