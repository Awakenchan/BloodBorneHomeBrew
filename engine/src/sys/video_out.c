// SPDX-License-Identifier: GPL-3.0-or-later
#include "video_out.h"
#include "sys.h"
#include "runtime.h"
#include <stdio.h>
#include <string.h>
#include <x86intrin.h>

#define VO_ERROR(code) ((int32_t)(UINT32_C(0x80290000) | (code)))
#define E_VALUE VO_ERROR(0x01)
#define E_ADDRESS VO_ERROR(0x02)
#define E_PITCH VO_ERROR(0x04)
#define E_TILING VO_ERROR(0x07)
#define E_ASPECT VO_ERROR(0x08)
#define E_BUSY VO_ERROR(0x09)
#define E_INDEX VO_ERROR(0x0A)
#define E_BAD_PORT VO_ERROR(0x0B)
#define E_QUEUE VO_ERROR(0x0C)
#define E_NO_SLOT VO_ERROR(0x0F)
#define E_OCCUPIED VO_ERROR(0x10)
#define E_FLIP_FULL VO_ERROR(0x12)

enum { VIDEO_HANDLE = 1, BUFFER_SLOTS = 16, GROUPS = 4, MAX_PENDING = 16, MAX_EVENTS = 8, FLIP_EVENT_ID = 6 };

typedef struct {
    int32_t group;
    uint64_t address;
} Slot;

static HostMutex status_lock = HOST_MUTEX_INIT;
static const SysVideoOutHost *host;
static SysVideoOutGroup groups[GROUPS];
static Slot slots[BUFFER_SLOTS];
static uint64_t labels[BUFFER_SLOTS];
static SysVideoOutFlipStatus flip_status = {.flip_arg = -1, .current_buffer = -1};
static int64_t flip_queues[MAX_EVENTS];
static int flip_queue_count;
static int32_t flip_rate;
static int previous_index = -1;
static int is_open;

static void reset_port(void) {
    memset(labels, 0, sizeof(labels));
    memset(groups, 0, sizeof(groups));
    memset(slots, 0, sizeof(slots));
    for (int i = 0; i < BUFFER_SLOTS; ++i) slots[i].group = -1;
    flip_status = (SysVideoOutFlipStatus){.flip_arg = -1, .current_buffer = -1};
    flip_rate = 0;
    previous_index = -1;
}

void sys_video_out_set_host(const SysVideoOutHost *value) {
    host = value;
    reset_port();
    is_open = 0;
    flip_queue_count = 0;
}

int sys_video_out_is_open(void) {
    return __atomic_load_n(&is_open, __ATOMIC_ACQUIRE);
}

int32_t sys_video_out_flip_rate(void) {
    return __atomic_load_n(&flip_rate, __ATOMIC_RELAXED);
}

const uint64_t *sys_video_out_labels(void) {
    return labels;
}

int sys_video_out_buffer(int32_t index, const SysVideoOutGroup **group, uint64_t *address) {
    if (index < 0 || index >= BUFFER_SLOTS || slots[index].group < 0) return 0;
    *group = &groups[slots[index].group];
    *address = slots[index].address;
    return 1;
}

static SYS_ABI void set_buffer_attribute(SysVideoOutAttribute *attribute, uint32_t format, int32_t tiling, int32_t aspect,
                                         uint32_t width, uint32_t height, uint32_t pitch) {
    memset(attribute, 0, sizeof(*attribute));
    attribute->pixel_format = format;
    attribute->tiling_mode = tiling;
    attribute->aspect_ratio = aspect;
    attribute->width = width;
    attribute->height = height;
    attribute->pitch_in_pixel = pitch;
}

static SYS_ABI int32_t add_flip_event(int64_t queue, int32_t handle, void *udata) {
    if (handle != VIDEO_HANDLE) return E_BAD_PORT;
    if (!sys_equeue_exists(queue)) return E_QUEUE;
    sys_equeue_add(queue, FLIP_EVENT_ID, SYS_EQUEUE_VIDEO_OUT, udata);
    host_lock(&status_lock);
    if (flip_queue_count < MAX_EVENTS) flip_queues[flip_queue_count++] = queue;
    host_unlock(&status_lock);
    return 0;
}

static int32_t check_attribute(const SysVideoOutAttribute *attribute) {
    if (attribute->reserved0 || attribute->reserved1) return E_VALUE;
    if (attribute->aspect_ratio) return E_ASPECT;
    if (attribute->width > attribute->pitch_in_pixel) return E_PITCH;
    if (attribute->tiling_mode < 0 || attribute->tiling_mode > 1) return E_TILING;
    return 0;
}

static SYS_ABI int32_t register_buffers(int32_t handle, int32_t start, void *const *addresses, int32_t count,
                                        const SysVideoOutAttribute *attribute) {
    if (!addresses || !attribute) return E_ADDRESS;
    if (handle != VIDEO_HANDLE || !sys_video_out_is_open()) return E_BAD_PORT;
    int group = 0;
    while (group < GROUPS && groups[group].is_occupied) ++group;
    if (group == GROUPS) return E_NO_SLOT;
    if (start + count > BUFFER_SLOTS || start > BUFFER_SLOTS || count > BUFFER_SLOTS) return E_VALUE;
    for (int32_t i = start; count > 0 && i < start + count; ++i)
        if (slots[i].group != -1) return E_OCCUPIED;
    int32_t bad = check_attribute(attribute);
    if (bad) return bad;
    groups[group].attrib = *attribute;
    groups[group].is_occupied = 1;
    for (int32_t i = 0; i < count; ++i) {
        uint64_t address = (uint64_t)(uintptr_t)addresses[i];
        slots[start + i] = (Slot){group, address};
        __atomic_store_n(&labels[start + i], 0, __ATOMIC_RELEASE);
        if (host) {
            host->label_changed();
            host->surface(&groups[group], address);
        }
    }
    return group;
}

