// SPDX-License-Identifier: GPL-3.0-or-later
#include "sys.h"
#include "runtime.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define KERNEL_ERROR(code) ((int32_t)(UINT32_C(0x80020000) | (code)))
enum {
    E_SEARCH = 3,
    E_NO_MEMORY = 12,
    E_ACCESS = 13,
    E_BUSY = 16,
    E_INVALID = 22,
    E_NO_SPACE = 28,
    E_TIMED_OUT = 60,
    E_CANCELLED = 85,
};

typedef struct Waiter {
    int32_t need, result;
    int done;
    HostCond wake;
    struct Waiter *next;
} Waiter;

typedef struct {
    uint32_t id, attr;
    int32_t count, initial, maximum;
    unsigned waiting;
    int deleted;
    char name[32];
    Waiter *queue;
} Semaphore;

static HostMutex lock = HOST_MUTEX_INIT;
static Semaphore **table;
static size_t table_size, table_used, table_tombstones;
static uint32_t last_id;
static size_t created, deleted, acquired, signals, timeouts;
static uint64_t lookups, probes;

#define TOMBSTONE ((Semaphore *)(uintptr_t)1)

static size_t slot_of(uint32_t id, size_t size) {
    return (size_t)(id * UINT32_C(2654435761)) & (size - 1);
}

static int grow_table(void) {
    size_t size = table_size ? table_size * 2 : 64;
    Semaphore **grown = calloc(size, sizeof(*grown));
    if (!grown) return 0;
    for (size_t i = 0; i < table_size; ++i) {
        Semaphore *s = table[i];
        if (!s || s == TOMBSTONE) continue;
        size_t at = slot_of(s->id, size);
        while (grown[at]) at = (at + 1) & (size - 1);
        grown[at] = s;
    }
    free(table);
    table = grown;
    table_size = size;
    table_tombstones = 0;
    return 1;
}

static int insert(Semaphore *s) {
    if ((table_used + table_tombstones + 1) * 4 > table_size * 3 && !grow_table()) return 0;
    size_t at = slot_of(s->id, table_size);
    while (table[at] && table[at] != TOMBSTONE) at = (at + 1) & (table_size - 1);
    if (table[at] == TOMBSTONE) --table_tombstones;
    table[at] = s;
    ++table_used;
    return 1;
}

static size_t locate(uint32_t id) {
    ++lookups;
    if (!table_size) return SIZE_MAX;
    for (size_t at = slot_of(id, table_size);; at = (at + 1) & (table_size - 1)) {
        ++probes;
        if (!table[at]) return SIZE_MAX;
        if (table[at] != TOMBSTONE && table[at]->id == id) return at;
    }
}

static Semaphore *find(uint32_t id) {
    size_t at = locate(id);
    return at == SIZE_MAX ? NULL : table[at];
}

static __attribute__((noreturn)) void stop(const char *why, int code) {
    fprintf(stderr, "STOP: %s %d\n", why, code);
    exit(21);
}

static void init_wake(HostCond *wake) {
#ifdef _WIN32
    host_cond_init(wake);
#else
    pthread_condattr_t attr;
    int e = pthread_condattr_init(&attr);
    if (!e) e = pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    if (!e) e = pthread_cond_init(wake, &attr);
    if (!e) e = pthread_condattr_destroy(&attr);
    if (e) stop("semaphore host error", e);
#endif
}

static void free_wake(HostCond *wake) {
#ifdef _WIN32
    (void)wake;
#else
    int e = pthread_cond_destroy(wake);
    if (e) stop("semaphore host error", e);
#endif
}

static int sleep_on(HostCond *wake, uint64_t deadline, int timed) {
#ifdef _WIN32
    if (!timed) {
        host_cond_wait(wake, &lock);
        return 0;
    }
    uint64_t now = host_monotonic_ns();
    return now < deadline ? host_cond_timedwait(wake, &lock, deadline - now) : ETIMEDOUT;
#else
    if (!timed) return pthread_cond_wait(wake, &lock);
    struct timespec end = {.tv_sec = (time_t)(deadline / 1000000000), .tv_nsec = (long)(deadline % 1000000000)};
    return pthread_cond_timedwait(wake, &lock, &end);
#endif
}

