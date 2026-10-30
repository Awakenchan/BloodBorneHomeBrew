// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "sys.h"
#include "runtime.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifndef _WIN32
#include <sys/syscall.h>
#include <unistd.h>
#endif

#define KERNEL_ERROR(code) ((int32_t)(UINT32_C(0x80020000) | (code)))
#define DESTROYED_HANDLE 2
#define DESTROYED_COND UINT32_C(0x80000000)

enum { TYPE_ERRORCHECK = 1, TYPE_RECURSIVE = 2, TYPE_NORMAL = 3, TYPE_ADAPTIVE = 4 };
enum { MODE_WAIT, MODE_TRY, MODE_TIMED };

static __attribute__((noreturn)) void stop(const char *why, int code) {
    fprintf(stderr, "STOP: %s %d\n", why, code);
    exit(21);
}

#ifdef _WIN32
typedef SRWLOCK RawLock;
typedef CONDITION_VARIABLE RawCond;

static void raw_init(RawLock *l) {
    InitializeSRWLock(l);
}

static void raw_free(RawLock *l) {
    (void)l;
}

static int raw_try(RawLock *l) {
    return TryAcquireSRWLockExclusive(l) != 0;
}

static void raw_lock(RawLock *l) {
    AcquireSRWLockExclusive(l);
}

static int raw_lock_until(RawLock *l, uint64_t deadline) {
    for (unsigned attempt = 0;; ++attempt) {
        if (TryAcquireSRWLockExclusive(l)) return 0;
        if (host_realtime_ns() >= deadline) return ETIMEDOUT;
        if (attempt < 64) YieldProcessor();
        else if (attempt < 128) SwitchToThread();
        else Sleep(1);
    }
}

static void raw_unlock(RawLock *l) {
    ReleaseSRWLockExclusive(l);
}

static void raw_cond_init(RawCond *c) {
    InitializeConditionVariable(c);
}

static void raw_cond_free(RawCond *c) {
    (void)c;
}

static int raw_cond_wait(RawCond *c, RawLock *l, uint64_t deadline) {
    DWORD ms = INFINITE;
    if (deadline) {
        uint64_t now = host_realtime_ns();
        if (now >= deadline) return ETIMEDOUT;
        uint64_t left = (deadline - now + 999999) / 1000000;
        ms = left >= INFINITE ? INFINITE - 1 : (DWORD)left;
    }
    if (SleepConditionVariableSRW(c, l, ms, 0)) return 0;
    return GetLastError() == ERROR_TIMEOUT ? ETIMEDOUT : EINVAL;
}

static void raw_signal(RawCond *c, int all) {
    if (all) WakeAllConditionVariable(c);
    else WakeConditionVariable(c);
}

static uint32_t thread_self(void) {
    return (uint32_t)GetCurrentThreadId();
}
#else
typedef pthread_mutex_t RawLock;
typedef pthread_cond_t RawCond;

static void check(int e) {
    if (e) stop("mutex host error", e);
}

static struct timespec realtime(uint64_t ns) {
    return (struct timespec){.tv_sec = (time_t)(ns / 1000000000), .tv_nsec = (long)(ns % 1000000000)};
}

static void raw_init(RawLock *l) {
    check(pthread_mutex_init(l, NULL));
}

static void raw_free(RawLock *l) {
    check(pthread_mutex_destroy(l));
}

static int raw_try(RawLock *l) {
    return pthread_mutex_trylock(l) == 0;
}

static void raw_lock(RawLock *l) {
    check(pthread_mutex_lock(l));
}

static int raw_lock_until(RawLock *l, uint64_t deadline) {
    struct timespec end = realtime(deadline);
    int e = pthread_mutex_timedlock(l, &end);
    if (e && e != ETIMEDOUT) stop("mutex host error", e);
    return e;
}

static void raw_unlock(RawLock *l) {
    check(pthread_mutex_unlock(l));
}

