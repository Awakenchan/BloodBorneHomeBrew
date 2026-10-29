// SPDX-License-Identifier: GPL-3.0-or-later
#include "pad.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

typedef struct {
    uint32_t ms, buttons;
    uint8_t axes[4], l2, r2;
} PadSample;

enum { REPLAY_OFF, REPLAY_RUNNING, REPLAY_DONE };

static struct {
    uint32_t buttons;
    int stick[4];
    int touch_side;
} injected = {0, {-1, -1, -1, -1}, -1};

static int replay_state;
static uint64_t replay_started;
static PadSample *replay;
static size_t replay_count, replay_next;

static FILE *recording;
static uint64_t recording_started;
static PadSample last_recorded;

static const char *env_once(const char *name, const char **cache, int *read) {
    if (!*read) {
        *cache = getenv(name);
        *read = 1;
    }
    return *cache;
}

static void read_pad_file(void) {
    static const char *path;
    static int path_read;
    static uint64_t last_look;
    static long long seen_time, seen_stamp;
    env_once("BB_PAD_FILE", &path, &path_read);
    if (!path || !*path) return;
    uint64_t now = pad_now_us();
    if (now - last_look < 20000) return;
    last_look = now;
    struct stat info;
    if (stat(path, &info)) return;
#ifdef _WIN32
    long long stamp = (long long)info.st_size;
#else
    long long stamp = (long long)info.st_mtim.tv_nsec;
#endif
    if ((long long)info.st_mtime == seen_time && stamp == seen_stamp) return;
    seen_time = (long long)info.st_mtime;
    seen_stamp = stamp;
    FILE *file = fopen(path, "r");
    if (!file) return;
    static const struct { const char *name; uint32_t button; } words[] = {
        {"cross", PAD_CROSS}, {"circle", PAD_CIRCLE}, {"square", PAD_SQUARE}, {"triangle", PAD_TRIANGLE},
        {"l1", PAD_L1}, {"r1", PAD_R1}, {"l2", PAD_L2}, {"r2", PAD_R2}, {"l3", PAD_L3}, {"r3", PAD_R3},
        {"options", PAD_OPTIONS}, {"touchpad", PAD_TOUCHPAD},
        {"up", PAD_UP}, {"down", PAD_DOWN}, {"left", PAD_LEFT}, {"right", PAD_RIGHT},
    };
    static const char stick_words[4][4] = {"lx=", "ly=", "rx=", "ry="};
    injected.buttons = 0;
    injected.touch_side = -1;
    for (int i = 0; i < 4; ++i) injected.stick[i] = -1;
    char word[64];
    while (fscanf(file, "%63s", word) == 1) {
        if (!strcmp(word, "replay") && replay_state != REPLAY_RUNNING) {
            replay_state = REPLAY_RUNNING;
            replay_started = 0;
        }
        if (!strcmp(word, "touchpad_left") || !strcmp(word, "touchpad_right")) {
            injected.buttons |= PAD_TOUCHPAD;
            injected.touch_side = !strcmp(word, "touchpad_right");
        }
        for (size_t i = 0; i < sizeof(words) / sizeof(*words); ++i)
            if (!strcmp(word, words[i].name)) injected.buttons |= words[i].button;
        for (int i = 0; i < 4; ++i)
            if (!strncmp(word, stick_words[i], 3)) {
                int value = atoi(word + 3);
                injected.stick[i] = value < 0 ? 0 : value > 255 ? 255 : value;
            }
    }
    fclose(file);
    printf("Runtime: pad file: buttons 0x%x sticks %d %d %d %d\n", injected.buttons, injected.stick[0],
           injected.stick[1], injected.stick[2], injected.stick[3]);
}

