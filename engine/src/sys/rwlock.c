// SPDX-License-Identifier: GPL-3.0-or-later
#include "sys.h"
#include "runtime.h"
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
    int64_t seconds, nanoseconds;
} GuestTime;

#ifdef _WIN32
typedef SRWLOCK NativeLock;

static int native_init(NativeLock *l) {
    InitializeSRWLock(l);
    return 0;
}

static int native_destroy(NativeLock *l) {
    (void)l;
    return 0;
}

static int native_try(NativeLock *l, int writer) {
    return (writer ? TryAcquireSRWLockExclusive(l) : TryAcquireSRWLockShared(l)) ? 0 : EBUSY;
}

static int native_lock(NativeLock *l, int writer) {
    if (writer) AcquireSRWLockExclusive(l);
    else AcquireSRWLockShared(l);
    return 0;
}

static int native_lock_until(NativeLock *l, int writer, const GuestTime *time) {
    uint64_t deadline = (uint64_t)time->seconds * 1000000000 + (uint64_t)time->nanoseconds;
    for (unsigned attempt = 0;; ++attempt) {
        if (!native_try(l, writer)) return 0;
        if (host_realtime_ns() >= deadline) return ETIMEDOUT;
        if (attempt < 64) YieldProcessor();
        else if (attempt < 128) SwitchToThread();
        else Sleep(1);
    }
}

static int native_unlock(NativeLock *l, int writer) {
    if (writer) ReleaseSRWLockExclusive(l);
    else ReleaseSRWLockShared(l);
    return 0;
}
#else
typedef pthread_rwlock_t NativeLock;

static int native_init(NativeLock *l) {
    return pthread_rwlock_init(l, NULL);
}

static int native_destroy(NativeLock *l) {
    return pthread_rwlock_destroy(l);
}

static int native_try(NativeLock *l, int writer) {
    return writer ? pthread_rwlock_trywrlock(l) : pthread_rwlock_tryrdlock(l);
}

static int native_lock(NativeLock *l, int writer) {
    return writer ? pthread_rwlock_wrlock(l) : pthread_rwlock_rdlock(l);
}

static int native_lock_until(NativeLock *l, int writer, const GuestTime *time) {
    struct timespec deadline = {.tv_sec = (time_t)time->seconds, .tv_nsec = (long)time->nanoseconds};
    return writer ? pthread_rwlock_timedwrlock(l, &deadline) : pthread_rwlock_timedrdlock(l, &deadline);
}

static int native_unlock(NativeLock *l, int writer) {
    (void)writer;
    return pthread_rwlock_unlock(l);
}
#endif

typedef struct Holder {
    HostThreadId thread;
    unsigned readers, writer, pending;
    struct Holder *next;
} Holder;

typedef struct {
    NativeLock native;
    Holder *holders;
    unsigned in_flight;
} Rwlock;

#define TOMBSTONE ((Rwlock *)(uintptr_t)1)
#define DESTROYED_HANDLE ((Rwlock *)(uintptr_t)1)

static HostMutex lock = HOST_MUTEX_INIT;
static Rwlock **table;
static size_t table_size, table_used, table_tombstones;
static size_t created, destroyed, reads, writes, unlocks;
static uint64_t lookups, probes;

static int32_t guest_error(int e) {
    if (!e) return 0;
    unsigned code;
    switch (e) {
    case EPERM: code = 1; break;
    case EDEADLK: code = 11; break;
    case ENOMEM: code = 12; break;
    case EBUSY: code = 16; break;
    case EINVAL: code = 22; break;
    case EAGAIN: code = 35; break;
    case ETIMEDOUT: code = 60; break;
    default:
        fprintf(stderr, "STOP: unmapped rwlock error %d\n", e);
        exit(21);
    }
    return (int32_t)(UINT32_C(0x80020000) | code);
}

static size_t slot_of(const Rwlock *r, size_t size) {
    uintptr_t v = (uintptr_t)r >> 4;
    return (size_t)((v * UINT64_C(0x9E3779B97F4A7C15)) >> 20) & (size - 1);
}