static void raw_cond_init(RawCond *c) {
    check(pthread_cond_init(c, NULL));
}

static void raw_cond_free(RawCond *c) {
    check(pthread_cond_destroy(c));
}

static int raw_cond_wait(RawCond *c, RawLock *l, uint64_t deadline) {
    if (!deadline) return pthread_cond_wait(c, l);
    struct timespec end = realtime(deadline);
    return pthread_cond_timedwait(c, l, &end);
}

static void raw_signal(RawCond *c, int all) {
    check(all ? pthread_cond_broadcast(c) : pthread_cond_signal(c));
}

static uint32_t thread_self(void) {
    static __thread uint32_t id;
    if (!id) id = (uint32_t)syscall(SYS_gettid);
    return id;
}
#endif

typedef struct {
    RawLock raw;
    uint32_t owner, depth, waiters;
    int type;
    char name[32];
} Mutex;

typedef struct {
    RawCond raw;
    uint32_t refs;
} Cond;

typedef struct {
    int type;
} MutexAttr;

typedef struct {
    int64_t seconds, nanoseconds;
} GuestTime;

typedef struct Counts {
    uint64_t locks, unlocks, waits, wakeups;
    struct Counts *next;
} Counts;

static HostMutex counts_lock = HOST_MUTEX_INIT, init_lock = HOST_MUTEX_INIT;
static Counts *all_counts;
static __thread Counts *mine;
static uint64_t mutexes_created, conds_created;

static Counts *counts(void) {
    if (mine) return mine;
    Counts *c = calloc(1, sizeof(*c));
    if (!c) stop("out of memory for mutex counters", ENOMEM);
    host_lock(&counts_lock);
    c->next = all_counts;
    all_counts = c;
    host_unlock(&counts_lock);
    return mine = c;
}

#define COUNT(field)                                                   \
    do {                                                               \
        Counts *c_ = counts();                                         \
        __atomic_store_n(&c_->field, c_->field + 1, __ATOMIC_RELAXED); \
    } while (0)

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
    default: stop("unmapped mutex error", e);
    }
    return KERNEL_ERROR(code);
}

static uint32_t owner_of(const Mutex *m) {
    return __atomic_load_n(&m->owner, __ATOMIC_RELAXED);
}

static void take_ownership(Mutex *m, uint32_t self, uint32_t depth) {
    __atomic_store_n(&m->owner, self, __ATOMIC_RELAXED);
    m->depth = depth;
}

static void drop_ownership(Mutex *m) {
    __atomic_store_n(&m->owner, 0, __ATOMIC_RELAXED);
    m->depth = 0;
}

static int acquire(Mutex *m, int mode, uint64_t deadline) {
    uint32_t self = thread_self();
    if (owner_of(m) == self) {
        if (m->type == TYPE_RECURSIVE) {
            if (m->depth == UINT32_MAX) return EAGAIN;
            ++m->depth;
            COUNT(locks);
            return 0;
        }
        if (mode == MODE_TRY) return EBUSY;
        if (m->type != TYPE_NORMAL) return EDEADLK;
    }
    if (!raw_try(&m->raw)) {
        if (mode == MODE_TRY) return EBUSY;
        __atomic_fetch_add(&m->waiters, 1, __ATOMIC_RELAXED);
        int e = 0;
        if (mode == MODE_TIMED) {
            e = raw_lock_until(&m->raw, deadline);
        } else {
            uint64_t started = runtime_wait_clock();
            int slot = runtime_block_begin(RUNTIME_BLOCK_MUTEX, (uintptr_t)m);
            raw_lock(&m->raw);
            runtime_block_end(slot);
            runtime_wait_note(1, runtime_wait_clock() - started);
        }
        __atomic_fetch_sub(&m->waiters, 1, __ATOMIC_RELAXED);
        if (e) return e;
    }
    take_ownership(m, self, 1);
    COUNT(locks);
    return 0;
}

