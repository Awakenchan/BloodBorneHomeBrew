// SPDX-License-Identifier: GPL-3.0-or-later
#include "runtime.h"
#include "sys/sys.h"
#include "sys/video_out.h"
#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GUEST __attribute__((sysv_abi))
#define CODE(x) ((uint32_t)(x))

typedef struct {
    uint64_t ident;
    int16_t filter;
    uint16_t flags;
    uint32_t fflags;
    uint64_t data;
    void *udata;
} Event;

typedef int32_t (GUEST *CreateFn)(int64_t *, const char *);
typedef int32_t (GUEST *DeleteFn)(int64_t);
typedef int32_t (GUEST *WaitFn)(int64_t, Event *, int, int *, const uint32_t *);
typedef int32_t (GUEST *OpenFn)(int32_t, int32_t, int32_t, const void *);
typedef int32_t (GUEST *CloseFn)(int32_t);
typedef int32_t (GUEST *AddFlipFn)(int64_t, int32_t, void *);
typedef int32_t (GUEST *StatusFn)(int32_t, SysVideoOutFlipStatus *);
typedef int32_t (GUEST *RegisterFn)(int32_t, int32_t, void *const *, int32_t, const SysVideoOutAttribute *);
typedef void (GUEST *SetAttributeFn)(SysVideoOutAttribute *, uint32_t, int32_t, int32_t, uint32_t, uint32_t, uint32_t);
typedef int32_t (GUEST *RateFn)(int32_t, int32_t);
typedef int32_t (GUEST *SubmitFn)(int32_t, int32_t, int32_t, int64_t);

static CreateFn create;
static DeleteFn destroy;
static WaitFn wait_for;
static OpenFn vo_open;
static CloseFn vo_close;
static AddFlipFn add_flip;
static StatusFn status;
static RegisterFn register_buffers;
static SetAttributeFn set_attribute;
static RateFn set_rate;
static SubmitFn submit;

static int failures;
#define EXPECT(condition) do { if (!(condition)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); ++failures; } } while (0)

static void *fn(const char *nid) {
    const SysExport *found = sys_find(nid, NULL);
    if (!found) {
        printf("FAIL: %s is missing\n", nid);
        exit(1);
    }
    return found->function;
}

static void bind(void) {
    create = (CreateFn)fn("D0OdFMjp46I");
    destroy = (DeleteFn)fn("jpFjmgAC5AE");
    wait_for = (WaitFn)fn("fzyMKs9kim0");
    vo_open = (OpenFn)fn("Up36PTk687E");
    vo_close = (CloseFn)fn("uquVH4-Du78");
    add_flip = (AddFlipFn)fn("HXzjK9yI30k");
    status = (StatusFn)fn("SbU3dwp80lQ");
    register_buffers = (RegisterFn)fn("w3BY+tAEiQY");
    set_attribute = (SetAttributeFn)fn("i6-sR91Wt-4");
    set_rate = (RateFn)fn("CBiu4mCE1DA");
    submit = (SubmitFn)fn("U46NwOiJpys");
}

typedef struct {
    int32_t index;
    int64_t argument;
    int from_gpu;
} Queued;

static HostMutex queue_lock = HOST_MUTEX_INIT;
static Queued queued[4096];
static unsigned queued_head, queued_tail;
static atomic_int opened_calls, surface_calls, label_calls;

static void host_opened(void) { atomic_fetch_add(&opened_calls, 1); }
static void host_surface(const SysVideoOutGroup *group, uint64_t address) {
    (void)group;
    (void)address;
    atomic_fetch_add(&surface_calls, 1);
}
static void host_queue(int32_t index, int64_t argument, int from_gpu) {
    host_lock(&queue_lock);
    queued[queued_tail++ % 4096] = (Queued){index, argument, from_gpu};
    host_unlock(&queue_lock);
}
static void host_label(void) { atomic_fetch_add(&label_calls, 1); }
static const SysVideoOutHost fake_host = {host_opened, host_surface, host_queue, host_label};

static int take_queued(Queued *out) {
    host_lock(&queue_lock);
    int have = queued_head != queued_tail;
    if (have) *out = queued[queued_head++ % 4096];
    host_unlock(&queue_lock);
    return have;
}

