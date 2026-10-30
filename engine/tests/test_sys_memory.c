// SPDX-License-Identifier: GPL-3.0-or-later
#include "runtime.h"
#include "sys/sys.h"
#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GUEST __attribute__((sysv_abi))
#define PAGE UINT64_C(16384)
#define WINDOW UINT64_C(0x2000000000)
#define E_INVALID 0x80020016u
#define E_NO_MEMORY 0x8002000cu
#define E_ACCESS 0x8002000du
#define E_NO_SPACE 0x80020023u
#define CODE(x) ((uint32_t)(x))

typedef void (*HookFn)(uintptr_t, uint64_t);
void runtime_memory_set_gpu_hooks(HookFn map, HookFn unmap, HookFn invalidate);
void runtime_memory_set_note_write_hook(HookFn note);
void runtime_memory_set_cpu_write_hook(HookFn hook);
void runtime_memory_set_write_watch(uintptr_t address, uint64_t size, int watch);
int runtime_memory_write_backing(uintptr_t address, const void *data, uint64_t size);
void runtime_memory_read_backing(uintptr_t address, void *data, uint64_t size);
uint64_t runtime_memory_clamp(uintptr_t address, uint64_t size);
int runtime_memory_region(uintptr_t address, uintptr_t *start, uintptr_t *end, int *mapped);
int runtime_memory_direct_phys(uintptr_t address, uint64_t *phys, uintptr_t *end);
int runtime_memory_vma_info(uintptr_t address, int *prot, int *type, uintptr_t *end);
void runtime_memory_gpu_protect(uintptr_t address, uint64_t size, int read, int write);

typedef struct {
    uint64_t start, end, offset;
    int32_t protection, memory_type;
    uint32_t flags;
    char name[32];
} Query;

typedef struct {
    void *start;
    uint64_t offset, length;
    int8_t prot, type;
    int16_t reserved;
    int32_t operation;
} Batch;

typedef uint64_t (GUEST *SizeFn)(void);
typedef int32_t (GUEST *AllocFn)(int64_t, int64_t, uint64_t, uint64_t, int, int64_t *);
typedef int32_t (GUEST *MapFn)(void **, uint64_t, int, int, int64_t, uint64_t);
typedef int32_t (GUEST *UnmapFn)(void *, uint64_t);
typedef int32_t (GUEST *ReleaseFn)(uint64_t, uint64_t);
typedef int32_t (GUEST *FlexFn)(void **, uint64_t, int, int);
typedef int32_t (GUEST *FlexNamedFn)(void **, uint64_t, int, int, const char *);
typedef int32_t (GUEST *ReserveFn)(void **, uint64_t, int, uint64_t);
typedef int32_t (GUEST *ProtectFn)(const void *, uint64_t, int);
typedef int32_t (GUEST *QueryProtFn)(void *, void **, void **, uint32_t *);
typedef int32_t (GUEST *QueryFn)(const void *, int, Query *, uint64_t);
typedef int32_t (GUEST *TypeFn)(uint64_t, int *, void **, void **);
typedef int32_t (GUEST *BatchFn)(Batch *, int, int *);
typedef void *(GUEST *MmapFn)(void *, uint64_t, int, int, int, int64_t);

static SizeFn pool_size;
static AllocFn alloc;
static MapFn map;
static UnmapFn unmap, flex_release, munmap_;
static ReleaseFn release;
static FlexFn flex;
static FlexNamedFn flex_named;
static ReserveFn reserve;
static ProtectFn protect;
static QueryProtFn query_prot;
static QueryFn query;
static TypeFn memtype;
static BatchFn batch;
static MmapFn mmap_;

#ifdef MEMORY_OLD
uintptr_t runtime_memory_resolve(const char *name);
static void *fn(const char *nid) {
    char full[32];
    snprintf(full, sizeof(full), "%s#p#J", nid);
    return (void *)runtime_memory_resolve(full);
}
#else
static void *fn(const char *nid) {
    const SysExport *found = sys_find(nid, NULL);
    return found ? found->function : NULL;
}
#endif

static int failures;
#define EXPECT(condition) do { if (!(condition)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); ++failures; } } while (0)

static void bind(void) {
    pool_size = (SizeFn)fn("pO96TwzOm5E");
    alloc = (AllocFn)fn("rTXw65xmLIA");
    map = (MapFn)fn("L-Q3LEjIbgA");
    release = (ReleaseFn)fn("MBuItvba6z8");
    unmap = (UnmapFn)fn("cQke9UuBQOk");
    munmap_ = (UnmapFn)fn("UqDGjXA5yUM");
    flex = (FlexFn)fn("IWIBBdTHit4");
    flex_named = (FlexNamedFn)fn("mL8NDH86iQI");
    flex_release = (UnmapFn)fn("teiItL2boFw");
    reserve = (ReserveFn)fn("7oxv3PPCumo");
    protect = (ProtectFn)fn("vSMAm3cxYTY");
    query_prot = (QueryProtFn)fn("WFcfL2lzido");
    query = (QueryFn)fn("rVjRvHJ0X6c");
    memtype = (TypeFn)fn("BC+OG5m9+bw");
    batch = (BatchFn)fn("2SKEx6bSq-4");
    mmap_ = (MmapFn)fn("BPE9s9vQQXo");
    if (!pool_size || !alloc || !map || !release || !unmap || !munmap_ || !flex || !flex_named || !flex_release ||
        !reserve || !protect || !query_prot || !query || !memtype || !batch || !mmap_) {
        puts("FAIL: a memory export is missing");
        exit(1);
    }
}

