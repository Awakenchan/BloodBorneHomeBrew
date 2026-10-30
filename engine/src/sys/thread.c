// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "sys.h"
#include "runtime.h"
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <malloc.h>
#define EXIT_SET(buf) RUNTIME_RECOVER_SET(buf)
#define EXIT_JUMP(buf) RUNTIME_RECOVER_JUMP(buf)
typedef RuntimeRecoverBuf ExitJump;
#else
#include <asm/prctl.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#define EXIT_SET(buf) setjmp(buf)
#define EXIT_JUMP(buf) longjmp(buf, 1)
typedef jmp_buf ExitJump;
#endif

#define KERNEL_ERROR(code) ((int32_t)(UINT32_C(0x80020000) | (code)))
#define ATTR_MAGIC UINT32_C(0x41545452)
#define STACK_MARGIN (256 * 1024)
#define MIN_STACK (64 * 1024)
#define DEFAULT_STACK (1024 * 1024)
#define DEFAULT_PRIO 700
#define HIGH_PRIO_LAST 300
#define KEYS 256
#define TOMBSTONE ((void *)(uintptr_t)1)

enum { E_SEARCH = 3, E_DEADLOCK = 11, E_NO_MEMORY = 12, E_INVALID = 22, E_AGAIN = 35 };

typedef void *(SYS_ABI *GuestEntry)(void *);
typedef void (SYS_ABI *KeyDestructor)(void *);
typedef void (SYS_ABI *OnceRoutine)(void);

typedef struct {
    uint32_t magic;
    int detached, policy, prio, inherit;
    uint64_t stack, guard, affinity;
} ThreadAttr;

typedef struct {
    uint64_t *tcb;
    unsigned char *tls_block;
    HostThread host;
    GuestEntry entry;
    void *argument, *result;
    ThreadAttr attr;
    char name[32];
    int detached, finished, joined, host_owned;
    ExitJump exit_jump;
} GuestThread;

typedef struct {
    void **slots;
    size_t size, used, tombstones;
} Registry;

static HostMutex lock = HOST_MUTEX_INIT, low_lock = HOST_MUTEX_INIT, key_lock = HOST_MUTEX_INIT;
static Registry threads, attributes;
static _Thread_local GuestThread *current;
static _Thread_local int32_t guest_errno;
static _Thread_local void *key_values[KEYS];
static struct {
    int used;
    KeyDestructor destructor;
} keys[KEYS];
static const unsigned char *tls_template;
static uint64_t tls_filesz, tls_memsz, tls_align = 16;
static size_t created, joined_count, exited;
static uint64_t lookups, probes;

static size_t slot_of(const void *p, size_t size) {
    uintptr_t v = (uintptr_t)p >> 4;
    return (size_t)((v * UINT64_C(0x9E3779B97F4A7C15)) >> 20) & (size - 1);
}

static int grow(Registry *r) {
    size_t size = r->size ? r->size * 2 : 64;
    void **grown = calloc(size, sizeof(*grown));
    if (!grown) return 0;
    for (size_t i = 0; i < r->size; ++i) {
        void *p = r->slots[i];
        if (!p || p == TOMBSTONE) continue;
        size_t at = slot_of(p, size);
        while (grown[at]) at = (at + 1) & (size - 1);
        grown[at] = p;
    }
    free(r->slots);
    r->slots = grown;
    r->size = size;
    r->tombstones = 0;
    return 1;
}

static int insert(Registry *r, void *p) {
    if ((r->used + r->tombstones + 1) * 4 > r->size * 3 && !grow(r)) return 0;
    size_t at = slot_of(p, r->size);
    while (r->slots[at] && r->slots[at] != TOMBSTONE) at = (at + 1) & (r->size - 1);
    if (r->slots[at] == TOMBSTONE) --r->tombstones;
    r->slots[at] = p;
    ++r->used;
    return 1;
}

static size_t locate(const Registry *r, const void *p) {
    if (!r->size || !p || p == TOMBSTONE) return SIZE_MAX;
    for (size_t at = slot_of(p, r->size);; at = (at + 1) & (r->size - 1)) {
        ++probes;
        if (!r->slots[at]) return SIZE_MAX;
        if (r->slots[at] == p) return at;
    }
}

