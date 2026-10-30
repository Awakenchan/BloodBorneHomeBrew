// SPDX-License-Identifier: GPL-3.0-or-later
#include <stddef.h>
#include <stdint.h>
#include <time.h>
#ifndef _WIN32
#include <pthread.h>
#endif

#define WEAK __attribute__((weak))

typedef struct BbPcPad BbPcPad;

WEAK uint64_t host_realtime_ns(void) { return UINT64_C(1793318400) * 1000000000 + 123456789; }
WEAK uint64_t host_monotonic_ns(void) { return 0; }
WEAK void host_sleep_until_ns(uint64_t deadline) { (void)deadline; }
WEAK int bbgpu_window_in_background(void) { return 0; }
WEAK int bbgpu_overlay_captures_input(void) { return 0; }
WEAK int bbgpu_menu_buttons(uint32_t *confirm, uint32_t *back) { (void)confirm; (void)back; return 0; }
WEAK int bbgpu_pc_input(BbPcPad *pad, const _Bool *keys, int count, int gamepad) { (void)pad; (void)keys; (void)count; (void)gamepad; return 0; }
WEAK int bbgpu_text_input_begin(const char *initial, const char *prompt) { (void)initial; (void)prompt; return 0; }
WEAK int bbgpu_text_input_poll(char *out, uint64_t size) { (void)out; (void)size; return 0; }
WEAK int64_t runtime_file_open(const char *path, int flags, int mode) { (void)path; (void)flags; (void)mode; return -1; }
WEAK int64_t runtime_file_read(int fd, void *buffer, uint64_t size) { (void)fd; (void)buffer; (void)size; return -1; }
WEAK int64_t runtime_file_close(int fd) { (void)fd; return 0; }
WEAK const char *runtime_file_user_dir(void) { return "no-such-user-dir"; }
WEAK int runtime_file_mount(const char *guest, const char *host) { (void)guest; (void)host; return 0; }
WEAK void runtime_file_unmount(const char *guest) { (void)guest; }
WEAK int runtime_block_begin(int kind, uintptr_t object) { (void)kind; (void)object; return 0; }
WEAK void runtime_block_end(int slot) { (void)slot; }
WEAK void runtime_wait_note(int kind, uint64_t ns) { (void)kind; (void)ns; }
WEAK void host_sleep_ns(uint64_t ns) {
    struct timespec t = {(time_t)(ns / 1000000000), (long)(ns % 1000000000)};
    nanosleep(&t, NULL);
}
WEAK void runtime_report(void) {}
WEAK void runtime_file_stream_report(void) {}
WEAK void *runtime_low_map(size_t size, int prot) { (void)size; (void)prot; return NULL; }
#ifndef _WIN32
WEAK int host_thread_join(pthread_t thread) { return pthread_join(thread, NULL); }
WEAK void host_thread_detach(pthread_t thread) { pthread_detach(thread); }
#endif
#ifdef _WIN32
WEAK int runtime_win_remove_tree(const char *path) { (void)path; return 0; }
#endif
