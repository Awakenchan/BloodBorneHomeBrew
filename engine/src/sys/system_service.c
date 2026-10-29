// SPDX-License-Identifier: GPL-3.0-or-later
#include "sys.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum {
    SYSTEM_ERROR_PARAMETER = (int32_t)0x80A10003,
    SYSTEM_ERROR_NO_EVENT = (int32_t)0x80A10004,
};

enum {
    PARAM_LANGUAGE = 1,
    PARAM_DATE_FORMAT = 2,
    PARAM_TIME_FORMAT = 3,
    PARAM_TIME_ZONE = 4,
    PARAM_SUMMER_TIME = 5,
    PARAM_GAME_PARENTAL_LEVEL = 7,
    PARAM_ENTER_BUTTON = 1000,
};

static int32_t time_zone_minutes(void) {
    time_t now = time(NULL);
    struct tm local;
#ifdef _WIN32
    if (localtime_s(&local, &now)) return 0;
    return (int32_t)((_mkgmtime(&local) - now) / 60);
#else
    if (!localtime_r(&now, &local)) return 0;
    return (int32_t)(local.tm_gmtoff / 60);
#endif
}

static SYS_ABI int32_t system_param_get_int(int32_t id, int32_t *value) {
    if (!value) return SYSTEM_ERROR_PARAMETER;
    switch (id) {
    case PARAM_LANGUAGE: {
        const char *language = getenv("BB_LANGUAGE");
        *value = language ? atoi(language) : 1;
        return 0;
    }
    case PARAM_DATE_FORMAT:
    case PARAM_TIME_FORMAT:
    case PARAM_ENTER_BUTTON:
        *value = 1;
        return 0;
    case PARAM_TIME_ZONE:
        *value = time_zone_minutes();
        return 0;
    case PARAM_SUMMER_TIME:
    case PARAM_GAME_PARENTAL_LEVEL:
        *value = 0;
        return 0;
    }
    fprintf(stderr, "Runtime: unsupported system parameter %d\n", id);
    return SYSTEM_ERROR_PARAMETER;
}

static SYS_ABI int32_t system_get_status(void *status) {
    if (!status) return SYSTEM_ERROR_PARAMETER;
    memset(status, 0, 12);
    return 0;
}

static SYS_ABI int32_t system_receive_event(void *event) {
    (void)event;
    return SYSTEM_ERROR_NO_EVENT;
}

static SYS_ABI int32_t system_hide_splash_screen(void) {
    puts("Runtime: SystemService: splash screen hidden");
    return 0;
}

static SYS_ABI int32_t system_launch_web_browser(void) {
    puts("Runtime: SystemService: web browser request ignored (offline)");
    return 0;
}

static const SysExport exports[] = {
    {"rPo6tV8D9bM", "sceSystemServiceGetStatus", system_get_status},
    {"Vo5V8KAwCmk", "sceSystemServiceHideSplashScreen", system_hide_splash_screen},
    {"-+3hY+y8bNo", "sceSystemServiceLaunchWebBrowser", system_launch_web_browser},
    {"fZo48un7LK4", "sceSystemServiceParamGetInt", system_param_get_int},
    {"656LMQSrg6U", "sceSystemServiceReceiveEvent", system_receive_event},
};

SYS_LIBRARY(sys_system_service, "libSceSystemService", exports);
