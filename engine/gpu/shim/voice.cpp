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
constexpr int kFrame = 1920;
constexpr int kBitrate = 16000;
constexpr int kMaxFrameBytes = 255;
constexpr float kVadLevel = 0.02f;
constexpr auto kVadHold = std::chrono::milliseconds(400);
constexpr auto kTalkingFor = std::chrono::milliseconds(300);
constexpr int kPrebufferBytes = kRate * 2 * 60 / 1000;
constexpr int kMostQueuedBytes = kRate * 2 / 5;
constexpr int kMostConcealed = 2;
constexpr std::size_t kLongestName = 32;

struct Remote {
    OpusDecoder* decoder = nullptr;
    SDL_AudioStream* stream = nullptr;
    std::uint16_t last = 0;
    bool started = false;
};

std::atomic<bool> g_started{false};
std::atomic<bool> g_available{false};
std::atomic<bool> g_speaking{false};
std::mutex g_mu;
std::map<std::string, Clock::time_point> g_heard;
std::set<std::string> g_muted;

bool Wanted() {
    static const bool off = [] {
        const char* v = std::getenv("BB_VOICE");
        return v && std::strcmp(v, "0") == 0;
    }();
    return !off && BbSettings::Get().voice_enabled && BbRoom::Active();
}

bool PushToTalkHeld() {
    if (!SDL_GetKeyboardFocus()) return false;
    const int key = BbSettings::Get().voice_talk_key;
    if (key < 0) return (SDL_GetMouseState(nullptr, nullptr) & SDL_BUTTON_MASK(-key)) != 0;
    int count = 0;
    const bool* keys = SDL_GetKeyboardState(&count);
    return keys && key < count && keys[key];
}

float Level(const std::int16_t* pcm, int n) {
    double sum = 0;
    for (int i = 0; i < n; ++i) sum += double(pcm[i]) * double(pcm[i]);
    return float(std::sqrt(sum / n) / 32768.0);
}

void Drop(Remote& r) {
    if (r.stream) SDL_DestroyAudioStream(r.stream);
    if (r.decoder) opus_decoder_destroy(r.decoder);
    r = Remote{};
}

void Play(Remote& r, const std::uint8_t* data, int size, std::uint16_t seq, std::int16_t* pcm) {
    const auto gap = std::uint16_t(seq - r.last);
    if (r.started && (gap == 0 || gap > 0x8000)) return;
    if (SDL_GetAudioStreamQueued(r.stream) > kMostQueuedBytes) SDL_ClearAudioStream(r.stream);
    if (SDL_GetAudioStreamQueued(r.stream) == 0) {
        std::vector<std::int16_t> quiet(kPrebufferBytes / 2, 0);
        SDL_PutAudioStreamData(r.stream, quiet.data(), int(quiet.size() * 2));
    } else if (r.started && gap > 1) {
        for (int i = 0; i < std::min<int>(gap - 1, kMostConcealed); ++i) {
            const int n = opus_decode(r.decoder, nullptr, 0, pcm, kFrame, 0);
            if (n > 0) SDL_PutAudioStreamData(r.stream, pcm, n * 2);
        }
    }
    r.last = seq;
    r.started = true;
    const int n = opus_decode(r.decoder, data, size, pcm, kFrame, 0);
    if (n > 0) SDL_PutAudioStreamData(r.stream, pcm, n * 2);
}