static FILE *out;
static uint64_t hash_value = UINT64_C(14695981039346656037);
static uint64_t rng_state;

static void hash_bytes(const void *data, size_t size) {
    const unsigned char *p = data;
    for (size_t i = 0; i < size; ++i) hash_value = (hash_value ^ p[i]) * UINT64_C(1099511628211);
}

static uint32_t rng(void) {
    rng_state = rng_state * UINT64_C(6364136223846793005) + UINT64_C(1442695040888963407);
    return (uint32_t)(rng_state >> 33);
}

static uint32_t pick(uint32_t n) {
    return rng() % n;
}

#define LOG(...) do { if (out) fprintf(out, __VA_ARGS__); } while (0)

static void hook_map(uintptr_t a, uint64_t s) { LOG("  hook map %#" PRIx64 " %#" PRIx64 "\n", (uint64_t)a, s); }
static void hook_unmap(uintptr_t a, uint64_t s) { LOG("  hook unmap %#" PRIx64 " %#" PRIx64 "\n", (uint64_t)a, s); }
static void hook_invalidate(uintptr_t a, uint64_t s) { LOG("  hook invalidate %#" PRIx64 " %#" PRIx64 "\n", (uint64_t)a, s); }
static void hook_note(uintptr_t a, uint64_t s) { LOG("  hook note %#" PRIx64 " %#" PRIx64 "\n", (uint64_t)a, s); }
static int cpu_writes;
static void hook_cpu(uintptr_t a, uint64_t s) { ++cpu_writes; LOG("  hook cpu %#" PRIx64 " %#" PRIx64 "\n", (uint64_t)a, s); }

static uint64_t some_size(void) {
    static const uint64_t pages[] = {1, 1, 1, 2, 2, 3, 4, 8, 16, 32, 64, 128};
    return pages[pick(12)] * PAGE;
}

static uint64_t some_alignment(void) {
    static const uint64_t values[] = {0, 0, 0, PAGE, 2 * PAGE, 4 * PAGE, UINT64_C(2) << 20, 4096, 3 * PAGE, UINT64_C(1) << 40};
    return values[pick(10)];
}

static int some_prot(void) {
    static const int values[] = {0, 1, 2, 3, 3, 3, 0x11, 0x22, 0x33, 0x10, 0x20, 5, 7, 0x40, 0x13};
    return values[pick(15)];
}

static int some_flags(void) {
    static const int values[] = {0, 0, 0x10, 0x10, 0x10, 0x90, 0x90, 0x80};
    return values[pick(8)];
}

static uint64_t some_address(void) {
    if (!pick(40)) return 0;
    if (!pick(40)) return WINDOW + pick(4096);
    return WINDOW + pick(1024) * PAGE;
}

static void walk(void) {
    uint64_t at = 0;
    unsigned count = 0;
    for (;;) {
        Query q;
        memset(&q, 0x55, sizeof(q));
        int32_t r = query((void *)(uintptr_t)at, 1, &q, sizeof(q));
        if (r) {
            LOG("  end %#x after %u\n", CODE(r), count);
            break;
        }
        LOG("  range %#" PRIx64 " %#" PRIx64 " off %#" PRIx64 " prot %#x type %d flags %#x %s\n", q.start, q.end, q.offset,
            q.protection, q.memory_type, q.flags, q.name);
        static unsigned char buffer[1 << 16];
        uint64_t n = q.end - q.start < sizeof(buffer) ? q.end - q.start : sizeof(buffer);
        runtime_memory_read_backing((uintptr_t)q.start, buffer, n);
        hash_bytes(buffer, n);
        at = q.end;
        ++count;
    }
    LOG("  regions %" PRIu64 " hash %#" PRIx64 "\n", runtime_memory_regions(), hash_value);
}