static int grow_table(void) {
    size_t size = table_size ? table_size * 2 : 64;
    Rwlock **grown = calloc(size, sizeof(*grown));
    if (!grown) return 0;
    for (size_t i = 0; i < table_size; ++i) {
        Rwlock *r = table[i];
        if (!r || r == TOMBSTONE) continue;
        size_t at = slot_of(r, size);
        while (grown[at]) at = (at + 1) & (size - 1);
        grown[at] = r;
    }
    free(table);
    table = grown;
    table_size = size;
    table_tombstones = 0;
    return 1;
}

static size_t locate(const Rwlock *r) {
    ++lookups;
    if (!table_size || r == TOMBSTONE) return SIZE_MAX;
    for (size_t at = slot_of(r, table_size);; at = (at + 1) & (table_size - 1)) {
        ++probes;
        if (!table[at]) return SIZE_MAX;
        if (table[at] == r) return at;
    }
}

static int create(Rwlock **handle) {
    if ((table_used + table_tombstones + 1) * 4 > table_size * 3 && !grow_table()) return ENOMEM;
    Rwlock *r = calloc(1, sizeof(*r));
    if (!r) return ENOMEM;
    int e = native_init(&r->native);
    if (e) {
        free(r);
        return e;
    }
    size_t at = slot_of(r, table_size);
    while (table[at] && table[at] != TOMBSTONE) at = (at + 1) & (table_size - 1);
    if (table[at] == TOMBSTONE) --table_tombstones;
    table[at] = r;
    ++table_used;
    *handle = r;
    ++created;
    return 0;
}

static Holder *holder_for_this_thread(Rwlock *r) {
    HostThreadId me = host_thread_id();
    Holder *h = r->holders;
    while (h && !host_thread_equal(h->thread, me)) h = h->next;
    return h;
}

static void forget_idle_holder(Rwlock *r, Holder *h) {
    if (h->readers || h->writer || h->pending) return;
    Holder **link = &r->holders;
    while (*link != h) link = &(*link)->next;
    *link = h->next;
    free(h);
}

static SYS_ABI int32_t rw_init(Rwlock **handle, void *const *attr, const char *name) {
    (void)name;
    if (!handle || (attr && !*attr)) return guest_error(EINVAL);
    if (attr) {
        fputs("STOP: non-default PS4 rwlock attributes are not implemented\n", stderr);
        exit(21);
    }
    host_lock(&lock);
    int e = create(handle);
    host_unlock(&lock);
    return guest_error(e);
}

enum { MODE_WAIT, MODE_TRY, MODE_TIMED };

static int32_t acquire(Rwlock **handle, int writer, int mode, const GuestTime *time) {
    if (!handle) return guest_error(EINVAL);
    host_lock(&lock);
    int e = *handle ? 0 : create(handle);
    size_t at = e ? SIZE_MAX : locate(*handle);
    if (at == SIZE_MAX) {
        host_unlock(&lock);
        return guest_error(e ? e : EINVAL);
    }
    Rwlock *r = table[at];
    Holder *h = holder_for_this_thread(r);
    if (h && (h->writer || (writer && h->readers))) {
        host_unlock(&lock);
        return guest_error(mode == MODE_TRY ? EBUSY : EDEADLK);
    }
    if (h && h->readers == UINT_MAX) {
        host_unlock(&lock);
        return guest_error(EAGAIN);
    }
    if (!h) {
        h = calloc(1, sizeof(*h));
        if (!h) {
            host_unlock(&lock);
            return guest_error(ENOMEM);
        }
        h->thread = host_thread_id();
        h->next = r->holders;
        r->holders = h;
    }
    ++r->in_flight;
    ++h->pending;
    host_unlock(&lock);
    e = mode == MODE_WAIT ? native_lock(&r->native, writer) : native_try(&r->native, writer);
    if (mode == MODE_TIMED && e == EBUSY) {
        if (!time || time->nanoseconds < 0 || time->nanoseconds >= 1000000000 || time->seconds < 0) e = EINVAL;
        else e = native_lock_until(&r->native, writer, time);
    }
    host_lock(&lock);
    --r->in_flight;
    --h->pending;
    if (!e) {
        if (writer) {
            h->writer = 1;
            ++writes;
        } else {
            ++h->readers;
            ++reads;
        }
    }
    forget_idle_holder(r, h);
    host_unlock(&lock);
    return guest_error(e);
}

