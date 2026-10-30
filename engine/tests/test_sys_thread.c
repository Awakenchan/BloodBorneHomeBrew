// SPDX-License-Identifier: GPL-3.0-or-later
#define _GNU_SOURCE
#include "runtime.h"
#include "sys/sys.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

typedef void *Handle;
typedef void *(SYS_ABI *Entry)(void *);

static int failures;
#define EXPECT(condition) do { if (!(condition)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); ++failures; } } while (0)

static void *fn(const char *name) {
    const SysLibrary *libraries[] = {&sys_thread, &sys_posix_thread};
    for (size_t l = 0; l < 2; ++l)
        for (size_t i = 0; i < libraries[l]->count; ++i)
            if (!strcmp(libraries[l]->exports[i].name, name)) return libraries[l]->exports[i].function;
    printf("FAIL missing %s\n", name);
    ++failures;
    return NULL;
}

static void *(SYS_ABI *self)(void);
static int32_t *(SYS_ABI *error_location)(void);
static int32_t (SYS_ABI *attr_init)(Handle *);
static int32_t (SYS_ABI *attr_destroy)(Handle *);
static int32_t (SYS_ABI *attr_get)(Handle, Handle *);
static int32_t (SYS_ABI *attr_set_stack)(Handle *, uint64_t);
static int32_t (SYS_ABI *attr_get_stack)(Handle *, uint64_t *);
static int32_t (SYS_ABI *attr_set_detach)(Handle *, int);
static int32_t (SYS_ABI *attr_get_detach)(Handle *, int *);
static int32_t (SYS_ABI *thread_create)(Handle *, Handle *, Entry, void *, const char *);
static int32_t (SYS_ABI *thread_join)(Handle, void **);
static int32_t (SYS_ABI *thread_detach)(Handle);
static void (SYS_ABI *thread_exit)(void *);
static int32_t (SYS_ABI *get_prio)(Handle, int *);
static int32_t (SYS_ABI *set_prio)(Handle, int);
static int32_t (SYS_ABI *once)(int32_t *, void (SYS_ABI *)(void));
static int32_t (SYS_ABI *key_create)(uint32_t *, void (SYS_ABI *)(void *));
static int32_t (SYS_ABI *key_delete)(uint32_t);
static int32_t (SYS_ABI *key_set)(uint32_t, void *);
static void *(SYS_ABI *key_get)(uint32_t);
static int32_t (SYS_ABI *posix_create)(Handle *, Handle *, Entry, void *);
static int32_t (SYS_ABI *posix_join)(Handle, void **);

static void nap(unsigned ms) {
    struct timespec t = {0, (long)ms * 1000000};
    nanosleep(&t, NULL);
}

static uint32_t key;
static int destroyed;
static uintptr_t destroyed_value;

static void SYS_ABI destructor(void *value) {
    __atomic_fetch_add(&destroyed, 1, __ATOMIC_SEQ_CST);
    __atomic_store_n(&destroyed_value, (uintptr_t)value, __ATOMIC_SEQ_CST);
}

typedef struct { Handle handle; int same, deadlock, own_errno; } Probe;

static void SYS_ABI leave(void *value) { thread_exit(value); }

static void *SYS_ABI child(void *p) {
    Probe *probe = p;
    probe->same = self() == probe->handle;
    probe->deadlock = thread_join(self(), NULL);
    *error_location() = 99;
    probe->own_errno = *error_location() == 99;
    key_set(key, (void *)(uintptr_t)42);
    leave((void *)(uintptr_t)0x5150);
    return NULL;
}

static void *SYS_ABI returns(void *value) {
    nap(20);
    return value;
}

static int once_calls;
static int32_t once_word;
static void SYS_ABI once_routine(void) {
    nap(10);
    __atomic_fetch_add(&once_calls, 1, __ATOMIC_SEQ_CST);
}
static void *SYS_ABI once_caller(void *unused) {
    (void)unused;
    return (void *)(intptr_t)once(&once_word, once_routine);
}

static void attributes(void) {
    Handle a = NULL, bogus = (Handle)(uintptr_t)0x1230;
    uint64_t stack = 0;
    int detached = -1;
    EXPECT((uint32_t)attr_init(NULL) == 0x80020016 && attr_init(&a) == 0);
    EXPECT(attr_get_stack(&a, &stack) == 0 && stack == 1024 * 1024);
    EXPECT((uint32_t)attr_set_stack(&a, 16383) == 0x80020016 && attr_set_stack(&a, 16384) == 0);
    EXPECT((uint32_t)attr_set_detach(&a, 2) == 0x80020016 && attr_set_detach(&a, 1) == 0);
    EXPECT(attr_get_detach(&a, &detached) == 0 && detached == 1);
    EXPECT((uint32_t)attr_set_stack(&bogus, 65536) == 0x80020016);
    EXPECT((uint32_t)attr_get(bogus, &a) == 0x80020003);
    EXPECT(attr_get(self(), &a) == 0 && attr_get_detach(&a, &detached) == 0 && detached == 0);
    Handle copy = a;
    EXPECT(attr_destroy(&a) == 0 && a == NULL && (uint32_t)attr_destroy(&copy) == 0x80020016);
}

