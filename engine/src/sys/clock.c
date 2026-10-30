// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "sys.h"
#include "runtime.h"
#include <errno.h>
#include <stdatomic.h>
#include <time.h>
#include <x86intrin.h>

#define KERNEL_ERROR(code) ((int32_t)(UINT32_C(0x80020000) | (code)))

typedef struct {
    int64_t seconds, nanoseconds;
} GuestTimespec;

typedef struct {
    int64_t seconds, microseconds;
} GuestTimeval;

typedef struct {
    int32_t minutes_west, dst_time;
} GuestTimezone;

#ifdef _WIN32
typedef enum { REALTIME, MONOTONIC, THREAD_CPU, PROCESS_CPU } HostClock;
#define HOST_CLOCK(linux_id, windows_id) windows_id
#else
typedef clockid_t HostClock;
#define HOST_CLOCK(linux_id, windows_id) linux_id
#endif

static uint64_t process_start, tsc_hz;
static _Atomic uint64_t sleep_calls, sleep_total_ns;

__attribute__((constructor)) static void remember_start(void) {
    process_start = host_monotonic_ns();
}

static int32_t fail_posix(int e) {
    *runtime_errno() = runtime_guest_errno(e);
    return -1;
}

static int host_clock(uint32_t id, HostClock *out) {
    switch (id) {
    case 0: case 9: case 10:
        *out = HOST_CLOCK(CLOCK_REALTIME, REALTIME);
        return 1;
    case 4: case 5: case 7: case 8: case 11: case 12: case 16: case 17: case 18: case 19:
        *out = HOST_CLOCK(CLOCK_MONOTONIC, MONOTONIC);
        return 1;
    case 13:
        *out = HOST_CLOCK(CLOCK_REALTIME_COARSE, REALTIME);
        return 1;
    case 14:
        *out = HOST_CLOCK(CLOCK_THREAD_CPUTIME_ID, THREAD_CPU);
        return 1;
    case 2: case 15:
        *out = HOST_CLOCK(CLOCK_PROCESS_CPUTIME_ID, PROCESS_CPU);
        return 1;
    default:
        return 0;
    }
}

static int clock_read(uint32_t id, GuestTimespec *ts) {
    HostClock host;
    if (!ts || !host_clock(id, &host)) return EINVAL;
#ifdef _WIN32
    uint64_t ns;
    if (host == REALTIME) {
        ns = host_realtime_ns();
    } else if (host == MONOTONIC) {
        ns = host_monotonic_ns();
    } else {
        int64_t user, system;
        runtime_cpu_times(host == THREAD_CPU, &user, &system);
        ns = (uint64_t)(user + system) * 1000;
    }
    ts->seconds = (int64_t)(ns / 1000000000);
    ts->nanoseconds = id == 13 ? 0 : (int64_t)(ns % 1000000000);
#else
    struct timespec t;
    if (clock_gettime(host, &t)) return errno;
    ts->seconds = t.tv_sec;
    ts->nanoseconds = id == 13 ? 0 : t.tv_nsec;
#endif
    return 0;
}

static SYS_ABI int32_t kernel_clock_gettime(uint32_t id, GuestTimespec *ts) {
    int e = clock_read(id, ts);
    return e ? KERNEL_ERROR(runtime_guest_errno(e)) : 0;
}

static SYS_ABI int32_t posix_clock_gettime(uint32_t id, GuestTimespec *ts) {
    int e = clock_read(id, ts);
    return e ? fail_posix(e) : 0;
}

static SYS_ABI uint64_t process_time(void) {
    return (host_monotonic_ns() - process_start) / 1000;
}

static SYS_ABI uint64_t read_tsc(void) {
    return __rdtsc();
}

uint64_t runtime_process_time_us(void) {
    return process_time();
}

uint64_t runtime_process_time_counter(void) {
    return host_monotonic_ns() - process_start;
}

