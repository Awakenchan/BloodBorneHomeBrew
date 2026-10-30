// SPDX-License-Identifier: GPL-3.0-or-later
#include "sys.h"
#include "runtime.h"
#include <stdio.h>

enum {
    PLAYGO_ERROR_HANDLE = (int32_t)0x80B20009,
    PLAYGO_ERROR_POINTER = (int32_t)0x80B2000A,
    PLAYGO_ERROR_SIZE = (int32_t)0x80B2000B,
    PLAYGO_ERROR_CHUNK = (int32_t)0x80B2000C,
};

#define LOCUS_LOCAL_FAST 3
#define THE_HANDLE 1

static int32_t open_handle;
static uint32_t chunk_count;

static SYS_ABI int32_t playgo_initialize(const void *params) {
    (void)params;
    chunk_count = 1;
    int fd = (int)sys_file_open("/app0/sce_sys/playgo-chunk.dat", 0, 0);
    if (fd >= 0) {
        unsigned char header[12];
        if (sys_file_read(fd, header, sizeof(header)) == (int64_t)sizeof(header))
            chunk_count = (uint32_t)header[10] | (uint32_t)header[11] << 8;
        sys_file_close(fd);
    }
    printf("Runtime: PlayGo initialized; %d chunks, all installed locally\n", (int)chunk_count);
    return 0;
}

static SYS_ABI int32_t playgo_open(int32_t *handle, const void *param) {
    (void)param;
    if (!handle) return PLAYGO_ERROR_POINTER;
    open_handle = THE_HANDLE;
    *handle = THE_HANDLE;
    return 0;
}

static int valid(int32_t handle) {
    return handle && handle == open_handle;
}

static SYS_ABI int32_t playgo_get_chunk_id(int32_t handle, uint16_t *ids, uint32_t capacity, uint32_t *count) {
    if (!valid(handle)) return PLAYGO_ERROR_HANDLE;
    if (!count) return PLAYGO_ERROR_POINTER;
    if (!ids) {
        *count = chunk_count;
        return 0;
    }
    if (!capacity) return PLAYGO_ERROR_SIZE;
    uint32_t n = capacity < chunk_count ? capacity : chunk_count;
    for (uint32_t i = 0; i < n; ++i) ids[i] = (uint16_t)i;
    *count = n;
    return 0;
}

static SYS_ABI int32_t playgo_get_locus(int32_t handle, const uint16_t *ids, uint32_t count, int8_t *loci) {
    if (!valid(handle)) return PLAYGO_ERROR_HANDLE;
    if (!ids || !loci) return PLAYGO_ERROR_POINTER;
    if (!count) return PLAYGO_ERROR_SIZE;
    for (uint32_t i = 0; i < count; ++i) {
        if (ids[i] >= chunk_count) return PLAYGO_ERROR_CHUNK;
        loci[i] = LOCUS_LOCAL_FAST;
    }
    return 0;
}

static SYS_ABI int32_t playgo_set_install_speed(int32_t handle, int32_t speed) {
    (void)speed;
    return valid(handle) ? 0 : PLAYGO_ERROR_HANDLE;
}

static const SysExport exports[] = {
    {"73fF1MFU8hA", "scePlayGoGetChunkId", playgo_get_chunk_id},
    {"uWIYLFkkwqk", "scePlayGoGetLocus", playgo_get_locus},
    {"ts6GlZOKRrE", "scePlayGoInitialize", playgo_initialize},
    {"M1Gma1ocrGE", "scePlayGoOpen", playgo_open},
    {"4AAcTU9R3XM", "scePlayGoSetInstallSpeed", playgo_set_install_speed},
};

SYS_LIBRARY(sys_play_go, "libScePlayGo", exports);
