// SPDX-License-Identifier: GPL-3.0-or-later
#include "sys.h"
#include <stdio.h>
#include <string.h>

enum {
    DIALOG_ERROR_NOT_INITIALIZED = (int32_t)0x80B80003,
    DIALOG_ERROR_ALREADY_INITIALIZED = (int32_t)0x80B80004,
};

enum { STATUS_NONE = 0, STATUS_INITIALIZED = 1, STATUS_FINISHED = 3 };

typedef struct {
    const char *name;
    int initialized;
    int32_t status;
} Dialog;

static Dialog msg = {.name = "MsgDialog"}, save_data = {.name = "SaveDataDialog"},
              np_profile = {.name = "NpProfileDialog"}, np_commerce = {.name = "NpCommerceDialog"};

static int32_t dialog_initialize(Dialog *d) {
    if (d->initialized) return DIALOG_ERROR_ALREADY_INITIALIZED;
    d->initialized = 1;
    d->status = STATUS_INITIALIZED;
    return 0;
}

static int32_t dialog_open(Dialog *d) {
    if (!d->initialized) return DIALOG_ERROR_NOT_INITIALIZED;
    printf("Runtime: %s opened; completed immediately (no dialog UI yet)\n", d->name);
    d->status = STATUS_FINISHED;
    return 0;
}

static int32_t dialog_terminate(Dialog *d) {
    if (!d->initialized) return DIALOG_ERROR_NOT_INITIALIZED;
    d->initialized = 0;
    d->status = STATUS_NONE;
    return 0;
}

#define DIALOG_FUNCTIONS(prefix, dialog)                                                       \
    static SYS_ABI int32_t prefix##_initialize(void) { return dialog_initialize(&dialog); }      \
    static SYS_ABI int32_t prefix##_open(const void *param) { (void)param; return dialog_open(&dialog); } \
    static SYS_ABI int32_t prefix##_update_status(void) { return dialog.status; }               \
    static SYS_ABI int32_t prefix##_terminate(void) { return dialog_terminate(&dialog); }

DIALOG_FUNCTIONS(msg, msg)
DIALOG_FUNCTIONS(save_data, save_data)
DIALOG_FUNCTIONS(np_profile, np_profile)
DIALOG_FUNCTIONS(np_commerce, np_commerce)

static SYS_ABI int32_t common_dialog_initialize(void) {
    return 0;
}

static SYS_ABI int32_t np_profile_get_result(void *result) {
    if (result) memset(result, 0, 4);
    return 0;
}

static const SysExport common_exports[] = {
    {"uoUpLGNkygk", "sceCommonDialogInitialize", common_dialog_initialize},
};

static const SysExport msg_exports[] = {
    {"lDqxaY1UbEo", "sceMsgDialogInitialize", msg_initialize},
    {"b06Hh0DPEaE", "sceMsgDialogOpen", msg_open},
    {"ePw-kqZmelo", "sceMsgDialogTerminate", msg_terminate},
    {"6fIC3XKt2k0", "sceMsgDialogUpdateStatus", msg_update_status},
};

static const SysExport save_data_exports[] = {
    {"s9e3+YpRnzw", "sceSaveDataDialogInitialize", save_data_initialize},
    {"4tPhsP6FpDI", "sceSaveDataDialogOpen", save_data_open},
    {"YuH2FA7azqQ", "sceSaveDataDialogTerminate", save_data_terminate},
    {"KK3Bdg1RWK0", "sceSaveDataDialogUpdateStatus", save_data_update_status},
};

static const SysExport np_profile_exports[] = {
    {"8rhLl1-0W-o", "sceNpProfileDialogGetResult", np_profile_get_result},
    {"Lg+NCE6pTwQ", "sceNpProfileDialogInitialize", np_profile_initialize},
    {"uj9Cz7Tk0cc", "sceNpProfileDialogOpen", np_profile_open},
    {"0Sp9vJcB1-w", "sceNpProfileDialogTerminate", np_profile_terminate},
    {"haVZE9FgKqE", "sceNpProfileDialogUpdateStatus", np_profile_update_status},
};

static const SysExport np_commerce_exports[] = {
    {"0aR2aWmQal4", "sceNpCommerceDialogInitialize", np_commerce_initialize},
    {"DfSCDRA3EjY", "sceNpCommerceDialogOpen", np_commerce_open},
    {"m-I92Ab50W8", "sceNpCommerceDialogTerminate", np_commerce_terminate},
    {"LR5cwFMMCVE", "sceNpCommerceDialogUpdateStatus", np_commerce_update_status},
};

SYS_LIBRARY(sys_common_dialog, "libSceCommonDialog", common_exports);
SYS_LIBRARY(sys_msg_dialog, "libSceMsgDialog", msg_exports);
SYS_LIBRARY(sys_save_data_dialog, "libSceSaveDataDialog", save_data_exports);
SYS_LIBRARY(sys_np_profile_dialog, "libSceNpProfileDialog", np_profile_exports);
SYS_LIBRARY(sys_np_commerce, "libSceNpCommerce", np_commerce_exports);
