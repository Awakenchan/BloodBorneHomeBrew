// SPDX-License-Identifier: GPL-3.0-or-later
#define _GNU_SOURCE
#include "runtime.h"
#include "sys/sys.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

typedef struct { int64_t seconds, nanoseconds; } GuestTimespec;
typedef struct { int64_t seconds, microseconds; } GuestTimeval;
typedef struct { int32_t minutes_west, dst_time; } GuestTimezone;
typedef struct { uint32_t bits[4]; } GuestSigset;

void runtime_sleep_stats(uint64_t *calls, uint64_t *ns);

static int failures;
#define EXPECT(condition) do { if (!(condition)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); ++failures; } } while (0)

static uint64_t read_clock(clockid_t id) {
    struct timespec t;
    clock_gettime(id, &t);
    return (uint64_t)t.tv_sec * 1000000000 + (uint64_t)t.tv_nsec;
}
uint64_t host_realtime_ns(void) { return read_clock(CLOCK_REALTIME); }
uint64_t host_monotonic_ns(void) { return read_clock(CLOCK_MONOTONIC); }

static void *fn(const char *name) {
    const SysLibrary *libraries[] = {&sys_clock, &sys_posix_clock, &sys_libc_time, &sys_process};
    for (size_t l = 0; l < 4; ++l)
        for (size_t i = 0; i < libraries[l]->count; ++i)
            if (!strcmp(libraries[l]->exports[i].name, name)) return libraries[l]->exports[i].function;
    printf("FAIL missing %s\n", name);
    ++failures;
    return NULL;
}

int main(void) {
    int32_t (SYS_ABI *kernel_clock)(uint32_t, GuestTimespec *) = fn("sceKernelClockGettime");
    int32_t (SYS_ABI *posix_clock)(uint32_t, GuestTimespec *) = fn("clock_gettime");
    uint64_t (SYS_ABI *process_time)(void) = fn("sceKernelGetProcessTime");
    int32_t (SYS_ABI *usleep_)(uint32_t) = fn("sceKernelUsleep");
    int32_t (SYS_ABI *nanosleep_)(const GuestTimespec *, GuestTimespec *) = fn("nanosleep");
    int32_t (SYS_ABI *timezone_)(GuestTimezone *) = fn("sceKernelGettimezone");
    int32_t (SYS_ABI *gettimeofday_)(GuestTimeval *) = fn("sceKernelGettimeofday");
    int64_t (SYS_ABI *time_)(int64_t *) = fn("time");
    int32_t (SYS_ABI *pagesize)(void) = fn("getpagesize");
    uintptr_t (SYS_ABI *signal_)(int, uintptr_t) = fn("signal");
    int32_t (SYS_ABI *sigprocmask_)(int, const GuestSigset *, GuestSigset *) = fn("sigprocmask");
    int32_t (SYS_ABI *sysctl_)(const int32_t *, uint32_t, void *, uint64_t *, const void *, uint64_t) = fn("sysctl");
    if (failures) return 1;
    EXPECT(sys_resolve("QBi7HCK03hw#p#J") == (uintptr_t)kernel_clock);
    EXPECT(sys_resolve("wLlFkwG9UcQ#q#q") == (uintptr_t)time_);

    GuestTimespec ts = {-1, -1};
    EXPECT(kernel_clock(0, &ts) == 0 && ts.seconds > 1700000000 && ts.nanoseconds >= 0 && ts.nanoseconds < 1000000000);
    EXPECT(kernel_clock(13, &ts) == 0 && ts.nanoseconds == 0);
    EXPECT((uint32_t)kernel_clock(1, &ts) == 0x80020016 && (uint32_t)kernel_clock(4, NULL) == 0x80020016);
    *runtime_errno() = 0;
    EXPECT(posix_clock(3, &ts) == -1 && *runtime_errno() == 22);

    uint64_t before = process_time();
    EXPECT(usleep_(2000) == 0 && process_time() >= before + 1500);
    GuestTimespec request = {0, 1000000}, remaining = {5, 5}, bad = {0, 1000000000};
    *runtime_errno() = 0;
    EXPECT(nanosleep_(&bad, &remaining) == -1 && *runtime_errno() == 22 && remaining.seconds == 5);
    EXPECT(nanosleep_(&request, &remaining) == 0 && remaining.seconds == 0 && remaining.nanoseconds == 0);
    uint64_t calls = 0, slept = 0;
    runtime_sleep_stats(&calls, &slept);
    EXPECT(calls == 2 && slept >= 3000000);

    GuestTimezone zone = {1, 1};
    EXPECT((uint32_t)timezone_(NULL) == 0x80020016 && timezone_(&zone) == 0 && zone.dst_time == 0);
    GuestTimeval tv = {0, -1};
    EXPECT((uint32_t)gettimeofday_(NULL) == 0x80020016 && gettimeofday_(&tv) == 0 && tv.microseconds >= 0);
    int64_t out = 0;
    EXPECT(time_(&out) == out && out >= tv.seconds);

    EXPECT(pagesize() == 16384);
    *runtime_errno() = 0;
    EXPECT(signal_(0, 1) == (uintptr_t)-1 && *runtime_errno() == 22);
    EXPECT(signal_(11, 0x1111) == 0 && signal_(11, 0x2222) == 0x1111);

    GuestSigset block = {{1, 2, 0, 8}}, unblock = {{1, 0x100, 0, 0}}, now;
    EXPECT(sigprocmask_(1, &block, NULL) == 0 && sigprocmask_(2, &unblock, &now) == 0);
    EXPECT(sigprocmask_(0, NULL, &now) == 0 && now.bits[0] == 0 && now.bits[1] == 2 && now.bits[3] == 8);
    *runtime_errno() = 0;
    EXPECT(sigprocmask_(9, &block, NULL) == -1 && *runtime_errno() == 22);

    int32_t page_mib[2] = {6, 7}, random_mib[2] = {1, 37}, unknown_mib[2] = {1, 2}, value = 0;
    uint64_t length = 4, tiny = 2;
    EXPECT(sysctl_(page_mib, 2, &value, &length, NULL, 0) == 0 && value == 16384 && length == 4);
    *runtime_errno() = 0;
    EXPECT(sysctl_(page_mib, 2, &value, &tiny, NULL, 0) == -1 && *runtime_errno() == 12);
    unsigned char random[32] = {0};
    uint64_t random_length = sizeof random;
    int nonzero = 0;
    EXPECT(sysctl_(random_mib, 2, random, &random_length, NULL, 0) == 0);
    for (size_t i = 0; i < sizeof random; ++i) nonzero += random[i] != 0;
    EXPECT(nonzero > 8);
    *runtime_errno() = 0;
    EXPECT(sysctl_(unknown_mib, 2, &value, &length, NULL, 0) == -1 && *runtime_errno() == 2);

    EXPECT(runtime_guest_errno(0) == 0 && runtime_guest_errno(ENOTEMPTY) == 66 && runtime_guest_errno(EAGAIN) == 35);
    EXPECT(runtime_guest_errno(ETIMEDOUT) == 60 && runtime_guest_errno(ENAMETOOLONG) == 63 && runtime_guest_errno(9999) == 5);

    printf("%s\n", failures ? "sys clock: FAILED" : "sys clock: ok");
    return failures != 0;
}
