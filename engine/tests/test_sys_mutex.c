// SPDX-License-Identifier: GPL-3.0-or-later
#define _GNU_SOURCE
#include "sys/sys.h"
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

typedef void *Handle;
typedef struct { int64_t seconds, nanoseconds; } GuestTime;

static int failures;
#define EXPECT(condition) do { if (!(condition)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); ++failures; } } while (0)

static uint64_t read_clock(clockid_t id) {
    struct timespec t;
    clock_gettime(id, &t);
    return (uint64_t)t.tv_sec * 1000000000 + (uint64_t)t.tv_nsec;
}
uint64_t host_realtime_ns(void) { return read_clock(CLOCK_REALTIME); }
uint64_t host_monotonic_ns(void) { return read_clock(CLOCK_MONOTONIC); }

static void nap(unsigned ms) {
    struct timespec t = {0, (long)ms * 1000000};
    nanosleep(&t, NULL);
}

static void *fn(const char *name) {
    const SysLibrary *libraries[] = {&sys_mutex, &sys_posix_mutex};
    for (size_t l = 0; l < 2; ++l)
        for (size_t i = 0; i < libraries[l]->count; ++i)
            if (!strcmp(libraries[l]->exports[i].name, name)) return libraries[l]->exports[i].function;
    printf("FAIL missing %s\n", name);
    ++failures;
    return NULL;
}

static int32_t (SYS_ABI *attr_init)(Handle *);
static int32_t (SYS_ABI *attr_settype)(Handle *, int);
static int32_t (SYS_ABI *attr_destroy)(Handle *);
static int32_t (SYS_ABI *mutex_init)(Handle *, Handle *, const char *);
static int32_t (SYS_ABI *mutex_lock)(Handle *);
static int32_t (SYS_ABI *mutex_trylock)(Handle *);
static int32_t (SYS_ABI *mutex_timedlock)(Handle *, uint32_t);
static int32_t (SYS_ABI *mutex_unlock)(Handle *);
static int32_t (SYS_ABI *mutex_destroy)(Handle *);
static int32_t (SYS_ABI *cond_init)(Handle *, Handle *, const char *);
static int32_t (SYS_ABI *cond_destroy)(Handle *);
static int32_t (SYS_ABI *cond_wait)(Handle *, Handle *);
static int32_t (SYS_ABI *cond_timedwait)(Handle *, Handle *, uint32_t);
static int32_t (SYS_ABI *cond_signal)(Handle *);
static int32_t (SYS_ABI *cond_broadcast)(Handle *);
static int32_t (SYS_ABI *posix_mutex_lock)(Handle *);
static int32_t (SYS_ABI *posix_mutex_unlock)(Handle *);
static int32_t (SYS_ABI *posix_cond_timedwait)(Handle *, Handle *, const GuestTime *);

static Handle make(int type, const char *name) {
    Handle attr = NULL, m = NULL;
    EXPECT(attr_init(&attr) == 0 && attr_settype(&attr, type) == 0);
    EXPECT(mutex_init(&m, &attr, name) == 0 && attr_destroy(&attr) == 0);
    return m;
}

static void types(void) {
    Handle m = make(1, "errorcheck");
    EXPECT(mutex_lock(&m) == 0);
    EXPECT((uint32_t)mutex_lock(&m) == 0x8002000b && (uint32_t)mutex_trylock(&m) == 0x80020010);
    EXPECT((uint32_t)mutex_destroy(&m) == 0x80020010);
    EXPECT(mutex_unlock(&m) == 0 && (uint32_t)mutex_unlock(&m) == 0x80020001);
    EXPECT(mutex_destroy(&m) == 0 && (uintptr_t)m == 2 && (uint32_t)mutex_lock(&m) == 0x80020016);

    m = make(2, "recursive");
    EXPECT(mutex_lock(&m) == 0 && mutex_lock(&m) == 0 && mutex_trylock(&m) == 0);
    EXPECT(mutex_unlock(&m) == 0 && mutex_unlock(&m) == 0 && mutex_unlock(&m) == 0);
    EXPECT((uint32_t)mutex_unlock(&m) == 0x80020001 && mutex_destroy(&m) == 0);

    m = make(3, "normal");
    EXPECT(mutex_lock(&m) == 0 && (uint32_t)mutex_timedlock(&m, 1000) == 0x8002003c);
    EXPECT(mutex_unlock(&m) == 0 && mutex_destroy(&m) == 0);

    Handle unset = NULL;
    EXPECT((uint32_t)mutex_unlock(&unset) == 0x80020001 && unset == NULL);
    EXPECT(mutex_lock(&unset) == 0 && unset != NULL && (uint32_t)mutex_lock(&unset) == 0x8002000b);
    EXPECT(mutex_unlock(&unset) == 0 && mutex_destroy(&unset) == 0);
    EXPECT(posix_mutex_lock(&unset) == 22 && posix_mutex_unlock(&unset) == 22);
}