static void *wait_blocked(void *argument) {
    Event event;
    int out = 0;
    int32_t r = wait_for(*(int64_t *)argument, &event, 1, &out, NULL);
    EXPECT(r == 0 && out == 1 && event.ident == 0x77);
    return NULL;
}

static void test_equeue(void) {
    int64_t q = 0;
    Event events[4];
    int out = -1;
    const uint32_t zero = 0, short_wait = 2000;
    EXPECT(CODE(create(NULL, "x")) == 0x80020016);
    EXPECT(CODE(create(&q, NULL)) == 0x80020016);
    EXPECT(CODE(create(&q, "012345678901234567890123456789012")) == 0x8002003f);
    EXPECT(create(&q, "01234567890123456789012345678901") == 0);
    EXPECT(q >= 0x40000);
    EXPECT(CODE(wait_for(q + 5000, events, 1, &out, &zero)) == 0x80020009);
    EXPECT(CODE(wait_for(q, NULL, 1, &out, &zero)) == 0x8002000e);
    out = 9;
    EXPECT(CODE(wait_for(q, events, 0, &out, &zero)) == 0x80020016 && out == 0);
    EXPECT(CODE(wait_for(q, events, 1, &out, &zero)) == 0x8002003c && out == 0);
    uint64_t started = host_monotonic_ns();
    EXPECT(CODE(wait_for(q, events, 1, &out, &short_wait)) == 0x8002003c && out == 0);
    EXPECT(host_monotonic_ns() - started >= 1500000);

    EXPECT(sys_equeue_add(q, 0x40, SYS_EQUEUE_GRAPHICS_CORE, (void *)7) == 0);
    EXPECT(sys_equeue_trigger(q, 0x41, SYS_EQUEUE_GRAPHICS_CORE, 1) == -1);
    EXPECT(sys_equeue_trigger(q, 0x40, SYS_EQUEUE_VIDEO_OUT, 1) == -1);
    EXPECT(sys_equeue_trigger(q + 5000, 0x40, SYS_EQUEUE_GRAPHICS_CORE, 1) == -1);
    EXPECT(sys_equeue_trigger(q, 0x40, SYS_EQUEUE_GRAPHICS_CORE, 0x40) == 0);
    EXPECT(wait_for(q, events, 4, &out, NULL) == 0 && out == 1);
    EXPECT(events[0].ident == 0x40 && events[0].filter == -14 && events[0].flags == 0x20 && events[0].data == 0x40 && events[0].udata == (void *)7);
    EXPECT(sys_equeue_event_data(&events[0]) == 0x40);
    EXPECT(CODE(wait_for(q, events, 4, &out, &zero)) == 0x8002003c);

    EXPECT(sys_equeue_add(q, 0x40, SYS_EQUEUE_GRAPHICS_CORE, (void *)8) == 0);
    EXPECT(sys_equeue_trigger(q, 0x40, SYS_EQUEUE_GRAPHICS_CORE, 0x40) == 0);
    EXPECT(sys_equeue_add(q, 0x40, SYS_EQUEUE_GRAPHICS_CORE, (void *)9) == 0);
    EXPECT(wait_for(q, events, 1, &out, &zero) == 0 && out == 1 && events[0].udata == (void *)9);

    EXPECT(sys_equeue_add(q, 6, SYS_EQUEUE_VIDEO_OUT, (void *)1) == 0);
    EXPECT(sys_equeue_trigger(q, 6, SYS_EQUEUE_VIDEO_OUT, 6 | (UINT64_C(0x1234) << 16)) == 0);
    EXPECT(sys_equeue_trigger(q, 6, SYS_EQUEUE_VIDEO_OUT, 6 | (UINT64_C(0x5678) << 16)) == 0);
    EXPECT(wait_for(q, events, 1, &out, &zero) == 0 && out == 1);
    EXPECT((events[0].data >> 16) == 0x5678 && ((events[0].data >> 12) & 0xf) == 2);
    EXPECT(sys_equeue_trigger(q, 6, SYS_EQUEUE_VIDEO_OUT, 7 | (UINT64_C(0x9999) << 16)) == 0);
    EXPECT(wait_for(q, events, 1, &out, &zero) == 0 && events[0].data == 0);
    for (int i = 0; i < 20; ++i) EXPECT(sys_equeue_trigger(q, 6, SYS_EQUEUE_VIDEO_OUT, 6 | (UINT64_C(1) << 16)) == 0);
    EXPECT(wait_for(q, events, 1, &out, &zero) == 0 && ((events[0].data >> 12) & 0xf) == 0xf);

    EXPECT(sys_equeue_trigger(q, 6, SYS_EQUEUE_VIDEO_OUT, 6 | 0xf000 | (UINT64_C(1) << 16)) == 0);
    EXPECT(wait_for(q, events, 1, &out, &zero) == 0 && ((events[0].data >> 12) & 0xf) == 1 && (events[0].data >> 16) == 1);

    EXPECT(sys_equeue_remove(q, 6, SYS_EQUEUE_VIDEO_OUT) == 0);
    EXPECT(sys_equeue_remove(q, 6, SYS_EQUEUE_VIDEO_OUT) == -1);
    EXPECT(!sys_equeue_exists(q + (INT64_C(1) << 40)) && !sys_equeue_exists(q - (INT64_C(1) << 40)));

    EXPECT(sys_equeue_add(q, 9, SYS_EQUEUE_GRAPHICS_CORE, (void *)1) == 0 && sys_equeue_add(q, 9, SYS_EQUEUE_VIDEO_OUT, (void *)2) == 0);
    EXPECT(sys_equeue_remove(q, 9, SYS_EQUEUE_VIDEO_OUT) == 0);
    EXPECT(sys_equeue_trigger(q, 9, SYS_EQUEUE_VIDEO_OUT, 9) == -1 && sys_equeue_trigger(q, 9, SYS_EQUEUE_GRAPHICS_CORE, 9) == 0);
    EXPECT(wait_for(q, events, 1, &out, &zero) == 0 && out == 1 && events[0].filter == SYS_EQUEUE_GRAPHICS_CORE && events[0].udata == (void *)1);

    for (uint64_t i = 1; i <= 4; ++i) EXPECT(sys_equeue_add(q, 0x50 + i, SYS_EQUEUE_GRAPHICS_CORE, NULL) == 0);
    EXPECT(sys_equeue_remove(q, 0x51, SYS_EQUEUE_GRAPHICS_CORE) == 0);
    for (uint64_t i = 2; i <= 4; ++i) EXPECT(sys_equeue_trigger(q, 0x50 + i, SYS_EQUEUE_GRAPHICS_CORE, i) == 0);
    EXPECT(wait_for(q, events, 2, &out, &zero) == 0 && out == 2 && events[0].ident == 0x52 && events[1].ident == 0x53);
    EXPECT(wait_for(q, events, 4, &out, &zero) == 0 && out == 1 && events[0].ident == 0x54);

    HostThread blocked;
    EXPECT(sys_equeue_add(q, 0x77, SYS_EQUEUE_GRAPHICS_CORE, NULL) == 0);
    host_thread_start(&blocked, 0, wait_blocked, &q);
    host_sleep_ns(20000000);
    EXPECT(sys_equeue_trigger(q, 0x77, SYS_EQUEUE_GRAPHICS_CORE, 1) == 0);
    host_thread_join(blocked);
    EXPECT(destroy(q) == 0);
    EXPECT(CODE(destroy(q)) == 0x80020009);
    EXPECT(!sys_equeue_exists(q));
    EXPECT(sys_equeue_add(q, 1, SYS_EQUEUE_GRAPHICS_CORE, NULL) == -1);
}

