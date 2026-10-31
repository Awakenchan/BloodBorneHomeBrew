// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstdlib>
#include <chrono>
#include <cstdint>
#include <cstddef>
#include <setjmp.h>
#ifdef _WIN32
struct alignas(16) RuntimeRecoverBuf {
    unsigned char registers[256];
};
#else
typedef sigjmp_buf RuntimeRecoverBuf;
#endif
extern "C" {
void* runtime_low_map(size_t, int) { return nullptr; }
int runtime_file_mount(const char*, const char*) { return -1; }
void runtime_file_unmount(const char*) {}
__thread RuntimeRecoverBuf* runtime_fault_recover = nullptr;
int runtime_setjmp(RuntimeRecoverBuf*) { return 0; }
[[noreturn]] void runtime_longjmp(RuntimeRecoverBuf*) { std::abort(); }
uint32_t runtime_disabled_optimizations = 0;
uint64_t runtime_experiment_bits = 0;
uint64_t runtime_tsc_frequency() { return 1000000000; }
int runtime_file_translate(const char*, char*, size_t) { std::abort(); }
uint64_t runtime_memory_clamp(uintptr_t, uint64_t) { std::abort(); }
uint64_t runtime_process_time_us() { return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
int runtime_memory_region(uintptr_t, uintptr_t*, uintptr_t*, int*) { std::abort(); }
void runtime_memory_set_gpu_hooks(void (*)(uintptr_t, uint64_t),
    void (*)(uintptr_t, uint64_t), void (*)(uintptr_t, uint64_t)) { std::abort(); }
void runtime_thread_attach_host(const char*) {}
int32_t runtime_thread_spawn(void**, void* (*)(void*), void*, uint64_t, const char*) { std::abort(); }
int32_t runtime_thread_join_spawned(void*, void**) { std::abort(); }
[[noreturn]] void runtime_thread_exit(void*) { std::abort(); }
void runtime_blocked_report() {}
int runtime_pad_type() { return 0; }
int runtime_pad_button(uint32_t, int) { return 0; }
void runtime_memory_gpu_protect(uintptr_t, uint64_t, int, int) { std::abort(); }
void runtime_restart() { std::abort(); }
uint64_t runtime_process_time_counter() { return runtime_process_time_us() * 1000; }
int32_t* runtime_errno() { std::abort(); }
int runtime_memory_write_backing(uintptr_t, const void*, uint64_t) { std::abort(); }
void runtime_memory_read_backing(uintptr_t, void*, uint64_t) { std::abort(); }
const uint64_t* runtime_memory_generation() { static const uint64_t generation = 0; return &generation; }
void runtime_memory_note_write(uintptr_t, uint64_t) { std::abort(); }
void runtime_memory_set_note_write_hook(void (*)(uintptr_t, uint64_t)) {}
void runtime_memory_set_cpu_write_hook(void (*)(uintptr_t, uint64_t)) {}
void runtime_memory_set_write_watch(uintptr_t, uint64_t, int) { std::abort(); }
int runtime_memory_restore_access(void*, uintptr_t, int) { return 0; }
void runtime_memory_set_guest_chunk_allocator(int (*)(uint64_t, uint64_t)) {}
void runtime_memory_set_guest_chunk_whole(int) {}
unsigned char* runtime_memory_backing(uint64_t* direct_size) {
    *direct_size = 0;
    return nullptr;
}
#ifdef _WIN32
int win_mem_commit(uint64_t, uint64_t) { return 0; }
#endif
int runtime_memory_vma_info(uintptr_t, int*, int*, uintptr_t*) { std::abort(); }
int runtime_memory_direct_phys(uintptr_t, uint64_t*, uintptr_t*) { std::abort(); }
void runtime_wait_report(double) {}
void runtime_sleep_stats(uint64_t* calls, uint64_t* ns) { *calls = 0; *ns = 0; }
uint64_t runtime_heap_growths() { return 0; }
void runtime_rwlock_census(uint64_t* live, uint64_t* found, uint64_t* steps) { *live = *found = *steps = 0; }
void runtime_sema_census(uint64_t* live, uint64_t* found, uint64_t* steps) { *live = *found = *steps = 0; }
void runtime_thread_census(uint64_t* made, uint64_t* finished, uint64_t* found, uint64_t* steps) {
    *made = *finished = *found = *steps = 0;
}
uint64_t runtime_memory_regions() { return 0; }
}