static void probe(uint64_t address) {
    uint64_t size = some_size() + pick(4096);
    uintptr_t start = 0, end = 0;
    int mapped = -1, prot = -1, type = -1;
    uint64_t phys = 0;
    int have = runtime_memory_region((uintptr_t)address, &start, &end, &mapped);
    LOG("  region %d %#" PRIx64 " %#" PRIx64 " %d\n", have, (uint64_t)start, (uint64_t)end, mapped);
    LOG("  clamp %#" PRIx64 " mapped %d writable %d\n", runtime_memory_clamp((uintptr_t)address, size),
        runtime_memory_is_mapped((uintptr_t)address, size), runtime_memory_guest_writable((uintptr_t)address, size));
    start = end = 0;
    have = runtime_memory_direct_phys((uintptr_t)address, &phys, &end);
    LOG("  phys %d %#" PRIx64 " %#" PRIx64 "\n", have, phys, (uint64_t)end);
    end = 0;
    have = runtime_memory_vma_info((uintptr_t)address, &prot, &type, &end);
    LOG("  vma %d %d %d %#" PRIx64 "\n", have, prot, type, (uint64_t)end);
    void *a = NULL, *b = NULL;
    uint32_t p = 99;
    int32_t r = query_prot((void *)(uintptr_t)address, &a, &b, &p);
    LOG("  prot %#x %p %p %u\n", CODE(r), a, b, p);
    Query q;
    memset(&q, 0, sizeof(q));
    r = query((void *)(uintptr_t)address, 0, &q, sizeof(q));
    LOG("  query %#x %#" PRIx64 " %#" PRIx64 " %s\n", CODE(r), q.start, q.end, q.name);
    runtime_memory_note_write((uintptr_t)address, size);
}


static void transcript(unsigned steps) {
    int64_t phys[32];
    uint64_t phys_size[32];
    memset(phys, -1, sizeof(phys));
    memset(phys_size, 0, sizeof(phys_size));
    runtime_memory_set_gpu_hooks(hook_map, hook_unmap, hook_invalidate);
    runtime_memory_set_note_write_hook(hook_note);
    runtime_memory_set_cpu_write_hook(hook_cpu);
    LOG("pool %#" PRIx64 "\n", pool_size());
    for (unsigned step = 0; step < steps; ++step) {
        uint32_t op = pick(16);
        LOG("%u op %u\n", step, op);
        if (op <= 1) {
            uint64_t size = pick(30) ? some_size() : pick(3) ? 4096 : 0;
            int64_t low = pick(5) ? 0 : (int64_t)(pick(64) * PAGE), high = pick(5) ? (int64_t)pool_size() : low + (int64_t)some_size();
            int type = pick(30) ? (int)pick(11) : 11;
            int64_t got = -2;
            uint64_t alignment = some_alignment();
            int32_t r = alloc(low, high, size, alignment, type, &got);
            LOG(" alloc %#" PRIx64 " %#" PRIx64 " %#" PRIx64 " %d = %#x %#" PRIx64 "\n", (uint64_t)low, size, alignment, type, CODE(r), (uint64_t)got);
            if (!r) {
                unsigned slot = pick(32);
                phys[slot] = got;
                phys_size[slot] = size;
            }
        } else if (op <= 4) {
            unsigned slot = pick(32);
            void *address = (void *)(uintptr_t)some_address();
            uint64_t size = phys_size[slot] ? phys_size[slot] : some_size();
            if (!pick(4) && size > PAGE) size -= PAGE;
            int64_t offset = phys[slot] >= 0 ? phys[slot] : (int64_t)(pick(64) * PAGE);
            int prot = some_prot(), flags = some_flags();
            uint64_t alignment = some_alignment();
            int32_t r = map(&address, size, prot, flags, offset, alignment);
            LOG(" map %#" PRIx64 " %#x %#x %#" PRIx64 " %#" PRIx64 " = %#x %p\n", (uint64_t)offset, prot, flags, size, alignment, CODE(r), address);
        } else if (op <= 6) {
            void *address = (void *)(uintptr_t)some_address();
            uint64_t size = pick(8) ? some_size() : pick(2) ? 0 : 4096;
            int32_t r = (pick(2) ? unmap : munmap_)(address, size);
            LOG(" unmap %p %#" PRIx64 " = %#x\n", address, size, CODE(r));
        } else if (op == 7) {
            unsigned slot = pick(32);
            uint64_t start = phys[slot] >= 0 ? (uint64_t)phys[slot] : pick(64) * PAGE, size = phys_size[slot] ? phys_size[slot] : some_size();
            if (!pick(4) && size > PAGE) {
                start += PAGE;
                size -= PAGE;
            }
            int32_t r = release(start, pick(20) ? size : 4096);
            LOG(" release %#" PRIx64 " %#" PRIx64 " = %#x\n", start, size, CODE(r));
            if (!r) {
                phys[slot] = -1;
                phys_size[slot] = 0;
            }
        } else if (op <= 9) {
            void *address = (void *)(uintptr_t)some_address();
            uint64_t size = pick(30) ? some_size() : 4096;
            int prot = some_prot(), flags = some_flags();
            int32_t r = pick(2) ? flex(&address, size, prot, flags) : flex_named(&address, size, prot, flags, "test");
            LOG(" flex %#" PRIx64 " %#x %#x = %#x %p\n", size, prot, flags, CODE(r), address);
        } else if (op == 10) {
            void *address = (void *)(uintptr_t)some_address();
            uint64_t size = some_size(), alignment = some_alignment();
            int flags = some_flags();
            int32_t r = reserve(&address, size, flags, alignment);
            LOG(" reserve %#" PRIx64 " %#x %#" PRIx64 " = %#x %p\n", size, flags, alignment, CODE(r), address);
        } else if (op == 11) {
            uint64_t address = some_address();
            uint64_t size = some_size() + pick(2) * 512;
            int prot = some_prot();
            int32_t r = protect((void *)(uintptr_t)address, size, prot);
            LOG(" protect %#" PRIx64 " %#" PRIx64 " %#x = %#x\n", address, size, prot, CODE(r));
        } else if (op == 12) {
            Batch items[4];
            int count = 1 + (int)pick(4);
            for (int i = 0; i < count; ++i) {
                unsigned slot = pick(32);
                memset(&items[i], 0, sizeof(items[i]));
                items[i].start = (void *)(uintptr_t)(WINDOW + pick(1024) * PAGE);
                items[i].operation = (int32_t)pick(6);
                items[i].offset = phys[slot] >= 0 ? (uint64_t)phys[slot] : 0;
                items[i].length = phys_size[slot] ? phys_size[slot] : some_size();
                items[i].prot = (int8_t)some_prot();
                items[i].type = (int8_t)pick(11);
            }
            int processed = -1;
            int32_t r = batch(items, count, &processed);
            LOG(" batch %d = %#x done %d", count, CODE(r), processed);
            for (int i = 0; i < count; ++i) LOG(" [%d %p]", items[i].operation, items[i].start);
            LOG("\n");
        } else if (op == 13) {
            uint64_t address = some_address();
            uint64_t size = some_size();
            unsigned char data[256];
            for (unsigned i = 0; i < sizeof(data); ++i) data[i] = (unsigned char)(step + i);
            int w = runtime_memory_write_backing((uintptr_t)address, data, sizeof(data));
            unsigned char back[300];
            memset(back, 0xee, sizeof(back));
            runtime_memory_read_backing((uintptr_t)address - 8, back, sizeof(back));
            hash_bytes(back, sizeof(back));
            LOG(" backing %#" PRIx64 " %d %#" PRIx64 "\n", address, w, hash_value);
            int prot = -1, type = -1;
            uintptr_t end = 0;
            if (runtime_memory_vma_info((uintptr_t)address, &prot, &type, &end) && (prot & 2) && end - address >= 8) {
                *(volatile uint64_t *)(uintptr_t)address = step;
                LOG(" poke %d\n", (int)runtime_memory_is_mapped((uintptr_t)address, 8));
            }
            runtime_memory_set_write_watch((uintptr_t)address, size, (int)pick(2));
            runtime_memory_note_cpu_write((uintptr_t)address + pick(4096), 1 + pick(4096));
        } else if (op == 14) {
            int type = -1;
            void *a = NULL, *b = NULL;
            uint64_t at = pick(128) * PAGE;
            int32_t r = memtype(at, &type, &a, &b);
            LOG(" type %#" PRIx64 " = %#x %d %p %p\n", at, CODE(r), type, a, b);
            probe(some_address());
        } else {
            void *address = pick(2) ? (void *)(uintptr_t)some_address() : NULL;
            uint64_t size = some_size() - pick(2) * 100;
            int flags = 0x1000 | (pick(3) ? 0 : 0x10);
            int fd = pick(10) ? -1 : 3;
            int64_t offset = pick(10) ? 0 : 16384;
            void *p = mmap_(address, size, 3, flags, fd, offset);
            LOG(" mmap %#" PRIx64 " %#x = %p errno %d\n", size, flags, p, p == (void *)-1 ? *runtime_errno() : 0);
        }
        if (step % 50 == 49) walk();
    }
    walk();
}