static SYS_ABI int32_t rw_rdlock(Rwlock **h) { return acquire(h, 0, MODE_WAIT, NULL); }
static SYS_ABI int32_t rw_wrlock(Rwlock **h) { return acquire(h, 1, MODE_WAIT, NULL); }
static SYS_ABI int32_t rw_tryrdlock(Rwlock **h) { return acquire(h, 0, MODE_TRY, NULL); }
static SYS_ABI int32_t rw_trywrlock(Rwlock **h) { return acquire(h, 1, MODE_TRY, NULL); }
static SYS_ABI int32_t rw_timedrdlock(Rwlock **h, const GuestTime *t) { return acquire(h, 0, MODE_TIMED, t); }
static SYS_ABI int32_t rw_timedwrlock(Rwlock **h, const GuestTime *t) { return acquire(h, 1, MODE_TIMED, t); }

static SYS_ABI int32_t rw_unlock(Rwlock **handle) {
    if (!handle) return guest_error(EINVAL);
    host_lock(&lock);
    size_t at = locate(*handle);
    if (at == SIZE_MAX) {
        host_unlock(&lock);
        return guest_error(EINVAL);
    }
    Rwlock *r = table[at];
    Holder *h = holder_for_this_thread(r);
    if (!h || (!h->readers && !h->writer)) {
        host_unlock(&lock);
        return guest_error(EPERM);
    }
    int e = native_unlock(&r->native, h->writer);
    if (!e) {
        if (h->writer) h->writer = 0;
        else --h->readers;
        ++unlocks;
    }
    forget_idle_holder(r, h);
    host_unlock(&lock);
    return guest_error(e);
}

static SYS_ABI int32_t rw_destroy(Rwlock **handle) {
    if (!handle) return guest_error(EINVAL);
    host_lock(&lock);
    if (!*handle) {
        host_unlock(&lock);
        return 0;
    }
    size_t at = locate(*handle);
    if (at == SIZE_MAX) {
        host_unlock(&lock);
        return guest_error(EINVAL);
    }
    Rwlock *r = table[at];
    if (r->in_flight || r->holders) {
        host_unlock(&lock);
        return guest_error(EBUSY);
    }
    int e = native_destroy(&r->native);
    if (!e) {
        table[at] = TOMBSTONE;
        --table_used;
        ++table_tombstones;
        free(r);
        *handle = DESTROYED_HANDLE;
        ++destroyed;
    }
    host_unlock(&lock);
    return guest_error(e);
}

void sys_rwlock_census(uint64_t *live, uint64_t *found, uint64_t *steps) {
    host_lock(&lock);
    *live = created - destroyed;
    *found = lookups;
    *steps = probes;
    host_unlock(&lock);
}

void sys_rwlock_report(void) {
    host_lock(&lock);
    printf("Runtime: rwlocks created=%zu, destroyed=%zu, reads=%zu, writes=%zu, unlocks=%zu\n", created, destroyed, reads,
           writes, unlocks);
    host_unlock(&lock);
}

static const SysExport exports[] = {
    {"BB+kb08Tl9A", "scePthreadRwlockDestroy", rw_destroy},
    {"6ULAa0fq4jA", "scePthreadRwlockInit", rw_init},
    {"Ox9i0c7L5w0", "scePthreadRwlockRdlock", rw_rdlock},
    {"iPtZRWICjrM", "scePthreadRwlockTimedrdlock", rw_timedrdlock},
    {"adh--6nIqTk", "scePthreadRwlockTimedwrlock", rw_timedwrlock},
    {"XD3mDeybCnk", "scePthreadRwlockTryrdlock", rw_tryrdlock},
    {"bIHoZCTomsI", "scePthreadRwlockTrywrlock", rw_trywrlock},
    {"+L98PIbGttk", "scePthreadRwlockUnlock", rw_unlock},
    {"mqdNorrB+gI", "scePthreadRwlockWrlock", rw_wrlock},
};

SYS_LIBRARY(sys_rwlock, "libkernel", exports);