static SYS_ABI int32_t attr_init(MutexAttr **out) {
    if (!out) return guest_error(EINVAL);
    MutexAttr *attr = malloc(sizeof(*attr));
    if (!attr) return guest_error(ENOMEM);
    attr->type = TYPE_ERRORCHECK;
    *out = attr;
    return 0;
}

static SYS_ABI int32_t attr_settype(MutexAttr **attr, int type) {
    if (!attr || !*attr || type < TYPE_ERRORCHECK || type > TYPE_ADAPTIVE) return guest_error(EINVAL);
    (*attr)->type = type;
    return 0;
}

static SYS_ABI int32_t attr_setprotocol(MutexAttr **attr, int protocol) {
    if (!attr || !*attr || protocol < 0 || protocol > 2) return guest_error(EINVAL);
    return 0;
}

static SYS_ABI int32_t attr_destroy(MutexAttr **attr) {
    if (!attr || !*attr) return guest_error(EINVAL);
    free(*attr);
    *attr = NULL;
    return 0;
}

static SYS_ABI int32_t mutex_init(Mutex **out, MutexAttr **attr, const char *name) {
    if (!out || (attr && !*attr)) return guest_error(EINVAL);
    int type = attr ? (*attr)->type : TYPE_ERRORCHECK;
    if (type < TYPE_ERRORCHECK || type > TYPE_ADAPTIVE) return guest_error(EINVAL);
    Mutex *m = calloc(1, sizeof(*m));
    if (!m) return guest_error(ENOMEM);
    raw_init(&m->raw);
    m->type = type;
    snprintf(m->name, sizeof(m->name), "%s", name ? name : "");
    *out = m;
    __atomic_fetch_add(&mutexes_created, 1, __ATOMIC_RELAXED);
    return 0;
}

static int32_t mutex_ready(Mutex **handle) {
    if (!handle) return guest_error(EINVAL);
    uintptr_t value = __atomic_load_n((uintptr_t *)handle, __ATOMIC_ACQUIRE);
    if (value == DESTROYED_HANDLE) return guest_error(EINVAL);
    if (value > DESTROYED_HANDLE) return 0;
    host_lock(&init_lock);
    int32_t e = 0;
    if ((uintptr_t)*handle < DESTROYED_HANDLE) {
        Mutex *m = NULL;
        e = mutex_init(&m, NULL, NULL);
        if (!e) __atomic_store_n(handle, m, __ATOMIC_RELEASE);
    }
    host_unlock(&init_lock);
    return e;
}

static SYS_ABI int32_t mutex_lock(Mutex **handle) {
    int32_t e = mutex_ready(handle);
    return e ? e : guest_error(acquire(*handle, MODE_WAIT, 0));
}

static SYS_ABI int32_t mutex_trylock(Mutex **handle) {
    int32_t e = mutex_ready(handle);
    return e ? e : guest_error(acquire(*handle, MODE_TRY, 0));
}

static SYS_ABI int32_t mutex_timedlock(Mutex **handle, uint32_t usec) {
    int32_t e = mutex_ready(handle);
    return e ? e : guest_error(acquire(*handle, MODE_TIMED, host_realtime_ns() + (uint64_t)usec * 1000));
}

static SYS_ABI int32_t mutex_unlock(Mutex **handle) {
    if (!handle || (uintptr_t)*handle == DESTROYED_HANDLE) return guest_error(EINVAL);
    if ((uintptr_t)*handle < DESTROYED_HANDLE) return guest_error(EPERM);
    Mutex *m = *handle;
    uint32_t self = thread_self(), held = owner_of(m);
    if (held != self && (m->type != TYPE_NORMAL || !held)) return guest_error(EPERM);
    COUNT(unlocks);
    if (held == self && --m->depth) return 0;
    drop_ownership(m);
    raw_unlock(&m->raw);
    return 0;
}