static void bad_arguments(void) {
    Query q;
    void *address = NULL, *a = NULL, *b = NULL;
    int64_t got = -1;
    int type;
    uint32_t prot;
    EXPECT(CODE(alloc(0, (int64_t)pool_size(), PAGE, 0, 0, NULL)) == E_INVALID);
    EXPECT(CODE(alloc(-1, (int64_t)pool_size(), PAGE, 0, 0, &got)) == E_INVALID);
    EXPECT(CODE(alloc(0, -1, PAGE, 0, 0, &got)) == E_INVALID);
    EXPECT(CODE(alloc(0, (int64_t)pool_size(), 0, 0, 0, &got)) == E_INVALID);
    EXPECT(CODE(alloc(0, (int64_t)pool_size(), 4096, 0, 0, &got)) == E_INVALID);
    EXPECT(CODE(alloc(0, (int64_t)pool_size(), PAGE, 4096, 0, &got)) == E_INVALID);
    EXPECT(CODE(alloc(0, (int64_t)pool_size(), PAGE, 3 * PAGE, 0, &got)) == E_INVALID);
    EXPECT(CODE(alloc(0, (int64_t)pool_size(), PAGE, 0, -1, &got)) == E_INVALID);
    EXPECT(CODE(alloc(0, (int64_t)pool_size(), PAGE, 0, 11, &got)) == E_INVALID);
    EXPECT(CODE(alloc(PAGE, PAGE, PAGE, 0, 0, &got)) == E_NO_SPACE);
    EXPECT(CODE(alloc(0, 8192, PAGE, 0, 0, &got)) == E_NO_SPACE);
    EXPECT(got == -1);
    EXPECT(CODE(map(NULL, PAGE, 3, 0, 0, 0)) == E_INVALID);
    EXPECT(CODE(map(&address, PAGE, 3, 0, 0, 0)) == E_INVALID);
    EXPECT(CODE(map(&address, PAGE, 3, 0, -1, 0)) == E_INVALID);
    EXPECT(CODE(flex(NULL, PAGE, 3, 0)) == E_INVALID);
    EXPECT(CODE(flex(&address, 0, 3, 0)) == E_INVALID);
    EXPECT(CODE(flex(&address, 4096, 3, 0)) == E_INVALID);
    EXPECT(CODE(flex(&address, PAGE, 0x40, 0)) == E_INVALID);
    EXPECT(CODE(flex(&address, (UINT64_C(448) << 20) + PAGE, 3, 0)) == E_NO_MEMORY);
    EXPECT(CODE(reserve(NULL, PAGE, 0, 0)) == E_INVALID);
    EXPECT(CODE(reserve(&address, 0, 0, 0)) == E_INVALID);
    EXPECT(CODE(reserve(&address, PAGE, 0, 4096)) == E_INVALID);
    EXPECT(CODE(query(NULL, 0, &q, 4)) == E_INVALID);
    EXPECT(CODE(query(NULL, 0, NULL, sizeof(q))) == E_INVALID);
    EXPECT(CODE(query((void *)(uintptr_t)UINT64_C(0xff00000000), 1, &q, sizeof(q))) == E_ACCESS);
    EXPECT(CODE(query_prot((void *)(uintptr_t)UINT64_C(0xff00000000), &a, &b, &prot)) == E_ACCESS);
    EXPECT(CODE(memtype(pool_size() - PAGE, &type, &a, &b)) == E_ACCESS);
    EXPECT(CODE(release(4096, PAGE)) == E_INVALID);
    EXPECT(CODE(release(0, 4096)) == E_INVALID);
    EXPECT(release(0, 0) == 0);
    EXPECT(CODE(release(pool_size() - PAGE, PAGE)) == E_INVALID);
    EXPECT(CODE(unmap((void *)(uintptr_t)4096, PAGE)) == E_INVALID);
    EXPECT(CODE(unmap((void *)(uintptr_t)WINDOW, 0)) == E_INVALID);
    EXPECT(unmap((void *)(uintptr_t)UINT64_C(0xf000000000), PAGE) == 0);
    EXPECT(CODE(protect((void *)(uintptr_t)UINT64_C(0xf000000000), PAGE, 3)) == E_INVALID);
    EXPECT(CODE(batch(NULL, 1, NULL)) == E_INVALID);
    EXPECT(CODE(batch((Batch *)&q, -1, NULL)) == E_INVALID);
    EXPECT(mmap_(NULL, PAGE, 3, 0, -1, 0) == (void *)-1 && *runtime_errno() == 19);
    EXPECT(mmap_(NULL, 0, 3, 0x1000, -1, 0) == (void *)-1 && *runtime_errno() == 22);
    EXPECT(mmap_(NULL, UINT64_C(1) << 40, 3, 0x1000, -1, 0) == (void *)-1 && *runtime_errno() == 12);
}

