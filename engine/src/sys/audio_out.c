// SPDX-License-Identifier: GPL-3.0-or-later
#include "sys.h"
#include "gpu/bbgpu.h"
#include "host_sync.h"
#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    AUDIO_ERROR_NOT_OPENED = (int32_t)0x80260001,
    AUDIO_ERROR_PORT = (int32_t)0x80260003,
    AUDIO_ERROR_POINTER = (int32_t)0x80260004,
    AUDIO_ERROR_PORT_FULL = (int32_t)0x80260005,
    AUDIO_ERROR_SIZE = (int32_t)0x80260006,
    AUDIO_ERROR_FORMAT = (int32_t)0x80260007,
    AUDIO_ERROR_FREQUENCY = (int32_t)0x80260008,
    AUDIO_ERROR_VOLUME = (int32_t)0x80260009,
    AUDIO_ERROR_TYPE = (int32_t)0x8026000A,
    AUDIO_ERROR_ALREADY_INITIALIZED = (int32_t)0x8026000E,
    AUDIO_ERROR_NOT_INITIALIZED = (int32_t)0x8026000F,
};

#define PORT_COUNT 25
#define RATE 48000
#define MAX_FRAMES 2048
#define MAX_CHANNELS 8
#define UNITY_VOLUME 32768
#define HANDLE_TAG 0x20000000
#define HANDLE_TAG_MASK 0x3F000000
#define STATS_PERIOD_NS UINT64_C(5000000000)

typedef struct {
    int open, type, channels, device_channels, is_float, frames, sample_bytes, standard_layout;
    int32_t volume[MAX_CHANNELS];
    float fold_gain;
    SDL_AudioStream *stream;
    FILE *dump;
    HostMutex lock;
    uint64_t next_deadline;
    int64_t correction;
    int lowest_queued, window;
    unsigned resets;
    int stats;
    struct {
        uint64_t start, last, longest_gap, bytes;
        unsigned buffers, starved;
        int lowest_queued, queued_at_start;
    } stat;
} Port;

typedef struct {
    uint16_t output;
    uint8_t channel, reserved;
    int16_t volume;
    uint16_t reroute;
    uint64_t flag, reserved2[2];
} PortState;
_Static_assert(sizeof(PortState) == 32, "PortState is 32 bytes");

static HostMutex table_lock = HOST_MUTEX_INIT;
static Port ports[PORT_COUNT];
static int initialized, backend = -1;
static size_t buffers_written, ports_opened;
static Port *watched_port;

static struct {
    uint64_t last, longest_gap, frames;
    unsigned calls;
    int rate, seen;
} device_thread;

static int open_backend(void) {
    if (backend < 0) {
        const char *mode = getenv("BB_AUDIO");
        backend = (!mode || strcmp(mode, "none")) && SDL_InitSubSystem(SDL_INIT_AUDIO);
        printf("Runtime: audio backend %s\n", backend ? SDL_GetCurrentAudioDriver() : "timer (silent)");
    }
    return backend;
}

static int raise_device_priority(void) {
    const char *setting = getenv("BB_AUDIO_THREAD_PRIORITY");
    return !(setting && setting[0] == '0');
}

static void SDLCALL device_wrote(void *user, const SDL_AudioSpec *spec, float *buffer, int length) {
    (void)user;
    (void)buffer;
    uint64_t now = host_monotonic_ns(), last = __atomic_load_n(&device_thread.last, __ATOMIC_RELAXED);
    if (last && now - last > __atomic_load_n(&device_thread.longest_gap, __ATOMIC_RELAXED))
        __atomic_store_n(&device_thread.longest_gap, now - last, __ATOMIC_RELAXED);
    __atomic_store_n(&device_thread.last, now, __ATOMIC_RELAXED);
    __atomic_add_fetch(&device_thread.calls, 1u, __ATOMIC_RELAXED);
    __atomic_add_fetch(&device_thread.frames, (uint64_t)length / ((uint64_t)spec->channels * sizeof(float)), __ATOMIC_RELAXED);
    if (!device_thread.seen) {
        device_thread.seen = 1;
#ifdef _WIN32
        int before = GetThreadPriority(GetCurrentThread());
        int raise = before < THREAD_PRIORITY_HIGHEST && raise_device_priority();
        if (raise) SDL_SetCurrentThreadPriority(SDL_THREAD_PRIORITY_TIME_CRITICAL);
        printf("Runtime: audio device thread priority %d%s\n", before, raise ? ", raised to time-critical" : "");
#endif
    }
}