static SYS_ABI int32_t set_flip_rate(int32_t handle, int32_t rate) {
    (void)handle;
    __atomic_store_n(&flip_rate, rate, __ATOMIC_RELAXED);
    return 0;
}

int sys_video_out_queue_flip(int32_t index, int64_t argument, int from_gpu) {
    host_lock(&status_lock);
    if (index != -1 && flip_status.flip_pending_num > MAX_PENDING && !from_gpu) {
        host_unlock(&status_lock);
        return 0;
    }
    if (from_gpu) ++flip_status.gc_queue_num;
    ++flip_status.flip_pending_num;
    flip_status.submit_tsc = __rdtsc();
    host_unlock(&status_lock);
    if (host) host->queue_flip(index, argument, from_gpu);
    return 1;
}

static SYS_ABI int32_t submit_flip(int32_t handle, int32_t index, int32_t mode, int64_t argument) {
    (void)mode;
    if (handle != VIDEO_HANDLE) return E_BAD_PORT;
    if (index < -1 || index >= BUFFER_SLOTS) return E_INDEX;
    if (index != -1 && slots[index].group < 0) return E_INDEX;
    return sys_video_out_queue_flip(index, argument, 0) ? 0 : E_FLIP_FULL;
}

void sys_video_out_flip_done(int32_t index, int64_t argument, int from_gpu) {
    if (previous_index != -1) {
        __atomic_store_n(&labels[previous_index], 0, __ATOMIC_RELEASE);
        if (host) host->label_changed();
    }
    previous_index = index;
    int64_t queues[MAX_EVENTS];
    host_lock(&status_lock);
    ++flip_status.count;
    flip_status.process_time = runtime_process_time_us();
    flip_status.tsc = __rdtsc();
    flip_status.flip_arg = argument;
    flip_status.current_buffer = index;
    if (from_gpu) --flip_status.gc_queue_num;
    --flip_status.flip_pending_num;
    int queue_count = flip_queue_count;
    memcpy(queues, flip_queues, (size_t)queue_count * sizeof(*queues));
    host_unlock(&status_lock);
    for (int i = 0; i < queue_count; ++i)
        sys_equeue_trigger(queues[i], FLIP_EVENT_ID, SYS_EQUEUE_VIDEO_OUT, (uint64_t)FLIP_EVENT_ID | ((uint64_t)argument << 16));
}

static SYS_ABI int32_t get_flip_status(int32_t handle, SysVideoOutFlipStatus *status) {
    if (!status) return E_ADDRESS;
    if (handle != VIDEO_HANDLE) return E_BAD_PORT;
    host_lock(&status_lock);
    *status = flip_status;
    host_unlock(&status_lock);
    return 0;
}

static SYS_ABI int32_t video_out_open(int32_t user, int32_t bus, int32_t index, const void *params) {
    (void)user;
    (void)params;
    if (bus != 0) return E_VALUE;
    if (index != 0) return E_VALUE;
    if (sys_video_out_is_open()) return E_BUSY;
    __atomic_store_n(&is_open, 1, __ATOMIC_RELEASE);
    if (host) host->opened();
    return VIDEO_HANDLE;
}

static SYS_ABI int32_t video_out_close(int32_t handle) {
    (void)handle;
    host_lock(&status_lock);
    __atomic_store_n(&is_open, 0, __ATOMIC_RELEASE);
    reset_port();
    int queue_count = flip_queue_count;
    int64_t queues[MAX_EVENTS];
    memcpy(queues, flip_queues, (size_t)queue_count * sizeof(*queues));
    flip_queue_count = 0;
    host_unlock(&status_lock);
    for (int i = 0; i < queue_count; ++i) sys_equeue_remove(queues[i], FLIP_EVENT_ID, SYS_EQUEUE_VIDEO_OUT);
    return 0;
}

static const SysExport video_out_exports[] = {
    {"Up36PTk687E", "sceVideoOutOpen", video_out_open},
    {"uquVH4-Du78", "sceVideoOutClose", video_out_close},
    {"HXzjK9yI30k", "sceVideoOutAddFlipEvent", add_flip_event},
    {"SbU3dwp80lQ", "sceVideoOutGetFlipStatus", get_flip_status},
    {"w3BY+tAEiQY", "sceVideoOutRegisterBuffers", register_buffers},
    {"i6-sR91Wt-4", "sceVideoOutSetBufferAttribute", set_buffer_attribute},
    {"CBiu4mCE1DA", "sceVideoOutSetFlipRate", set_flip_rate},
    {"U46NwOiJpys", "sceVideoOutSubmitFlip", submit_flip},
};
SYS_LIBRARY(sys_video_out, "libSceVideoOut", video_out_exports);