static __attribute__((noreturn)) void stop(const char *why, int code) {
    fprintf(stderr, "STOP: %s: %d\n", why, code);
    exit(21);
}

void runtime_set_main_tls(const void *data, uint64_t filesz, uint64_t memsz, uint64_t align) {
    tls_template = data;
    tls_filesz = filesz;
    tls_memsz = memsz;
    tls_align = align ? align : 16;
}

static uint64_t tls_offset(void) {
    return (tls_memsz + tls_align - 1) & ~(tls_align - 1);
}

static void set_gs(void *base) {
#ifdef _WIN32
    runtime_win_set_tcb(base);
#else
    if (syscall(SYS_arch_prctl, ARCH_SET_GS, (unsigned long)base)) {
        perror("STOP: arch_prctl(ARCH_SET_GS)");
        exit(21);
    }
#endif
}

static void *low_alloc(size_t size) {
    static unsigned char *chunk;
    static size_t left;
    size_t need = (size + 63) & ~(size_t)63, chunk_size = need > 65536 ? need : 65536;
    void *p = NULL;
    host_lock(&low_lock);
    if (left < need) {
        chunk = runtime_low_map(chunk_size, PROT_READ | PROT_WRITE);
        left = chunk ? chunk_size : 0;
    }
    if (left >= need) {
        p = chunk;
        chunk += need;
        left -= need;
    }
    host_unlock(&low_lock);
    if (p) {
        memset(p, 0, size);
        return p;
    }
    static int warned;
    if (!__atomic_exchange_n(&warned, 1, __ATOMIC_RELAXED))
        fputs("Runtime: no low memory left for thread records; heap memory instead\n", stderr);
#ifdef _WIN32
    p = _aligned_malloc(need, 64);
#else
    p = aligned_alloc(64, need);
#endif
    if (p) memset(p, 0, size);
    return p;
}

static void attach(GuestThread *t) {
    uint64_t offset = tls_offset();
    size_t total = offset + 256;
    unsigned char *block = low_alloc(total);
    if (!block) {
        fputs("Cannot allocate guest TLS\n", stderr);
        exit(1);
    }
    if (tls_filesz) memcpy(block, tls_template, tls_filesz);
    uint64_t *tcb = (uint64_t *)(block + offset);
    static uint64_t dtv[3];
    tcb[0] = (uint64_t)(uintptr_t)tcb;
    tcb[1] = (uint64_t)(uintptr_t)dtv;
    tcb[2] = (uint64_t)(uintptr_t)t;
    t->tls_block = block;
    t->tcb = tcb;
    set_gs(tcb);
    current = t;
}

static GuestThread *new_thread(void) {
    GuestThread *t = low_alloc(sizeof(*t));
    if (t)
        t->attr = (ThreadAttr){
            .magic = ATTR_MAGIC, .policy = 1, .prio = DEFAULT_PRIO, .stack = DEFAULT_STACK, .affinity = 0x7f};
    return t;
}

static void publish(GuestThread *t) {
    host_lock(&lock);
    if (!insert(&threads, t)) stop("out of memory for the thread registry", 12);
    host_unlock(&lock);
}

static GuestThread *current_thread(void) {
    if (current) return current;
    GuestThread *t = new_thread();
    if (!t) {
        fputs("Cannot allocate guest thread\n", stderr);
        exit(1);
    }
#ifndef _WIN32
    t->host = pthread_self();
#endif
    t->host_owned = 1;
    snprintf(t->name, sizeof(t->name), "host");
    attach(t);
    publish(t);
    return t;
}

void runtime_thread_attach_host(const char *name) {
    GuestThread *t = current_thread();
    snprintf(t->name, sizeof(t->name), "%s", name ? name : "host");
}

void runtime_thread_attach_main(void) {
    snprintf(current_thread()->name, sizeof(current->name), "main");
}

int runtime_thread_has_record(void) {
    return current != NULL;
}

int runtime_thread_is_loader(void) {
    return current && !strncmp(current->name, "Core.Res.", 9);
}

int32_t *runtime_errno(void) {
    return &guest_errno;
}

