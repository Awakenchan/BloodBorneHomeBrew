// SPDX-License-Identifier: GPL-3.0-or-later
#include "sys.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    USER_ERROR_ARGUMENT = (int32_t)0x80960005,
    USER_ERROR_NO_EVENT = (int32_t)0x80960007,
    USER_ERROR_BUFFER = (int32_t)0x8096000A,
};

#define THE_USER 1
#define NO_USER (-1)

static atomic_int login_announced;

static const char *user_name(void) {
    const char *name = getenv("BB_USER_NAME");
    return name ? name : "Hunter";
}

static SYS_ABI int32_t user_initialize(const void *params) {
    (void)params;
    puts("Runtime: UserService initialized (user 1 logged in)");
    return 0;
}

static SYS_ABI int32_t user_terminate(void) {
    return 0;
}

static SYS_ABI int32_t user_get_initial_user(int32_t *user) {
    if (!user) return USER_ERROR_ARGUMENT;
    *user = THE_USER;
    return 0;
}

static SYS_ABI int32_t user_get_login_user_id_list(int32_t *users) {
    if (!users) return USER_ERROR_ARGUMENT;
    users[0] = THE_USER;
    for (int i = 1; i < 4; ++i) users[i] = NO_USER;
    return 0;
}

static SYS_ABI int32_t user_get_user_name(int32_t user, char *out, uint64_t size) {
    if (user != THE_USER || !out) return USER_ERROR_ARGUMENT;
    const char *name = user_name();
    size_t length = strlen(name);
    if (length + 1 > size) return USER_ERROR_BUFFER;
    memcpy(out, name, length + 1);
    return 0;
}

static SYS_ABI int32_t user_get_event(int32_t *event) {
    if (!event) return USER_ERROR_ARGUMENT;
    if (atomic_exchange(&login_announced, 1)) return USER_ERROR_NO_EVENT;
    event[0] = 0;
    event[1] = THE_USER;
    return 0;
}

static const SysExport exports[] = {
    {"yH17Q6NWtVg", "sceUserServiceGetEvent", user_get_event},
    {"CdWp0oHWGr0", "sceUserServiceGetInitialUser", user_get_initial_user},
    {"fPhymKNvK-A", "sceUserServiceGetLoginUserIdList", user_get_login_user_id_list},
    {"1xxcMiGu2fo", "sceUserServiceGetUserName", user_get_user_name},
    {"j3YMu1MVNNo", "sceUserServiceInitialize", user_initialize},
    {"bwFjS+bX9mA", "sceUserServiceTerminate", user_terminate},
};

SYS_LIBRARY(sys_user_service, "libSceUserService", exports);
