// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LANTERN_SYS_H
#define LANTERN_SYS_H
#include <stddef.h>
#include <stdint.h>

#define SYS_ABI __attribute__((sysv_abi))

typedef struct {
    const char *nid;
    const char *name;
    void *function;
} SysExport;

typedef struct {
    const char *library;
    const SysExport *exports;
    size_t count;
} SysLibrary;

#define SYS_LIBRARY(variable, library_name, table) \
    const SysLibrary variable = { library_name, table, sizeof(table) / sizeof(*(table)) }

extern const SysLibrary sys_rtc, sys_user_service, sys_system_service, sys_np_trophy, sys_common_dialog,
    sys_msg_dialog, sys_save_data_dialog, sys_np_profile_dialog, sys_np_commerce, sys_ime_dialog, sys_play_go,
    sys_disc_map, sys_mouse, sys_voice, sys_audio_in, sys_app_content, sys_sysmodule, sys_posix_time, sys_pad,
    sys_audio_out, sys_ajm, sys_save_data, sys_semaphore, sys_rwlock;

int32_t sys_new_id(void);
void sys_app_content_configure(const uint32_t values[5]);
void sys_app_content_report(void);
void sys_pad_report(void);
void sys_audio_out_report(void);
void sys_ajm_report(void);
void sys_save_data_configure(const char *title);
void sys_save_data_report(void);
void sys_sema_describe(uint32_t id, char *out, size_t size);
unsigned sys_sema_waiters(uint32_t id);
void sys_sema_census(uint64_t *live, uint64_t *found, uint64_t *steps);
void sys_sema_report(void);
void sys_rwlock_census(uint64_t *live, uint64_t *found, uint64_t *steps);
void sys_rwlock_report(void);
void sys_pad_lock(void);
void sys_pad_unlock(void);
int sys_pad_type(void);
int sys_pad_button(uint32_t button, int right_side);

uintptr_t sys_resolve(const char *nid);
const SysExport *sys_find(const char *nid, const SysLibrary **library);
size_t sys_library_count(void);
const SysLibrary *sys_library(size_t index);

#endif
