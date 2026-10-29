// SPDX-License-Identifier: GPL-3.0-or-later
#include "sys.h"
#include "host_sync.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

enum {
    RTC_ERROR_POINTER = (int32_t)0x80B50002,
    RTC_ERROR_VALUE = (int32_t)0x80B50003,
    RTC_ERROR_YEAR = (int32_t)0x80B50008,
    RTC_ERROR_MONTH = (int32_t)0x80B50009,
    RTC_ERROR_DAY = (int32_t)0x80B5000A,
    RTC_ERROR_HOUR = (int32_t)0x80B5000B,
    RTC_ERROR_MINUTE = (int32_t)0x80B5000C,
    RTC_ERROR_SECOND = (int32_t)0x80B5000D,
    RTC_ERROR_MICROSECOND = (int32_t)0x80B5000E,
};

typedef struct {
    uint16_t year, month, day, hour, minute, second;
    uint32_t microsecond;
} RtcDateTime;
_Static_assert(sizeof(RtcDateTime) == 16, "RtcDateTime is 16 bytes");

#define US_PER_SECOND UINT64_C(1000000)
#define US_PER_DAY (UINT64_C(86400) * US_PER_SECOND)
#define DAYS_PER_400_YEARS 146097
#define UNIX_EPOCH_DAYS UINT64_C(719162)
#define UNIX_EPOCH_TICK (UNIX_EPOCH_DAYS * US_PER_DAY)

static const uint16_t days_before_month[2][13] = {
    {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334, 365},
    {0, 31, 60, 91, 121, 152, 182, 213, 244, 274, 305, 335, 366},
};

static int is_leap_year(uint32_t year) {
    return year % 400 == 0 || (year % 4 == 0 && year % 100 != 0);
}

static uint32_t month_length(uint32_t year, uint32_t month) {
    const uint16_t *table = days_before_month[is_leap_year(year)];
    return table[month] - table[month - 1];
}

static uint64_t days_before_year(uint64_t year) {
    uint64_t y = year - 1;
    return y * 365 + y / 4 - y / 100 + y / 400;
}

static uint64_t day_number(uint32_t year, uint32_t month, uint32_t day) {
    return days_before_year(year) + days_before_month[is_leap_year(year)][month - 1] + day - 1;
}

static void date_of_day(uint64_t days, uint32_t *year, uint32_t *month, uint32_t *day) {
    uint64_t y = 1 + days * 400 / DAYS_PER_400_YEARS;
    while (days_before_year(y) > days) --y;
    while (days_before_year(y + 1) <= days) ++y;
    uint64_t in_year = days - days_before_year(y);
    const uint16_t *table = days_before_month[is_leap_year((uint32_t)y)];
    uint32_t m = 1;
    while (in_year >= table[m]) ++m;
    *year = (uint32_t)y;
    *month = m;
    *day = (uint32_t)(in_year - table[m - 1] + 1);
}

static int32_t check_date(uint32_t year, uint32_t month, uint32_t day) {
    if (year < 1 || year > 9999) return RTC_ERROR_YEAR;
    if (month < 1 || month > 12) return RTC_ERROR_MONTH;
    if (day < 1 || day > month_length(year, month)) return RTC_ERROR_DAY;
    return 0;
}

static int32_t check_date_time(const RtcDateTime *t) {
    int32_t error = check_date(t->year, t->month, t->day);
    if (error) return error;
    if (t->hour > 23) return RTC_ERROR_HOUR;
    if (t->minute > 59) return RTC_ERROR_MINUTE;
    if (t->second > 59) return RTC_ERROR_SECOND;
    if (t->microsecond > 999999) return RTC_ERROR_MICROSECOND;
    return 0;
}

static uint64_t tick_of(const RtcDateTime *t) {
    uint64_t seconds = (uint64_t)t->hour * 3600 + (uint64_t)t->minute * 60 + t->second;
    return day_number(t->year, t->month, t->day) * US_PER_DAY + seconds * US_PER_SECOND + t->microsecond;
}

static void date_time_of(uint64_t tick, RtcDateTime *t) {
    uint32_t year, month, day;
    date_of_day(tick / US_PER_DAY, &year, &month, &day);
    uint64_t within = tick % US_PER_DAY;
    uint64_t seconds = within / US_PER_SECOND;
    t->year = (uint16_t)year;
    t->month = (uint16_t)month;
    t->day = (uint16_t)day;
    t->hour = (uint16_t)(seconds / 3600);
    t->minute = (uint16_t)(seconds / 60 % 60);
    t->second = (uint16_t)(seconds % 60);
    t->microsecond = (uint32_t)(within % US_PER_SECOND);
}

static uint64_t utc_now(void) {
    return UNIX_EPOCH_TICK + host_realtime_ns() / 1000;
}

static int64_t zone_offset_us(uint64_t utc_tick) {
    time_t when = (time_t)(((int64_t)utc_tick - (int64_t)UNIX_EPOCH_TICK) / (int64_t)US_PER_SECOND);
    struct tm local;
#ifdef _WIN32
    if (localtime_s(&local, &when)) return 0;
    return (int64_t)(_mkgmtime(&local) - when) * (int64_t)US_PER_SECOND;
#else
    if (!localtime_r(&when, &local)) return 0;
    return (int64_t)local.tm_gmtoff * (int64_t)US_PER_SECOND;
#endif
}

