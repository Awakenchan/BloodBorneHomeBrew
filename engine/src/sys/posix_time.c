// SPDX-License-Identifier: GPL-3.0-or-later
#include "sys.h"
#include "host_sync.h"
#include <time.h>

typedef struct {
    int64_t seconds, microseconds;
} GuestTimeval;

typedef struct {
    int32_t minutes_west, dst_time;
} GuestTimezone;

static int32_t minutes_west(time_t when) {
    struct tm local;
#ifdef _WIN32
    if (localtime_s(&local, &when)) return 0;
    return (int32_t)(-(int64_t)(_mkgmtime(&local) - when) / 60);
#else
    if (!localtime_r(&when, &local)) return 0;
    return (int32_t)(-local.tm_gmtoff / 60);
#endif
}

static SYS_ABI int32_t posix_gettimeofday(GuestTimeval *now, GuestTimezone *zone) {
    uint64_t ns = host_realtime_ns();
    if (now) {
        now->seconds = (int64_t)(ns / 1000000000);
        now->microseconds = (int64_t)(ns % 1000000000 / 1000);
    }
    if (zone) {
        zone->minutes_west = minutes_west((time_t)(ns / 1000000000));
        zone->dst_time = 0;
    }
    return 0;
}

static const SysExport exports[] = {
    {"n88vx3C5nW8", "gettimeofday", posix_gettimeofday},
};

SYS_LIBRARY(sys_posix_time, "libScePosix", exports);