static void watch_device(Port *port) {
    if (watched_port) return;
    watched_port = port;
    SDL_AudioDeviceID device = SDL_GetAudioStreamDevice(port->stream);
    SDL_AudioSpec spec;
    int frames = 0;
    if (SDL_GetAudioDeviceFormat(device, &spec, &frames)) {
        device_thread.rate = spec.freq;
        printf("Runtime: audio output %s: %d Hz, %d channels, %d frames per write (%.1f ms; the game: 48000 Hz, converted)\n",
               SDL_GetCurrentAudioDriver(), spec.freq, spec.channels, frames, spec.freq ? 1000.0 * frames / spec.freq : 0.0);
    }
    if ((port->stats || raise_device_priority()) && !SDL_SetAudioPostmixCallback(device, device_wrote, NULL))
        fprintf(stderr, "Runtime: audio postmix callback failed (%s)\n", SDL_GetError());
}

static int ports_for_type(int type, int *first, int *last) {
    static const struct { int type, first, last; } ranges[] = {
        {0, 0, 7}, {1, 8, 8}, {2, 9, 12}, {3, 13, 16}, {4, 17, 20}, {126, 21, 22}, {127, 23, 24},
    };
    for (size_t i = 0; i < sizeof(ranges) / sizeof(*ranges); ++i)
        if (ranges[i].type == type) {
            *first = ranges[i].first;
            *last = ranges[i].last;
            return 1;
        }
    return 0;
}

static Port *find_port(int32_t handle, int32_t *error) {
    host_lock(&table_lock);
    int index = handle & 0xFF;
    Port *port = NULL;
    if ((handle & HANDLE_TAG_MASK) != HANDLE_TAG || index >= PORT_COUNT) *error = AUDIO_ERROR_PORT;
    else if (!ports[index].open) *error = AUDIO_ERROR_NOT_OPENED;
    else port = &ports[index];
    host_unlock(&table_lock);
    return port;
}

static SYS_ABI int32_t audio_init(void) {
    host_lock(&table_lock);
    int32_t result = initialized ? AUDIO_ERROR_ALREADY_INITIALIZED : 0;
    initialized = 1;
    host_unlock(&table_lock);
    return result;
}

static void open_stream(Port *port, int index) {
    if (!open_backend()) return;
    SDL_AudioSpec device;
    int device_frames = 0;
    const char *sdl_downmix = getenv("BB_AUDIO_SDL_DOWNMIX");
    if (port->channels == 8 && !(sdl_downmix && sdl_downmix[0] == '1') &&
        SDL_GetAudioDeviceFormat(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &device, &device_frames) && device.channels == 2)
        port->device_channels = 2;
    SDL_AudioSpec spec = {port->is_float ? SDL_AUDIO_F32 : SDL_AUDIO_S16, port->device_channels, RATE};
    port->stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
    if (port->stream) SDL_ResumeAudioStreamDevice(port->stream);
    else fprintf(stderr, "Runtime: SDL audio stream failed (%s); port %d uses the timer sink\n", SDL_GetError(), index);
}

