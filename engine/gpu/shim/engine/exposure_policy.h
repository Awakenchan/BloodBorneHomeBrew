// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace BbEngine::Exposure {
enum class Status { Off, Waiting, Calibrating, Active, Invalid, Unsupported };

struct NativeResponse {
    float sensitivity, speed;
};

inline NativeResponse Response(double elapsed, float native_dt) {
    // Preserve the approved 30 FPS preview's wall-clock pace without changing
    // YEBIS's shared timestep or its log-space, velocity-smoothed controller.
    return {float(-std::expm1(std::log(0.5) * elapsed / (2.0 * native_dt))),
            float(0.25 * elapsed / native_dt)};
}

class Policy {
public:
    struct Input {
        double now;
        std::uint64_t resource;
        std::uint32_t area;
        int index;
        float baseline, luminance, adapted, native_dt;
        bool valid_sample;
    };
    struct Result {
        Status status = Status::Calibrating;
        float exposure = 1, gray = 0.18f, minimum = 1, maximum = 1;
        NativeResponse response{};
        bool reset_output = false;
    };

    void Reset() { *this = {}; }

    Result Update(const Input& in, bool recalibrate = false) {
        Result out;
        out.exposure = out.minimum = out.maximum = in.baseline;
        if (!std::isfinite(in.now) || !std::isfinite(in.baseline) || in.baseline <= 0 ||
            !std::isfinite(in.native_dt) || in.native_dt <= 0 || in.native_dt > 0.1f ||
            !in.resource || in.index < 0 || in.index >= 17) {
            Reset();
            out.status = Status::Invalid;
            return out;
        }
        double elapsed = in.now - previous_time;
        if (!initialized || recalibrate || resource != in.resource || area != in.area ||
            std::abs(in.baseline - baseline) > baseline * 0.001f ||
            elapsed <= 0 || elapsed > 0.5 || in.now - last_sample_time > 1.5) {
            Reset();
            initialized = true;
            resource = in.resource;
            area = in.area;
            baseline = in.baseline;
            index = in.index;
            last_sample_time = in.now;
            elapsed = 1.0 / 30;
            out.reset_output = true;
        }
        previous_time = in.now;
        out.response = Response(std::min(elapsed, 0.1), in.native_dt);
        const bool fresh = index != in.index;
        if (fresh) {
            index = in.index;
            last_sample_time = in.now;
            if (!in.valid_sample || !std::isfinite(in.luminance) || in.luminance <= 0) {
                Reset();
                out.status = Status::Invalid;
                out.reset_output = true;
                return out;
            }
            if (warmup < 17) {
                ++warmup;
            } else if (reference == 0) {
                const double log_luminance = std::log2(in.luminance);
                if (!samples || log_luminance < lowest - 0.15 || log_luminance > highest + 0.15 ||
                    std::max(highest, log_luminance) - std::min(lowest, log_luminance) > 0.15) {
                    samples = 0;
                    window_start = in.now;
                    lowest = highest = log_luminance;
                    log_sum = 0;
                }
                lowest = std::min(lowest, log_luminance);
                highest = std::max(highest, log_luminance);
                log_sum += log_luminance;
                ++samples;
                if (samples >= 3 && in.now - window_start >= 1.0) {
                    reference = std::exp2(log_sum / samples);
                    out.reset_output = true;
                }
            }
        }
        if (reference == 0) return out;
        const double gray = -std::expm1(-reference * in.baseline);
        if (!(gray > 0 && gray < 1) || !std::isfinite(in.adapted) || in.adapted <= 0) {
            Reset();
            out.status = Status::Invalid;
            out.reset_output = true;
            return out;
        }
        out.gray = float(gray);
        if (!(out.gray > 0 && out.gray < 1)) {
            Reset();
            out.status = Status::Invalid;
            return out;
        }
        out.status = Status::Active;
        out.minimum = in.baseline * 0.5f;
        out.maximum = in.baseline * std::sqrt(2.0f);
        out.exposure = out.reset_output ? in.baseline :
            std::clamp(in.adapted, out.minimum, out.maximum);
        return out;
    }

private:
    bool initialized = false;
    std::uint64_t resource = 0;
    std::uint32_t area = 0;
    float baseline = 0;
    int index = -1, warmup = 0, samples = 0;
    double previous_time = 0, last_sample_time = 0, window_start = 0;
    double lowest = 0, highest = 0, log_sum = 0, reference = 0;
};
}