static void facts(void) {
    int64_t a = -1, b = -1, c = -1;
    uint64_t pool = pool_size();
    EXPECT(alloc(0, (int64_t)pool, PAGE, UINT64_C(2) << 20, 2, &a) == 0 && a == 0);
    EXPECT(alloc(0, (int64_t)pool, PAGE, UINT64_C(2) << 20, 3, &b) == 0 && b == (UINT64_C(2) << 20));
    EXPECT(alloc(0, (int64_t)pool, PAGE, 0, 4, &c) == 0 && c == (int64_t)PAGE);
    int type = -1;
    void *first = NULL, *last = NULL;
    EXPECT(memtype((uint64_t)b + 100, &type, &first, &last) == 0 && type == 3 && first == (void *)(uintptr_t)b && last == (void *)(uintptr_t)(b + (int64_t)PAGE));
    void *x = NULL, *y = NULL;
    EXPECT(map(&x, PAGE, 3, 0, a, UINT64_C(2) << 20) == 0 && !((uintptr_t)x % (UINT64_C(2) << 20)));
    EXPECT(map(&y, PAGE, 3, 0, a, 0) == 0);
    *(volatile uint64_t *)x = UINT64_C(0xabcdef0123456789);
    EXPECT(*(volatile uint64_t *)y == UINT64_C(0xabcdef0123456789));
    uint64_t phys = 99;
    uintptr_t end = 0;
    EXPECT(runtime_memory_direct_phys((uintptr_t)x + 8, &phys, &end) && phys == (uint64_t)a + 8 && end == (uintptr_t)x + PAGE);
    uint64_t seen = 0;
    runtime_memory_read_backing((uintptr_t)x, &seen, 8);
    EXPECT(seen == UINT64_C(0xabcdef0123456789));
    seen = 7;
    EXPECT(runtime_memory_write_backing((uintptr_t)y, &seen, 8) && *(volatile uint64_t *)x == 7);
    EXPECT(protect(x, PAGE, 1) == 0);
    uint32_t prot = 0;
    EXPECT(query_prot(x, &first, &last, &prot) == 0 && prot == 1 && first == x && last == (char *)x + PAGE);
    EXPECT(runtime_memory_guest_writable((uintptr_t)x, 8) == 0 && runtime_memory_guest_writable((uintptr_t)y, 8) == 1);
    EXPECT(unmap(x, PAGE) == 0 && unmap(y, PAGE) == 0);
    EXPECT(!runtime_memory_is_mapped((uintptr_t)x, 8));
    EXPECT(release((uint64_t)a, PAGE) == 0 && release((uint64_t)b, PAGE) == 0 && release((uint64_t)c, PAGE) == 0);
    EXPECT(alloc(0, (int64_t)pool, PAGE, 0, 0, &a) == 0 && a == 0);
    x = NULL;
    EXPECT(map(&x, PAGE, 3, 0, a, 0) == 0 && *(volatile uint64_t *)x == 0);
    EXPECT(unmap(x, PAGE) == 0 && release((uint64_t)a, PAGE) == 0);

    EXPECT(alloc(0, (int64_t)pool, 3 * PAGE, 0, 0, &a) == 0);
    x = NULL;
    EXPECT(map(&x, 3 * PAGE, 3, 0, a, 0) == 0);
    EXPECT(protect((char *)x + 0x100, PAGE, 1) == 0);
    EXPECT(query_prot(x, &first, &last, &prot) == 0 && prot == 1 && first == x && last == (char *)x + 2 * PAGE);
    EXPECT(query_prot((char *)x + 2 * PAGE, &first, &last, &prot) == 0 && prot == 3 && first == (char *)x + 2 * PAGE && last == (char *)x + 3 * PAGE);
    EXPECT(protect((char *)x + 0x100, PAGE - 0x100, 3) == 0);
    EXPECT(query_prot(x, &first, &last, &prot) == 0 && prot == 3 && first == x && last == (char *)x + PAGE);
    EXPECT(query_prot((char *)x + PAGE, &first, &last, &prot) == 0 && prot == 1 && first == (char *)x + PAGE && last == (char *)x + 2 * PAGE);
    EXPECT(release((uint64_t)a, 3 * PAGE) == 0);
    EXPECT(!runtime_memory_is_mapped((uintptr_t)x, 8));

    const uintptr_t watched = UINT64_C(0x100000000);
    runtime_memory_set_cpu_write_hook(hook_cpu);
    runtime_memory_set_write_watch(watched + 4096 * 8, 4096 * 2, 1);
    cpu_writes = 0;
    runtime_memory_note_cpu_write(watched, 4096);
    runtime_memory_note_cpu_write(watched + 4096 * 7, 4096);
    runtime_memory_note_cpu_write(watched + 4096 * 10, 4096);
    EXPECT(cpu_writes == 0);
    runtime_memory_note_cpu_write(watched + 4096 * 9, 1);
    EXPECT(cpu_writes == 1);
    runtime_memory_set_write_watch(watched + 4096 * 8, 4096 * 2, 0);
    runtime_memory_note_cpu_write(watched + 4096 * 9, 1);
    EXPECT(cpu_writes == 1);

    void *f1 = NULL, *f2 = NULL;
    EXPECT(flex(&f1, 2 * PAGE, 3, 0) == 0 && flex(&f2, PAGE, 3, 0) == 0);
    *(volatile uint64_t *)f1 = 11;
    *(volatile uint64_t *)((char *)f1 + PAGE) = 12;
    Query q;
    EXPECT(query(f1, 0, &q, sizeof(q)) == 0 && q.flags == 17 && !strcmp(q.name, "flexible"));
    EXPECT(flex_release((char *)f1 + PAGE, PAGE) == 0);
    EXPECT(*(volatile uint64_t *)f1 == 11);
    EXPECT(query(f1, 0, &q, sizeof(q)) == 0 && q.end == (uint64_t)(uintptr_t)f1 + PAGE);
    void *f3 = NULL;
    EXPECT(flex(&f3, PAGE, 3, 0) == 0 && *(volatile uint64_t *)f3 == 0);
    EXPECT(flex_release(f1, PAGE) == 0 && flex_release(f2, PAGE) == 0 && flex_release(f3, PAGE) == 0);

    void *r = (void *)(uintptr_t)(WINDOW + 512 * PAGE);
    EXPECT(reserve(&r, 4 * PAGE, 0x10, 0) == 0);
    EXPECT(query(r, 0, &q, sizeof(q)) == 0 && !strcmp(q.name, "reserved") && q.flags == 0);
    EXPECT(CODE(query_prot(r, NULL, NULL, NULL)) == E_ACCESS);
    EXPECT(!runtime_memory_is_mapped((uintptr_t)r, 8) && runtime_memory_clamp((uintptr_t)r, 8) == 0);
    void *over = r;
    EXPECT(CODE(reserve(&over, PAGE, 0x10, 0)) == E_NO_MEMORY);
    over = r;
    EXPECT(flex(&over, PAGE, 3, 0x90) == 0 && over == r);
    over = (char *)r + PAGE;
    EXPECT(flex(&over, PAGE, 3, 0x90) == 0);
    EXPECT(query_prot(r, &first, &last, &prot) == 0 && first == r && last == (char *)r + 2 * PAGE);
    over = r;
    EXPECT(CODE(flex(&over, PAGE, 3, 0x90)) == E_NO_MEMORY);
    EXPECT(unmap(r, 4 * PAGE) == 0);

    void *big[8];
    for (unsigned round = 0; round < 2; ++round) {
        for (unsigned i = 0; i < 7; ++i) {
            big[i] = NULL;
            EXPECT(flex(&big[i], UINT64_C(64) << 20, 3, 0) == 0);
        }
        big[7] = NULL;
        EXPECT(CODE(flex(&big[7], PAGE, 3, 0)) == E_NO_MEMORY);
        for (unsigned i = 0; i < 7; ++i) EXPECT(flex_release(big[i], UINT64_C(64) << 20) == 0);
    }
}