static SYS_ABI int32_t audio_open(int32_t user, int32_t type, int32_t index, uint32_t frames, uint32_t rate,
                                  uint32_t param) {
    (void)user;
    (void)index;
    static const int format_channels[8] = {1, 2, 8, 1, 2, 8, 8, 8};
    if (!initialized) return AUDIO_ERROR_NOT_INITIALIZED;
    if (!frames || frames > MAX_FRAMES || (frames & 0xFF)) return AUDIO_ERROR_SIZE;
    if (rate != RATE) return AUDIO_ERROR_FREQUENCY;
    uint32_t format = param & 0xFF;
    if (format > 7) return AUDIO_ERROR_FORMAT;
    int first, last;
    if (!ports_for_type(type, &first, &last)) return AUDIO_ERROR_TYPE;
    host_lock(&table_lock);
    int id = -1;
    for (int i = first; i <= last && id < 0; ++i)
        if (!ports[i].open) id = i;
    if (id < 0) {
        host_unlock(&table_lock);
        return AUDIO_ERROR_PORT_FULL;
    }
    Port *port = &ports[id];
    memset(port, 0, sizeof(*port));
    host_mutex_init(&port->lock);
    port->open = 1;
    port->type = type;
    port->channels = format_channels[format];
    port->is_float = format >= 3 && format != 6;
    port->sample_bytes = port->is_float ? 4 : 2;
    port->frames = (int)frames;
    port->standard_layout = format >= 6;
    for (int c = 0; c < MAX_CHANNELS; ++c) port->volume[c] = UNITY_VOLUME;
    port->device_channels = port->channels;
    open_stream(port, id);
    port->stats = getenv("BB_AUDIO_STATS") != NULL;
    if (port->stream) watch_device(port);
    const char *dump = getenv("BB_AUDIO_DUMP");
    if (dump) {
        char path[4096];
        snprintf(path, sizeof(path), "%s.port%d.%dch.%s", dump, id, port->device_channels, port->is_float ? "f32" : "s16");
        port->dump = fopen(path, "wb");
    }
    ++ports_opened;
    host_unlock(&table_lock);
    printf("Runtime: audio port %d opened (type %d, %d ch, %s, %u frames%s)\n", id, type, port->channels,
           port->is_float ? "float" : "s16", frames, port->device_channels != port->channels ? ", folded to stereo" : "");
    return (type << 16) | id | HANDLE_TAG;
}

static SYS_ABI int32_t audio_close(int32_t handle) {
    host_lock(&table_lock);
    int index = handle & 0xFF;
    int32_t result = 0;
    if ((handle & HANDLE_TAG_MASK) != HANDLE_TAG || index >= PORT_COUNT) result = AUDIO_ERROR_PORT;
    else if (!ports[index].open) result = AUDIO_ERROR_NOT_OPENED;
    else {
        Port *port = &ports[index];
        host_lock(&port->lock);
        if (port->stream) SDL_DestroyAudioStream(port->stream);
        if (port->dump) fclose(port->dump);
        port->dump = NULL;
        port->stream = NULL;
        port->open = 0;
        host_unlock(&port->lock);
    }
    host_unlock(&table_lock);
    return result;
}

static void apply_volume(const Port *port, const void *in, unsigned char *out) {
    static const int wide_to_standard[8] = {0, 1, 2, 3, 6, 7, 4, 5};
    int remap = port->channels == 8 && !port->standard_layout;
    for (size_t f = 0; f < (size_t)port->frames; ++f)
        for (int c = 0; c < port->channels; ++c) {
            size_t from = f * (size_t)port->channels + (size_t)c;
            size_t to = f * (size_t)port->channels + (size_t)(remap ? wide_to_standard[c] : c);
            float gain = (float)port->volume[c] / UNITY_VOLUME;
            if (port->is_float) {
                float v;
                memcpy(&v, (const char *)in + from * 4, 4);
                v *= gain;
                memcpy(out + to * 4, &v, 4);
            } else {
                int16_t v;
                memcpy(&v, (const char *)in + from * 2, 2);
                v = (int16_t)((float)v * gain);
                memcpy(out + to * 2, &v, 2);
            }
        }
}

static float read_sample(const Port *port, const unsigned char *buffer, size_t index) {
    if (port->is_float) {
        float v;
        memcpy(&v, buffer + index * 4, 4);
        return v;
    }
    int16_t v;
    memcpy(&v, buffer + index * 2, 2);
    return (float)v;
}

static void write_sample(const Port *port, unsigned char *buffer, size_t index, float value) {
    if (port->is_float) {
        float v = value > 1.0f ? 1.0f : value < -1.0f ? -1.0f : value;
        memcpy(buffer + index * 4, &v, 4);
    } else {
        float v = value > 32767.0f ? 32767.0f : value < -32768.0f ? -32768.0f : value;
        int16_t s = (int16_t)v;
        memcpy(buffer + index * 2, &s, 2);
    }
}

