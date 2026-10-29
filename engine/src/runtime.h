#ifndef BB_RUNTIME_H
#define BB_RUNTIME_H
#include <time.h>
#include <stdint.h>
#include <stddef.h>
#ifdef _WIN32
#include "win32_compat.h"
#endif
#include "host_sync.h"
#ifdef _WIN32
typedef struct { _Alignas(16) unsigned char registers[256]; } RuntimeRecoverBuf;
int runtime_setjmp(RuntimeRecoverBuf *buf) __attribute__((returns_twice));
__attribute__((noreturn)) void runtime_longjmp(RuntimeRecoverBuf *buf);
#define RUNTIME_RECOVER_SET(buf) runtime_setjmp(&(buf))
#define RUNTIME_RECOVER_JUMP(buf) runtime_longjmp(&(buf))
#else
#include <setjmp.h>
typedef sigjmp_buf RuntimeRecoverBuf;
#define RUNTIME_RECOVER_SET(buf) sigsetjmp(buf, 0)
#define RUNTIME_RECOVER_JUMP(buf) siglongjmp(buf, 1)
#endif
extern __thread RuntimeRecoverBuf *runtime_fault_recover;
void runtime_restart(void);
#define ABI __attribute__((sysv_abi))
typedef void (ABI *GuestCallback)(void);
void runtime_start(uint64_t capabilities);
uintptr_t runtime_resolve(const char *name, int is_data);
void runtime_report(void);
void runtime_finalize(void *dso);
uintptr_t runtime_mutex_resolve(const char *name);
void runtime_mutex_report(void);
uintptr_t runtime_memory_resolve(const char *name);
void runtime_memory_report(void);
uint64_t runtime_memory_regions(void);
int runtime_memory_is_mapped(uintptr_t address, uint64_t size);
int runtime_memory_guest_writable(uintptr_t address, uint64_t size);
int runtime_memory_restore_access(void *context, uintptr_t address, int repeated);
void runtime_memory_note_write(uintptr_t address, uint64_t size);
void runtime_memory_note_cpu_write(uintptr_t address, uint64_t size);
void runtime_wait_note(int kind, uint64_t ns);
void runtime_wait_report(double frames);
void runtime_guest_call_sites(uint64_t out[3]);
uint64_t runtime_heap_growths(void);
static inline uint64_t runtime_wait_clock(void) { return host_monotonic_ns(); }
const char *runtime_import_name(const char *name);
void runtime_set_libc_tls(const void *data, uint64_t filesz, uint64_t memsz);
void runtime_set_module_tls(uint64_t module, const void *data, uint64_t filesz, uint64_t memsz);
void runtime_set_procparam(void *param);
void **runtime_application_heap_api(void);
uintptr_t runtime_thread_resolve(const char *name);
void runtime_thread_report(void);
void runtime_thread_census(uint64_t *made, uint64_t *finished, uint64_t *found, uint64_t *steps);
void runtime_set_main_tls(const void *data, uint64_t filesz, uint64_t memsz, uint64_t align);
void runtime_thread_attach_main(void);
int runtime_thread_has_record(void);
int runtime_thread_is_loader(void);
int32_t *runtime_errno(void);
enum { RUNTIME_BLOCK_MUTEX = 1, RUNTIME_BLOCK_SEMA = 2, RUNTIME_BLOCK_COND = 3 };
int runtime_block_begin(int kind, uintptr_t object);
void runtime_block_end(int slot);
void runtime_blocked_report(void);
void runtime_mutex_describe(uintptr_t mutex, char *out, size_t size);
typedef struct { const char *name; void *function; } RuntimeExport;
#define RUNTIME_LOOKUP(table, nid) runtime_lookup(table, sizeof(table)/sizeof(*(table)), nid)
const char *runtime_symbol(const char *nid);
uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *nid);
uintptr_t runtime_kernel_resolve(const char *name);
uintptr_t runtime_file_resolve(const char *name);
uintptr_t runtime_services_resolve(const char *name);
void runtime_file_report(void);
void runtime_file_stream_report(void);
void runtime_file_configure(const char *app0, const char *user);
int runtime_file_mount(const char *guest, const char *host);
void runtime_file_unmount(const char *guest);
int runtime_file_translate(const char *guest, char *out, size_t size);
int64_t runtime_file_open(const char *path, int flags, int mode);
int64_t runtime_file_close(int fd);
int64_t runtime_file_read(int fd, void *buffer, uint64_t size);
int64_t runtime_file_pread(int fd, void *buffer, uint64_t size, int64_t offset);
int64_t runtime_file_write(int fd, const void *buffer, uint64_t size);
int64_t runtime_file_lseek(int fd, int64_t offset, int whence);
int64_t runtime_file_stat(const char *path, void *guest_stat);
int64_t runtime_file_fstat(int fd, void *guest_stat);
int64_t runtime_file_getdents(int fd, char *buffer, uint64_t size, int64_t *base);
void runtime_thread_keys_cleanup(void);
int32_t runtime_guest_errno(int host_errno);
void *runtime_low_map(size_t size, int prot);
const char *runtime_file_user_dir(void);
void runtime_thread_attach_host(const char *name);
#endif