typedef struct {
    atomic_int stop;
    atomic_uint_fast64_t reads, wrong, cycles;
    uint64_t steady;
    uint64_t moving;
} Stress;

static void *stress_reader(void *argument) {
    Stress *s = argument;
    uint64_t reads = 0, wrong = 0;
    while (!atomic_load_explicit(&s->stop, memory_order_relaxed)) {
        for (int i = 0; i < 256; ++i) {
            if (runtime_memory_clamp((uintptr_t)s->steady + 64, 4096) != 4096) ++wrong;
            if (!runtime_memory_is_mapped((uintptr_t)s->steady, PAGE)) ++wrong;
            uint64_t moved = runtime_memory_clamp((uintptr_t)s->moving, PAGE);
            if (moved && moved != PAGE) ++wrong;
            uintptr_t start = 0, end = 0;
            int mapped = 0;
            if (!runtime_memory_region((uintptr_t)s->steady, &start, &end, &mapped) || !mapped || start != (uintptr_t)s->steady) ++wrong;
            reads += 4;
        }
    }
    atomic_fetch_add(&s->reads, reads);
    atomic_fetch_add(&s->wrong, wrong);
    return NULL;
}

static void *stress_writer(void *argument) {
    Stress *s = argument;
    uint64_t base = s->moving;
    unsigned step = 0;
    while (!atomic_load_explicit(&s->stop, memory_order_relaxed)) {
        void *at = (void *)(uintptr_t)base;
        EXPECT(flex(&at, PAGE, 3, 0x10) == 0);
        *(volatile uint64_t *)at = ++step;
        Query q;
        EXPECT(query(at, 0, &q, sizeof(q)) == 0 && q.start == base);
        void *hole = (void *)(uintptr_t)(base + PAGE * (2 + step % 512));
        void *got = hole;
        EXPECT(reserve(&got, PAGE, 0x10, 0) == 0);
        EXPECT(unmap(hole, PAGE) == 0);
        EXPECT(unmap(at, PAGE) == 0);
        atomic_fetch_add_explicit(&s->cycles, 1, memory_order_relaxed);
    }
    return NULL;
}

