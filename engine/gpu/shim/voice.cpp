// SPDX-License-Identifier: GPL-3.0-or-later
#include "voice.h"
#include "bbport_settings.h"
#include "../bbnet/room/room_service.h"
#include <SDL3/SDL.h>
#include <opus.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <thread>

namespace BbVoice {
namespace {
using Clock = std::chrono::steady_clock;

constexpr int kRate = 48000;
constexpr int kFrame = 960;
constexpr int kFramesPerPacket = 2;
constexpr int kBitrate = 16000;
constexpr int kMaxFrameBytes = 200;
constexpr float kVadLevel = 0.02f;
constexpr auto kVadHold = std::chrono::milliseconds(400);
constexpr auto kTalkingFor = std::chrono::milliseconds(300);
constexpr int kMostQueuedBytes = kRate * 2 / 5;

struct Remote {
    OpusDecoder* decoder = nullptr;
    SDL_AudioStream* stream = nullptr;
    Clock::time_point heard{};
};

std::atomic<bool> g_started{false};
std::atomic<bool> g_available{false};
std::atomic<bool> g_speaking{false};
std::mutex g_mu;
std::map<int, Clock::time_point> g_heard;
std::set<int> g_muted;

bool Wanted() {
    static const bool off = [] {
        const char* v = std::getenv("BB_VOICE");
        return v && std::strcmp(v, "0") == 0;
    }();
    return !off && BbSettings::Get().voice_enabled && BbRoom::Active();
}

bool PushToTalkHeld() {
    if (!SDL_GetKeyboardFocus()) return false;
    int count = 0;
    const bool* keys = SDL_GetKeyboardState(&count);
    return keys && SDL_SCANCODE_V < count && keys[SDL_SCANCODE_V];
}

float Level(const std::int16_t* pcm, int n) {
    double sum = 0;
    for (int i = 0; i < n; ++i) sum += double(pcm[i]) * double(pcm[i]);
    return float(std::sqrt(sum / n) / 32768.0);
}

void Loop() {
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        std::printf("Voice: off (no audio: %s)\n", SDL_GetError());
        return;
    }
    const SDL_AudioSpec spec{SDL_AUDIO_S16, 1, kRate};
    SDL_AudioStream* mic = nullptr;
    OpusEncoder* encoder = nullptr;
    std::map<int, Remote> remotes;
    std::vector<std::uint8_t> packet;
    int packed = 0;
    std::uint16_t seq = 0;
    Clock::time_point loud{};
    bool told_mic = false;
    std::int16_t pcm[kFrame];
    std::int16_t decoded[kFrame * 3];
    std::uint8_t frame[kMaxFrameBytes];
    const auto close_all = [&] {
        if (mic) SDL_DestroyAudioStream(mic);
        mic = nullptr;
        for (auto& [slot, r] : remotes) {
            if (r.stream) SDL_DestroyAudioStream(r.stream);
            if (r.decoder) opus_decoder_destroy(r.decoder);
        }
        remotes.clear();
        packet.clear();
        packed = 0;
        g_speaking = false;
    };
    const auto flush = [&] {
        if (!packed) return;
        packet[2] = std::uint8_t(packed);
        BbRoom::SendVoice(packet);
        packet.clear();
        packed = 0;
    };
    for (;;) {
        if (!Wanted()) {
            close_all();
            BbRoom::TakeVoice();
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            continue;
        }
        g_available = true;
        const auto& s = BbSettings::Get();
        if (!encoder) {
            int error = 0;
            encoder = opus_encoder_create(kRate, 1, OPUS_APPLICATION_VOIP, &error);
            if (!encoder) {
                std::printf("Voice: off (Opus encoder error %d)\n", error);
                g_available = false;
                return;
            }
            opus_encoder_ctl(encoder, OPUS_SET_BITRATE(kBitrate));
            opus_encoder_ctl(encoder, OPUS_SET_COMPLEXITY(5));
            opus_encoder_ctl(encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
        }
        if (s.voice_mic_muted) {
            if (mic) SDL_DestroyAudioStream(mic);
            mic = nullptr;
            flush();
            g_speaking = false;
        } else if (!mic) {
            mic = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_RECORDING, &spec, nullptr, nullptr);
            if (mic) {
                SDL_ResumeAudioStreamDevice(mic);
                if (!told_mic) std::printf("Voice: microphone open (%s)\n", s.voice_push_to_talk ? "push to talk, hold V" : "voice activated");
            } else if (!told_mic) {
                std::printf("Voice: no microphone (%s); listening only\n", SDL_GetError());
            }
            told_mic = true;
        }
        while (mic && SDL_GetAudioStreamAvailable(mic) >= int(sizeof pcm)) {
            SDL_GetAudioStreamData(mic, pcm, sizeof pcm);
            const auto now = Clock::now();
            bool send = false;
            if (s.voice_push_to_talk) {
                send = PushToTalkHeld();
            } else {
                if (Level(pcm, kFrame) >= kVadLevel) loud = now;
                send = now - loud < kVadHold;
            }
            g_speaking = send;
            if (!send) {
                flush();
                continue;
            }
            const int bytes = opus_encode(encoder, pcm, kFrame, frame, sizeof frame);
            if (bytes <= 0 || bytes > 255) continue;
            if (packet.empty()) {
                packet = {std::uint8_t(seq), std::uint8_t(seq >> 8), 0};
                ++seq;
            }
            packet.push_back(std::uint8_t(bytes));
            packet.insert(packet.end(), frame, frame + bytes);
            if (++packed == kFramesPerPacket) flush();
        }
        for (auto& [slot, data] : BbRoom::TakeVoice()) {
            {
                std::lock_guard lock(g_mu);
                if (g_muted.count(slot)) continue;
                g_heard[slot] = Clock::now();
            }
            Remote& r = remotes[slot];
            if (!r.decoder) {
                int error = 0;
                r.decoder = opus_decoder_create(kRate, 1, &error);
            }
            if (!r.stream) {
                r.stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
                if (r.stream) SDL_ResumeAudioStreamDevice(r.stream);
            }
            if (!r.decoder || !r.stream || data.size() < 3) continue;
            if (SDL_GetAudioStreamQueued(r.stream) > kMostQueuedBytes) SDL_ClearAudioStream(r.stream);
            const int frames = data[2];
            std::size_t at = 3;
            for (int f = 0; f < frames && at < data.size(); ++f) {
                const std::size_t n = data[at++];
                if (n == 0 || at + n > data.size()) break;
                const int samples = opus_decode(r.decoder, data.data() + at, opus_int32(n), decoded, kFrame * 3, 0);
                at += n;
                if (samples > 0) SDL_PutAudioStreamData(r.stream, decoded, samples * 2);
            }
        }
        {
            std::lock_guard lock(g_mu);
            for (auto it = remotes.begin(); it != remotes.end();) {
                if (g_muted.count(it->first)) {
                    if (it->second.stream) SDL_DestroyAudioStream(it->second.stream);
                    if (it->second.decoder) opus_decoder_destroy(it->second.decoder);
                    it = remotes.erase(it);
                } else {
                    ++it;
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}
}

void Start() {
    if (g_started.exchange(true)) return;
    std::thread(Loop).detach();
}

bool Available() {
    return g_available.load();
}

bool Speaking() {
    return g_speaking.load();
}

std::vector<Talker> Talkers() {
    std::vector<Talker> out;
    const auto now = Clock::now();
    std::lock_guard lock(g_mu);
    for (const BbRoom::Vitals& v : BbRoom::Party(60000)) {
        Talker t;
        t.slot = v.slot;
        t.name = v.name;
        const auto heard = g_heard.find(v.slot);
        t.talking = heard != g_heard.end() && now - heard->second < kTalkingFor;
        t.muted = g_muted.count(v.slot) != 0;
        out.push_back(std::move(t));
    }
    return out;
}

bool TalkingName(const std::string& name) {
    for (const Talker& t : Talkers())
        if (t.name == name) return t.talking;
    return false;
}

void SetMuted(int slot, bool muted) {
    std::lock_guard lock(g_mu);
    if (muted) g_muted.insert(slot);
    else g_muted.erase(slot);
}
}