static SYS_ABI int32_t mutex_destroy(Mutex **handle) {
    if (!handle || (uintptr_t)*handle == DESTROYED_HANDLE) return guest_error(EINVAL);
    if ((uintptr_t)*handle < DESTROYED_HANDLE) return 0;
    Mutex *m = *handle;
    if (owner_of(m) || __atomic_load_n(&m->waiters, __ATOMIC_ACQUIRE) || !raw_try(&m->raw)) return guest_error(EBUSY);
    raw_unlock(&m->raw);
    raw_free(&m->raw);
    free(m);
    *handle = (Mutex *)(uintptr_t)DESTROYED_HANDLE;
    return 0;
}

static SYS_ABI int32_t cond_init(Cond **out, void **attr, const char *name) {
    (void)attr;
    (void)name;
    if (!out) return guest_error(EINVAL);
    Cond *c = calloc(1, sizeof(*c));
    if (!c) return guest_error(ENOMEM);
    raw_cond_init(&c->raw);
    *out = c;
    __atomic_fetch_add(&conds_created, 1, __ATOMIC_RELAXED);
    return 0;
}

static int32_t cond_ready(Cond **handle) {
    if (!handle) return guest_error(EINVAL);
    if (__atomic_load_n((uintptr_t *)handle, __ATOMIC_ACQUIRE) >= DESTROYED_HANDLE) return 0;
    host_lock(&init_lock);
    int32_t e = 0;
    if ((uintptr_t)*handle < DESTROYED_HANDLE) {
        Cond *c = NULL;
        e = cond_init(&c, NULL, NULL);
        if (!e) __atomic_store_n(handle, c, __ATOMIC_RELEASE);
    }
    host_unlock(&init_lock);
    return e;
}

static void free_cond(Cond *c) {
    raw_cond_free(&c->raw);
    free(c);
}

static SYS_ABI int32_t cond_destroy(Cond **handle) {
    if (!handle) return guest_error(EINVAL);
    if ((uintptr_t)*handle < DESTROYED_HANDLE) return 0;
    Cond *c = *handle;
    *handle = NULL;
    if (!(__atomic_fetch_or(&c->refs, DESTROYED_COND, __ATOMIC_ACQ_REL) & ~DESTROYED_COND)) free_cond(c);
    return 0;
}

static int wait_on(Cond *c, Mutex *m, uint64_t deadline) {
    uint32_t self = thread_self();
    if (owner_of(m) != self) return EPERM;
    if (deadline && host_realtime_ns() >= deadline) return ETIMEDOUT;
    __atomic_fetch_add(&c->refs, 1, __ATOMIC_ACQ_REL);
    __atomic_fetch_add(&m->waiters, 1, __ATOMIC_RELAXED);
    uint32_t depth = m->depth;
    drop_ownership(m);
    int e = raw_cond_wait(&c->raw, &m->raw, deadline);
    take_ownership(m, self, depth);
    __atomic_fetch_sub(&m->waiters, 1, __ATOMIC_RELAXED);
    if (__atomic_fetch_sub(&c->refs, 1, __ATOMIC_ACQ_REL) == (DESTROYED_COND | 1)) free_cond(c);
    if (e && e != ETIMEDOUT) stop("condition variable host error", e);
    return e;
}

static int32_t wait_until(Cond **cond, Mutex **mutex, uint64_t deadline, int blocking) {
    int32_t e = cond_ready(cond);
    if (e) return e;
    if (!mutex || (uintptr_t)*mutex <= DESTROYED_HANDLE) return guest_error(EINVAL);
    COUNT(waits);
    uint64_t started = runtime_wait_clock();
    int slot = blocking ? runtime_block_begin(RUNTIME_BLOCK_COND, (uintptr_t)*cond) : -1;
    int result = wait_on(*cond, *mutex, deadline);
    if (blocking) runtime_block_end(slot);
    runtime_wait_note(0, runtime_wait_clock() - started);
    return guest_error(result);
}

static SYS_ABI int32_t cond_wait(Cond **cond, Mutex **mutex) {
    return wait_until(cond, mutex, 0, 1);
}