static void stress(unsigned milliseconds, unsigned readers, int print) {
    Stress s;
    memset(&s, 0, sizeof(s));
    void *steady = NULL;
    EXPECT(flex(&steady, PAGE, 3, 0) == 0);
    s.steady = (uint64_t)(uintptr_t)steady;
    s.moving = WINDOW + 8192 * PAGE;
    HostThread threads[32];
    for (unsigned i = 0; i < readers; ++i) host_thread_start(&threads[i], 0, stress_reader, &s);
    HostThread writer;
    host_thread_start(&writer, 0, stress_writer, &s);
    host_sleep_ns((uint64_t)milliseconds * 1000000);
    atomic_store(&s.stop, 1);
    host_thread_join(writer);
    for (unsigned i = 0; i < readers; ++i) host_thread_join(threads[i]);
    EXPECT(atomic_load(&s.wrong) == 0);
    EXPECT(atomic_load(&s.reads) > 0);
    if (print) printf("stress: %u readers, %" PRIu64 " lookups in %u ms, %" PRIu64 " wrong\n", readers, (uint64_t)atomic_load(&s.reads), milliseconds, (uint64_t)atomic_load(&s.wrong));
    EXPECT(unmap(steady, PAGE) == 0);
}

static double bench_ns(void) {
    return (double)host_monotonic_ns();
}

static void *bench_reader(void *argument) {
    Stress *s = argument;
    uint64_t reads = 0;
    while (!atomic_load_explicit(&s->stop, memory_order_relaxed)) {
        for (int i = 0; i < 1024; ++i) {
            uint64_t at = s->steady + PAGE * (uint64_t)(i % 2000) * 2;
            int prot, type;
            uintptr_t end;
            runtime_memory_vma_info((uintptr_t)at, &prot, &type, &end);
            ++reads;
        }
    }
    atomic_fetch_add(&s->reads, reads);
    return NULL;
}