static GuestThread *find_thread(const void *handle) {
    host_lock(&lock);
    ++lookups;
    size_t at = locate(&threads, handle);
    GuestThread *t = at == SIZE_MAX ? NULL : threads.slots[at];
    host_unlock(&lock);
    return t;
}

static ThreadAttr *find_attr(ThreadAttr **slot) {
    if (!slot || !*slot) return NULL;
    host_lock(&lock);
    size_t at = locate(&attributes, *slot);
    ThreadAttr *a = at == SIZE_MAX ? NULL : attributes.slots[at];
    host_unlock(&lock);
    return a && a->magic == ATTR_MAGIC ? a : NULL;
}

static SYS_ABI void *thread_self(void) {
    return current_thread();
}

static SYS_ABI int32_t *error_location(void) {
    return &guest_errno;
}

static SYS_ABI int32_t attr_init(ThreadAttr **out) {
    if (!out) return KERNEL_ERROR(E_INVALID);
    ThreadAttr *a = calloc(1, sizeof(*a));
    if (!a) return KERNEL_ERROR(E_NO_MEMORY);
    *a = (ThreadAttr){.magic = ATTR_MAGIC,
                      .policy = 1,
                      .prio = DEFAULT_PRIO,
                      .inherit = 4,
                      .stack = DEFAULT_STACK,
                      .guard = 4096,
                      .affinity = 0x7f};
    host_lock(&lock);
    int ok = insert(&attributes, a);
    host_unlock(&lock);
    if (!ok) {
        free(a);
        return KERNEL_ERROR(E_NO_MEMORY);
    }
    *out = a;
    return 0;
}

static SYS_ABI int32_t attr_destroy(ThreadAttr **slot) {
    if (!slot || !*slot) return KERNEL_ERROR(E_INVALID);
    host_lock(&lock);
    size_t at = locate(&attributes, *slot);
    ThreadAttr *a = at == SIZE_MAX ? NULL : attributes.slots[at];
    if (!a || a->magic != ATTR_MAGIC) {
        host_unlock(&lock);
        return KERNEL_ERROR(E_INVALID);
    }
    attributes.slots[at] = TOMBSTONE;
    --attributes.used;
    ++attributes.tombstones;
    a->magic = 0;
    host_unlock(&lock);
    free(a);
    *slot = NULL;
    return 0;
}

static SYS_ABI int32_t attr_get(void *thread, ThreadAttr **out) {
    ThreadAttr *a = find_attr(out);
    if (!a || !thread) return KERNEL_ERROR(E_INVALID);
    GuestThread *t = find_thread(thread);
    if (!t) return KERNEL_ERROR(E_SEARCH);
    *a = t->attr;
    a->magic = ATTR_MAGIC;
    a->detached = t->detached;
    return 0;
}

static SYS_ABI int32_t attr_set_stack(ThreadAttr **slot, uint64_t size) {
    ThreadAttr *a = find_attr(slot);
    if (!a || size < 16384) return KERNEL_ERROR(E_INVALID);
    a->stack = size;
    return 0;
}

static SYS_ABI int32_t attr_get_stack(ThreadAttr **slot, uint64_t *size) {
    ThreadAttr *a = find_attr(slot);
    if (!a || !size) return KERNEL_ERROR(E_INVALID);
    *size = a->stack;
    return 0;
}

static SYS_ABI int32_t attr_set_detach(ThreadAttr **slot, int state) {
    ThreadAttr *a = find_attr(slot);
    if (!a || (state != 0 && state != 1)) return KERNEL_ERROR(E_INVALID);
    a->detached = state;
    return 0;
}

static SYS_ABI int32_t attr_get_detach(ThreadAttr **slot, int *state) {
    ThreadAttr *a = find_attr(slot);
    if (!a || !state) return KERNEL_ERROR(E_INVALID);
    *state = a->detached;
    return 0;
}

static SYS_ABI int32_t attr_set_policy(ThreadAttr **slot, int policy) {
    ThreadAttr *a = find_attr(slot);
    if (!a || policy < 1 || policy > 3) return KERNEL_ERROR(E_INVALID);
    a->policy = policy;
    return 0;
}