static SYS_ABI int32_t rtc_get_current_clock_local_time(RtcDateTime *out) {
    if (!out) return RTC_ERROR_POINTER;
    uint64_t now = utc_now();
    date_time_of((uint64_t)((int64_t)now + zone_offset_us(now)), out);
    return 0;
}

static SYS_ABI int32_t rtc_get_current_network_tick(uint64_t *out) {
    if (!out) return RTC_ERROR_POINTER;
    *out = utc_now();
    return 0;
}

static SYS_ABI int32_t rtc_convert_utc_to_local_time(const uint64_t *utc, uint64_t *local) {
    if (!utc || !local) return RTC_ERROR_POINTER;
    *local = (uint64_t)((int64_t)*utc + zone_offset_us(*utc));
    return 0;
}

static SYS_ABI int32_t rtc_convert_local_time_to_utc(const uint64_t *local, uint64_t *utc) {
    if (!local || !utc) return RTC_ERROR_POINTER;
    *utc = (uint64_t)((int64_t)*local - zone_offset_us(*local));
    return 0;
}

static SYS_ABI int32_t rtc_get_day_of_week(int32_t year, int32_t month, int32_t day) {
    int32_t error = check_date((uint32_t)year, (uint32_t)month, (uint32_t)day);
    if (error) return error;
    return (int32_t)((day_number((uint32_t)year, (uint32_t)month, (uint32_t)day) + 1) % 7);
}

static SYS_ABI int32_t rtc_get_tick(const RtcDateTime *date, uint64_t *out) {
    if (!date || !out) return RTC_ERROR_POINTER;
    int32_t error = check_date_time(date);
    if (error) return error;
    *out = tick_of(date);
    return 0;
}

static SYS_ABI int32_t rtc_set_tick(RtcDateTime *out, const uint64_t *tick) {
    if (!out || !tick) return RTC_ERROR_POINTER;
    date_time_of(*tick, out);
    return 0;
}

static SYS_ABI uint32_t rtc_get_tick_resolution(void) {
    return (uint32_t)US_PER_SECOND;
}

static SYS_ABI int32_t rtc_set_time_t(RtcDateTime *out, int64_t seconds) {
    if (!out) return RTC_ERROR_POINTER;
    if (seconds < 0) return RTC_ERROR_VALUE;
    date_time_of(UNIX_EPOCH_TICK + (uint64_t)seconds * US_PER_SECOND, out);
    return 0;
}

static SYS_ABI int32_t rtc_format_rfc2822_local_time(char *out, const uint64_t *utc) {
    static const char weekdays[7][4] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static const char months[12][4] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                       "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    if (!out) return RTC_ERROR_POINTER;
    uint64_t when = utc ? *utc : utc_now();
    int64_t offset = zone_offset_us(when);
    uint64_t local = (uint64_t)((int64_t)when + offset);
    RtcDateTime t;
    date_time_of(local, &t);
    int64_t minutes = offset / 60 / (int64_t)US_PER_SECOND;
    uint64_t magnitude = (uint64_t)(minutes < 0 ? -minutes : minutes);
    char text[40];
    int length = snprintf(text, sizeof(text), "%s, %02u %s %04u %02u:%02u:%02u %c%02u%02u",
                          weekdays[(local / US_PER_DAY + 1) % 7], t.day, months[t.month - 1], t.year, t.hour,
                          t.minute, t.second, minutes < 0 ? '-' : '+', (unsigned)(magnitude / 60),
                          (unsigned)(magnitude % 60));
    memcpy(out, text, (size_t)length + 1);
    return 0;
}

static const SysExport exports[] = {
    {"8Yr143yEnRo", "sceRtcConvertLocalTimeToUtc", rtc_convert_local_time_to_utc},
    {"M1TvFst-jrM", "sceRtcConvertUtcToLocalTime", rtc_convert_utc_to_local_time},
    {"AxHBk3eat04", "sceRtcFormatRFC2822LocalTime", rtc_format_rfc2822_local_time},
    {"ZPD1YOKI+Kw", "sceRtcGetCurrentClockLocalTime", rtc_get_current_clock_local_time},
    {"zO9UL3qIINQ", "sceRtcGetCurrentNetworkTick", rtc_get_current_network_tick},
    {"CyIK-i4XdgQ", "sceRtcGetDayOfWeek", rtc_get_day_of_week},
    {"8w-H19ip48I", "sceRtcGetTick", rtc_get_tick},
    {"jMNwqYr4R-k", "sceRtcGetTickResolution", rtc_get_tick_resolution},
    {"ueega6v3GUw", "sceRtcSetTick", rtc_set_tick},
    {"bDEVVP4bTjQ", "sceRtcSetTime_t", rtc_set_time_t},
};

SYS_LIBRARY(sys_rtc, "libSceRtc", exports);