static void fold_to_stereo(Port *port, unsigned char *buffer) {
    const float k = 0.70710678f, full_scale = port->is_float ? 1.0f : 32767.0f, ceiling = 0.97f * full_scale;
    const float release = 1.0f / (0.05f * 48000.0f);
    float gain = port->fold_gain > 0.0f ? port->fold_gain : 1.0f;
    for (size_t f = 0; f < (size_t)port->frames; ++f) {
        float s[8];
        for (int c = 0; c < 8; ++c) s[c] = read_sample(port, buffer, f * 8 + (size_t)c);
        float left = s[0] + k * (s[2] + s[4] + s[6]), right = s[1] + k * (s[2] + s[5] + s[7]);
        float peak = fmaxf(fabsf(left), fabsf(right));
        float target = peak > ceiling ? ceiling / peak : 1.0f;
        gain = target < gain ? target : gain + (target - gain) * release;
        write_sample(port, buffer, f * 2, left * gain);
        write_sample(port, buffer, f * 2 + 1, right * gain);
    }
    port->fold_gain = gain;
}

static void report_stats(Port *port, uint64_t now) {
    int queued_now = port->stream ? SDL_GetAudioStreamQueued(port->stream) : 0;
    double taken = (double)port->stat.bytes - (double)(queued_now - port->stat.queued_at_start);
    double nominal = (double)(now - port->stat.start) / 1e9 * 48000.0 * port->device_channels * port->sample_bytes;
    printf("Audio stats port %d: %u buffers/5s (expected %u), max gap %.2f ms, min queued %d bytes, starved %u, "
           "device took %.1f%% of 48 kHz, queue resets %u\n",
           (int)(port - ports), port->stat.buffers, (unsigned)(5u * 48000u / (unsigned)port->frames),
           port->stat.longest_gap / 1e6, port->stat.lowest_queued, port->stat.starved,
           port->stream ? 100.0 * taken / nominal : 0.0, port->resets);
    if (port == watched_port && device_thread.rate) {
        unsigned calls = __atomic_exchange_n(&device_thread.calls, 0u, __ATOMIC_RELAXED);
        uint64_t frames = __atomic_exchange_n(&device_thread.frames, (uint64_t)0, __ATOMIC_RELAXED);
        uint64_t gap = __atomic_exchange_n(&device_thread.longest_gap, (uint64_t)0, __ATOMIC_RELAXED);
        printf("Audio device thread: %u writes/5s, %.1f%% of %d Hz, max gap %.2f ms\n", calls,
               100.0 * (double)frames / ((double)(now - port->stat.start) / 1e9 * device_thread.rate), device_thread.rate,
               gap / 1e6);
    }
    port->stat.start = now;
    port->stat.longest_gap = 0;
    port->stat.starved = 0;
    port->stat.buffers = 0;
    port->stat.lowest_queued = 1 << 30;
    port->stat.queued_at_start = queued_now;
    port->stat.bytes = 0;
}

static void count_stats(Port *port, size_t bytes) {
    uint64_t now = host_monotonic_ns();
    if (!port->stat.start) {
        port->stat.start = now;
        port->stat.lowest_queued = 1 << 30;
        port->stat.queued_at_start = port->stream ? SDL_GetAudioStreamQueued(port->stream) : 0;
        port->stat.bytes = 0;
    } else if (now - port->stat.last > port->stat.longest_gap) {
        port->stat.longest_gap = now - port->stat.last;
    }
    port->stat.last = now;
    ++port->stat.buffers;
    port->stat.bytes += bytes;
    if (port->stream) {
        int queued = SDL_GetAudioStreamQueued(port->stream);
        if (queued < port->stat.lowest_queued) port->stat.lowest_queued = queued;
        if (!queued) ++port->stat.starved;
    }
    if (now - port->stat.start >= STATS_PERIOD_NS) report_stats(port, now);
}

static void wait_for_slot(Port *port, uint64_t period) {
    uint64_t now = host_monotonic_ns();
    if (!port->next_deadline || now > port->next_deadline + 8 * period) port->next_deadline = now;
    else host_sleep_until_ns(port->next_deadline);
    port->next_deadline += period;
}

static int muted_in_background(void) {
    static int setting = -1;
    if (setting < 0) {
        const char *mute = getenv("LANTERN_MUTE_UNFOCUSED");
        setting = mute && mute[0] == '1';
    }
    return setting && bbgpu_window_in_background();
}

