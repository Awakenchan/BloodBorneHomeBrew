// SPDX-License-Identifier: GPL-3.0-or-later
#include "sys.h"
#include "gpu/bbgpu.h"
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    IME_ERROR_BUSY = (int32_t)0x80BC0003,
    IME_ERROR_PARAMETER = (int32_t)0x80BC0004,
    IME_ERROR_NOT_FINISHED = (int32_t)0x80BC0101,
};

enum { IME_STATUS_NONE = 0, IME_STATUS_RUNNING = 1, IME_STATUS_FINISHED = 2 };
enum { IME_END_OK = 0, IME_END_CANCELLED = 1 };

typedef struct {
    int32_t user;
    uint32_t type;
    uint64_t languages;
    uint32_t enter_label, input_method;
    void *filter;
    uint32_t option, max_length;
    uint16_t *text;
    float x, y;
    uint32_t horizontal_align, vertical_align;
    const uint16_t *placeholder, *title;
    int8_t reserved[16];
} ImeParam;
_Static_assert(offsetof(ImeParam, max_length) == 36 && offsetof(ImeParam, text) == 40 &&
                   offsetof(ImeParam, title) == 72 && sizeof(ImeParam) == 96,
               "ImeParam layout");

static struct {
    int running, finished;
    uint32_t end_status;
    uint16_t *text;
    uint32_t capacity;
} dialog;

static size_t put_utf8(uint32_t c, char *out) {
    if (c < 0x80) {
        out[0] = (char)c;
        return 1;
    }
    if (c < 0x800) {
        out[0] = (char)(0xC0 | c >> 6);
        out[1] = (char)(0x80 | (c & 0x3F));
        return 2;
    }
    if (c < 0x10000) {
        out[0] = (char)(0xE0 | c >> 12);
        out[1] = (char)(0x80 | (c >> 6 & 0x3F));
        out[2] = (char)(0x80 | (c & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | c >> 18);
    out[1] = (char)(0x80 | (c >> 12 & 0x3F));
    out[2] = (char)(0x80 | (c >> 6 & 0x3F));
    out[3] = (char)(0x80 | (c & 0x3F));
    return 4;
}

static void utf16_to_utf8(const uint16_t *in, size_t units, char *out, size_t size) {
    size_t used = 0;
    for (size_t i = 0; in && i < units && in[i] && used + 4 < size; ++i) {
        uint32_t c = in[i];
        int high = c >= 0xD800 && c < 0xDC00;
        if (high && i + 1 < units && in[i + 1] >= 0xDC00 && in[i + 1] < 0xE000)
            c = 0x10000 + ((c - 0xD800) << 10) + (in[++i] - 0xDC00);
        used += put_utf8(c, out + used);
    }
    out[used] = 0;
}

static void utf8_to_utf16(const char *in, uint16_t *out, uint32_t capacity) {
    const unsigned char *p = (const unsigned char *)in;
    uint32_t used = 0;
    while (*p && used < capacity) {
        uint32_t c;
        int continuation;
        if (*p < 0x80) c = *p, continuation = 0;
        else if ((*p & 0xE0) == 0xC0) c = *p & 0x1F, continuation = 1;
        else if ((*p & 0xF0) == 0xE0) c = *p & 0x0F, continuation = 2;
        else c = *p & 0x07, continuation = 3;
        ++p;
        for (int k = 0; k < continuation && (*p & 0xC0) == 0x80; ++k) c = c << 6 | (*p++ & 0x3F);
        if (c >= 0x10000) {
            if (used + 2 > capacity) break;
            c -= 0x10000;
            out[used++] = (uint16_t)(0xD800 + (c >> 10));
            out[used++] = (uint16_t)(0xDC00 + (c & 0x3FF));
        } else {
            out[used++] = (uint16_t)c;
        }
    }
    out[used] = 0;
}

static void finish(uint32_t end_status, const char *text) {
    if (end_status == IME_END_OK && dialog.text && dialog.capacity) utf8_to_utf16(text, dialog.text, dialog.capacity);
    dialog.end_status = end_status;
    dialog.finished = 1;
    dialog.running = 0;
    if (end_status == IME_END_OK) printf("Runtime: ImeDialog text: %s\n", text);
    else puts("Runtime: ImeDialog cancelled");
}

static SYS_ABI int32_t ime_init(const ImeParam *param, const void *extended) {
    (void)extended;
    if (!param || !param->text || !param->max_length) return IME_ERROR_PARAMETER;
    if (dialog.running) return IME_ERROR_BUSY;
    memset(&dialog, 0, sizeof(dialog));
    dialog.text = param->text;
    dialog.capacity = param->max_length;
    dialog.running = 1;
    char current[512], title[256];
    utf16_to_utf8(param->text, param->max_length, current, sizeof(current));
    utf16_to_utf8(param->title, 128, title, sizeof(title));
    const char *scripted = getenv("BB_IME_TEXT");
    if (scripted) {
        finish(IME_END_OK, scripted);
        return 0;
    }
    const char *user = getenv("BB_USER_NAME");
    const char *initial = current[0] ? current : user && user[0] ? user : "Hunter";
    if (bbgpu_text_input_begin(initial, title[0] ? title : "Text"))
        puts("Runtime: ImeDialog opened: type in the game window; Enter or Cross accepts, Esc or Circle cancels");
    else
        finish(IME_END_OK, user ? user : current[0] ? current : "Hunter");
    return 0;
}

static SYS_ABI int32_t ime_get_status(void) {
    if (dialog.running) {
        char text[512];
        int state = bbgpu_text_input_poll(text, sizeof(text));
        if (state) finish(state == 1 ? IME_END_OK : IME_END_CANCELLED, text);
    }
    return dialog.running ? IME_STATUS_RUNNING : dialog.finished ? IME_STATUS_FINISHED : IME_STATUS_NONE;
}

static SYS_ABI int32_t ime_get_result(uint32_t *end_status) {
    if (!dialog.finished) return IME_ERROR_NOT_FINISHED;
    if (end_status) *end_status = dialog.end_status;
    return 0;
}

static SYS_ABI int32_t ime_term(void) {
    memset(&dialog, 0, sizeof(dialog));
    return 0;
}

static const SysExport exports[] = {
    {"oBmw4xrmfKs", "sceImeDialogAbort", ime_term},
    {"x01jxu+vxlc", "sceImeDialogGetResult", ime_get_result},
    {"IADmD4tScBY", "sceImeDialogGetStatus", ime_get_status},
    {"NUeBrN7hzf0", "sceImeDialogInit", ime_init},
    {"gyTyVn+bXMw", "sceImeDialogTerm", ime_term},
};

SYS_LIBRARY(sys_ime_dialog, "libSceImeDialog", exports);
