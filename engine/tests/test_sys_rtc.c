// SPDX-License-Identifier: GPL-3.0-or-later
#include "sys/sys.h"
#include <stdio.h>
#include <string.h>

typedef struct { uint16_t year, month, day, hour, minute, second; uint32_t microsecond; } DateTime;

static int failures;
#define EXPECT(condition) do { if (!(condition)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); ++failures; } } while (0)

static void *fn(const char *name) {
    const SysLibrary *rtc = &sys_rtc;
    for (size_t i = 0; i < rtc->count; ++i)
        if (!strcmp(rtc->exports[i].name, name)) return rtc->exports[i].function;
    printf("FAIL missing %s\n", name);
    ++failures;
    return NULL;
}

int main(void) {
    int32_t (SYS_ABI *get_tick)(const DateTime *, uint64_t *) = fn("sceRtcGetTick");
    int32_t (SYS_ABI *set_tick)(DateTime *, const uint64_t *) = fn("sceRtcSetTick");
    int32_t (SYS_ABI *day_of_week)(int32_t, int32_t, int32_t) = fn("sceRtcGetDayOfWeek");
    int32_t (SYS_ABI *set_time_t)(DateTime *, int64_t) = fn("sceRtcSetTime_t");
    int32_t (SYS_ABI *network_tick)(uint64_t *) = fn("sceRtcGetCurrentNetworkTick");
    uint32_t (SYS_ABI *resolution)(void) = fn("sceRtcGetTickResolution");
    int32_t (SYS_ABI *rfc2822)(char *, const uint64_t *) = fn("sceRtcFormatRFC2822LocalTime");
    if (failures) return 1;

    uint64_t tick = 0;
    DateTime epoch = {1970, 1, 1, 0, 0, 0, 0};
    EXPECT(get_tick(&epoch, &tick) == 0 && tick == UINT64_C(62135596800000000));
    DateTime first = {1, 1, 1, 0, 0, 0, 0};
    EXPECT(get_tick(&first, &tick) == 0 && tick == 0);
    DateTime last = {9999, 12, 31, 23, 59, 59, 999999}, back;
    EXPECT(get_tick(&last, &tick) == 0 && set_tick(&back, &tick) == 0 && !memcmp(&last, &back, sizeof back));
    DateTime leap = {2000, 2, 29, 12, 30, 15, 42};
    EXPECT(get_tick(&leap, &tick) == 0 && set_tick(&back, &tick) == 0 && !memcmp(&leap, &back, sizeof back));
    DateTime not_leap = {1900, 2, 29, 0, 0, 0, 0};
    EXPECT(get_tick(&not_leap, &tick) == (int32_t)0x80B5000A);
    DateTime bad_hour = {2026, 10, 30, 24, 0, 0, 0};
    EXPECT(get_tick(&bad_hour, &tick) == (int32_t)0x80B5000B);
    DateTime bad_us = {2026, 10, 30, 0, 0, 0, 1000000};
    EXPECT(get_tick(&bad_us, &tick) == (int32_t)0x80B5000E);
    EXPECT(get_tick(NULL, &tick) == (int32_t)0x80B50002);

    EXPECT(day_of_week(2026, 10, 30) == 5);
    EXPECT(day_of_week(1, 1, 1) == 1);
    EXPECT(day_of_week(2000, 2, 29) == 2);
    EXPECT(day_of_week(0, 1, 1) == (int32_t)0x80B50008);
    EXPECT(day_of_week(2026, 13, 1) == (int32_t)0x80B50009);
    EXPECT(day_of_week(2026, 2, 29) == (int32_t)0x80B5000A);

    DateTime from_seconds;
    EXPECT(set_time_t(&from_seconds, 1793318400) == 0 && from_seconds.year == 2026 && from_seconds.month == 10 &&
           from_seconds.day == 30 && from_seconds.hour == 0);
    EXPECT(set_time_t(&from_seconds, -1) == (int32_t)0x80B50003);

    EXPECT(network_tick(&tick) == 0 && tick == UINT64_C(62135596800000000) + UINT64_C(1793318400123456));
    EXPECT(resolution() == 1000000);

    char text[64];
    EXPECT(rfc2822(text, NULL) == 0 && strlen(text) == 31 && text[3] == ',' && (text[26] == '+' || text[26] == '-'));

    printf("%s\n", failures ? "sys rtc: FAILED" : "sys rtc: ok");
    return failures != 0;
}