static void test_ports(void) {
    sys_video_out_set_host(&fake_host);
    SysVideoOutAttribute a;
    memset(&a, 0xff, sizeof(a));
    set_attribute(&a, 0x80000000, 0, 0, 1920, 1080, 1920);
    EXPECT(a.pixel_format == 0x80000000 && a.tiling_mode == 0 && a.width == 1920 && a.height == 1080 && a.pitch_in_pixel == 1920);
    EXPECT(a.aspect_ratio == 0 && a.option == 0 && a.reserved0 == 0 && a.reserved1 == 0);

    void *addresses[3] = {(void *)0x1000, (void *)0x2000, (void *)0x3000};
    EXPECT(CODE(register_buffers(1, 0, addresses, 1, &a)) == 0x8029000b);
    EXPECT(CODE(vo_open(0, 5, 0, NULL)) == 0x80290001);
    EXPECT(CODE(vo_open(0, 0, 1, NULL)) == 0x80290001);
    EXPECT(vo_open(0, 0, 0, NULL) == 1);
    EXPECT(atomic_load(&opened_calls) == 1);
    EXPECT(CODE(vo_open(0, 0, 0, NULL)) == 0x80290009);
    EXPECT(CODE(register_buffers(1, 0, NULL, 1, &a)) == 0x80290002);
    EXPECT(CODE(register_buffers(1, 0, addresses, 1, NULL)) == 0x80290002);
    EXPECT(CODE(register_buffers(2, 0, addresses, 1, &a)) == 0x8029000b);
    EXPECT(CODE(register_buffers(1, 15, addresses, 2, &a)) == 0x80290001);
    EXPECT(CODE(register_buffers(1, 17, addresses, 0, &a)) == 0x80290001);
    SysVideoOutAttribute bad = a;
    bad.reserved0 = 1;
    EXPECT(CODE(register_buffers(1, 0, addresses, 1, &bad)) == 0x80290001);
    bad = a;
    bad.reserved1 = 1;
    EXPECT(CODE(register_buffers(1, 0, addresses, 1, &bad)) == 0x80290001);
    bad = a;
    bad.aspect_ratio = 1;
    EXPECT(CODE(register_buffers(1, 0, addresses, 1, &bad)) == 0x80290008);
    bad = a;
    bad.width = bad.pitch_in_pixel + 1;
    EXPECT(CODE(register_buffers(1, 0, addresses, 1, &bad)) == 0x80290004);
    bad = a;
    bad.tiling_mode = 2;
    EXPECT(CODE(register_buffers(1, 0, addresses, 1, &bad)) == 0x80290007);
    bad.tiling_mode = -1;
    EXPECT(CODE(register_buffers(1, 0, addresses, 1, &bad)) == 0x80290007);
    EXPECT(register_buffers(1, 0, addresses, 2, &a) == 0);
    EXPECT(atomic_load(&surface_calls) == 2);
    EXPECT(CODE(register_buffers(1, 1, addresses, 1, &a)) == 0x80290010);
    EXPECT(register_buffers(1, 2, addresses + 2, 1, &a) == 1);
    EXPECT(register_buffers(1, 3, addresses, 0, &a) == 2);
    EXPECT(register_buffers(1, 3, addresses, 0, &a) == 3);
    EXPECT(CODE(register_buffers(1, 3, addresses, 0, &a)) == 0x8029000f);

    const SysVideoOutGroup *group = NULL;
    uint64_t address = 0;
    EXPECT(sys_video_out_buffer(1, &group, &address) && address == 0x2000 && group->is_occupied && group->attrib.width == 1920);
    EXPECT(sys_video_out_buffer(2, &group, &address) && address == 0x3000);
    EXPECT(!sys_video_out_buffer(3, &group, &address));
    EXPECT(!sys_video_out_buffer(-1, &group, &address));
    EXPECT(!sys_video_out_buffer(16, &group, &address));

    int64_t q = 0;
    EXPECT(create(&q, "flips") == 0);
    EXPECT(CODE(add_flip(q, 2, NULL)) == 0x8029000b);
    EXPECT(CODE(add_flip(q + 5000, 1, NULL)) == 0x8029000c);
    EXPECT(add_flip(q, 1, (void *)0x77) == 0);

    SysVideoOutFlipStatus s;
    EXPECT(CODE(status(1, NULL)) == 0x80290002);
    EXPECT(CODE(status(2, &s)) == 0x8029000b);
    EXPECT(status(1, &s) == 0 && s.count == 0 && s.flip_arg == -1 && s.current_buffer == -1 && s.flip_pending_num == 0);

    EXPECT(CODE(submit(2, 0, 1, 1)) == 0x8029000b);
    EXPECT(CODE(submit(1, -2, 1, 1)) == 0x8029000a);
    EXPECT(CODE(submit(1, 16, 1, 1)) == 0x8029000a);
    EXPECT(CODE(submit(1, 3, 1, 1)) == 0x8029000a);
    EXPECT(submit(1, 1, 1, 42) == 0);
    Queued item;
    EXPECT(take_queued(&item) && item.index == 1 && item.argument == 42 && !item.from_gpu);
    EXPECT(status(1, &s) == 0 && s.flip_pending_num == 1 && s.gc_queue_num == 0 && s.submit_tsc != 0 && s.count == 0);
    uint64_t *labels = (uint64_t *)sys_video_out_labels();
    labels[1] = 1;
    int before = atomic_load(&label_calls);
    sys_video_out_flip_done(1, 42, 0);
    EXPECT(labels[1] == 1 && atomic_load(&label_calls) == before);
    EXPECT(status(1, &s) == 0 && s.count == 1 && s.flip_pending_num == 0 && s.flip_arg == 42 && s.current_buffer == 1 && s.tsc != 0);
    Event events[2];
    int out = 0;
    EXPECT(wait_for(q, events, 2, &out, NULL) == 0 && out == 1 && events[0].ident == 6 && events[0].udata == (void *)0x77 && (events[0].data >> 16) == 42);

    EXPECT(sys_video_out_queue_flip(0, -5, 1) == 1);
    EXPECT(take_queued(&item) && item.index == 0 && item.argument == -5 && item.from_gpu);
    EXPECT(status(1, &s) == 0 && s.flip_pending_num == 1 && s.gc_queue_num == 1);
    labels[0] = 1;
    sys_video_out_flip_done(0, -5, 1);
    EXPECT(labels[1] == 0 && labels[0] == 1 && atomic_load(&label_calls) == before + 1);
    EXPECT(status(1, &s) == 0 && s.flip_pending_num == 0 && s.gc_queue_num == 0 && s.flip_arg == -5 && s.current_buffer == 0 && s.count == 2);
    EXPECT(wait_for(q, events, 2, &out, NULL) == 0 && (int64_t)events[0].data >> 16 == -5);

    EXPECT(submit(1, -1, 1, 3) == 0);
    EXPECT(take_queued(&item) && item.index == -1);
    sys_video_out_flip_done(-1, 3, 0);
    EXPECT(labels[0] == 0);
    EXPECT(status(1, &s) == 0 && s.current_buffer == -1);
    labels[0] = 1;
    EXPECT(submit(1, 0, 1, 4) == 0 && take_queued(&item));
    sys_video_out_flip_done(0, 4, 0);
    EXPECT(labels[0] == 1);

    for (int i = 0; i < 17; ++i) EXPECT(submit(1, 1, 1, i) == 0);
    EXPECT(CODE(submit(1, 1, 1, 99)) == 0x80290012);
    EXPECT(submit(1, -1, 1, 99) == 0);
    EXPECT(sys_video_out_queue_flip(1, 99, 1) == 1);
    while (take_queued(&item)) sys_video_out_flip_done(item.index, item.argument, item.from_gpu);
    EXPECT(status(1, &s) == 0 && s.flip_pending_num == 0 && s.gc_queue_num == 0);

    EXPECT(set_rate(1, 3) == 0 && sys_video_out_flip_rate() == 3);
    sys_video_out_flip_done(1, 0, 0);
    EXPECT(vo_close(1) == 0);
    EXPECT(!sys_video_out_is_open() && sys_video_out_flip_rate() == 0);
    EXPECT(!sys_video_out_buffer(1, &group, &address));
    EXPECT(status(1, &s) == 0 && s.count == 0 && s.flip_arg == -1);
    EXPECT(sys_equeue_trigger(q, 6, SYS_EQUEUE_VIDEO_OUT, 6) == -1);
    EXPECT(vo_open(0, 0, 0, NULL) == 1);
    EXPECT(register_buffers(1, 0, addresses, 1, &a) == 0);
    labels[1] = 1;
    sys_video_out_flip_done(0, 0, 0);
    EXPECT(labels[1] == 1);
    EXPECT(vo_close(1) == 0);
    EXPECT(destroy(q) == 0);
}

