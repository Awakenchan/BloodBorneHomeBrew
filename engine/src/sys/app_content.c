// SPDX-License-Identifier: GPL-3.0-or-later
#include "sys.h"
#include "runtime.h"
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

enum {
    SYSMODULE_ERROR_ID = (int32_t)0x805A1000,
    SYSMODULE_ERROR_NOT_LOADED = (int32_t)0x805A1001,
    CONTENT_ERROR_NOT_INITIALIZED = (int32_t)0x80D90001,
    CONTENT_ERROR_PARAMETER = (int32_t)0x80D90002,
    CONTENT_ERROR_BUSY = (int32_t)0x80D90003,
};

#define APP_CONTENT_MODULE 0xB4
#define LABEL_SIZE 17
#define MAX_ADDONS 64
#define ADDON_INSTALLED 4u
#define PATH_SIZE 4096

typedef struct {
    char label[LABEL_SIZE];
    char padding[3];
    uint32_t status;
} AddonInfo;
_Static_assert(sizeof(AddonInfo) == 24, "AddonInfo is 24 bytes");

static uint32_t module_references[0x10000];
static uint32_t parameters[5];
static int configured, initialized;
static unsigned parameter_queries, list_queries;
static char addons[MAX_ADDONS][LABEL_SIZE];
static uint32_t addon_count;
static int addons_scanned;

static __attribute__((noreturn)) void stop(const char *why) {
    fprintf(stderr, "STOP: %s\n", why);
    exit(21);
}

void sys_app_content_configure(const uint32_t values[5]) {
    if (module_references[APP_CONTENT_MODULE] || initialized || (values[0] != 1 && values[0] != 3)) {
        fputs("ERROR: invalid or late content profile\n", stderr);
        exit(1);
    }
    memcpy(parameters, values, sizeof(parameters));
    configured = 1;
}

void sys_app_content_report(void) {
    printf("Runtime: AppContent references=%u, initialized=%u, parameter queries=%u, list queries=%u\n",
           module_references[APP_CONTENT_MODULE], (unsigned)initialized, parameter_queries, list_queries);
}

static void need_profile(void) {
    if (!configured) stop("AppContent needs --content-profile");
}

static SYS_ABI int32_t sysmodule_load(uint16_t id) {
    if (!id) return SYSMODULE_ERROR_ID;
    if (id == APP_CONTENT_MODULE) need_profile();
    if (module_references[id] == UINT32_MAX) stop("module reference overflow");
    ++module_references[id];
    if (id == APP_CONTENT_MODULE)
        printf("Runtime: AppContent host provider loaded; references=%u\n", module_references[id]);
    else
        printf("Runtime: system module 0x%x loaded (host implementation)\n", id);
    return 0;
}

static SYS_ABI int32_t sysmodule_is_loaded(uint16_t id) {
    if (!id) return SYSMODULE_ERROR_ID;
    return module_references[id] ? 0 : SYSMODULE_ERROR_NOT_LOADED;
}

static SYS_ABI int32_t sysmodule_unload(uint16_t id) {
    int32_t result = sysmodule_is_loaded(id);
    if (result) return result;
    --module_references[id];
    if (id == APP_CONTENT_MODULE && !module_references[id]) initialized = 0;
    return 0;
}

static SYS_ABI int32_t content_initialize(const unsigned char *init, unsigned char *boot) {
    need_profile();
    if (!module_references[APP_CONTENT_MODULE]) stop("AppContent called before module load");
    if (!init || !boot) return CONTENT_ERROR_PARAMETER;
    if (initialized) return CONTENT_ERROR_BUSY;
    for (int i = 0; i < 32; ++i)
        if (init[i]) return CONTENT_ERROR_PARAMETER;
    memset(boot, 0, 40);
    initialized = 1;
    puts("Runtime: AppContent initialized; offline base-game profile, no mounted add-ons");
    return 0;
}

static int32_t ready(void) {
    need_profile();
    if (module_references[APP_CONTENT_MODULE] && initialized) return 0;
    fputs("Runtime: AppContent query before initialization\n", stderr);
    return CONTENT_ERROR_NOT_INITIALIZED;
}

static SYS_ABI int32_t content_app_param_get_int(uint32_t id, int32_t *value) {
    int32_t result = ready();
    if (result) return result;
    if (id > 4 || !value) return CONTENT_ERROR_PARAMETER;
    memcpy(value, &parameters[id], sizeof(*value));
    ++parameter_queries;
    printf("Runtime: AppContent parameter %u = %d\n", id, *value);
    return 0;
}