static SYS_ABI int32_t cond_timedwait(Cond **cond, Mutex **mutex, uint32_t usec) {
    return wait_until(cond, mutex, host_realtime_ns() + (uint64_t)usec * 1000, 0);
}

static SYS_ABI int32_t cond_abs_timedwait(Cond **cond, Mutex **mutex, const GuestTime *time) {
    if (!time || time->nanoseconds < 0 || time->nanoseconds >= 1000000000 || time->seconds < 0)
        return guest_error(EINVAL);
    uint64_t deadline = (uint64_t)time->seconds * 1000000000 + (uint64_t)time->nanoseconds;
    return wait_until(cond, mutex, deadline ? deadline : 1, 0);
}

static int32_t wake(Cond **cond, int all) {
    int32_t e = cond_ready(cond);
    if (e) return e;
    COUNT(wakeups);
    raw_signal(&(*cond)->raw, all);
    return 0;
}

static SYS_ABI int32_t cond_signal(Cond **cond) {
    return wake(cond, 0);
}

static SYS_ABI int32_t cond_broadcast(Cond **cond) {
    return wake(cond, 1);
}

static int32_t posix(int32_t result) {
    return result ? (int32_t)((uint32_t)result & 0xffff) : 0;
}

static SYS_ABI int32_t posix_attr_init(MutexAttr **out) { return posix(attr_init(out)); }
static SYS_ABI int32_t posix_attr_settype(MutexAttr **attr, int type) { return posix(attr_settype(attr, type)); }
static SYS_ABI int32_t posix_attr_destroy(MutexAttr **attr) { return posix(attr_destroy(attr)); }
static SYS_ABI int32_t posix_mutex_init(Mutex **out, MutexAttr **attr) { return posix(mutex_init(out, attr, NULL)); }
static SYS_ABI int32_t posix_mutex_destroy(Mutex **m) { return posix(mutex_destroy(m)); }
static SYS_ABI int32_t posix_mutex_lock(Mutex **m) { return posix(mutex_lock(m)); }
static SYS_ABI int32_t posix_mutex_trylock(Mutex **m) { return posix(mutex_trylock(m)); }
static SYS_ABI int32_t posix_mutex_unlock(Mutex **m) { return posix(mutex_unlock(m)); }
static SYS_ABI int32_t posix_cond_init(Cond **c, void **attr) { return posix(cond_init(c, attr, NULL)); }
static SYS_ABI int32_t posix_cond_destroy(Cond **c) { return posix(cond_destroy(c)); }
static SYS_ABI int32_t posix_cond_wait(Cond **c, Mutex **m) { return posix(cond_wait(c, m)); }
static SYS_ABI int32_t posix_cond_timedwait(Cond **c, Mutex **m, const GuestTime *t) { return posix(cond_abs_timedwait(c, m, t)); }
static SYS_ABI int32_t posix_cond_signal(Cond **c) { return posix(cond_signal(c)); }
static SYS_ABI int32_t posix_cond_broadcast(Cond **c) { return posix(cond_broadcast(c)); }

void sys_mutex_describe(uintptr_t object, char *out, size_t size) {
    const Mutex *m = (const Mutex *)object;
    uint32_t held = owner_of(m);
    int n = snprintf(out, size, "mutex %#llx%s%s%s, type %d, ", (unsigned long long)object, m->name[0] ? " '" : "",
                     m->name, m->name[0] ? "'" : "", m->type);
    if (n < 0 || (size_t)n >= size) return;
    if (held) snprintf(out + n, size - (size_t)n, "held by thread %u (depth %u)", held, m->depth);
    else snprintf(out + n, size - (size_t)n, "free");
}