typedef struct {
    int64_t queue;
    atomic_int stop;
    atomic_uint_fast64_t delivered, triggered, flips;
    uint64_t wake_ns_total, wake_count, wake_ns_worst;
} Stress;

static void *stress_waiter(void *argument) {
    Stress *s = argument;
    const uint32_t timeout = 5000;
    for (;;) {
        Event events[4];
        int out = 0;
        int32_t r = wait_for(s->queue, events, 4, &out, &timeout);
        if (!r) atomic_fetch_add(&s->delivered, (uint64_t)out);
        if (atomic_load(&s->stop)) break;
    }
    return NULL;
}

static void *stress_trigger(void *argument) {
    Stress *s = argument;
    while (!atomic_load_explicit(&s->stop, memory_order_relaxed)) {
        EXPECT(sys_equeue_trigger(s->queue, 0x40, SYS_EQUEUE_GRAPHICS_CORE, 0x40) == 0);
        atomic_fetch_add_explicit(&s->triggered, 1, memory_order_relaxed);
        host_yield();
    }
    return NULL;
}

static void stress(unsigned milliseconds, int print) {
    Stress s;
    memset(&s, 0, sizeof(s));
    EXPECT(create(&s.queue, "stress") == 0);
    EXPECT(sys_equeue_add(s.queue, 0x40, SYS_EQUEUE_GRAPHICS_CORE, NULL) == 0);
    HostThread waiters[3], triggers[3];
    for (int i = 0; i < 3; ++i) host_thread_start(&waiters[i], 0, stress_waiter, &s);
    for (int i = 0; i < 3; ++i) host_thread_start(&triggers[i], 0, stress_trigger, &s);
    host_sleep_ns((uint64_t)milliseconds * 1000000);
    atomic_store(&s.stop, 1);
    for (int i = 0; i < 3; ++i) host_thread_join(triggers[i]);
    for (int i = 0; i < 3; ++i) host_thread_join(waiters[i]);
    EXPECT(atomic_load(&s.delivered) > 0 && atomic_load(&s.delivered) <= atomic_load(&s.triggered));
    if (print) printf("stress: %" PRIu64 " triggers, %" PRIu64 " delivered in %u ms\n", (uint64_t)atomic_load(&s.triggered), (uint64_t)atomic_load(&s.delivered), milliseconds);
    EXPECT(destroy(s.queue) == 0);
}