uint64_t runtime_tsc_frequency(void) {
    if (!tsc_hz) {
        uint64_t started = host_monotonic_ns(), t0 = __rdtsc();
        host_sleep_ns(20000000);
        uint64_t ns = host_monotonic_ns() - started, t1 = __rdtsc();
        tsc_hz = (t1 - t0) * 1000000000 / (ns ? ns : 1);
    }
    return tsc_hz;
}

void runtime_sleep_stats(uint64_t *calls, uint64_t *ns) {
    *calls = atomic_exchange(&sleep_calls, 0);
    *ns = atomic_exchange(&sleep_total_ns, 0);
    sys_file_stream_report();
}

static void sleep_ns(uint64_t ns) {
    uint64_t start = host_monotonic_ns();
    host_sleep_ns(ns);
    uint64_t elapsed = host_monotonic_ns() - start;
    atomic_fetch_add(&sleep_calls, 1);
    atomic_fetch_add(&sleep_total_ns, elapsed);
    runtime_wait_note(3, elapsed);
}

static SYS_ABI int32_t kernel_usleep(uint32_t usec) {
    sleep_ns((uint64_t)usec * 1000);
    return 0;
}

static SYS_ABI uint32_t posix_sleep(uint32_t seconds) {
    sleep_ns((uint64_t)seconds * 1000000000);
    return 0;
}

static SYS_ABI int32_t posix_nanosleep(const GuestTimespec *request, GuestTimespec *remaining) {
    if (!request || request->nanoseconds < 0 || request->nanoseconds >= 1000000000 || request->seconds < 0)
        return fail_posix(EINVAL);
    sleep_ns((uint64_t)request->seconds * 1000000000 + (uint64_t)request->nanoseconds);
    if (remaining) {
        remaining->seconds = 0;
        remaining->nanoseconds = 0;
    }
    return 0;
}

static SYS_ABI int32_t kernel_gettimezone(GuestTimezone *zone) {
    if (!zone) return KERNEL_ERROR(22);
#ifdef _WIN32
    zone->minutes_west = (int32_t)(-runtime_utc_offset(time(NULL)) / 60);
#else
    time_t now = time(NULL);
    struct tm local;
    localtime_r(&now, &local);
    zone->minutes_west = (int32_t)(-local.tm_gmtoff / 60);
#endif
    zone->dst_time = 0;
    return 0;
}

static SYS_ABI int32_t kernel_gettimeofday(GuestTimeval *now) {
    if (!now) return KERNEL_ERROR(22);
    uint64_t ns = host_realtime_ns();
    now->seconds = (int64_t)(ns / 1000000000);
    now->microseconds = (int64_t)(ns % 1000000000 / 1000);
    return 0;
}

static SYS_ABI int64_t libc_time(int64_t *out) {
    int64_t now = (int64_t)time(NULL);
    if (out) *out = now;
    return now;
}

static const SysExport kernel_exports[] = {
    {"QBi7HCK03hw", "sceKernelClockGettime", kernel_clock_gettime},
    {"lLMT9vJAck0", "clock_gettime", posix_clock_gettime},
    {"4J2sUJmuHZQ", "sceKernelGetProcessTime", process_time},
    {"-2IRUCO--PM", "sceKernelReadTsc", read_tsc},
    {"1jfXLRVzisc", "sceKernelUsleep", kernel_usleep},
    {"yS8U2TGCe1A", "nanosleep", posix_nanosleep},
    {"kOcnerypnQA", "sceKernelGettimezone", kernel_gettimezone},
    {"ejekcaNQNq0", "sceKernelGettimeofday", kernel_gettimeofday},
};

static const SysExport posix_exports[] = {
    {"QcteRwbsnV0", "usleep", kernel_usleep},
    {"0wu33hunNdE", "sleep", posix_sleep},
};

static const SysExport libc_exports[] = {
    {"wLlFkwG9UcQ", "time", libc_time},
};

SYS_LIBRARY(sys_clock, "libkernel", kernel_exports);
SYS_LIBRARY(sys_posix_clock, "libScePosix", posix_exports);
SYS_LIBRARY(sys_libc_time, "libSceLibcInternal", libc_exports);