void pad_script_record(const PadData *pad) {
    static const char *path;
    static int path_read, f9_held;
    env_once("BB_PAD_RECORD", &path, &path_read);
    if (!path || !*path || !pad_host_sdl_state()) return;
    const bool *keys = SDL_GetKeyboardState(NULL);
    int f9 = keys && keys[SDL_SCANCODE_F9];
    if (f9 && !f9_held) {
        if (recording) {
            fclose(recording);
            recording = NULL;
            printf("Runtime: pad recording stopped (%s)\n", path);
        } else if ((recording = fopen(path, "w"))) {
            recording_started = pad_now_us();
            memset(&last_recorded, 0xFF, sizeof(last_recorded));
            printf("Runtime: pad recording started (%s, F9 stops)\n", path);
        }
    }
    f9_held = f9;
    if (!recording) return;
    PadSample s = {(uint32_t)((pad_now_us() - recording_started) / 1000), pad->buttons,
                   {pad->left_x, pad->left_y, pad->right_x, pad->right_y}, pad->l2, pad->r2};
    if (s.buttons == last_recorded.buttons && !memcmp(s.axes, last_recorded.axes, 4) && s.l2 == last_recorded.l2 &&
        s.r2 == last_recorded.r2)
        return;
    last_recorded = s;
    fprintf(recording, "%u %u %u %u %u %u %u %u\n", s.ms, s.buttons, s.axes[0], s.axes[1], s.axes[2], s.axes[3],
            s.l2, s.r2);
    fflush(recording);
}

static void load_replay(void) {
    const char *path = getenv("BB_PAD_REPLAY");
    FILE *file = path ? fopen(path, "r") : NULL;
    size_t capacity = 0;
    unsigned v[8];
    while (file && fscanf(file, "%u %u %u %u %u %u %u %u", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]) == 8) {
        if (replay_count == capacity) {
            capacity = capacity ? capacity * 2 : 1024;
            PadSample *grown = realloc(replay, capacity * sizeof(*replay));
            if (!grown) break;
            replay = grown;
        }
        replay[replay_count++] = (PadSample){v[0], v[1], {(uint8_t)v[2], (uint8_t)v[3], (uint8_t)v[4], (uint8_t)v[5]},
                                             (uint8_t)v[6], (uint8_t)v[7]};
    }
    if (file) fclose(file);
    printf("Runtime: pad replay of %zu samples from %s\n", replay_count, path ? path : "(unset)");
}

static void play_replay(PadData *pad) {
    if (replay_state == REPLAY_OFF) return;
    if (!replay_started) {
        static int loaded;
        if (!loaded) {
            loaded = 1;
            load_replay();
        }
        replay_started = pad_now_us();
        replay_next = 0;
    }
    uint32_t ms = (uint32_t)((pad_now_us() - replay_started) / 1000);
    while (replay_next < replay_count && replay[replay_next].ms <= ms) ++replay_next;
    if (!replay_next) return;
    if (replay_next == replay_count && ms > replay[replay_count - 1].ms + 500) {
        if (replay_state == REPLAY_RUNNING) {
            puts("Runtime: pad replay finished");
            replay_state = REPLAY_DONE;
        }
        return;
    }
    const PadSample *s = &replay[replay_next - 1];
    pad->buttons = s->buttons;
    pad->left_x = s->axes[0];
    pad->left_y = s->axes[1];
    pad->right_x = s->axes[2];
    pad->right_y = s->axes[3];
    pad->l2 = s->l2;
    pad->r2 = s->r2;
}

void pad_script_buttons(PadData *pad) {
    read_pad_file();
    play_replay(pad);
    pad->buttons |= injected.buttons;
}

void pad_script_overrides(PadData *pad) {
    if (injected.touch_side >= 0) pad_touch_click(pad, injected.touch_side);
    else if ((pad->buttons & PAD_TOUCHPAD) && !pad->touch_count) pad_touch_click(pad, 0);
    if (injected.buttons & PAD_L2) pad->l2 = 255;
    if (injected.buttons & PAD_R2) pad->r2 = 255;
    uint8_t *axes[4] = {&pad->left_x, &pad->left_y, &pad->right_x, &pad->right_y};
    for (int i = 0; i < 4; ++i)
        if (injected.stick[i] >= 0) *axes[i] = (uint8_t)injected.stick[i];
}