static void bench(void) {
    enum { REGIONS = 2000 };
    void *addresses[REGIONS];
    int64_t physical[REGIONS];
    double t0 = bench_ns();
    for (unsigned i = 0; i < REGIONS; ++i) EXPECT(alloc(0, (int64_t)pool_size(), PAGE, 0, 3, &physical[i]) == 0);
    double t1 = bench_ns();
    for (unsigned i = 0; i < REGIONS; ++i) {
        addresses[i] = (void *)(uintptr_t)(WINDOW + (uint64_t)i * 2 * PAGE);
        EXPECT(map(&addresses[i], PAGE, 3, 0x10, physical[i], 0) == 0);
    }
    double t2 = bench_ns();
    enum { LOOKUPS = 2000000 };
    uint64_t sink = 0;
    for (unsigned i = 0; i < LOOKUPS; ++i) sink += runtime_memory_clamp((uintptr_t)addresses[(i * 7) % REGIONS], 64);
    double t3 = bench_ns();
    for (unsigned i = 0; i < LOOKUPS; ++i) {
        int prot, type;
        uintptr_t end;
        sink += (uint64_t)runtime_memory_vma_info((uintptr_t)addresses[(i * 7) % REGIONS], &prot, &type, &end);
    }
    double t4 = bench_ns();
    double t_flex = t4;
    void *flexes[256];
    for (int round = 0; round < 20; ++round) {
        for (unsigned i = 0; i < 256; ++i) {
            flexes[i] = NULL;
            EXPECT(flex(&flexes[i], PAGE, 3, 0) == 0);
        }
        for (unsigned i = 0; i < 256; ++i) EXPECT(flex_release(flexes[i], PAGE) == 0);
    }
    double t5 = bench_ns();
    Query q;
    for (unsigned i = 0; i < LOOKUPS / 4; ++i) sink += (uint64_t)query(addresses[(i * 7) % REGIONS], 0, &q, sizeof(q));
    double t6 = bench_ns();
    for (unsigned i = 0; i < REGIONS; i += 2) EXPECT(protect(addresses[i], PAGE, 1) == 0);
    double t7 = bench_ns();
    printf("%u regions: allocate %.0f ns, map %.0f ns, protect %.0f ns, flexible map+release %.0f ns\n", REGIONS, (t1 - t0) / REGIONS, (t2 - t1) / REGIONS,
           (t7 - t6) / (REGIONS / 2), (t5 - t_flex) / (20 * 256));
    printf("lookups, one thread: clamp %.1f ns, vma_info %.1f ns, virtual query %.1f ns (sink %" PRIu64 ")\n", (t3 - t2) / LOOKUPS, (t4 - t3) / LOOKUPS, (t6 - t5) / (LOOKUPS / 4), sink);
    for (unsigned readers = 1; readers <= 8; readers *= 2) {
        Stress s;
        memset(&s, 0, sizeof(s));
        s.steady = WINDOW;
        HostThread threads[8];
        for (unsigned i = 0; i < readers; ++i) host_thread_start(&threads[i], 0, bench_reader, &s);
        host_sleep_ns(400000000);
        atomic_store(&s.stop, 1);
        for (unsigned i = 0; i < readers; ++i) host_thread_join(threads[i]);
        printf("%u reader threads: %.0f million vma_info per second\n", readers, (double)atomic_load(&s.reads) / 0.4 / 1e6);
    }
    double t8 = bench_ns();
    for (unsigned i = 0; i < REGIONS; ++i) EXPECT(unmap(addresses[i], PAGE) == 0);
    double t9 = bench_ns();
    for (unsigned i = 0; i < REGIONS; ++i) EXPECT(release((uint64_t)physical[i], PAGE) == 0);
    double t10 = bench_ns();
    printf("unmap %.0f ns, release %.0f ns\n", (t9 - t8) / REGIONS, (t10 - t9) / REGIONS);
    stress(1000, 4, 1);
}

int main(int argc, char **argv) {
#ifdef _WIN32
    _putenv("BB_POOL_LAZY=1");
    SetErrorMode(SEM_NOGPFAULTERRORBOX);
#endif
    setvbuf(stdout, NULL, _IOLBF, 4096);
    bind();
    if (argc > 3 && !strcmp(argv[1], "--transcript")) {
        rng_state = strtoull(argv[3], NULL, 10);
        out = fopen(argv[2], "w");
        if (!out) return 2;
        bad_arguments();
        transcript(argc > 4 ? (unsigned)strtoul(argv[4], NULL, 10) : 6000);
        fclose(out);
        return failures ? 1 : 0;
    }
    if (argc > 1 && !strcmp(argv[1], "--bench")) {
        bench();
        return failures ? 1 : 0;
    }
    if (argc > 1 && !strcmp(argv[1], "--stress")) {
        stress(argc > 2 ? (unsigned)strtoul(argv[2], NULL, 10) : 5000, 4, 1);
        return failures ? 1 : 0;
    }
    bad_arguments();
    facts();
    rng_state = 12345;
    transcript(800);
    stress(200, 3, 0);
    if (failures) return 1;
    puts("sys memory: PASS");
    return 0;
}
