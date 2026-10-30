// SPDX-License-Identifier: GPL-3.0-or-later
#include "sys.h"
#include "video_out.h"
#include "runtime.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <x86intrin.h>

#define KERNEL_ERROR(code) ((int32_t)(UINT32_C(0x80020000) | (code)))
#define E_BAD_HANDLE KERNEL_ERROR(9)
#define E_NO_MEMORY KERNEL_ERROR(12)
#define E_FAULT KERNEL_ERROR(14)
#define E_INVALID KERNEL_ERROR(22)
#define E_TIMED_OUT KERNEL_ERROR(60)
#define E_NAME_TOO_LONG KERNEL_ERROR(63)

enum { FIRST_HANDLE = 0x40000, MAX_QUEUES = 1024, MAX_NAME = 32 };
enum { FLAG_ADD = 1, FLAG_CLEAR = 0x20, FLAG_ONE_SHOT = 0x10 };
enum { FLIP_ALWAYS_IDENT = 0xfe };

typedef struct {
    uint64_t ident;
    int16_t filter;
    uint16_t flags;
    uint32_t fflags;
    uint64_t data;
    void *udata;
} Event;
_Static_assert(sizeof(Event) == 32, "SceKernelEvent");

typedef struct {
    Event event;
    int triggered;
} Entry;

typedef struct {
    HostMutex lock;
    HostCond ready;
    Entry *entries;
    size_t count, capacity;
    int deleted;
    char name[MAX_NAME + 1];
    _Atomic uint64_t waits, wait_ns, delivered;
} Queue;

static _Atomic(Queue *) queues[MAX_QUEUES];
static _Atomic int32_t next_slot;

static Queue *queue_of(int64_t handle) {
    int64_t slot = handle - FIRST_HANDLE;
    if (slot < 0 || slot >= MAX_QUEUES) return NULL;
    Queue *queue = atomic_load_explicit(&queues[slot], memory_order_acquire);
    return queue && !__atomic_load_n(&queue->deleted, __ATOMIC_ACQUIRE) ? queue : NULL;
}

static Entry *find_entry(Queue *queue, uint64_t ident, int16_t filter) {
    for (size_t i = 0; i < queue->count; ++i)
        if (queue->entries[i].event.ident == ident && queue->entries[i].event.filter == filter) return &queue->entries[i];
    return NULL;
}

int sys_equeue_exists(int64_t handle) {
    return queue_of(handle) != NULL;
}

int sys_equeue_add(int64_t handle, uint64_t ident, int16_t filter, void *udata) {
    Queue *queue = queue_of(handle);
    if (!queue) return -1;
    int result = 0;
    host_lock(&queue->lock);
    Entry *entry = find_entry(queue, ident, filter);
    if (entry) {
        entry->event.udata = udata;
    } else {
        if (queue->count == queue->capacity) {
            size_t capacity = queue->capacity ? queue->capacity * 2 : 8;
            Entry *grown = realloc(queue->entries, capacity * sizeof(*grown));
            if (grown) {
                queue->entries = grown;
                queue->capacity = capacity;
            }
        }
        if (queue->count < queue->capacity)
            queue->entries[queue->count++] = (Entry){{ident, filter, FLAG_CLEAR, 0, 0, udata}, 0};
        else
            result = -1;
    }
    host_unlock(&queue->lock);
    return result;
}

int sys_equeue_remove(int64_t handle, uint64_t ident, int16_t filter) {
    Queue *queue = queue_of(handle);
    if (!queue) return -1;
    host_lock(&queue->lock);
    Entry *entry = find_entry(queue, ident, filter);
    if (entry) memmove(entry, entry + 1, (size_t)(queue->entries + --queue->count - entry) * sizeof(*entry));
    host_unlock(&queue->lock);
    return entry ? 0 : -1;
}

int sys_equeue_trigger(int64_t handle, uint64_t ident, int16_t filter, uint64_t value) {
    Queue *queue = queue_of(handle);
    if (!queue) return -1;
    int found = 0;
    host_lock(&queue->lock);
    for (size_t i = 0; i < queue->count; ++i) {
        Entry *entry = &queue->entries[i];
        if (entry->event.ident != ident || entry->event.filter != filter) continue;
        found = 1;
        entry->triggered = 1;
        if (filter != SYS_EQUEUE_VIDEO_OUT) {
            entry->event.data = value;
        } else if (value && (value & 0xff) == ident && ident != FLIP_ALWAYS_IDENT) {
            uint64_t count = (entry->event.data >> 12) & 0xf;
            if (count != 0xf) ++count;
            entry->event.data = (__rdtsc() & 0xfff) | (count << 12) | (value & UINT64_C(0xffffffffffff0000));
        }
    }
    host_unlock(&queue->lock);
    if (found) host_cond_signal(&queue->ready);
    return found ? 0 : -1;
}

