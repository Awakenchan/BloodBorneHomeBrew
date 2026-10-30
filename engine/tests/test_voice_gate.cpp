// SPDX-License-Identifier: GPL-3.0-or-later
#include <chrono>
#include <cstdio>
#include <random>
#include <vector>

#include "gpu/shim/voice_gate.h"

using BbVoice::VoiceGate;

namespace {

int failures = 0;

void Check(bool ok, const char* what) {
    std::printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) ++failures;
}

struct Frame {
    float level;
    bool speech;
};

std::vector<Frame> Talk(float noise, float speech, int phrases, int pause_ms) {
    std::mt19937 random(7);
    std::uniform_real_distribution<float> wobble(0.8f, 1.2f);
    std::vector<Frame> out;
    const auto quiet = [&](int ms) {
        for (int i = 0; i < ms / VoiceGate::kFrameMs; ++i) out.push_back({noise * wobble(random), false});
    };
    quiet(3000);
    for (int p = 0; p < phrases; ++p) {
        for (int syllable = 0; syllable < 6; ++syllable) {
            for (int i = 0; i < 5; ++i) out.push_back({speech * wobble(random), true});
            quiet(120);
        }
        quiet(pause_ms);
    }
    return out;
}

struct Result {
    int speech = 0, lost = 0, sent_quiet = 0, quiet = 0;
};

Result RunOld(const std::vector<Frame>& frames) {
    Result r;
    int since_loud = 1 << 20;
    for (const Frame& f : frames) {
        since_loud = f.level >= 0.02f ? 0 : since_loud + VoiceGate::kFrameMs;
        const bool send = since_loud < 400;
        f.speech ? ++r.speech : ++r.quiet;
        if (f.speech && !send) ++r.lost;
        if (!f.speech && send) ++r.sent_quiet;
    }
    return r;
}

Result RunNew(const std::vector<Frame>& frames) {
    Result r;
    VoiceGate gate;
    std::vector<char> sent(frames.size(), 0);
    for (std::size_t i = 0; i < frames.size(); ++i) {
        if (gate.Step(frames[i].level)) sent[i] = 1;
        if (gate.JustOpened()) {
            for (int back = 1; back <= VoiceGate::kPreRollFrames && back <= static_cast<int>(i); ++back) sent[i - back] = 1;
        }
    }
    for (std::size_t i = 0; i < frames.size(); ++i) {
        frames[i].speech ? ++r.speech : ++r.quiet;
        if (frames[i].speech && !sent[i]) ++r.lost;
        if (!frames[i].speech && sent[i]) ++r.sent_quiet;
    }
    return r;
}

void Report(const char* name, const Result& before, const Result& after) {
    std::printf("%-34s speech lost: before %3d%%, after %3d%%; quiet frames sent: before %3d%%, after %3d%%\n", name,
                before.speech ? before.lost * 100 / before.speech : 0, after.speech ? after.lost * 100 / after.speech : 0,
                before.quiet ? before.sent_quiet * 100 / before.quiet : 0, after.quiet ? after.sent_quiet * 100 / after.quiet : 0);
}

} // namespace

int main() {
    const auto normal = Talk(0.0015f, 0.012f, 6, 1000);
    const Result normal_old = RunOld(normal), normal_new = RunNew(normal);
    Report("normal voice, 1 s pauses", normal_old, normal_new);
    Check(normal_new.lost * 100 / normal_new.speech <= 1, "normal voice after a pause gets through");

    const auto quiet_mic = Talk(0.0008f, 0.006f, 6, 1000);
    const Result quiet_old = RunOld(quiet_mic), quiet_new = RunNew(quiet_mic);
    Report("quiet microphone, 1 s pauses", quiet_old, quiet_new);
    Check(quiet_new.lost * 100 / quiet_new.speech <= 2, "a quiet microphone gets through");

    const auto loud = Talk(0.0015f, 0.05f, 6, 1000);
    const Result loud_old = RunOld(loud), loud_new = RunNew(loud);
    Report("loud voice, 1 s pauses", loud_old, loud_new);
    Check(loud_new.lost == 0, "a loud voice still gets through");

    const auto fan = Talk(0.012f, 0.05f, 4, 3000);
    const Result fan_old = RunOld(fan), fan_new = RunNew(fan);
    Report("loud fan noise, long pauses", fan_old, fan_new);
    Check(fan_new.lost * 100 / fan_new.speech <= 2, "speech over a fan gets through");
    Check(fan_new.sent_quiet * 100 / fan_new.quiet <= 60, "a steady fan does not hold the microphone open");

    VoiceGate silent;
    int open_frames = 0;
    for (int i = 0; i < 1500; ++i) open_frames += silent.Step(i % 2 ? 0.0012f : 0.0018f);
    Check(open_frames == 0, "a quiet room never opens the microphone");

    VoiceGate clicks;
    int click_opens = 0;
    for (int i = 0; i < 1000; ++i) {
        clicks.Step(i % 50 == 49 ? 0.012f : 0.0015f);
        click_opens += clicks.JustOpened();
    }
    Check(click_opens == 0, "single key clicks do not open the microphone");

    if (failures) return 1;
    std::printf("voice-gate-test: ok\n");
    return 0;
}