static void feed_device(Port *port, const unsigned char *buffer, int bytes, uint64_t period) {
    static const unsigned char silence[MAX_FRAMES * MAX_CHANNELS * 4];
    int floor = 2 * bytes, queued = SDL_GetAudioStreamQueued(port->stream);
    if (queued < bytes) {
        SDL_PutAudioStreamData(port->stream, silence, floor - queued < bytes ? floor - queued : bytes);
        queued = floor;
    }
    if (queued > floor + 32 * bytes) {
        SDL_ClearAudioStream(port->stream);
        SDL_PutAudioStreamData(port->stream, silence, bytes);
        SDL_PutAudioStreamData(port->stream, silence, bytes);
        queued = floor;
        port->window = 0;
        if (++port->resets <= 4)
            printf("Runtime: audio port %d: the device took far less than it was given; queue dropped back to %d bytes\n",
                   (int)(port - ports), floor);
    }
    if (!port->window || queued < port->lowest_queued) port->lowest_queued = queued;
    if (++port->window == 32) {
        int excess = port->lowest_queued - (floor + 2 * bytes);
        int64_t correction = excess > 0                  ? (int64_t)period * excess / (32 * (int64_t)bytes)
                             : port->lowest_queued < floor ? -(int64_t)(period / 32)
                                                           : 0;
        if (correction > (int64_t)(period / 4)) correction = (int64_t)(period / 4);
        port->correction = correction;
        port->window = 0;
    }
    port->next_deadline += (uint64_t)port->correction;
    SDL_PutAudioStreamData(port->stream, muted_in_background() ? silence : buffer, bytes);
}

static SYS_ABI int32_t audio_output(int32_t handle, const void *samples) {
    int32_t error = 0;
    Port *port = find_port(handle, &error);
    if (!port) return error;
    host_lock(&port->lock);
    size_t count = (size_t)port->frames * (size_t)port->device_channels;
    size_t bytes = count * (size_t)port->sample_bytes;
    uint64_t period = (uint64_t)port->frames * 1000000000u / RATE;
    if (samples) {
        unsigned char buffer[MAX_FRAMES * MAX_CHANNELS * 4];
        apply_volume(port, samples, buffer);
        if (port->device_channels == 2 && port->channels == 8) fold_to_stereo(port, buffer);
        if (port->dump) fwrite(buffer, 1, bytes, port->dump);
        if (port->stats) count_stats(port, bytes);
        wait_for_slot(port, period);
        if (port->stream) feed_device(port, buffer, (int)bytes, period);
        ++buffers_written;
    }
    host_unlock(&port->lock);
    return samples ? (int32_t)count : 0;
}

static SYS_ABI int32_t audio_set_volume(int32_t handle, int32_t channels, const int32_t *volume) {
    int32_t error = 0;
    Port *port = find_port(handle, &error);
    if (!port) return error;
    if (!volume) return AUDIO_ERROR_POINTER;
    host_lock(&port->lock);
    int32_t result = 0;
    for (int c = 0; c < MAX_CHANNELS && !result; ++c) {
        if (!(channels & (1 << c))) continue;
        if (volume[c] < 0 || volume[c] > UNITY_VOLUME) result = AUDIO_ERROR_VOLUME;
        else port->volume[c] = volume[c];
    }
    host_unlock(&port->lock);
    return result;
}

static SYS_ABI int32_t audio_get_port_state(int32_t handle, PortState *state) {
    int32_t error = 0;
    Port *port = find_port(handle, &error);
    if (!port) return error;
    if (!state) return AUDIO_ERROR_POINTER;
    memset(state, 0, sizeof(*state));
    if (port->type == 2 || port->type == 3) {
        state->output = 0x40;
        state->channel = 1;
    } else if (port->type == 4) {
        state->output = 0x04;
        state->channel = 1;
        state->volume = 127;
    } else {
        state->output = 0x01;
        state->channel = (uint8_t)(port->channels > 2 ? 2 : port->channels);
    }
    return 0;
}

void sys_audio_out_report(void) {
    printf("Runtime: audio ports opened=%zu, buffers output=%zu\n", ports_opened, buffers_written);
}

static const SysExport exports[] = {
    {"s1--uE9mBFw", "sceAudioOutClose", audio_close},
    {"GrQ9s4IrNaQ", "sceAudioOutGetPortState", audio_get_port_state},
    {"JfEPXVxhFqA", "sceAudioOutInit", audio_init},
    {"ekNvsT22rsY", "sceAudioOutOpen", audio_open},
    {"QOQtbeDqsT4", "sceAudioOutOutput", audio_output},
    {"b+uAV89IlxE", "sceAudioOutSetVolume", audio_set_volume},
};

SYS_LIBRARY(sys_audio_out, "libSceAudioOut", exports);