static int collect(Queue *queue, Event *out, int capacity) {
    int count = 0;
    for (size_t i = 0; i < queue->count && count < capacity; ++i) {
        Entry *entry = &queue->entries[i];
        if (!entry->triggered) continue;
        out[count++] = entry->event;
        if (entry->event.flags & FLAG_CLEAR) {
            entry->triggered = 0;
            entry->event.fflags = 0;
            entry->event.data = 0;
        }
    }
    return count;
}

static SYS_ABI int32_t equeue_create(int64_t *handle, const char *name) {
    if (!handle || !name) return E_INVALID;
    if (strlen(name) > MAX_NAME) return E_NAME_TOO_LONG;
    int32_t slot = atomic_fetch_add(&next_slot, 1);
    if (slot >= MAX_QUEUES) return E_NO_MEMORY;
    Queue *queue = calloc(1, sizeof(*queue));
    if (!queue) return E_NO_MEMORY;
    host_mutex_init(&queue->lock);
    host_cond_init(&queue->ready);
    snprintf(queue->name, sizeof(queue->name), "%s", name);
    atomic_store_explicit(&queues[slot], queue, memory_order_release);
    *handle = FIRST_HANDLE + slot;
    return 0;
}

static SYS_ABI int32_t equeue_delete(int64_t handle) {
    Queue *queue = queue_of(handle);
    if (!queue) return E_BAD_HANDLE;
    host_lock(&queue->lock);
    __atomic_store_n(&queue->deleted, 1, __ATOMIC_RELEASE);
    free(queue->entries);
    queue->entries = NULL;
    queue->count = queue->capacity = 0;
    host_unlock(&queue->lock);
    host_cond_broadcast(&queue->ready);
    return 0;
}

static SYS_ABI int32_t equeue_wait(int64_t handle, Event *events, int capacity, int *out, const uint32_t *timeout_us) {
    Queue *queue = queue_of(handle);
    if (!queue) return E_BAD_HANDLE;
    if (!events) return E_FAULT;
    if (capacity < 1) {
        *out = 0;
        return E_INVALID;
    }
    uint64_t started = host_monotonic_ns();
    int deadline_set = timeout_us != NULL;
    uint64_t deadline = deadline_set ? started + (uint64_t)*timeout_us * 1000 : 0;
    int count;
    host_lock(&queue->lock);
    while (!(count = collect(queue, events, capacity)) && !queue->deleted) {
        if (!deadline_set) {
            host_cond_wait(&queue->ready, &queue->lock);
            continue;
        }
        uint64_t now = host_monotonic_ns();
        if (now >= deadline) break;
        host_cond_timedwait(&queue->ready, &queue->lock, deadline - now);
    }
    host_unlock(&queue->lock);
    atomic_fetch_add_explicit(&queue->waits, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&queue->wait_ns, host_monotonic_ns() - started, memory_order_relaxed);
    atomic_fetch_add_explicit(&queue->delivered, (uint64_t)count, memory_order_relaxed);
    *out = count;
    return count ? 0 : E_TIMED_OUT;
}

uint64_t sys_equeue_event_data(const void *event) {
    return ((const Event *)event)->data;
}

void sys_equeue_report(double frames) {
    if (frames <= 0) return;
    int printed = 0;
    for (int i = 0; i < MAX_QUEUES; ++i) {
        Queue *queue = atomic_load(&queues[i]);
        if (!queue) break;
        uint64_t waits = atomic_exchange(&queue->waits, 0), ns = atomic_exchange(&queue->wait_ns, 0);
        uint64_t delivered = atomic_exchange(&queue->delivered, 0);
        if (!waits) continue;
        printf("%s %s %.1f waits %.2f ms %.1f events", printed++ ? ";" : "Guest event waits per frame:", queue->name,
               (double)waits / frames, (double)ns / (frames * 1e6), (double)delivered / frames);
    }
    if (printed) printf("\n");
}

static const SysExport equeue_exports[] = {
    {"D0OdFMjp46I", "sceKernelCreateEqueue", equeue_create},
    {"jpFjmgAC5AE", "sceKernelDeleteEqueue", equeue_delete},
    {"fzyMKs9kim0", "sceKernelWaitEqueue", equeue_wait},
};
SYS_LIBRARY(sys_equeue, "libkernel", equeue_exports);
