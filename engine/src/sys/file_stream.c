// SPDX-License-Identifier: GPL-3.0-or-later
#include "sys.h"
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <winioctl.h>
#endif

typedef struct {
    uint64_t reads, bytes, read_ns, read_longest_ns, fast_reads, opens, open_ns, requests, request_ns,
        request_longest_ns, other_bytes, prefetch_bytes, prefetch_files, prefetch_folders, window_start, in_flight;
} StreamCounters;

static StreamCounters stream;
static int on_hard_disk = -1, sequential_hint, prefetch_on;
static FILE *trace_file;
static HostMutex trace_lock = HOST_MUTEX_INIT;
static unsigned trace_lines;
static uint64_t last_loader_read;

static uint64_t take(uint64_t *counter) {
    return __atomic_exchange_n(counter, 0, __ATOMIC_RELAXED);
}

static void add(uint64_t *counter, uint64_t value) {
    __atomic_add_fetch(counter, value, __ATOMIC_RELAXED);
}

static void raise_to(uint64_t *counter, uint64_t value) {
    uint64_t current = __atomic_load_n(counter, __ATOMIC_RELAXED);
    while (value > current && !__atomic_compare_exchange_n(counter, &current, value, 1, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
}

#ifdef _WIN32
enum { PREFETCH_QUEUE = 32, PREFETCH_SEEN = 1024, PREFETCH_CHUNK = 1 << 20, PREFETCH_PATH = 520 };
#define PREFETCH_IDLE_NS UINT64_C(500000000)

static HostMutex prefetch_lock = HOST_MUTEX_INIT;
static HostCond prefetch_wake = HOST_COND_INIT;
static char prefetch_queue[PREFETCH_QUEUE][PREFETCH_PATH];
static unsigned prefetch_head, prefetch_tail, prefetch_seen_count;
static uint64_t prefetch_seen[PREFETCH_SEEN], prefetch_folder_limit, prefetch_file_limit;
static int prefetch_started;

static int switch_setting(const char *name, int automatic) {
    const char *setting = getenv(name);
    if (setting && setting[0] == '0') return 0;
    if (setting && setting[0] == '1') return 1;
    return automatic;
}

static uint64_t megabytes_setting(const char *name, uint64_t fallback) {
    const char *setting = getenv(name);
    return (setting && *setting ? strtoull(setting, NULL, 10) : fallback) << 20;
}

static int seek_penalty(const char *path) {
    typedef struct { DWORD property, query; BYTE extra[4]; } Query;
    typedef struct { DWORD version, size; BOOLEAN seek_penalty; } Answer;
    char volume[MAX_PATH];
    if (!GetVolumePathNameA(path, volume, sizeof(volume))) return -1;
    size_t length = strlen(volume);
    if (length && (volume[length - 1] == '\\' || volume[length - 1] == '/')) volume[--length] = 0;
    if (length != 2 || volume[1] != ':') return -1;
    const char device[] = {'\\', '\\', '.', '\\', volume[0], ':', 0};
    HANDLE handle = CreateFileA(device, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (handle == INVALID_HANDLE_VALUE) return -1;
    Query query = {7, 0, {0}};
    Answer answer = {0, 0, 0};
    DWORD got = 0;
    BOOL ok = DeviceIoControl(handle, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query), &answer, sizeof(answer), &got, NULL);
    CloseHandle(handle);
    return ok && got > offsetof(Answer, seek_penalty) ? answer.seek_penalty != 0 : -1;
}

static void prefetch_one_file(const char *path, unsigned char *buffer) {
    HANDLE file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                              OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (file == INVALID_HANDLE_VALUE) return;
    for (;;) {
        while (__atomic_load_n(&stream.in_flight, __ATOMIC_RELAXED) ||
               host_monotonic_ns() - __atomic_load_n(&last_loader_read, __ATOMIC_RELAXED) < PREFETCH_IDLE_NS)
            Sleep(20);
        DWORD got = 0;
        if (!ReadFile(file, buffer, PREFETCH_CHUNK, &got, NULL) || !got) break;
        add(&stream.prefetch_bytes, got);
        if (got < PREFETCH_CHUNK) break;
    }
    CloseHandle(file);
    add(&stream.prefetch_files, 1);
}

static void prefetch_folder(const char *folder, unsigned char *buffer) {
    char pattern[600];
    snprintf(pattern, sizeof(pattern), "%s\\*", folder);
    WIN32_FIND_DATAA entry;
    HANDLE find = FindFirstFileA(pattern, &entry);
    if (find == INVALID_HANDLE_VALUE) return;
    const DWORD skipped = FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_OFFLINE | 0x00400000;
    uint64_t total = 0;
    do {
        if (entry.dwFileAttributes & skipped) continue;
        const uint64_t size = ((uint64_t)entry.nFileSizeHigh << 32) | entry.nFileSizeLow;
        if (!size || size > prefetch_file_limit) continue;
        if (total + size > prefetch_folder_limit) break;
        char path[900];
        snprintf(path, sizeof(path), "%s\\%s", folder, entry.cFileName);
        prefetch_one_file(path, buffer);
        total += size;
    } while (FindNextFileA(find, &entry));
    FindClose(find);
    add(&stream.prefetch_folders, 1);
}

static void *prefetch_thread(void *unused) {
    (void)unused;
    runtime_win_set_thread_name("bb:Prefetch");
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    typedef BOOL (WINAPI *SetInformation)(HANDLE, int, LPVOID, DWORD);
    SetInformation set_information = (SetInformation)(void *)GetProcAddress(GetModuleHandleA("kernel32.dll"), "SetThreadInformation");
    if (set_information) {
        ULONG low_memory_priority = 2;
        set_information(GetCurrentThread(), 0, &low_memory_priority, sizeof(low_memory_priority));
    }
    unsigned char *buffer = malloc(PREFETCH_CHUNK);
    if (!buffer) return NULL;
    char folder[PREFETCH_PATH];
    for (;;) {
        host_lock(&prefetch_lock);
        while (prefetch_head == prefetch_tail) host_cond_wait(&prefetch_wake, &prefetch_lock);
        memcpy(folder, prefetch_queue[prefetch_head % PREFETCH_QUEUE], sizeof(folder));
        ++prefetch_head;
        host_unlock(&prefetch_lock);
        prefetch_folder(folder, buffer);
    }
    return NULL;
}

static void prefetch_note(const char *guest, const char *host) {
    const char *map = strstr(guest, "/map/m");
    if (!map || !strchr(map + 5, '/')) return;
    const char *slash = strrchr(host, '/'), *backslash = strrchr(host, '\\');
    if (backslash > slash) slash = backslash;
    if (!slash || (size_t)(slash - host) >= PREFETCH_PATH) return;
    uint64_t hash = UINT64_C(0xcbf29ce484222325);
    for (const char *p = host; p < slash; ++p) hash = (hash ^ (unsigned char)(*p == '/' ? '\\' : *p)) * UINT64_C(0x100000001b3);
    host_lock(&prefetch_lock);
    for (unsigned i = 0; i < prefetch_seen_count; ++i) {
        if (prefetch_seen[i] == hash) {
            host_unlock(&prefetch_lock);
            return;
        }
    }
    if (prefetch_seen_count == PREFETCH_SEEN || prefetch_tail - prefetch_head == PREFETCH_QUEUE) {
        host_unlock(&prefetch_lock);
        return;
    }
    prefetch_seen[prefetch_seen_count++] = hash;
    char *slot = prefetch_queue[prefetch_tail % PREFETCH_QUEUE];
    memcpy(slot, host, (size_t)(slash - host));
    slot[slash - host] = 0;
    for (char *p = slot; *p; ++p) if (*p == '/') *p = '\\';
    ++prefetch_tail;
    int start = !prefetch_started;
    prefetch_started = 1;
    host_cond_signal(&prefetch_wake);
    host_unlock(&prefetch_lock);
    if (start) {
        HostThread thread;
        if (!host_thread_start(&thread, 256 * 1024, prefetch_thread, NULL)) host_thread_detach(thread);
    }
}
#else
static int seek_penalty(const char *path) {
    (void)path;
    return -1;
}

static void prefetch_note(const char *guest, const char *host) {
    (void)guest;
    (void)host;
}
#endif

void sys_file_trace(char op, int fd, int64_t a, int64_t b, int64_t c, const char *path) {
    if (!trace_file) return;
    host_lock(&trace_lock);
    fprintf(trace_file, "%llu %llu %c %d %lld %lld %lld %s\n", (unsigned long long)host_monotonic_ns(),
            (unsigned long long)host_thread_id(), op, fd, (long long)a, (long long)b, (long long)c, path ? path : "-");
    if (!(++trace_lines & 255)) fflush(trace_file);
    host_unlock(&trace_lock);
}

void sys_file_stream_configure(const char *app0) {
    on_hard_disk = seek_penalty(app0);
    const char *trace = getenv("BB_FILE_TRACE");
    if (trace && *trace) trace_file = fopen(trace, "w");
#ifdef _WIN32
    sequential_hint = switch_setting("BB_FILE_SEQUENTIAL", on_hard_disk == 1);
    prefetch_on = switch_setting("BB_FILE_PREFETCH", on_hard_disk == 1);
    prefetch_folder_limit = megabytes_setting("BB_FILE_PREFETCH_MB", 1024);
    prefetch_file_limit = megabytes_setting("BB_FILE_PREFETCH_FILE_MB", 32);
#endif
    stream.window_start = host_monotonic_ns();
    printf("Streaming: game files on %s; read-ahead hint %s (BB_FILE_SEQUENTIAL), map area prefetch %s (BB_FILE_PREFETCH)\n",
           on_hard_disk == 1 ? "a hard disk (seek penalty): models and textures load late while walking; an SSD loads them several times faster"
           : on_hard_disk == 0 ? "a solid-state disk" : "a disk of unknown type",
           sequential_hint ? "on" : "off", prefetch_on ? "on" : "off");
}

int sys_file_stream_sequential(void) {
    return sequential_hint;
}

void sys_file_stream_opened(const char *guest, const char *host, uint64_t started) {
    add(&stream.opens, 1);
    add(&stream.open_ns, host_monotonic_ns() - started);
    if (prefetch_on) prefetch_note(guest, host);
}

void sys_file_stream_closed(uint64_t opened) {
    const uint64_t ns = host_monotonic_ns() - opened;
    add(&stream.requests, 1);
    add(&stream.request_ns, ns);
    raise_to(&stream.request_longest_ns, ns);
}

uint64_t sys_file_stream_read_begin(int loader) {
    if (!loader) return 0;
    add(&stream.in_flight, 1);
    return host_monotonic_ns();
}

void sys_file_stream_read_end(int loader, uint64_t started, int64_t bytes) {
    if (!loader) {
        if (bytes > 0) add(&stream.other_bytes, (uint64_t)bytes);
        return;
    }
    const uint64_t ns = host_monotonic_ns() - started;
    __atomic_sub_fetch(&stream.in_flight, 1, __ATOMIC_RELAXED);
    __atomic_store_n(&last_loader_read, started + ns, __ATOMIC_RELAXED);
    add(&stream.reads, 1);
    if (bytes > 0) add(&stream.bytes, (uint64_t)bytes);
    add(&stream.read_ns, ns);
    if (ns < 1000000) add(&stream.fast_reads, 1);
    raise_to(&stream.read_longest_ns, ns);
}

void sys_file_stream_report(void) {
    if (trace_file) {
        host_lock(&trace_lock);
        fflush(trace_file);
        host_unlock(&trace_lock);
    }
    const uint64_t now = host_monotonic_ns();
    const uint64_t start = __atomic_exchange_n(&stream.window_start, now, __ATOMIC_RELAXED);
    const double window = start && now > start ? (double)(now - start) / 1e9 : 5.0;
    const uint64_t reads = take(&stream.reads), bytes = take(&stream.bytes), read_ns = take(&stream.read_ns),
                   read_longest = take(&stream.read_longest_ns), fast = take(&stream.fast_reads),
                   opens = take(&stream.opens), open_ns = take(&stream.open_ns), requests = take(&stream.requests),
                   request_ns = take(&stream.request_ns), request_longest = take(&stream.request_longest_ns),
                   other = take(&stream.other_bytes), prefetched = take(&stream.prefetch_bytes),
                   prefetched_files = take(&stream.prefetch_files);
    if (!reads && !opens && !prefetched) return;
    printf("Streaming: loaders read %.1f MB/s (%llu reads, %.0f KiB avg, %.2f ms avg, longest %.1f ms, %.0f%% under 1 ms; "
           "reading %.0f%% of the time); %llu files opened (%.2f ms to open), %llu closed %.1f ms after opening (longest %.0f ms); "
           "other threads %.1f MB/s; prefetch %.1f MB/s (%llu files, %llu folders so far)%s\n",
           (double)bytes / (window * 1e6), (unsigned long long)reads, reads ? (double)bytes / 1024.0 / (double)reads : 0.0,
           reads ? (double)read_ns / 1e6 / (double)reads : 0.0, (double)read_longest / 1e6,
           reads ? 100.0 * (double)fast / (double)reads : 0.0, 100.0 * (double)read_ns / (window * 1e9),
           (unsigned long long)opens, opens ? (double)open_ns / 1e6 / (double)opens : 0.0,
           (unsigned long long)requests, requests ? (double)request_ns / 1e6 / (double)requests : 0.0, (double)request_longest / 1e6,
           (double)other / (window * 1e6), (double)prefetched / (window * 1e6), (unsigned long long)prefetched_files,
           (unsigned long long)__atomic_load_n(&stream.prefetch_folders, __ATOMIC_RELAXED),
           on_hard_disk == 1 ? "; game on a hard disk" : "");
}