void sys_mutex_report(void) {
    uint64_t locks = 0, unlocks = 0, waits = 0, wakeups = 0;
    host_lock(&counts_lock);
    for (Counts *c = all_counts; c; c = c->next) {
        locks += __atomic_load_n(&c->locks, __ATOMIC_RELAXED);
        unlocks += __atomic_load_n(&c->unlocks, __ATOMIC_RELAXED);
        waits += __atomic_load_n(&c->waits, __ATOMIC_RELAXED);
        wakeups += __atomic_load_n(&c->wakeups, __ATOMIC_RELAXED);
    }
    host_unlock(&counts_lock);
    printf("Runtime: mutexes created=%llu, locks=%llu, unlocks=%llu\n",
           (unsigned long long)__atomic_load_n(&mutexes_created, __ATOMIC_RELAXED), (unsigned long long)locks,
           (unsigned long long)unlocks);
    printf("Runtime: condition variables created=%llu, waits=%llu, wakeups=%llu\n",
           (unsigned long long)__atomic_load_n(&conds_created, __ATOMIC_RELAXED), (unsigned long long)waits,
           (unsigned long long)wakeups);
}

static const SysExport kernel_exports[] = {
    {"F8bUHwAG284", "scePthreadMutexattrInit", attr_init},
    {"iMp8QpE+XO4", "scePthreadMutexattrSettype", attr_settype},
    {"1FGvU0i9saQ", "scePthreadMutexattrSetprotocol", attr_setprotocol},
    {"smWEktiyyG0", "scePthreadMutexattrDestroy", attr_destroy},
    {"cmo1RIYva9o", "scePthreadMutexInit", mutex_init},
    {"9UK1vLZQft4", "scePthreadMutexLock", mutex_lock},
    {"upoVrzMHFeE", "scePthreadMutexTrylock", mutex_trylock},
    {"IafI2PxcPnQ", "scePthreadMutexTimedlock", mutex_timedlock},
    {"tn3VlD0hG60", "scePthreadMutexUnlock", mutex_unlock},
    {"2Of0f+3mhhE", "scePthreadMutexDestroy", mutex_destroy},
    {"2Tb92quprl0", "scePthreadCondInit", cond_init},
    {"g+PZd2hiacg", "scePthreadCondDestroy", cond_destroy},
    {"WKAXJ4XBPQ4", "scePthreadCondWait", cond_wait},
    {"BmMjYxmew1w", "scePthreadCondTimedwait", cond_timedwait},
    {"kDh-NfxgMtE", "scePthreadCondSignal", cond_signal},
    {"JGgj7Uvrl+A", "scePthreadCondBroadcast", cond_broadcast},
};

static const SysExport posix_exports[] = {
    {"dQHWEsJtoE4", "pthread_mutexattr_init", posix_attr_init},
    {"mDmgMOGVUqg", "pthread_mutexattr_settype", posix_attr_settype},
    {"HF7lK46xzjY", "pthread_mutexattr_destroy", posix_attr_destroy},
    {"ttHNfU+qDBU", "pthread_mutex_init", posix_mutex_init},
    {"ltCfaGr2JGE", "pthread_mutex_destroy", posix_mutex_destroy},
    {"7H0iTOciTLo", "pthread_mutex_lock", posix_mutex_lock},
    {"K-jXhbt2gn4", "pthread_mutex_trylock", posix_mutex_trylock},
    {"2Z+PpY6CaJg", "pthread_mutex_unlock", posix_mutex_unlock},
    {"0TyVk4MSLt0", "pthread_cond_init", posix_cond_init},
    {"RXXqi4CtF8w", "pthread_cond_destroy", posix_cond_destroy},
    {"Op8TBGY5KHg", "pthread_cond_wait", posix_cond_wait},
    {"27bAgiJmOh0", "pthread_cond_timedwait", posix_cond_timedwait},
    {"2MOy+rUfuhQ", "pthread_cond_signal", posix_cond_signal},
    {"mkx2fVhNMsg", "pthread_cond_broadcast", posix_cond_broadcast},
};

SYS_LIBRARY(sys_mutex, "libkernel", kernel_exports);
SYS_LIBRARY(sys_posix_mutex, "libScePosix", posix_exports);