typedef struct { Handle *m, *c; int32_t result[4]; int ready, depth; } Job;

static void *contender(void *p) {
    Job *j = p;
    j->result[0] = mutex_trylock(j->m);
    j->result[1] = mutex_timedlock(j->m, 10000);
    j->result[2] = mutex_unlock(j->m);
    __atomic_store_n(&j->ready, 1, __ATOMIC_SEQ_CST);
    j->result[3] = mutex_lock(j->m);
    mutex_unlock(j->m);
    return NULL;
}

static void blocking(void) {
    Handle m = make(1, "blocking");
    EXPECT(mutex_lock(&m) == 0);
    Job j = {&m, NULL, {0}, 0, 0};
    pthread_t t;
    pthread_create(&t, NULL, contender, &j);
    while (!__atomic_load_n(&j.ready, __ATOMIC_SEQ_CST)) nap(1);
    nap(20);
    char text[160];
    sys_mutex_describe((uintptr_t)m, text, sizeof text);
    EXPECT(strstr(text, "'blocking', type 1, held by thread") != NULL);
    EXPECT((uint32_t)mutex_destroy(&m) == 0x80020010);
    EXPECT(mutex_unlock(&m) == 0);
    pthread_join(t, NULL);
    EXPECT((uint32_t)j.result[0] == 0x80020010 && (uint32_t)j.result[1] == 0x8002003c);
    EXPECT((uint32_t)j.result[2] == 0x80020001 && j.result[3] == 0);
    sys_mutex_describe((uintptr_t)m, text, sizeof text);
    EXPECT(strstr(text, "free") != NULL);
    EXPECT(mutex_destroy(&m) == 0);
}

static int woken;

static void *waiter(void *p) {
    Job *j = p;
    for (int i = 0; i < j->depth; ++i) mutex_lock(j->m);
    __atomic_store_n(&j->ready, 1, __ATOMIC_SEQ_CST);
    j->result[0] = cond_wait(j->c, j->m);
    __atomic_fetch_add(&woken, 1, __ATOMIC_SEQ_CST);
    for (int i = 0; i < j->depth; ++i) j->result[1] |= mutex_unlock(j->m);
    j->result[2] = mutex_unlock(j->m);
    return NULL;
}

static void wait_woken(int count) {
    for (int i = 0; i < 2000 && __atomic_load_n(&woken, __ATOMIC_SEQ_CST) < count; ++i) nap(1);
    nap(10);
    EXPECT(__atomic_load_n(&woken, __ATOMIC_SEQ_CST) == count);
}