typedef struct {
    int64_t queue;
    _Atomic uint64_t stamp;
    atomic_int done;
    uint64_t total, worst;
    unsigned count;
} Latency;

static void *latency_waiter(void *argument) {
    Latency *l = argument;
    while (!atomic_load(&l->done)) {
        Event event;
        int out = 0;
        const uint32_t timeout = 100000;
        if (wait_for(l->queue, &event, 1, &out, &timeout)) continue;
        uint64_t now = host_monotonic_ns(), sent = atomic_load(&l->stamp);
        uint64_t took = now > sent ? now - sent : 0;
        l->total += took;
        if (took > l->worst) l->worst = took;
        ++l->count;
    }
    return NULL;
}

static void bench(void) {
    int64_t q;
    EXPECT(create(&q, "bench") == 0);
    for (int i = 0; i < 8; ++i) EXPECT(sys_equeue_add(q, (uint64_t)i, SYS_EQUEUE_GRAPHICS_CORE, NULL) == 0);
    enum { CALLS = 2000000 };
    const uint32_t zero = 0;
    Event events[4];
    int out;
    uint64_t t0 = host_monotonic_ns();
    for (int i = 0; i < CALLS; ++i) wait_for(q, events, 4, &out, &zero);
    uint64_t t1 = host_monotonic_ns();
    for (int i = 0; i < CALLS; ++i) {
        sys_equeue_trigger(q, 7, SYS_EQUEUE_GRAPHICS_CORE, 7);
        wait_for(q, events, 4, &out, &zero);
    }
    uint64_t t2 = host_monotonic_ns();
    printf("empty poll %.1f ns, trigger + poll %.1f ns\n", (double)(t1 - t0) / CALLS, (double)(t2 - t1) / CALLS);
    Latency l;
    memset(&l, 0, sizeof(l));
    l.queue = q;
    HostThread waiter;
    host_thread_start(&waiter, 0, latency_waiter, &l);
    for (int i = 0; i < 3000; ++i) {
        host_sleep_ns(200000);
        atomic_store(&l.stamp, host_monotonic_ns());
        sys_equeue_trigger(q, 7, SYS_EQUEUE_GRAPHICS_CORE, 7);
    }
    atomic_store(&l.done, 1);
    host_thread_join(waiter);
    printf("wake latency: %.1f us average, %.1f us worst, %u wakes\n", (double)l.total / (l.count ? l.count : 1) / 1000.0, (double)l.worst / 1000.0, l.count);
    stress(1000, 1);
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IOLBF, 4096);
    bind();
    if (argc > 1 && !strcmp(argv[1], "--bench")) {
        bench();
        return failures ? 1 : 0;
    }
    test_equeue();
    test_ports();
    stress(300, 0);
    if (failures) return 1;
    puts("sys equeue and video out: PASS");
    return 0;
}