static SYS_ABI int32_t sema_create(uint32_t *id, const char *name, uint32_t attr, int32_t initial, int32_t maximum,
                                   const void *options) {
    if (!id || !name || attr > 2 || initial < 0 || maximum <= 0 || initial > maximum) return KERNEL_ERROR(E_INVALID);
    if (options) {
        fputs("STOP: semaphore options are not implemented\n", stderr);
        exit(21);
    }
    Semaphore *s = calloc(1, sizeof(*s));
    if (!s) return KERNEL_ERROR(E_NO_MEMORY);
    host_lock(&lock);
    if (last_id + 1 == UINT32_MAX) {
        host_unlock(&lock);
        free(s);
        return KERNEL_ERROR(E_NO_SPACE);
    }
    s->id = ++last_id;
    s->attr = attr;
    s->count = s->initial = initial;
    s->maximum = maximum;
    snprintf(s->name, sizeof(s->name), "%s", name);
    if (!insert(s)) {
        --last_id;
        host_unlock(&lock);
        free(s);
        return KERNEL_ERROR(E_NO_MEMORY);
    }
    *id = s->id;
    ++created;
    printf("Runtime: semaphore id=%u, attr=%u, initial=%d, max=%d\n", s->id, attr, initial, maximum);
    host_unlock(&lock);
    return 0;
}

static int32_t block_until_given(Semaphore *s, uint32_t id, int32_t need, uint32_t *timeout) {
    static int trace = -1;
    if (trace < 0) trace = getenv("BB_TRACE_SEMA") != NULL;
    if (trace) fprintf(stderr, "Runtime: blocking wait on semaphore %u (need %d, count %d)\n", id, need, s->count);
    Waiter self = {.need = need};
    init_wake(&self.wake);
    Waiter **tail = &s->queue;
    while (*tail) tail = &(*tail)->next;
    *tail = &self;
    ++s->waiting;
    uint64_t started = runtime_wait_clock();
    int block = runtime_block_begin(RUNTIME_BLOCK_SEMA, id);
    uint64_t deadline = timeout ? host_monotonic_ns() + (uint64_t)*timeout * 1000 : 0;
    while (!self.done) {
        int e = sleep_on(&self.wake, deadline, timeout != NULL);
        if (e == ETIMEDOUT) {
            if (self.done) continue;
            Waiter **link = &s->queue;
            while (*link != &self) link = &(*link)->next;
            *link = self.next;
            self.result = KERNEL_ERROR(E_TIMED_OUT);
            self.done = 1;
            ++timeouts;
        } else if (e) {
            stop("semaphore host error", e);
        }
    }
    runtime_block_end(block);
    runtime_wait_note(RUNTIME_BLOCK_SEMA, runtime_wait_clock() - started);
    if (!self.result) ++acquired;
    if (timeout) {
        uint64_t now = host_monotonic_ns();
        *timeout = self.result ? 0 : (uint32_t)(now >= deadline ? 0 : (deadline - now) / 1000);
    }
    free_wake(&self.wake);
    if (!--s->waiting && s->deleted) free(s);
    return self.result;
}

static int32_t take(uint32_t id, int32_t need, uint32_t *timeout, int may_block) {
    host_lock(&lock);
    Semaphore *s = find(id);
    int32_t result = 0;
    if (!s) {
        result = KERNEL_ERROR(E_SEARCH);
    } else if (need <= 0 || need > s->maximum) {
        result = KERNEL_ERROR(E_INVALID);
    } else if (s->count >= need) {
        s->count -= need;
        ++acquired;
    } else if (!may_block) {
        result = KERNEL_ERROR(E_BUSY);
    } else if (timeout && !*timeout) {
        result = KERNEL_ERROR(E_TIMED_OUT);
        ++timeouts;
    } else {
        result = block_until_given(s, id, need, timeout);
    }
    host_unlock(&lock);
    return result;
}

static SYS_ABI int32_t sema_wait(uint32_t id, int32_t need, uint32_t *timeout) {
    return take(id, need, timeout, 1);
}

static SYS_ABI int32_t sema_poll(uint32_t id, int32_t need) {
    return take(id, need, NULL, 0);
}

