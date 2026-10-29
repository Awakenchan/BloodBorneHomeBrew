// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LANTERN_SYS_PAD_H
#define LANTERN_SYS_PAD_H
#include <stddef.h>
#include <stdint.h>

enum {
    PAD_L3 = 0x2,
    PAD_R3 = 0x4,
    PAD_OPTIONS = 0x8,
    PAD_UP = 0x10,
    PAD_RIGHT = 0x20,
    PAD_DOWN = 0x40,
    PAD_LEFT = 0x80,
    PAD_L2 = 0x100,
    PAD_R2 = 0x200,
    PAD_L1 = 0x400,
    PAD_R1 = 0x800,
    PAD_TRIANGLE = 0x1000,
    PAD_CIRCLE = 0x2000,
    PAD_CROSS = 0x4000,
    PAD_SQUARE = 0x8000,
    PAD_TOUCHPAD = 0x100000,
};

typedef struct {
    uint16_t x, y;
    uint8_t id, reserved[3];
} PadTouch;

typedef struct {
    uint32_t buttons;
    uint8_t left_x, left_y, right_x, right_y;
    uint8_t l2, r2, analog_padding[2];
    float orientation[4], acceleration[3], angular_velocity[3];
    uint8_t touch_count, touch_padding[3];
    uint32_t touch_held_time;
    PadTouch touches[2];
    uint8_t connected, connected_padding[3];
    uint64_t timestamp;
    uint8_t extension[16];
    uint8_t connected_count, reserved[2], unique_length, unique[12];
} PadData;

_Static_assert(sizeof(PadTouch) == 8, "PadTouch is 8 bytes");
_Static_assert(sizeof(PadData) == 120, "PadData is 120 bytes");
_Static_assert(offsetof(PadData, touches) == 60, "PadData touches at 60");
_Static_assert(offsetof(PadData, timestamp) == 80, "PadData timestamp at 80");

#define PAD_TOUCH_WIDTH 1920
#define PAD_TOUCH_HEIGHT 943

uint64_t pad_now_us(void);
void pad_touch_click(PadData *pad, int right_half);

void pad_host_sample(PadData *pad);
void pad_host_refresh(void);
uint8_t pad_host_connected_count(void);
int pad_host_sdl_state(void);
void pad_host_rumble(uint8_t large, uint8_t small);
const char *pad_host_gamepad_name(void);

void pad_script_record(const PadData *pad);
void pad_script_buttons(PadData *pad);
void pad_script_overrides(PadData *pad);

#endif