static SYS_ABI int32_t attr_set_inherit(ThreadAttr **slot, int inherit) {
    ThreadAttr *a = find_attr(slot);
    if (!a || (inherit != 0 && inherit != 4)) return KERNEL_ERROR(E_INVALID);
    a->inherit = inherit;
    return 0;
}

static SYS_ABI int32_t attr_set_param(ThreadAttr **slot, const int *param) {
    ThreadAttr *a = find_attr(slot);
    if (!a || !param) return KERNEL_ERROR(E_INVALID);
    a->prio = *param;
    return 0;
}

static SYS_ABI int32_t attr_get_param(ThreadAttr **slot, int *param) {
    ThreadAttr *a = find_attr(slot);
    if (!a || !param) return KERNEL_ERROR(E_INVALID);
    *param = a->prio;
    return 0;
}

static SYS_ABI int32_t attr_set_affinity(ThreadAttr **slot, uint64_t mask) {
    ThreadAttr *a = find_attr(slot);
    if (!a) return KERNEL_ERROR(E_INVALID);
    a->affinity = mask;
    return 0;
}

static SYS_ABI int32_t attr_get_affinity(ThreadAttr **slot, uint64_t *mask) {
    ThreadAttr *a = find_attr(slot);
    if (!a || !mask) return KERNEL_ERROR(E_INVALID);
    *mask = a->affinity;
    return 0;
}

static SYS_ABI int32_t attr_set_guard(ThreadAttr **slot, uint64_t size) {
    ThreadAttr *a = find_attr(slot);
    if (!a) return KERNEL_ERROR(E_INVALID);
    a->guard = size;
    return 0;
}

static void set_host_name(const char *name) {
    char host[16] = {0};
    memcpy(host, name, strnlen(name, sizeof(host) - 1));
#ifdef _WIN32
    runtime_win_set_thread_name(host);
#else
    pthread_setname_np(pthread_self(), host);
#endif
}

static void apply_priority(int prio) {
    static int enabled = -1;
    if (enabled < 0) {
        const char *e = getenv("BB_AUDIO_THREAD_PRIORITY");
        enabled = !(e && e[0] == '0');
    }
    if (enabled) host_thread_set_high_priority(prio <= HIGH_PRIO_LAST);
    static int noted;
    if (prio <= HIGH_PRIO_LAST && !__atomic_exchange_n(&noted, 1, __ATOMIC_RELAXED))
        printf("Runtime: audio threads (priority %d-%d) %s\n", 256, HIGH_PRIO_LAST,
               enabled ? "above the game's other threads" : "at normal priority (BB_AUDIO_THREAD_PRIORITY=0)");
}

static void keys_cleanup(void) {
    for (int round = 0; round < 4; ++round) {
        int any = 0;
        for (uint32_t i = 1; i < KEYS; ++i) {
            void *value = key_values[i];
            if (!value || !keys[i].used || !keys[i].destructor) continue;
            key_values[i] = NULL;
            any = 1;
            keys[i].destructor(value);
        }
        if (!any) break;
    }
}

static void *host_start(void *p) {
    GuestThread *t = p;
    attach(t);
    set_host_name(t->name);
    if (t->attr.prio <= HIGH_PRIO_LAST) apply_priority(t->attr.prio);
    if (!EXIT_SET(t->exit_jump)) t->result = t->entry(t->argument);
    keys_cleanup();
    host_lock(&lock);
    t->finished = 1;
    ++exited;
    host_unlock(&lock);
    return t->result;
}

