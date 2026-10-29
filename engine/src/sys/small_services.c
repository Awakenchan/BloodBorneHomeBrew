// SPDX-License-Identifier: GPL-3.0-or-later
#include "sys.h"
#include <string.h>

#define DISC_MAP_NO_BITMAP ((int32_t)0x81100004)
#define MOUSE_ERROR_ARGUMENT ((int32_t)0x80DF0001)
#define VOICE_ERROR_ARGUMENT ((int32_t)0x8029000B)
#define AUDIO_IN_NOT_OPENED ((int32_t)0x80260109)
#define MOUSE_DATA_SIZE 40
#define VOICE_PORT_INFO_SIZE 40

static SYS_ABI int32_t succeed(void) {
    return 0;
}

static SYS_ABI int32_t disc_map_no_bitmap(void) {
    return DISC_MAP_NO_BITMAP;
}

static SYS_ABI int32_t mouse_open(int32_t user, int32_t type, int32_t index, const void *param) {
    (void)user;
    (void)type;
    (void)index;
    (void)param;
    return sys_new_id();
}

static SYS_ABI int32_t mouse_read(int32_t handle, void *data, int32_t count) {
    (void)handle;
    if (!data || count < 1) return MOUSE_ERROR_ARGUMENT;
    memset(data, 0, MOUSE_DATA_SIZE);
    return 1;
}

static SYS_ABI int32_t voice_create_port(const void *param, uint32_t *port) {
    (void)param;
    if (!port) return VOICE_ERROR_ARGUMENT;
    *port = (uint32_t)sys_new_id();
    return 0;
}

static SYS_ABI int32_t voice_get_port_info(uint32_t port, void *info) {
    (void)port;
    if (!info) return VOICE_ERROR_ARGUMENT;
    memset(info, 0, VOICE_PORT_INFO_SIZE);
    return 0;
}

static SYS_ABI int32_t voice_read_from_oport(uint32_t port, void *data, uint32_t *size) {
    (void)port;
    (void)data;
    if (size) *size = 0;
    return 0;
}

static SYS_ABI int32_t audio_in_not_opened(void) {
    return AUDIO_IN_NOT_OPENED;
}

static const SysExport disc_map_exports[] = {
    {"lbQKqsERhtE", "sceDiscMapIsRequestOnHDD", disc_map_no_bitmap},
    {"ioKMruft1ek", "sceDiscMap_8A828CAEE7EDD5E9", disc_map_no_bitmap},
};

static const SysExport mouse_exports[] = {
    {"cAnT0Rw-IwU", "sceMouseClose", succeed},
    {"Qs0wWulgl7U", "sceMouseInit", succeed},
    {"RaqxZIf6DvE", "sceMouseOpen", mouse_open},
    {"x8qnXqh-tiM", "sceMouseRead", mouse_read},
};

static const SysExport voice_exports[] = {
    {"oV9GAdJ23Gw", "sceVoiceConnectIPortToOPort", succeed},
    {"nXpje5yNpaE", "sceVoiceCreatePort", voice_create_port},
    {"b7kJI+nx2hg", "sceVoiceDeletePort", succeed},
    {"ajVj3QG2um4", "sceVoiceDisconnectIPortFromOPort", succeed},
    {"Oo0S5PH7FIQ", "sceVoiceEnd", succeed},
    {"CrLqDwWLoXM", "sceVoiceGetPortInfo", voice_get_port_info},
    {"9TrhuGzberQ", "sceVoiceInit", succeed},
    {"cQ6DGsQEjV4", "sceVoiceReadFromOPort", voice_read_from_oport},
    {"54phPH2LZls", "sceVoiceStart", succeed},
    {"Ao2YNSA7-Qo", "sceVoiceStop", succeed},
    {"YeJl6yDlhW0", "sceVoiceWriteToIPort", succeed},
};

static const SysExport audio_in_exports[] = {
    {"Jh6WbHhnI68", "sceAudioInClose", audio_in_not_opened},
    {"LozEOU8+anM", "sceAudioInInput", audio_in_not_opened},
    {"5NE8Sjc7VC8", "sceAudioInOpen", audio_in_not_opened},
};

SYS_LIBRARY(sys_disc_map, "libSceDiscMap", disc_map_exports);
SYS_LIBRARY(sys_mouse, "libSceMouse", mouse_exports);
SYS_LIBRARY(sys_voice, "libSceVoice", voice_exports);
SYS_LIBRARY(sys_audio_in, "libSceAudioIn", audio_in_exports);
