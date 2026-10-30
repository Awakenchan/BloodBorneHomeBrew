// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <chrono>
#include <ctime>
#include "common/types.h"

namespace Libraries::Kernel {

struct OrbisKernelTimeval {
    s64 tv_sec;
    s64 tv_usec;
};

struct OrbisKernelTimezone {
    s32 tz_minuteswest;
    s32 tz_dsttime;
};

struct OrbisKernelTimespec {
    s64 tv_sec;
    s64 tv_nsec;

    std::chrono::system_clock::time_point TimePoint() const noexcept {
        using namespace std::chrono;
        return system_clock::time_point{duration_cast<system_clock::duration>(seconds{tv_sec} + nanoseconds{tv_nsec})};
    }
};

constexpr int ORBIS_CLOCK_REALTIME = 0;
constexpr int ORBIS_CLOCK_MONOTONIC = 4;
constexpr int ORBIS_CLOCK_REALTIME_PRECISE = 9;
constexpr int ORBIS_CLOCK_REALTIME_FAST = 10;
constexpr int ORBIS_CLOCK_MONOTONIC_PRECISE = 11;
constexpr int ORBIS_CLOCK_MONOTONIC_FAST = 12;
constexpr int ORBIS_CLOCK_SECOND = 13;

u64 PS4_SYSV_ABI sceKernelGetTscFrequency();
u64 PS4_SYSV_ABI sceKernelGetProcessTime();
u64 PS4_SYSV_ABI sceKernelGetProcessTimeCounter();
u64 PS4_SYSV_ABI sceKernelGetProcessTimeCounterFrequency();
u64 PS4_SYSV_ABI sceKernelReadTsc();
s32 PS4_SYSV_ABI sceKernelUsleep(u32 microseconds);
s32 PS4_SYSV_ABI sceKernelClockGettime(u32 clock_id, OrbisKernelTimespec* time);

} // namespace Libraries::Kernel