static int32_t create(GuestThread **out, ThreadAttr **attr_slot, GuestEntry entry, void *argument, const char *name) {
    if (!out || !entry) return KERNEL_ERROR(E_INVALID);
    ThreadAttr *a = NULL;
    if (attr_slot && !(a = find_attr(attr_slot))) return KERNEL_ERROR(E_INVALID);
    GuestThread *t = new_thread();
    if (!t) return KERNEL_ERROR(E_NO_MEMORY);
    if (a) t->attr = *a;
    t->entry = entry;
    t->argument = argument;
    t->detached = t->attr.detached;
    snprintf(t->name, sizeof(t->name), "%s", name ? name : "guest");
    uint64_t stack = t->attr.stack < MIN_STACK ? MIN_STACK : t->attr.stack;
    size_t stack_bytes = (size_t)stack + STACK_MARGIN;
    publish(t);
    *out = t;
#ifdef _WIN32
    int e = host_thread_start(&t->host, stack_bytes, host_start, t);
#else
    pthread_attr_t host;
    pthread_attr_init(&host);
    void *stack_memory = runtime_low_map(stack_bytes, PROT_READ | PROT_WRITE);
    if (stack_memory) pthread_attr_setstack(&host, stack_memory, stack_bytes);
    else pthread_attr_setstacksize(&host, stack_bytes);
    int e = pthread_create(&t->host, &host, host_start, t);
    pthread_attr_destroy(&host);
#endif
    if (e) stop("host thread creation failed", e);
    if (t->detached) host_thread_detach(t->host);
    host_lock(&lock);
    ++created;
    host_unlock(&lock);
    printf("Runtime: guest thread '%s' created (stack=%llu, prio=%d)\n", t->name, (unsigned long long)stack,
           t->attr.prio);
    return 0;
}

static SYS_ABI int32_t thread_create(GuestThread **out, ThreadAttr **attr, GuestEntry entry, void *argument,
                                     const char *name) {
    return create(out, attr, entry, argument, name);
}

static SYS_ABI int32_t thread_join(GuestThread *t, void **result) {
    if (!find_thread(t) || t->host_owned) return KERNEL_ERROR(E_SEARCH);
    if (t == current) return KERNEL_ERROR(E_DEADLOCK);
    if (t->detached || t->joined) return KERNEL_ERROR(E_INVALID);
    t->joined = 1;
    int e = host_thread_join(t->host);
    if (e) stop("host thread join failed", e);
    if (result) *result = t->result;
    host_lock(&lock);
    ++joined_count;
    host_unlock(&lock);
    return 0;
}

static SYS_ABI int32_t thread_detach(GuestThread *t) {
    if (!find_thread(t)) return KERNEL_ERROR(E_SEARCH);
    if (t->detached) return KERNEL_ERROR(E_INVALID);
    t->detached = 1;
    if (!t->host_owned) host_thread_detach(t->host);
    return 0;
}

static SYS_ABI __attribute__((noreturn)) void thread_exit(void *value) {
    GuestThread *t = current_thread();
    if (t->host_owned) {
        fputs("STOP: pthread_exit on host-owned/main thread\n", stderr);
        exit(21);
    }
    t->result = value;
    EXIT_JUMP(t->exit_jump);
}

static SYS_ABI int32_t thread_yield(void) {
    host_yield();
    return 0;
}

static SYS_ABI int32_t thread_get_prio(GuestThread *t, int *prio) {
    if (!find_thread(t)) return KERNEL_ERROR(E_SEARCH);
    if (!prio) return KERNEL_ERROR(E_INVALID);
    *prio = t->attr.prio;
    return 0;
}

static SYS_ABI int32_t thread_set_prio(GuestThread *t, int prio) {
    if (!find_thread(t)) return KERNEL_ERROR(E_SEARCH);
    if (t == current && (prio <= HIGH_PRIO_LAST) != (t->attr.prio <= HIGH_PRIO_LAST)) apply_priority(prio);
    t->attr.prio = prio;
    return 0;
}

static SYS_ABI int32_t thread_set_affinity(GuestThread *t, uint64_t mask) {
    if (!find_thread(t)) return KERNEL_ERROR(E_SEARCH);
    t->attr.affinity = mask;
    return 0;
}

static SYS_ABI int32_t thread_get_affinity(GuestThread *t, uint64_t *mask) {
    if (!find_thread(t)) return KERNEL_ERROR(E_SEARCH);
    if (!mask) return KERNEL_ERROR(E_INVALID);
    *mask = t->attr.affinity;
    return 0;
}

static SYS_ABI int32_t thread_rename(GuestThread *t, const char *name) {
    if (!find_thread(t)) return KERNEL_ERROR(E_SEARCH);
    if (!name) return KERNEL_ERROR(E_INVALID);
    snprintf(t->name, sizeof(t->name), "%s", name);
    if (t == current) set_host_name(t->name);
    return 0;
}

