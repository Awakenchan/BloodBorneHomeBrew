// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LANTERN_SYS_AV_PLAYER_H
#define LANTERN_SYS_AV_PLAYER_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define SYS_AV_GUEST __attribute__((sysv_abi))

enum { SYS_AV_VIDEO = 0, SYS_AV_AUDIO = 1, SYS_AV_TEXT = 2, SYS_AV_UNKNOWN = 3 };
enum { SYS_AV_FRAME = 1, SYS_AV_MORE = 0, SYS_AV_END = -1, SYS_AV_ERROR = -2 };

typedef struct {
    uint32_t type;
    uint8_t reserved[4];
    union {
        uint8_t bytes[16];
        struct {
            uint16_t channel_count;
            uint8_t reserved[2];
            uint32_t sample_rate, size;
            char language[4];
        } audio;
        struct {
            uint32_t width, height;
            float aspect_ratio;
            char language[4];
        } video;
        struct {
            char language[4];
            uint16_t text_size, font_size;
            uint16_t top, left, bottom, right;
        } text;
    } details;
    uint64_t duration, start_time;
} SysAvStreamInfo;

typedef struct {
    uint16_t channel_count;
    uint32_t sample_rate, bytes;
    uint32_t width, height, pitch;
    float aspect_ratio;
    uint32_t crop_left, crop_right, crop_top, crop_bottom;
    uint64_t timestamp;
} SysAvFrame;

typedef struct {
    void *object;
    int32_t (SYS_AV_GUEST *open)(void *object, const char *name);
    int32_t (SYS_AV_GUEST *close)(void *object);
    int32_t (SYS_AV_GUEST *read_offset)(void *object, uint8_t *buffer, uint64_t position, uint32_t length);
    uint64_t (SYS_AV_GUEST *size)(void *object);
} SysAvFileCalls;

typedef struct SysAvMedia SysAvMedia;

typedef struct {
    SysAvMedia *(*open)(const char *guest_path, const SysAvFileCalls *file);
    void (*close)(SysAvMedia *media);
    int (*find_streams)(SysAvMedia *media);
    int (*stream_count)(SysAvMedia *media);
    int (*stream_info)(SysAvMedia *media, uint32_t index, SysAvStreamInfo *info);
    int (*enable_stream)(SysAvMedia *media, uint32_t index);
    int (*begin)(SysAvMedia *media, uint32_t *video_bytes);
    void (*end)(SysAvMedia *media);
    int (*read_packet)(SysAvMedia *media, void **packet, int *kind);
    int (*rewind)(SysAvMedia *media);
    void (*free_packet)(void *packet);
    int (*send_packet)(SysAvMedia *media, int kind, void *packet);
    int (*receive_frame)(SysAvMedia *media, int kind, void *buffer, uint32_t size, SysAvFrame *frame);
    uint64_t (*duration)(SysAvMedia *media);
} SysAvDecoder;

void sys_av_player_set_decoder(const SysAvDecoder *decoder);

#ifdef __cplusplus
}
#endif
#endif