static uint32_t read_le32(const unsigned char *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static int param_sfo_string(const char *path, const char *key, char *out, size_t size) {
    enum { LIMIT = 65536 };
    FILE *file = fopen(path, "rb");
    if (!file) return 0;
    unsigned char *sfo = malloc(LIMIT);
    size_t length = sfo ? fread(sfo, 1, LIMIT, file) : 0;
    fclose(file);
    int found = 0;
    if (length >= 20 && !memcmp(sfo, "\0PSF", 4)) {
        uint32_t key_table = read_le32(sfo + 8), data_table = read_le32(sfo + 12), entries = read_le32(sfo + 16);
        for (uint32_t i = 0; i < entries && 20 + 16 * ((size_t)i + 1) <= length; ++i) {
            const unsigned char *entry = sfo + 20 + 16 * (size_t)i;
            size_t key_at = (size_t)key_table + (entry[0] | entry[1] << 8);
            size_t value_at = (size_t)data_table + read_le32(entry + 12);
            size_t value_length = read_le32(entry + 4);
            if (key_at >= length || !memchr(sfo + key_at, 0, length - key_at) || strcmp((char *)sfo + key_at, key))
                continue;
            if (value_at < length && value_length <= length - value_at && size) {
                size_t n = value_length < size ? value_length : size - 1;
                memcpy(out, sfo + value_at, n);
                out[n] = 0;
                found = 1;
            }
            break;
        }
    }
    free(sfo);
    return found;
}

static int label_ok(const char *label) {
    size_t n = strlen(label);
    if (!n || n >= LABEL_SIZE) return 0;
    for (size_t i = 0; i < n; ++i)
        if (!isalnum((unsigned char)label[i])) return 0;
    return 1;
}

static void add_addon(const char *folder, const char *name, int need_param_sfo) {
    char sfo[PATH_SIZE], category[16] = "", content_id[64] = "", label[64] = "";
    if (snprintf(sfo, sizeof(sfo), "%s/sce_sys/param.sfo", folder) >= (int)sizeof(sfo)) return;
    if (param_sfo_string(sfo, "CATEGORY", category, sizeof(category))) {
        if (strcmp(category, "ac")) return;
        if (param_sfo_string(sfo, "CONTENT_ID", content_id, sizeof(content_id))) {
            const char *dash = strrchr(content_id, '-');
            snprintf(label, sizeof(label), "%s", dash ? dash + 1 : "");
        }
    } else if (need_param_sfo) {
        return;
    }
    if (!label[0]) snprintf(label, sizeof(label), "%s", name);
    if (!label_ok(label) || addon_count >= MAX_ADDONS) return;
    for (uint32_t i = 0; i < addon_count; ++i)
        if (!strcmp(addons[i], label)) return;
    memcpy(addons[addon_count++], label, strlen(label) + 1);
    printf("Runtime: add-on %s reported installed (%s)\n", label, folder);
}

static void scan_folder(const char *folder) {
    DIR *dir = opendir(folder);
    if (!dir) return;
    for (struct dirent *entry; (entry = readdir(dir));) {
        char path[PATH_SIZE];
        struct stat info;
        if (entry->d_name[0] == '.' || snprintf(path, sizeof(path), "%s/%s", folder, entry->d_name) >= (int)sizeof(path) ||
            stat(path, &info) || !S_ISDIR(info.st_mode))
            continue;
        add_addon(path, entry->d_name, 0);
    }
    closedir(dir);
}

static void scan_addons(void) {
    if (addons_scanned) return;
    addons_scanned = 1;
    const char *dlc = getenv("BB_DLC");
    if (dlc && dlc[0] == '0') {
        puts("Runtime: DLC switched off: no add-ons reported");
        return;
    }
    const char *folder = getenv("BB_DLC_DIR");
    if (folder && *folder) {
        add_addon(folder, "", 1);
        scan_folder(folder);
    }
    char root[PATH_SIZE];
    DIR *titles = snprintf(root, sizeof(root), "%s/addcont", sys_file_user_dir()) < (int)sizeof(root) ? opendir(root) : NULL;
    if (titles) {
        for (struct dirent *title; (title = readdir(titles));) {
            char path[PATH_SIZE];
            if (title->d_name[0] == '.' || snprintf(path, sizeof(path), "%s/%s", root, title->d_name) >= (int)sizeof(path))
                continue;
            scan_folder(path);
        }
        closedir(titles);
    }
    if (folder && *folder && !addon_count) printf("Runtime: DLC folder %s: no add-ons found\n", folder);
    if (dlc && dlc[0] == '1' && !addon_count) add_addon("", "SPEXPANSIONDLC03", 0);
}

static SYS_ABI int32_t content_get_addcont_info_list(uint32_t service_label, AddonInfo *list, uint32_t capacity,
                                                     uint32_t *hits) {
    int32_t result = ready();
    if (result) return result;
    static int service_noted;
    if (service_label && !service_noted++)
        fprintf(stderr, "Runtime: AppContent service label %u: no add-ons\n", service_label);
    if ((!capacity || !list) && !hits) return CONTENT_ERROR_PARAMETER;
    scan_addons();
    uint32_t count = service_label ? 0 : addon_count;
    if (list && capacity) {
        if (count > capacity) count = capacity;
        for (uint32_t i = 0; i < count; ++i) {
            AddonInfo info;
            memset(&info, 0, sizeof(info));
            memcpy(info.label, addons[i], strlen(addons[i]));
            info.status = ADDON_INSTALLED;
            memcpy(&list[i], &info, sizeof(info));
        }
    }
    if (hits) *hits = count;
    ++list_queries;
    return 0;
}

static const SysExport content_exports[] = {
    {"99b82IKXpH4", "sceAppContentAppParamGetInt", content_app_param_get_int},
    {"xnd8BJzAxmk", "sceAppContentGetAddcontInfoList", content_get_addcont_info_list},
    {"R9lA82OraNs", "sceAppContentInitialize", content_initialize},
};

static const SysExport sysmodule_exports[] = {
    {"fMP5NHUOaMk", "sceSysmoduleIsLoaded", sysmodule_is_loaded},
    {"g8cM39EUZ6o", "sceSysmoduleLoadModule", sysmodule_load},
    {"eR2bZFAAU0Q", "sceSysmoduleUnloadModule", sysmodule_unload},
};

SYS_LIBRARY(sys_app_content, "libSceAppContent", content_exports);
SYS_LIBRARY(sys_sysmodule, "libSceSysmodule", sysmodule_exports);