static void threads(void) {
    Handle main_thread = self();
    EXPECT(main_thread != NULL && self() == main_thread);
    EXPECT(key_create(&key, destructor) == 0 && key_set(key, (void *)(uintptr_t)7) == 0);

    Probe probe = {0};
    Handle attr = NULL;
    attr_init(&attr);
    EXPECT(thread_create(&probe.handle, &attr, child, &probe, "test-child") == 0);
    void *result = NULL;
    EXPECT(thread_join(probe.handle, &result) == 0 && result == (void *)(uintptr_t)0x5150);
    EXPECT(probe.same && (uint32_t)probe.deadlock == 0x8002000b && probe.own_errno);
    EXPECT(*error_location() != 99);
    EXPECT(destroyed == 1 && destroyed_value == 42 && key_get(key) == (void *)(uintptr_t)7);
    EXPECT((uint32_t)thread_join(probe.handle, NULL) == 0x80020016);
    EXPECT((uint32_t)thread_join(main_thread, NULL) == 0x80020003);
    EXPECT((uint32_t)thread_join((Handle)(uintptr_t)0x4560, NULL) == 0x80020003);

    int prio = 0;
    EXPECT(set_prio(probe.handle, 400) == 0 && get_prio(probe.handle, &prio) == 0 && prio == 400);

    Handle running = NULL;
    EXPECT(thread_create(&running, &attr, returns, NULL, "detached-later") == 0);
    EXPECT(thread_detach(running) == 0 && (uint32_t)thread_detach(running) == 0x80020016);
    EXPECT((uint32_t)thread_join(running, NULL) == 0x80020016);
    int detached = -1;
    EXPECT(attr_get(running, &attr) == 0 && attr_get_detach(&attr, &detached) == 0 && detached == 1);
    attr_destroy(&attr);

    Handle posix_thread = NULL;
    EXPECT(posix_create(&posix_thread, NULL, returns, (void *)(uintptr_t)9) == 0);
    EXPECT(posix_join(posix_thread, &result) == 0 && result == (void *)(uintptr_t)9);
    EXPECT(posix_join(posix_thread, NULL) == 22 && posix_create(&posix_thread, NULL, NULL, NULL) == 22);

    EXPECT(key_delete(key) == 0 && (uint32_t)key_delete(key) == 0x80020016 && (uint32_t)key_set(key, NULL) == 0x80020016);
    uint32_t k = 0, made = 0;
    while (key_create(&k, NULL) == 0) ++made;
    EXPECT(made == 255);
    EXPECT((uint32_t)key_create(&k, NULL) == 0x80020023);
    for (uint32_t i = 1; i < 256; ++i) key_delete(i);
}

static void onces(void) {
    Handle t[8];
    void *r[8];
    EXPECT((uint32_t)once(NULL, once_routine) == 0x80020016);
    for (int i = 0; i < 8; ++i) EXPECT(thread_create(&t[i], NULL, once_caller, NULL, "once") == 0);
    for (int i = 0; i < 8; ++i) EXPECT(thread_join(t[i], &r[i]) == 0 && r[i] == NULL);
    EXPECT(once_calls == 1 && once_word == 1);
}

int main(void) {
    self = fn("scePthreadSelf");
    error_location = fn("__error");
    attr_init = fn("scePthreadAttrInit");
    attr_destroy = fn("scePthreadAttrDestroy");
    attr_get = fn("scePthreadAttrGet");
    attr_set_stack = fn("scePthreadAttrSetstacksize");
    attr_get_stack = fn("scePthreadAttrGetstacksize");
    attr_set_detach = fn("scePthreadAttrSetdetachstate");
    attr_get_detach = fn("scePthreadAttrGetdetachstate");
    thread_create = fn("scePthreadCreate");
    thread_join = fn("scePthreadJoin");
    thread_detach = fn("scePthreadDetach");
    thread_exit = fn("scePthreadExit");
    get_prio = fn("scePthreadGetprio");
    set_prio = fn("scePthreadSetprio");
    once = fn("scePthreadOnce");
    key_create = fn("scePthreadKeyCreate");
    key_delete = fn("scePthreadKeyDelete");
    key_set = fn("scePthreadSetspecific");
    key_get = fn("scePthreadGetspecific");
    posix_create = fn("pthread_create");
    posix_join = fn("pthread_join");
    if (failures) return 1;
    EXPECT(sys_resolve("6UgtwV+0zb4#p#J") == (uintptr_t)thread_create);

    attributes();
    threads();
    onces();

    uint64_t made = 0, finished = 0, found = 0, steps = 0;
    runtime_thread_census(&made, &finished, &found, &steps);
    EXPECT(made == 11 && found > 0 && steps <= found * 4);

    printf("%s\n", failures ? "sys thread: FAILED" : "sys thread: ok");
    return failures != 0;
}
