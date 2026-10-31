// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>

namespace BbVoice {

class VoiceGate {
public:
    static constexpr int kFrameMs = 40;
    static constexpr int kPreRollFrames = 3;
    static constexpr float kQuietestSpeech = 0.002f;
    static constexpr float kLoudSpeech = 0.02f;
    static constexpr float kOpenOverNoise = 2.5f;
    static constexpr float kStayOverNoise = 1.6f;
    static constexpr int kFramesToOpen = 2;
    static constexpr int kHoldMs = 1500;

    bool Step(float level) {
        const float noise = std::max(floor_, 0.0005f);
        const float open_at = std::min(std::max(noise * kOpenOverNoise, kQuietestSpeech), kLoudSpeech);
        const float stay_at = std::min(std::max(noise * kStayOverNoise, kQuietestSpeech * 0.75f), kLoudSpeech);
        if (level >= open_at) {
            ++above_;
        } else {
            above_ = 0;
        }
        const bool opening = above_ >= kFramesToOpen || level >= kLoudSpeech;
        just_opened_ = false;
        if (opening && !open_) {
            open_ = true;
            just_opened_ = true;
        }
        if (open_ && (opening || level >= stay_at)) held_ms_ = 0;
        else if (open_) held_ms_ += kFrameMs;
        if (open_ && held_ms_ >= kHoldMs) open_ = false;
        const float rate = level < floor_ ? 0.25f : (above_ ? 0.002f : 0.03f);
        floor_ = started_ ? floor_ + (level - floor_) * rate : level;
        started_ = true;
        return open_;
    }

    bool JustOpened() const { return just_opened_; }
    float NoiseFloor() const { return floor_; }

private:
    float floor_ = 0.0f;
    int above_ = 0;
    int held_ms_ = 0;
    bool open_ = false;
    bool just_opened_ = false;
    bool started_ = false;
};

} // namespace BbVoice
