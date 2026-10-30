// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LANTERN_SYS_VIDEO_OUT_H
#define LANTERN_SYS_VIDEO_OUT_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#else
#include <stdbool.h>
#endif

#define SYS_EQUEUE_VIDEO_OUT (-13)
#define SYS_EQUEUE_GRAPHICS_CORE (-14)

typedef struct {
    uint32_t pixel_format;
    int32_t tiling_mode;
    int32_t aspect_ratio;
    uint32_t width, height, pitch_in_pixel, option, reserved0;
    uint64_t reserved1;
} SysVideoOutAttribute;

typedef struct {
    bool is_occupied;
    SysVideoOutAttribute attrib;
} SysVideoOutGroup;

typedef struct {
    uint64_t count, process_time, tsc;
    int64_t flip_arg;
    uint64_t submit_tsc, reserved0;
    int32_t gc_queue_num, flip_pending_num, current_buffer;
    uint32_t reserved1;
} SysVideoOutFlipStatus;

typedef struct {
    void (*opened)(void);
    void (*surface)(const SysVideoOutGroup *group, uint64_t address);
    void (*queue_flip)(int32_t index, int64_t argument, int from_gpu);
    void (*label_changed)(void);
} SysVideoOutHost;

void sys_video_out_set_host(const SysVideoOutHost *host);
int sys_video_out_is_open(void);
int32_t sys_video_out_flip_rate(void);
const uint64_t *sys_video_out_labels(void);
int sys_video_out_buffer(int32_t index, const SysVideoOutGroup **group, uint64_t *address);
int sys_video_out_queue_flip(int32_t index, int64_t argument, int from_gpu);
void sys_video_out_flip_done(int32_t index, int64_t argument, int from_gpu);

int sys_equeue_exists(int64_t handle);
int sys_equeue_add(int64_t handle, uint64_t ident, int16_t filter, void *udata);
int sys_equeue_remove(int64_t handle, uint64_t ident, int16_t filter);
int sys_equeue_trigger(int64_t handle, uint64_t ident, int16_t filter, uint64_t value);
uint64_t sys_equeue_event_data(const void *event);
void sys_equeue_report(double frames);

#ifdef __cplusplus
}
#endif
#endif