static void conditions(void) {
    Handle m = make(1, "cond"), c = NULL;
    EXPECT(cond_init(&c, NULL, "cond") == 0);
    EXPECT((uint32_t)cond_wait(&c, &m) == 0x80020001);
    EXPECT(mutex_lock(&m) == 0 && (uint32_t)cond_timedwait(&c, &m, 5000) == 0x8002003c);
    GuestTime past = {1, 0}, bad = {0, -1};
    EXPECT(posix_cond_timedwait(&c, &m, &past) == 60 && posix_cond_timedwait(&c, &m, &bad) == 22);
    EXPECT(mutex_unlock(&m) == 0);

    Job jobs[3];
    pthread_t threads[3];
    for (int i = 0; i < 3; ++i) {
        jobs[i] = (Job){&m, &c, {0}, 0, 1};
        pthread_create(&threads[i], NULL, waiter, &jobs[i]);
    }
    for (int i = 0; i < 3; ++i)
        while (!__atomic_load_n(&jobs[i].ready, __ATOMIC_SEQ_CST)) nap(1);
    nap(20);
    EXPECT((uint32_t)mutex_destroy(&m) == 0x80020010);
    EXPECT(cond_signal(&c) == 0);
    wait_woken(1);
    EXPECT(cond_broadcast(&c) == 0);
    wait_woken(3);
    for (int i = 0; i < 3; ++i) {
        pthread_join(threads[i], NULL);
        EXPECT(jobs[i].result[0] == 0 && jobs[i].result[1] == 0 && (uint32_t)jobs[i].result[2] == 0x80020001);
    }

    Handle r = make(2, "recursive");
    Job deep = {&r, &c, {0}, 0, 3};
    woken = 0;
    pthread_t t;
    pthread_create(&t, NULL, waiter, &deep);
    while (!__atomic_load_n(&deep.ready, __ATOMIC_SEQ_CST)) nap(1);
    nap(20);
    EXPECT(mutex_trylock(&r) == 0 && mutex_unlock(&r) == 0);
    EXPECT(cond_signal(&c) == 0);
    pthread_join(t, NULL);
    EXPECT(deep.result[0] == 0 && deep.result[1] == 0 && (uint32_t)deep.result[2] == 0x80020001);
    EXPECT(mutex_destroy(&r) == 0);

    Job late = {&m, &c, {0}, 0, 1};
    woken = 0;
    pthread_create(&t, NULL, waiter, &late);
    while (!__atomic_load_n(&late.ready, __ATOMIC_SEQ_CST)) nap(1);
    nap(20);
    Handle kept = c;
    EXPECT(cond_destroy(&c) == 0 && c == NULL);
    EXPECT(cond_signal(&kept) == 0);
    pthread_join(t, NULL);
    EXPECT(late.result[0] == 0);
    EXPECT(cond_destroy(&c) == 0 && (uint32_t)cond_destroy(NULL) == 0x80020016);
    EXPECT(mutex_destroy(&m) == 0);
}

static long counter;

static void *bump(void *p) {
    for (int i = 0; i < 100000; ++i) {
        mutex_lock(p);
        ++counter;
        mutex_unlock(p);
    }
    return NULL;
}

static void stress(void) {
    Handle m = NULL;
    pthread_t threads[8];
    for (int i = 0; i < 8; ++i) pthread_create(&threads[i], NULL, bump, &m);
    for (int i = 0; i < 8; ++i) pthread_join(threads[i], NULL);
    EXPECT(counter == 800000);
    EXPECT(mutex_destroy(&m) == 0);
}

int main(void) {
    attr_init = fn("scePthreadMutexattrInit");
    attr_settype = fn("scePthreadMutexattrSettype");
    attr_destroy = fn("scePthreadMutexattrDestroy");
    mutex_init = fn("scePthreadMutexInit");
    mutex_lock = fn("scePthreadMutexLock");
    mutex_trylock = fn("scePthreadMutexTrylock");
    mutex_timedlock = fn("scePthreadMutexTimedlock");
    mutex_unlock = fn("scePthreadMutexUnlock");
    mutex_destroy = fn("scePthreadMutexDestroy");
    cond_init = fn("scePthreadCondInit");
    cond_destroy = fn("scePthreadCondDestroy");
    cond_wait = fn("scePthreadCondWait");
    cond_timedwait = fn("scePthreadCondTimedwait");
    cond_signal = fn("scePthreadCondSignal");
    cond_broadcast = fn("scePthreadCondBroadcast");
    posix_mutex_lock = fn("pthread_mutex_lock");
    posix_mutex_unlock = fn("pthread_mutex_unlock");
    posix_cond_timedwait = fn("pthread_cond_timedwait");
    if (failures) return 1;
    EXPECT(sys_resolve("9UK1vLZQft4#p#J") == (uintptr_t)mutex_lock);
    EXPECT(sys_resolve("7H0iTOciTLo#I#J") == (uintptr_t)posix_mutex_lock);

    types();
    blocking();
    conditions();
    stress();

    printf("%s\n", failures ? "sys mutex: FAILED" : "sys mutex: ok");
    return failures != 0;
}