static SYS_ABI int32_t thread_equal(GuestThread *a, GuestThread *b) {
    return a == b;
}

static SYS_ABI int32_t thread_once(int32_t *once, OnceRoutine routine) {
    if (!once || !routine) return KERNEL_ERROR(E_INVALID);
    for (;;) {
        int32_t state = __atomic_load_n(once, __ATOMIC_ACQUIRE);
        if (state == 1) return 0;
        if (state == 0 && __atomic_compare_exchange_n(once, &state, 2, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            routine();
            __atomic_store_n(once, 1, __ATOMIC_RELEASE);
            return 0;
        }
        host_yield();
    }
}

static SYS_ABI int32_t key_create(uint32_t *key, KeyDestructor destructor) {
    if (!key) return KERNEL_ERROR(E_INVALID);
    host_lock(&key_lock);
    for (uint32_t i = 1; i < KEYS; ++i) {
        if (keys[i].used) continue;
        keys[i].used = 1;
        keys[i].destructor = destructor;
        host_unlock(&key_lock);
        *key = i;
        return 0;
    }
    host_unlock(&key_lock);
    return KERNEL_ERROR(E_AGAIN);
}

static SYS_ABI int32_t key_delete(uint32_t key) {
    if (!key || key >= KEYS) return KERNEL_ERROR(E_INVALID);
    host_lock(&key_lock);
    int32_t result = keys[key].used ? 0 : KERNEL_ERROR(E_INVALID);
    keys[key].used = 0;
    keys[key].destructor = NULL;
    host_unlock(&key_lock);
    return result;
}

static SYS_ABI int32_t key_set(uint32_t key, void *value) {
    if (!key || key >= KEYS || !keys[key].used) return KERNEL_ERROR(E_INVALID);
    key_values[key] = value;
    return 0;
}

static SYS_ABI void *key_get(uint32_t key) {
    return key && key < KEYS ? key_values[key] : NULL;
}

static int32_t posix(int32_t result) {
    return result ? (int32_t)((uint32_t)result & 0xffff) : 0;
}

static SYS_ABI int32_t posix_attr_init(ThreadAttr **a) { return posix(attr_init(a)); }
static SYS_ABI int32_t posix_attr_destroy(ThreadAttr **a) { return posix(attr_destroy(a)); }
static SYS_ABI int32_t posix_attr_set_detach(ThreadAttr **a, int s) { return posix(attr_set_detach(a, s)); }
static SYS_ABI int32_t posix_attr_set_stack(ThreadAttr **a, uint64_t s) { return posix(attr_set_stack(a, s)); }
static SYS_ABI int32_t posix_attr_set_param(ThreadAttr **a, const int *p) { return posix(attr_set_param(a, p)); }
static SYS_ABI int32_t posix_create(GuestThread **t, ThreadAttr **a, GuestEntry e, void *arg) {
    return posix(create(t, a, e, arg, "posix"));
}
static SYS_ABI int32_t posix_create_name(GuestThread **t, ThreadAttr **a, GuestEntry e, void *arg, const char *name) {
    return posix(create(t, a, e, arg, name));
}
static SYS_ABI int32_t posix_join(GuestThread *t, void **r) { return posix(thread_join(t, r)); }

int32_t runtime_thread_spawn(void **out, void *(SYS_ABI *entry)(void *), void *argument, uint64_t stack,
                             const char *name) {
    ThreadAttr *attr = NULL;
    int32_t result = attr_init(&attr);
    if (!result && stack) result = attr_set_stack(&attr, stack);
    if (!result) result = create((GuestThread **)out, &attr, entry, argument, name);
    if (attr) attr_destroy(&attr);
    return posix(result);
}

int32_t runtime_thread_join_spawned(void *thread, void **result) {
    return posix(thread_join(thread, result));
}

__attribute__((noreturn)) void runtime_thread_exit(void *value) {
    thread_exit(value);
}

void runtime_thread_census(uint64_t *made, uint64_t *finished, uint64_t *found, uint64_t *steps) {
    host_lock(&lock);
    *made = created;
    *finished = exited;
    *found = lookups;
    *steps = probes;
    host_unlock(&lock);
}

void runtime_thread_report(void) {
    host_lock(&lock);
    printf("Runtime: guest threads created=%zu, exited=%zu, joined=%zu\n", created, exited, joined_count);
    host_unlock(&lock);
}

static const SysExport kernel_exports[] = {
    {"aI+OeCz8xrQ", "scePthreadSelf", thread_self},
    {"9BcDykPmo1I", "__error", error_location},
    {"nsYoNRywwNg", "scePthreadAttrInit", attr_init},
    {"62KCwEMmzcM", "scePthreadAttrDestroy", attr_destroy},
    {"x1X76arYMxU", "scePthreadAttrGet", attr_get},
    {"8+s5BzZjxSg", "scePthreadAttrGetaffinity", attr_get_affinity},
    {"UTXzJbWhhTE", "scePthreadAttrSetstacksize", attr_set_stack},
    {"-Wreprtu0Qs", "scePthreadAttrSetdetachstate", attr_set_detach},
    {"4+h9EzwKF4I", "scePthreadAttrSetschedpolicy", attr_set_policy},
    {"DzES9hQF4f4", "scePthreadAttrSetschedparam", attr_set_param},
    {"3qxgM4ezETA", "scePthreadAttrSetaffinity", attr_set_affinity},
    {"eXbUSpEaTsA", "scePthreadAttrSetinheritsched", attr_set_inherit},
    {"JaRMy+QcpeU", "scePthreadAttrGetdetachstate", attr_get_detach},
    {"-fA+7ZlGDQs", "scePthreadAttrGetstacksize", attr_get_stack},
    {"FXPWHNk8Of0", "scePthreadAttrGetschedparam", attr_get_param},
    {"El+cQ20DynU", "scePthreadAttrSetguardsize", attr_set_guard},
    {"6UgtwV+0zb4", "scePthreadCreate", thread_create},
    {"onNY9Byn-W8", "scePthreadJoin", thread_join},
    {"4qGrR6eoP9Y", "scePthreadDetach", thread_detach},
    {"3kg7rT0NQIs", "scePthreadExit", thread_exit},
    {"T72hz6ffq08", "scePthreadYield", thread_yield},
    {"1tKyG7RlMJo", "scePthreadGetprio", thread_get_prio},
    {"W0Hpm2X0uPE", "scePthreadSetprio", thread_set_prio},
    {"bt3CTBKmGyI", "scePthreadSetaffinity", thread_set_affinity},
    {"rcrVFJsQWRY", "scePthreadGetaffinity", thread_get_affinity},
    {"GBUY7ywdULE", "scePthreadRename", thread_rename},
    {"3PtV6p3QNX4", "scePthreadEqual", thread_equal},
    {"14bOACANTBo", "scePthreadOnce", thread_once},
    {"geDaqgH9lTg", "scePthreadKeyCreate", key_create},
    {"PrdHuuDekhY", "scePthreadKeyDelete", key_delete},
    {"+BzXYkqYeLE", "scePthreadSetspecific", key_set},
    {"eoht7mQOCmo", "scePthreadGetspecific", key_get},
};

static const SysExport posix_exports[] = {
    {"EotR8a3ASf4", "pthread_self", thread_self},
    {"wtkt-teR1so", "pthread_attr_init", posix_attr_init},
    {"zHchY8ft5pk", "pthread_attr_destroy", posix_attr_destroy},
    {"E+tyo3lp5Lw", "pthread_attr_setdetachstate", posix_attr_set_detach},
    {"2Q0z6rnBrTE", "pthread_attr_setstacksize", posix_attr_set_stack},
    {"euKRgm0Vn2M", "pthread_attr_setschedparam", posix_attr_set_param},
    {"OxhIB8LB-PQ", "pthread_create", posix_create},
    {"Jmi+9w9u0E4", "pthread_create_name_np", posix_create_name},
    {"h9CcP3J0oVM", "pthread_join", posix_join},
    {"FJrT5LuUBAU", "pthread_exit", thread_exit},
};

SYS_LIBRARY(sys_thread, "libkernel", kernel_exports);
SYS_LIBRARY(sys_posix_thread, "libScePosix", posix_exports);