static SYS_ABI int32_t sema_signal(uint32_t id, int32_t count) {
    host_lock(&lock);
    Semaphore *s = find(id);
    int32_t result = 0;
    if (!s) {
        result = KERNEL_ERROR(E_SEARCH);
    } else if (count <= 0 || count > s->maximum - s->count) {
        result = KERNEL_ERROR(E_INVALID);
    } else {
        s->count += count;
        ++signals;
        for (Waiter **link = &s->queue; *link;) {
            Waiter *w = *link;
            if (w->need > s->count) {
                link = &w->next;
                continue;
            }
            s->count -= w->need;
            *link = w->next;
            w->done = 1;
            host_cond_signal(&w->wake);
        }
    }
    host_unlock(&lock);
    return result;
}

static int32_t release_all(Semaphore *s, int32_t result) {
    int32_t woken = 0;
    while (s->queue) {
        Waiter *w = s->queue;
        s->queue = w->next;
        w->result = result;
        w->done = 1;
        ++woken;
        host_cond_signal(&w->wake);
    }
    return woken;
}

static SYS_ABI int32_t sema_cancel(uint32_t id, int32_t count, int32_t *waiters) {
    host_lock(&lock);
    Semaphore *s = find(id);
    int32_t result = 0;
    if (!s) {
        result = KERNEL_ERROR(E_SEARCH);
    } else if (count > s->maximum) {
        result = KERNEL_ERROR(E_INVALID);
    } else {
        int32_t woken = release_all(s, KERNEL_ERROR(E_CANCELLED));
        if (waiters) *waiters = woken;
        s->count = count < 0 ? s->initial : count;
    }
    host_unlock(&lock);
    return result;
}

static SYS_ABI int32_t sema_delete(uint32_t id) {
    host_lock(&lock);
    size_t at = locate(id);
    if (at == SIZE_MAX) {
        host_unlock(&lock);
        return KERNEL_ERROR(E_SEARCH);
    }
    Semaphore *s = table[at];
    table[at] = TOMBSTONE;
    --table_used;
    ++table_tombstones;
    s->deleted = 1;
    ++deleted;
    release_all(s, KERNEL_ERROR(E_ACCESS));
    if (!s->waiting) free(s);
    host_unlock(&lock);
    return 0;
}

void sys_sema_describe(uint32_t id, char *out, size_t size) {
    host_lock(&lock);
    Semaphore *s = find(id);
    if (!s) {
        snprintf(out, size, "semaphore %u (deleted)", id);
    } else {
        int n = snprintf(out, size, "semaphore %u '%s', count %d of %d, waiters needing", id, s->name, s->count, s->maximum);
        for (Waiter *w = s->queue; w && n > 0 && (size_t)n < size; w = w->next)
            n += snprintf(out + n, size - (size_t)n, " %d", w->need);
    }
    host_unlock(&lock);
}

unsigned sys_sema_waiters(uint32_t id) {
    host_lock(&lock);
    Semaphore *s = find(id);
    unsigned count = 0;
    for (Waiter *w = s ? s->queue : NULL; w; w = w->next) ++count;
    host_unlock(&lock);
    return count;
}

void sys_sema_census(uint64_t *live, uint64_t *found, uint64_t *steps) {
    host_lock(&lock);
    *live = created - deleted;
    *found = lookups;
    *steps = probes;
    host_unlock(&lock);
}

void sys_sema_report(void) {
    host_lock(&lock);
    printf("Runtime: semaphores created=%zu, deleted=%zu, acquired=%zu, signals=%zu, timeouts=%zu\n", created, deleted,
           acquired, signals, timeouts);
    host_unlock(&lock);
}

static const SysExport exports[] = {
    {"188x57JYp0g", "sceKernelCreateSema", sema_create},
    {"4DM06U2BNEY", "sceKernelCancelSema", sema_cancel},
    {"R1Jvn8bSCW8", "sceKernelDeleteSema", sema_delete},
    {"12wOHk8ywb0", "sceKernelPollSema", sema_poll},
    {"4czppHBiriw", "sceKernelSignalSema", sema_signal},
    {"Zxa0VhQVTsk", "sceKernelWaitSema", sema_wait},
};

SYS_LIBRARY(sys_semaphore, "libkernel", exports);