void Loop() {
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        std::printf("Voice: off (no audio: %s)\n", SDL_GetError());
        return;
    }
    const SDL_AudioSpec spec{SDL_AUDIO_S16, 1, kRate};
    SDL_AudioStream* mic = nullptr;
    OpusEncoder* encoder = nullptr;
    std::map<std::string, Remote> remotes;
    std::uint16_t seq = 0;
    Clock::time_point loud{};
    bool told_mic = false;
    std::int16_t pcm[kFrame];
    std::int16_t decoded[kFrame];
    std::uint8_t frame[kMaxFrameBytes];
    const auto close_all = [&] {
        if (mic) SDL_DestroyAudioStream(mic);
        mic = nullptr;
        for (auto& [name, r] : remotes) Drop(r);
        remotes.clear();
        g_speaking = false;
    };
    for (;;) {
        if (!Wanted()) {
            close_all();
            BbRoom::SetVoice(false);
            BbRoom::TakeVoice();
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            continue;
        }
        BbRoom::SetVoice(true);
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
            opus_encoder_ctl(encoder, OPUS_SET_DTX(1));
        }
        if (s.voice_mic_muted) {
            if (mic) SDL_DestroyAudioStream(mic);
            mic = nullptr;
            g_speaking = false;
        } else if (!mic) {
            mic = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_RECORDING, &spec, nullptr, nullptr);
            if (mic) {
                SDL_ResumeAudioStreamDevice(mic);
                if (!told_mic)
                    std::printf("Voice: microphone open (%s%s)\n", s.voice_push_to_talk ? "push to talk, hold " : "voice activated",
                                s.voice_push_to_talk ? KeyName(s.voice_talk_key).c_str() : "");
            } else if (!told_mic) {
                std::printf("Voice: no microphone (%s); listening only\n", SDL_GetError());
            }
            told_mic = true;
        }
        const std::string own = BbRoom::PlayerName().substr(0, kLongestName);
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
            if (!send) continue;
            const int bytes = opus_encode(encoder, pcm, kFrame, frame, sizeof frame);
            ++seq;
            if (bytes <= 2) continue;
            std::vector<std::uint8_t> packet;
            packet.reserve(4 + own.size() + bytes);
            packet.push_back(std::uint8_t(own.size()));
            packet.insert(packet.end(), own.begin(), own.end());
            packet.push_back(std::uint8_t(seq));
            packet.push_back(std::uint8_t(seq >> 8));
            packet.insert(packet.end(), frame, frame + bytes);
            BbRoom::SendVoice(packet);
        }
        for (auto& [sender, data] : BbRoom::TakeVoice()) {
            if (data.empty() || data[0] == 0 || data[0] > kLongestName || data.size() < std::size_t(data[0]) + 4) continue;
            const std::string name(reinterpret_cast<const char*>(data.data() + 1), data[0]);
            const std::size_t at = 1 + data[0];
            const std::uint16_t packet_seq = std::uint16_t(data[at] | data[at + 1] << 8);
            {
                std::lock_guard lock(g_mu);
                if (g_muted.count(name)) continue;
                g_heard[name] = Clock::now();
            }
            Remote& r = remotes[name];
            if (!r.decoder) {
                int error = 0;
                r.decoder = opus_decoder_create(kRate, 1, &error);
            }
            if (!r.stream) {
                r.stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
                if (r.stream) SDL_ResumeAudioStreamDevice(r.stream);
            }
            if (!r.decoder || !r.stream) continue;
            SDL_SetAudioStreamGain(r.stream, s.voice_volume.load());
            Play(r, data.data() + at + 2, int(data.size() - at - 2), packet_seq, decoded);
        }
        {
            std::lock_guard lock(g_mu);
            for (auto it = remotes.begin(); it != remotes.end();) {
                if (g_muted.count(it->first)) {
                    Drop(it->second);
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
        const auto heard = g_heard.find(v.name);
        t.talking = heard != g_heard.end() && now - heard->second < kTalkingFor;
        t.muted = g_muted.count(v.name) != 0;
        out.push_back(std::move(t));
    }
    return out;
}

bool TalkingName(const std::string& name) {
    std::lock_guard lock(g_mu);
    const auto heard = g_heard.find(name);
    return heard != g_heard.end() && Clock::now() - heard->second < kTalkingFor;
}

std::string KeyName(int key) {
    if (key < 0) return "Mouse " + std::to_string(-key);
    const char* name = SDL_GetScancodeName(SDL_Scancode(key));
    return name && *name ? name : "key " + std::to_string(key);
}

void SetMuted(const std::string& name, bool muted) {
    std::lock_guard lock(g_mu);
    if (muted) g_muted.insert(name);
    else g_muted.erase(name);
}
}
