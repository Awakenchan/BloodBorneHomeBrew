// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "sys.h"
#include "runtime.h"
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include "win32_memory.h"
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>
#endif

#define KERNEL_ERROR(code) ((int32_t)(UINT32_C(0x80020000) | (code)))
#define E_INVALID KERNEL_ERROR(22)
#define E_NO_MEMORY KERNEL_ERROR(12)
#define E_ACCESS KERNEL_ERROR(13)
#define E_NO_SPACE KERNEL_ERROR(35)

#define PAGE_SIZE_GUEST UINT64_C(16384)
#define HOST_PAGE UINT64_C(4096)
#define LOW_START UINT64_C(0x0800000000)
#define USER_START UINT64_C(0x1000000000)
#define USER_END UINT64_C(0xfc00000000)
#define FLEX_LIMIT (UINT64_C(448) << 20)
#define FLEX_SPAN (UINT64_C(1) << 30)
#define FLEX_WORDS (FLEX_SPAN / PAGE_SIZE_GUEST / 64)
#define CHUNK_BYTES (UINT64_C(256) << 20)
#define WATCH_END (UINT64_C(1) << 40)
#define MAX_BACKINGS 256
#define CACHE_WAYS 8
#define SNAPSHOT_TRIES 4

enum { MAP_FIXED_BIT = 0x10, MAP_NO_OVERWRITE_BIT = 0x80, MAP_ANON_BIT = 0x1000 };
enum { RANGE_RESERVED = 1, RANGE_DIRECT, RANGE_FLEXIBLE };
enum { HOOK_MAP, HOOK_UNMAP, HOOK_INVALIDATE };

typedef struct {
    uint64_t start, end, phys;
    uint32_t kind, type;
    uint64_t prot;
} Range;

typedef struct {
    size_t capacity;
    Range items[];
} RangeTable;

typedef struct {
    uint64_t start, size;
    int32_t type;
} Block;

typedef struct {
    uint64_t start, end;
    uint64_t offset;
    int32_t protection, memory_type;
    uint32_t flags;
    char name[32];
} QueryResult;
_Static_assert(sizeof(QueryResult) == 72, "sceKernelVirtualQuery result");

typedef struct {
    void *start;
    uint64_t offset, length;
    int8_t prot, type;
    int16_t reserved;
    int32_t operation;
} BatchItem;
_Static_assert(sizeof(BatchItem) == 32, "sceKernelBatchMap entry");

typedef struct {
    int kind;
    uintptr_t address;
    uint64_t size;
} QueuedHook;

typedef struct {
    uint64_t generation, start, end;
    uint32_t kind;
} RegionHint;

typedef struct {
    const RangeTable *table;
    size_t count;
    uint64_t sequence;
    int mode;
    unsigned tries;
} Snapshot;

typedef void (*RangeHook)(uintptr_t address, uint64_t size);
typedef int (*BackingMaker)(uint64_t phys, uint64_t size);

uint64_t runtime_disabled_optimizations;
uint64_t runtime_experiment_bits;
__thread RuntimeRecoverBuf *runtime_fault_recover;

static HostRwlock table_lock = HOST_RWLOCK_INIT;
static _Atomic uint64_t table_sequence;
static __thread unsigned writer_depth, reader_depth;
static _Atomic(RangeTable *) ranges;
static _Atomic size_t range_count;

static Block *blocks;
static size_t block_count, block_capacity;
static uint64_t direct_live;

static uint64_t flex_used[FLEX_WORDS];
static uint64_t flex_in_use;
static unsigned char *backing;
static int backing_ready;

static HostMutex hook_lock = HOST_MUTEX_INIT;
static QueuedHook *hook_ring;
static size_t hook_head, hook_length, hook_capacity;
static RangeHook on_map, on_unmap, on_invalidate, on_note_write, on_cpu_write;

static _Atomic(uint64_t) *watch_bits;
static uintptr_t low_cursor = LOW_START;

static _Atomic size_t count_allocations, count_direct_maps, count_flex_maps, count_protects, count_queries;
static _Atomic uint64_t count_heap_growths;

static __thread RegionHint hints[CACHE_WAYS];
static __thread unsigned hint_next;

static uint64_t pool_bytes(void) {
    static uint64_t bytes;
    if (!bytes) {
        const char *setting = getenv("BB_DMEM_MB");
        uint64_t megabytes = setting ? strtoull(setting, NULL, 10) : 0;
        if (megabytes < 5056 || megabytes > 16384) megabytes = 5056;
        bytes = megabytes << 20;
    }
    return bytes;
}

static uint64_t round_up(uint64_t value, uint64_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

static uint64_t lower(uint64_t a, uint64_t b) {
    return a < b ? a : b;
}

static uint64_t higher(uint64_t a, uint64_t b) {
    return a > b ? a : b;
}

static int alignment_ok(uint64_t alignment) {
    return alignment >= PAGE_SIZE_GUEST && alignment <= pool_bytes() && !(alignment & (alignment - 1));
}

static int guest_prot_to_host(int prot) {
    int host = 0;
    if (prot & 0x11) host |= PROT_READ;
    if (prot & 0x22) host |= PROT_READ | PROT_WRITE;
    if (prot & 4) host |= PROT_READ | PROT_EXEC;
    return host;
}

static Range load_range(const Range *source) {
    Range r;
    r.start = __atomic_load_n(&source->start, __ATOMIC_RELAXED);
    r.end = __atomic_load_n(&source->end, __ATOMIC_RELAXED);
    r.phys = __atomic_load_n(&source->phys, __ATOMIC_RELAXED);
    r.kind = __atomic_load_n(&source->kind, __ATOMIC_RELAXED);
    r.type = __atomic_load_n(&source->type, __ATOMIC_RELAXED);
    r.prot = __atomic_load_n(&source->prot, __ATOMIC_RELAXED);
    return r;
}

static void store_range(Range *target, const Range *value) {
    __atomic_store_n(&target->start, value->start, __ATOMIC_RELAXED);
    __atomic_store_n(&target->end, value->end, __ATOMIC_RELAXED);
    __atomic_store_n(&target->phys, value->phys, __ATOMIC_RELAXED);
    __atomic_store_n(&target->kind, value->kind, __ATOMIC_RELAXED);
    __atomic_store_n(&target->type, value->type, __ATOMIC_RELAXED);
    __atomic_store_n(&target->prot, value->prot, __ATOMIC_RELAXED);
}

static void writer_enter(void) {
    if (writer_depth++) return;
    host_write_lock(&table_lock);
    atomic_fetch_add_explicit(&table_sequence, 1, memory_order_acq_rel);
}

static void writer_leave(void) {
    if (--writer_depth) return;
    atomic_fetch_add_explicit(&table_sequence, 1, memory_order_release);
    host_write_unlock(&table_lock);
}

static void reader_enter(void) {
    if (!writer_depth && !reader_depth++) host_read_lock(&table_lock);
}

static void reader_leave(void) {
    if (!writer_depth && !--reader_depth) host_read_unlock(&table_lock);
}

static void snapshot_load(Snapshot *s) {
    s->table = atomic_load_explicit(&ranges, memory_order_acquire);
    size_t count = atomic_load_explicit(&range_count, memory_order_relaxed);
    size_t capacity = s->table ? s->table->capacity : 0;
    s->count = count < capacity ? count : capacity;
}

static void snapshot_open(Snapshot *s) {
    if (writer_depth || reader_depth) {
        s->mode = 1;
    } else {
        s->sequence = atomic_load_explicit(&table_sequence, memory_order_acquire);
        if ((s->sequence & 1) || s->tries >= SNAPSHOT_TRIES) {
            host_read_lock(&table_lock);
            s->sequence = atomic_load_explicit(&table_sequence, memory_order_relaxed);
            s->mode = 2;
        } else {
            s->mode = 0;
        }
    }
    snapshot_load(s);
}

static void snapshot_begin(Snapshot *s) {
    s->tries = 0;
    s->sequence = 0;
    snapshot_open(s);
}

static int snapshot_retry(Snapshot *s) {
    if (s->mode == 1) return 0;
    if (s->mode == 2) {
        host_read_unlock(&table_lock);
        return 0;
    }
    atomic_thread_fence(memory_order_acquire);
    if (atomic_load_explicit(&table_sequence, memory_order_relaxed) == s->sequence) return 0;
    ++s->tries;
    snapshot_open(s);
    return 1;
}

static size_t index_in(const RangeTable *table, size_t count, uint64_t address) {
    if (!count) return 0;
    size_t base = 0;
    while (count > 1) {
        size_t half = count / 2;
        base += half & -(size_t)(__atomic_load_n(&table->items[base + half - 1].end, __ATOMIC_RELAXED) <= address);
        count -= half;
    }
    return base + (__atomic_load_n(&table->items[base].end, __ATOMIC_RELAXED) <= address);
}

#if defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define RANGE_MOVES_CHECKED 1
#endif
#endif

static void move_ranges(Range *to, const Range *from, size_t count) {
#ifdef RANGE_MOVES_CHECKED
    for (size_t i = 0; i < count; ++i) {
        size_t at = to < from ? i : count - 1 - i;
        Range r = load_range(&from[at]);
        store_range(&to[at], &r);
    }
#else
    memmove(to, from, count * sizeof(*to));
#endif
}

static RangeTable *write_table(void) {
    return atomic_load_explicit(&ranges, memory_order_relaxed);
}

static size_t write_count(void) {
    return atomic_load_explicit(&range_count, memory_order_relaxed);
}

static size_t find_range(uint64_t address) {
    return write_table() ? index_in(write_table(), write_count(), address) : 0;
}

static Range *range_at(size_t index) {
    return &write_table()->items[index];
}

static int insert_range(size_t at, const Range *value) {
    RangeTable *table = write_table();
    size_t count = write_count();
    if (!table || count == table->capacity) {
        size_t capacity = table ? table->capacity * 2 : 512;
        RangeTable *grown = malloc(sizeof(RangeTable) + capacity * sizeof(Range));
        if (!grown) return -1;
        grown->capacity = capacity;
        for (size_t i = 0; i < count; ++i) {
            Range r = load_range(&table->items[i]);
            store_range(&grown->items[i], &r);
        }
        atomic_store_explicit(&ranges, grown, memory_order_release);
        table = grown;
    }
    move_ranges(&table->items[at + 1], &table->items[at], count - at);
    store_range(&table->items[at], value);
    atomic_store_explicit(&range_count, count + 1, memory_order_relaxed);
    return 0;
}

static void erase_ranges(size_t at, size_t removed) {
    RangeTable *table = write_table();
    size_t count = write_count();
    move_ranges(&table->items[at], &table->items[at + removed], count - at - removed);
    atomic_store_explicit(&range_count, count - removed, memory_order_relaxed);
}

static int split_range(uint64_t address) {
    size_t i = find_range(address);
    if (i == write_count()) return 0;
    Range left = *range_at(i);
    if (left.start >= address) return 0;
    Range right = left;
    right.start = address;
    if (left.kind != RANGE_RESERVED) right.phys += address - left.start;
    __atomic_store_n(&range_at(i)->end, address, __ATOMIC_RELAXED);
    return insert_range(i + 1, &right);
}

static size_t split_span(uint64_t start, uint64_t end, int *failed) {
    *failed = split_range(start) || split_range(end);
    return find_range(start);
}

static void queue_hook(int kind, uint64_t address, uint64_t size) {
    host_lock(&hook_lock);
    RangeHook hook = kind == HOOK_MAP ? on_map : kind == HOOK_UNMAP ? on_unmap : on_invalidate;
    if (hook) {
        if (hook_length == hook_capacity) {
            size_t capacity = hook_capacity ? hook_capacity * 2 : 64;
            QueuedHook *grown = malloc(capacity * sizeof(*grown));
            if (!grown) {
                fputs("STOP: GPU hook queue overflow\n", stderr);
                exit(21);
            }
            for (size_t i = 0; i < hook_length; ++i) grown[i] = hook_ring[(hook_head + i) % hook_capacity];
            free(hook_ring);
            hook_ring = grown;
            hook_head = 0;
            hook_capacity = capacity;
        }
        hook_ring[(hook_head + hook_length++) % hook_capacity] = (QueuedHook){kind, (uintptr_t)address, size};
    }
    host_unlock(&hook_lock);
}

static void run_hooks(void) {
    for (;;) {
        host_lock(&hook_lock);
        if (!hook_length) {
            host_unlock(&hook_lock);
            return;
        }
        QueuedHook item = hook_ring[hook_head];
        hook_head = (hook_head + 1) % hook_capacity;
        --hook_length;
        RangeHook hook = item.kind == HOOK_MAP ? on_map : item.kind == HOOK_UNMAP ? on_unmap : on_invalidate;
        host_unlock(&hook_lock);
        if (hook) hook(item.address, item.size);
    }
}

#ifdef _WIN32
static int space_ready(void) {
    return win_mem_space(LOW_START, USER_END);
}

static int backing_open(void) {
    if (backing_ready) return 0;
    void *view;
    if (space_ready() || win_mem_section(pool_bytes() + FLEX_SPAN, &view)) return -1;
    backing = view;
    backing_ready = 1;
    return 0;
}

static void backing_touch(uint64_t phys, uint64_t size) {
    (void)phys;
    (void)size;
}

static void backing_clear(uint64_t phys, uint64_t size) {
    win_mem_zero(phys, size);
}
#else
typedef struct {
    uint64_t phys, size;
    int fd;
} Backing;

static int pool_fd = -1;
static Backing backings[MAX_BACKINGS];
static size_t backing_count;
static BackingMaker backing_maker;
static int backing_whole;

static int backing_open(void) {
    if (backing_ready) return 0;
    pool_fd = memfd_create("bb-guest-memory", MFD_CLOEXEC);
    uint64_t total = pool_bytes() + FLEX_SPAN;
    if (pool_fd < 0 || ftruncate(pool_fd, (off_t)total)) return -1;
    void *view = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_NORESERVE, pool_fd, 0);
    if (view == MAP_FAILED) return -1;
    backing = view;
    backing_ready = 1;
    return 0;
}

static const Backing *backing_find(uint64_t phys, uint64_t *next) {
    size_t low = 0, high = backing_count;
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        if (backings[middle].phys + backings[middle].size <= phys) low = middle + 1;
        else high = middle;
    }
    *next = low < backing_count ? backings[low].phys : pool_bytes();
    return low < backing_count && backings[low].phys <= phys ? &backings[low] : NULL;
}

static void backing_add(uint64_t phys, uint64_t size) {
    if (backing_count == MAX_BACKINGS) return;
    int fd = backing_maker ? backing_maker(phys, size) : -1;
    if (fd >= 0 && mmap(backing + phys, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0) == MAP_FAILED) {
        close(fd);
        fd = -1;
    }
    size_t at = backing_count++;
    for (; at && backings[at - 1].phys > phys; --at) backings[at] = backings[at - 1];
    backings[at] = (Backing){phys, size, fd};
}

static void backing_touch(uint64_t phys, uint64_t size) {
    uint64_t pool = pool_bytes();
    if (!backing_ready || phys >= pool) return;
    uint64_t end = lower(phys + size, pool);
    for (uint64_t at = phys; at < end;) {
        uint64_t next;
        const Backing *found = backing_find(at, &next);
        if (found) {
            at = found->phys + found->size;
            continue;
        }
        if (backing_whole) {
            uint64_t stop = lower(next, end);
            backing_add(at, stop - at);
            at = stop;
        } else {
            uint64_t first = at / CHUNK_BYTES * CHUNK_BYTES;
            backing_add(first, lower(CHUNK_BYTES, pool - first));
            at = first + CHUNK_BYTES;
        }
    }
}

static void *backing_map(uint64_t address, uint64_t size, int prot, uint64_t phys) {
    uint64_t pool = pool_bytes();
    if (phys >= pool) return mmap((void *)address, size, prot, MAP_SHARED | MAP_FIXED, pool_fd, (off_t)phys);
    backing_touch(phys, size);
    for (uint64_t done = 0; done < size;) {
        uint64_t at = phys + done, next;
        const Backing *found = backing_find(at, &next);
        uint64_t room = found ? found->phys + found->size - at : next - at;
        uint64_t piece = lower(size - done, room ? room : size - done);
        int use_fd = found && found->fd >= 0;
        void *mapped = mmap((void *)(address + done), piece, prot, MAP_SHARED | MAP_FIXED,
                            use_fd ? found->fd : pool_fd, (off_t)(use_fd ? at - found->phys : at));
        if (mapped == MAP_FAILED) {
            static _Atomic unsigned reported;
            if (atomic_fetch_add(&reported, 1) < 4)
                fprintf(stderr, "Runtime: mapping direct memory %#" PRIx64 "+%#" PRIx64 " from %s failed: %s\n", at, piece,
                        use_fd ? "a GPU-visible dma-buf" : "the memfd", strerror(errno));
            return MAP_FAILED;
        }
        done += piece;
    }
    return (void *)address;
}

static void backing_clear(uint64_t phys, uint64_t size) {
    uint64_t pool = pool_bytes();
    for (uint64_t done = 0; done < size;) {
        uint64_t at = phys + done, next = pool;
        const Backing *found = at < pool ? backing_find(at, &next) : NULL;
        uint64_t piece = at < pool ? lower(size - done, (found ? found->phys + found->size : next) - at) : size - done;
        if (found && found->fd >= 0) memset(backing + at, 0, piece);
        else fallocate(pool_fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, (off_t)at, (off_t)piece);
        done += piece;
    }
}
#endif

static void flex_mark(uint64_t first, uint64_t pages, int used) {
    for (uint64_t page = first, end = first + pages; page < end;) {
        uint64_t word = page / 64, bit = page % 64, take = lower(64 - bit, end - page);
        uint64_t mask = (take == 64 ? ~UINT64_C(0) : (UINT64_C(1) << take) - 1) << bit;
        if (used) flex_used[word] |= mask;
        else flex_used[word] &= ~mask;
        page += take;
    }
}

static uint64_t flex_take(uint64_t size) {
    uint64_t pages = size / PAGE_SIZE_GUEST, run = 0;
    for (uint64_t word = 0; word < FLEX_WORDS; ++word) {
        uint64_t bits = flex_used[word];
        if (!bits) {
            run += 64;
            if (run >= pages) {
                uint64_t first = (word + 1) * 64 - run;
                flex_mark(first, pages, 1);
                return pool_bytes() + first * PAGE_SIZE_GUEST;
            }
            continue;
        }
        if (bits == ~UINT64_C(0)) {
            run = 0;
            continue;
        }
        for (uint64_t bit = 0; bit < 64; ++bit) {
            if ((bits >> bit) & 1) {
                run = 0;
            } else if (++run == pages) {
                uint64_t first = word * 64 + bit + 1 - pages;
                flex_mark(first, pages, 1);
                return pool_bytes() + first * PAGE_SIZE_GUEST;
            }
        }
    }
    return UINT64_MAX;
}

static void flex_give_back(uint64_t phys, uint64_t size) {
    flex_mark((phys - pool_bytes()) / PAGE_SIZE_GUEST, size / PAGE_SIZE_GUEST, 0);
    backing_clear(phys, size);
}

static int fully_covered(uint64_t start, uint64_t end, int reserved_counts) {
    size_t i = find_range(start), count = write_count();
    for (uint64_t at = start; at < end; ++i) {
        if (i == count) return 0;
        const Range *r = range_at(i);
        if (r->start > at || (!reserved_counts && r->kind == RANGE_RESERVED)) return 0;
        at = r->end;
    }
    return 1;
}

static int anything_in(uint64_t start, uint64_t end, int ignore_reserved) {
    size_t count = write_count();
    for (size_t i = find_range(start); i < count && range_at(i)->start < end; ++i)
        if (!ignore_reserved || range_at(i)->kind != RANGE_RESERVED) return 1;
    return 0;
}

static uint64_t free_address(uint64_t hint, uint64_t size, uint64_t alignment) {
    uint64_t at = round_up(higher(hint, USER_START), alignment);
    size_t count = write_count();
    for (size_t i = find_range(at); at + size <= USER_END; ++i) {
        if (i == count || range_at(i)->start >= at + size) return at;
        at = round_up(range_at(i)->end, alignment);
    }
    return 0;
}

static void forget_span(uint64_t start, uint64_t end) {
    int failed;
    size_t first = split_span(start, end, &failed), removed = 0, count = write_count();
    for (; first + removed < count; ++removed) {
        const Range *r = range_at(first + removed);
        if (r->end > end || r->start < start) break;
        if (r->kind == RANGE_FLEXIBLE) {
            flex_in_use -= r->end - r->start;
            flex_give_back(r->phys, r->end - r->start);
        }
    }
    erase_ranges(first, removed);
}

#ifdef _WIN32
static void restore_after_split(uintptr_t start, uintptr_t end) {
    size_t count = write_count();
    for (size_t i = find_range(start); i < count && range_at(i)->start < end; ++i) {
        const Range *r = range_at(i);
        if (r->kind == RANGE_RESERVED) continue;
        win_mem_protect(higher(r->start, start), lower(r->end, end), guest_prot_to_host((int)r->prot));
    }
    queue_hook(HOOK_INVALIDATE, start, end - start);
}
#endif

static int32_t place_range(void **address_inout, uint64_t size, int prot, int flags, uint64_t alignment, int kind,
                           int type, uint64_t phys) {
    uint64_t address = (uint64_t)(uintptr_t)*address_inout;
    if (flags & MAP_FIXED_BIT) {
        if (!address || address % PAGE_SIZE_GUEST) return E_INVALID;
        if ((flags & MAP_NO_OVERWRITE_BIT) && anything_in(address, address + size, 1)) return E_NO_MEMORY;
    } else {
        address = free_address(address, size, alignment);
        if (!address) return E_NO_MEMORY;
    }
#ifdef _WIN32
    if (space_ready()) return E_NO_MEMORY;
    int failed = kind != RANGE_RESERVED
                     ? win_mem_map(address, address + size, phys, guest_prot_to_host(prot), restore_after_split)
                     : win_mem_release(address, address + size, restore_after_split);
    if (failed) return E_NO_MEMORY;
#else
    void *mapped = kind != RANGE_RESERVED
                       ? backing_map(address, size, guest_prot_to_host(prot), phys)
                       : mmap((void *)address, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED, -1, 0);
    if (mapped == MAP_FAILED) return E_NO_MEMORY;
#endif
    forget_span(address, address + size);
    Range entry = {address, address + size, phys, (uint32_t)kind, (uint32_t)type, (uint64_t)prot};
    if (insert_range(find_range(address), &entry)) return E_NO_MEMORY;
    if (kind == RANGE_FLEXIBLE) flex_in_use += size;
    if (kind != RANGE_RESERVED) queue_hook(HOOK_MAP, address, size);
    *address_inout = (void *)address;
    return 0;
}

static size_t find_block(uint64_t phys) {
    size_t low = 0, high = block_count;
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        if (blocks[middle].start + blocks[middle].size <= phys) low = middle + 1;
        else high = middle;
    }
    return low;
}

static int insert_block(size_t at, Block value) {
    if (block_count == block_capacity) {
        size_t capacity = block_capacity ? block_capacity * 2 : 256;
        Block *grown = realloc(blocks, capacity * sizeof(*blocks));
        if (!grown) return -1;
        blocks = grown;
        block_capacity = capacity;
    }
    memmove(blocks + at + 1, blocks + at, (block_count - at) * sizeof(*blocks));
    blocks[at] = value;
    ++block_count;
    return 0;
}

static int span_allocated(uint64_t phys, uint64_t size) {
    size_t i = find_block(phys);
    for (uint64_t at = phys; at < phys + size; ++i) {
        if (i == block_count || blocks[i].start > at) return 0;
        at = blocks[i].start + blocks[i].size;
    }
    return 1;
}

static SYS_ABI uint64_t direct_memory_size(void) {
    return pool_bytes();
}

static void report_full_pool(uint64_t size, uint64_t used) {
    static _Atomic unsigned reported;
    if (atomic_fetch_add(&reported, 1) >= 3) return;
    printf("Runtime: direct allocation of %" PRIu64 " MiB failed: the game's memory pool is full (%" PRIu64 " of %" PRIu64
           " MiB in use); a patch that enlarges the game's heaps needs BB_DMEM_MB=9152\n",
           size >> 20, used >> 20, pool_bytes() >> 20);
    fflush(stdout);
}

static SYS_ABI int32_t direct_allocate(int64_t low, int64_t high, uint64_t size, uint64_t alignment, int type, int64_t *out) {
    if (!alignment) alignment = PAGE_SIZE_GUEST;
    if (!out || low < 0 || high < 0 || !size || size % PAGE_SIZE_GUEST || !alignment_ok(alignment) || type < 0 || type > 10)
        return E_INVALID;
    uint64_t limit = lower((uint64_t)high, pool_bytes());
    if ((uint64_t)low >= limit || size > limit - (uint64_t)low) return E_NO_SPACE;
    writer_enter();
    if (backing_open()) {
        writer_leave();
        return E_NO_MEMORY;
    }
    uint64_t start = round_up((uint64_t)low, alignment);
    size_t i = find_block(start);
    for (;;) {
        if (start > limit || size > limit - start) {
            uint64_t used = direct_live;
            writer_leave();
            if ((uint64_t)high >= pool_bytes()) report_full_pool(size, used);
            return E_NO_SPACE;
        }
        while (i < block_count && blocks[i].start + blocks[i].size <= start) ++i;
        if (i == block_count || blocks[i].start >= start + size) break;
        start = round_up(blocks[i].start + blocks[i].size, alignment);
    }
    if (insert_block(i, (Block){start, size, type})) {
        writer_leave();
        return E_NO_MEMORY;
    }
    backing_touch(start, size);
    direct_live += size;
    writer_leave();
    atomic_fetch_add_explicit(&count_allocations, 1, memory_order_relaxed);
    *out = (int64_t)start;
    return 0;
}

static int32_t direct_map_held(void **address, uint64_t size, int prot, int flags, int64_t phys, uint64_t alignment) {
    if (!alignment) alignment = PAGE_SIZE_GUEST;
    if (!address || phys < 0 || (uint64_t)phys % PAGE_SIZE_GUEST || !size || size % PAGE_SIZE_GUEST ||
        !alignment_ok(alignment) || (prot & ~0x37))
        return E_INVALID;
    if (!span_allocated((uint64_t)phys, size)) return E_INVALID;
    size_t block = find_block((uint64_t)phys);
    int32_t result = place_range(address, size, prot, flags, alignment, RANGE_DIRECT, blocks[block].type, (uint64_t)phys);
    if (!result) atomic_fetch_add_explicit(&count_direct_maps, 1, memory_order_relaxed);
    return result;
}

static SYS_ABI int32_t direct_map(void **inout, uint64_t size, int prot, int flags, int64_t phys, uint64_t alignment) {
    void *address = inout ? *inout : NULL;
    writer_enter();
    int32_t result = direct_map_held(inout ? &address : NULL, size, prot, flags, phys, alignment);
    writer_leave();
    if (!result) *inout = address;
    run_hooks();
    if (result)
        printf("Runtime: mapping direct memory %#" PRIx64 ", size=%" PRIu64 " failed (error %#x)\n", (uint64_t)phys, size,
               (unsigned)result);
    return result;
}

static int32_t unmap_held(uint64_t start, uint64_t size, int queue) {
    if (start % PAGE_SIZE_GUEST || !size) return E_INVALID;
    uint64_t end = start + round_up(size, PAGE_SIZE_GUEST);
    size_t count = write_count(), first = find_range(start);
    if (queue)
        for (size_t i = first; i < count && range_at(i)->start < end; ++i)
            if (range_at(i)->kind != RANGE_RESERVED) {
                uint64_t from = higher(range_at(i)->start, start), to = lower(range_at(i)->end, end);
                queue_hook(HOOK_UNMAP, from, to - from);
            }
#ifdef _WIN32
    if (first < count && range_at(first)->start < end && win_mem_release(start, end, restore_after_split)) return E_INVALID;
#else
    for (size_t i = first; i < count && range_at(i)->start < end; ++i) {
        uint64_t from = higher(range_at(i)->start, start), to = lower(range_at(i)->end, end);
        if (munmap((void *)from, to - from)) return E_INVALID;
    }
#endif
    forget_span(start, end);
    return 0;
}

static SYS_ABI int32_t memory_unmap(void *address, uint64_t size) {
    uint64_t start = (uint64_t)(uintptr_t)address;
    host_lock(&hook_lock);
    RangeHook hook = on_unmap;
    host_unlock(&hook_lock);
    if (hook && !(start % PAGE_SIZE_GUEST) && size) hook((uintptr_t)start, round_up(size, PAGE_SIZE_GUEST));
    writer_enter();
    int32_t result = unmap_held(start, size, 0);
    writer_leave();
    run_hooks();
    return result;
}

static SYS_ABI int32_t direct_release(uint64_t start, uint64_t size) {
    if (start % PAGE_SIZE_GUEST || size % PAGE_SIZE_GUEST) return E_INVALID;
    if (!size) return 0;
    writer_enter();
    if (!span_allocated(start, size)) {
        writer_leave();
        return E_INVALID;
    }
    uint64_t end = start + size;
    for (size_t i = 0; i < write_count();) {
        Range r = *range_at(i);
        uint64_t length = r.end - r.start;
        if (r.kind == RANGE_DIRECT && r.phys < end && start < r.phys + length) {
            uint64_t from = higher(r.phys, start), to = lower(r.phys + length, end);
            if (unmap_held(r.start + (from - r.phys), to - from, 1)) {
                ++i;
                continue;
            }
            i = find_range(r.start);
            continue;
        }
        ++i;
    }
    for (size_t i = find_block(start); i < block_count && blocks[i].start < end;) {
        Block b = blocks[i];
        uint64_t from = higher(b.start, start), to = lower(b.start + b.size, end);
        uint64_t left = from - b.start, right = b.start + b.size - to;
        direct_live -= to - from;
        if (left && right) {
            blocks[i].size = left;
            insert_block(i + 1, (Block){to, right, b.type});
            i += 2;
        } else if (left) {
            blocks[i].size = left;
            ++i;
        } else if (right) {
            blocks[i].start = to;
            blocks[i].size = right;
            ++i;
        } else {
            memmove(blocks + i, blocks + i + 1, (block_count - i - 1) * sizeof(*blocks));
            --block_count;
        }
    }
    backing_clear(start, size);
    writer_leave();
    run_hooks();
    return 0;
}

static int32_t flexible_map_held(void **address, uint64_t size, int prot, int flags) {
    if (!address || !size || size % PAGE_SIZE_GUEST || (prot & ~0x37)) return E_INVALID;
    if (flex_in_use + size > FLEX_LIMIT || backing_open()) return E_NO_MEMORY;
    uint64_t phys = flex_take(size);
    if (phys == UINT64_MAX) return E_NO_MEMORY;
    int32_t result = place_range(address, size, prot, flags, PAGE_SIZE_GUEST, RANGE_FLEXIBLE, 0, phys);
    if (result) flex_give_back(phys, size);
    else atomic_fetch_add_explicit(&count_flex_maps, 1, memory_order_relaxed);
    return result;
}

static SYS_ABI int32_t flexible_map(void **inout, uint64_t size, int prot, int flags) {
    void *address = inout ? *inout : NULL;
    writer_enter();
    int32_t result = flexible_map_held(inout ? &address : NULL, size, prot, flags);
    uint64_t in_use = flex_in_use;
    writer_leave();
    if (!result) *inout = address;
    run_hooks();
    if (result)
        fprintf(stderr, "Runtime: flexible map of %" PRIu64 " bytes failed (0x%x), in use=%" PRIu64 "\n", size, (unsigned)result, in_use);
    return result;
}

static SYS_ABI int32_t flexible_map_named(void **inout, uint64_t size, int prot, int flags, const char *name) {
    (void)name;
    return flexible_map(inout, size, prot, flags);
}

static SYS_ABI int32_t flexible_release(void *address, uint64_t size) {
    return memory_unmap(address, size);
}

uint64_t runtime_heap_growths(void) {
    return atomic_load(&count_heap_growths);
}

static SYS_ABI void *posix_mmap(void *address, uint64_t size, int prot, int flags, int fd, int64_t offset) {
    atomic_fetch_add(&count_heap_growths, 1);
    if (!(flags & MAP_ANON_BIT) || fd != -1 || offset) {
        *runtime_errno() = 19;
        return (void *)-1;
    }
    if (!size) {
        *runtime_errno() = 22;
        return (void *)-1;
    }
    void *out = (flags & MAP_FIXED_BIT) ? address : NULL;
    int32_t result = flexible_map(&out, round_up(size, PAGE_SIZE_GUEST), prot & 0x37, flags & MAP_FIXED_BIT);
    uint64_t sites[3];
    runtime_guest_call_sites(sites);
    printf("Runtime: mmap %#" PRIx64 " bytes for the game (its heap grows) from +%#" PRIx64 " < +%#" PRIx64 " < +%#" PRIx64 "\n",
           size, sites[0], sites[1], sites[2]);
    if (result) {
        *runtime_errno() = result == E_NO_MEMORY ? 12 : 22;
        return (void *)-1;
    }
    return out;
}

static SYS_ABI int32_t reserve_range(void **inout, uint64_t size, int flags, uint64_t alignment) {
    if (!alignment) alignment = PAGE_SIZE_GUEST;
    if (!inout || !size || size % PAGE_SIZE_GUEST || !alignment_ok(alignment)) return E_INVALID;
    void *address = *inout;
    writer_enter();
    if ((flags & MAP_FIXED_BIT) && anything_in((uint64_t)(uintptr_t)address, (uint64_t)(uintptr_t)address + size, 0)) {
        writer_leave();
        return E_NO_MEMORY;
    }
    int32_t result = place_range(&address, size, 0, flags, alignment, RANGE_RESERVED, 0, 0);
    writer_leave();
    if (!result) *inout = address;
    return result;
}

static int32_t protect_held(uint64_t address, uint64_t size, int prot, int type) {
    uint64_t end = round_up(address + size, PAGE_SIZE_GUEST);
    uint64_t start = address & ~(PAGE_SIZE_GUEST - 1);
    if ((prot & ~0x37) || !fully_covered(start, end, 0)) return E_INVALID;
#ifdef _WIN32
    if (win_mem_protect(start, end, guest_prot_to_host(prot))) return E_INVALID;
#else
    if (mprotect((void *)start, end - start, guest_prot_to_host(prot))) return E_INVALID;
#endif
    int failed;
    size_t i = split_span(start, end, &failed), count = write_count();
    if (failed) return E_NO_MEMORY;
    for (; i < count && range_at(i)->start < end; ++i) {
        __atomic_store_n(&range_at(i)->prot, (uint64_t)prot, __ATOMIC_RELAXED);
        if (type >= 0) __atomic_store_n(&range_at(i)->type, (uint32_t)type, __ATOMIC_RELAXED);
    }
    queue_hook(HOOK_INVALIDATE, start, end - start);
    atomic_fetch_add_explicit(&count_protects, 1, memory_order_relaxed);
    return 0;
}

static SYS_ABI int32_t memory_protect(const void *address, uint64_t size, int prot) {
    writer_enter();
    int32_t result = protect_held((uint64_t)(uintptr_t)address, size, prot, -1);
    writer_leave();
    run_hooks();
    return result;
}

static SYS_ABI int32_t query_protection(void *address, void **start, void **end, uint32_t *prot) {
    uint64_t at = (uint64_t)(uintptr_t)address, run_start = 0, run_end = 0;
    uint32_t run_prot = 0;
    int found;
    Snapshot s;
    snapshot_begin(&s);
    do {
        found = 0;
        size_t i = index_in(s.table, s.count, at);
        if (i < s.count) {
            Range here = load_range(&s.table->items[i]);
            if (here.start <= at && here.kind != RANGE_RESERVED) {
                found = 1;
                run_prot = (uint32_t)here.prot;
                run_start = here.start;
                run_end = here.end;
                for (size_t j = i; j > 0; --j) {
                    Range left = load_range(&s.table->items[j - 1]);
                    if (left.end != run_start || left.kind == RANGE_RESERVED || left.prot != here.prot) break;
                    run_start = left.start;
                }
                for (size_t j = i + 1; j < s.count; ++j) {
                    Range right = load_range(&s.table->items[j]);
                    if (right.start != run_end || right.kind == RANGE_RESERVED || right.prot != here.prot) break;
                    run_end = right.end;
                }
            }
        }
    } while (snapshot_retry(&s));
    if (!found) return E_ACCESS;
    atomic_fetch_add_explicit(&count_queries, 1, memory_order_relaxed);
    if (start) *start = (void *)(uintptr_t)run_start;
    if (end) *end = (void *)(uintptr_t)run_end;
    if (prot) *prot = run_prot;
    return 0;
}

static SYS_ABI int32_t virtual_query(const void *address, int flags, QueryResult *info, uint64_t info_size) {
    if (!info || info_size != sizeof(*info)) return E_INVALID;
    uint64_t at = (uint64_t)(uintptr_t)address;
    Range r = {0};
    int found;
    Snapshot s;
    snapshot_begin(&s);
    do {
        found = 0;
        size_t i = index_in(s.table, s.count, at);
        if (i < s.count) {
            r = load_range(&s.table->items[i]);
            found = r.start <= at || (flags & 1);
        }
    } while (snapshot_retry(&s));
    if (!found) return E_ACCESS;
    atomic_fetch_add_explicit(&count_queries, 1, memory_order_relaxed);
    QueryResult result;
    memset(&result, 0, sizeof(result));
    result.start = r.start;
    result.end = r.end;
    result.offset = r.kind == RANGE_DIRECT ? r.phys : 0;
    result.protection = (int32_t)r.prot;
    result.memory_type = (int32_t)r.type;
    result.flags = (r.kind == RANGE_FLEXIBLE ? 1u : 0) | (r.kind == RANGE_DIRECT ? 2u : 0) | (r.kind != RANGE_RESERVED ? 16u : 0);
    strcpy(result.name, r.kind == RANGE_RESERVED ? "reserved" : r.kind == RANGE_DIRECT ? "direct" : "flexible");
    memcpy(info, &result, sizeof(result));
    return 0;
}

static SYS_ABI int32_t direct_memory_type(uint64_t phys, int *type, void **start, void **end) {
    reader_enter();
    size_t i = find_block(phys);
    int found = i < block_count && blocks[i].start <= phys;
    Block block = found ? blocks[i] : (Block){0, 0, 0};
    reader_leave();
    if (!found) return E_ACCESS;
    if (type) *type = block.type;
    if (start) *start = (void *)(uintptr_t)block.start;
    if (end) *end = (void *)(uintptr_t)(block.start + block.size);
    return 0;
}

static SYS_ABI int32_t batch_map(BatchItem *items, int count, int *processed) {
    if (!items || count < 0) return E_INVALID;
    int32_t result = 0;
    int done = 0;
    BatchItem small[64], *copy = small;
    if (count > 64 && !(copy = malloc((size_t)count * sizeof(*copy)))) return E_NO_MEMORY;
    if (count) memcpy(copy, items, (size_t)count * sizeof(*copy));
    writer_enter();
    for (; done < count && !result; ++done) {
        BatchItem *item = &copy[done];
        int prot = (uint8_t)item->prot;
        uint64_t start = (uint64_t)(uintptr_t)item->start;
        switch (item->operation) {
        case 0: result = direct_map_held(&item->start, item->length, prot, MAP_FIXED_BIT, (int64_t)item->offset, 0); break;
        case 1: result = unmap_held(start, item->length, 1); break;
        case 2: result = protect_held(start, item->length, prot, -1); break;
        case 3: result = flexible_map_held(&item->start, item->length, prot, MAP_FIXED_BIT); break;
        case 4: result = protect_held(start, item->length, prot, item->type); break;
        default: result = E_INVALID;
        }
    }
    if (result) --done;
    writer_leave();
    for (int i = 0; i < done; ++i)
        if (copy[i].operation == 0 || copy[i].operation == 3) items[i].start = copy[i].start;
    if (copy != small) free(copy);
    run_hooks();
    if (processed) *processed = done;
    return result;
}

void *runtime_low_map(size_t size, int prot) {
    size = round_up(size, PAGE_SIZE_GUEST);
    void *found = NULL;
    host_write_lock(&table_lock);
#ifdef _WIN32
    (void)prot;
    if (!space_ready())
        for (; !found && low_cursor + size <= USER_START; low_cursor += size + PAGE_SIZE_GUEST)
            found = win_mem_private(low_cursor, low_cursor + size);
#else
    for (; !found && low_cursor + size <= USER_START; low_cursor += size + PAGE_SIZE_GUEST) {
        void *mapped = mmap((void *)low_cursor, size, prot, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
        if (mapped != MAP_FAILED) found = mapped;
    }
#endif
    host_write_unlock(&table_lock);
    return found;
}

static void *toggle_watch(void *path) {
    for (unsigned long long last = ULLONG_MAX;;) {
        FILE *file = fopen(path, "r");
        unsigned long long value = 0, experiment = 0;
        if (file) {
            if (fscanf(file, "%llu %llu", &value, &experiment) < 1) value = 0;
            fclose(file);
        }
        __atomic_store_n(&runtime_experiment_bits, (uint64_t)experiment, __ATOMIC_RELEASE);
        if (value != last) {
            __atomic_store_n(&runtime_disabled_optimizations, (uint64_t)value, __ATOMIC_RELEASE);
            printf("Runtime: disabled optimizations mask=%llu\n", value);
            last = value;
        }
        host_sleep_ns(250000000);
    }
    return NULL;
}

unsigned char *runtime_memory_backing(uint64_t *direct_size) {
    *direct_size = backing ? pool_bytes() : 0;
    return backing;
}

#ifndef _WIN32
void runtime_memory_set_guest_chunk_whole(int whole) {
    backing_whole = whole;
}

void runtime_memory_set_guest_chunk_allocator(BackingMaker maker) {
    writer_enter();
    backing_maker = maker;
    writer_leave();
}
#endif

void runtime_memory_set_gpu_hooks(RangeHook map, RangeHook unmap, RangeHook invalidate) {
    static int watching;
    static HostThread watcher;
    const char *toggles = getenv("BB_TOGGLE_FILE");
    if (toggles && !watching) watching = !host_thread_start(&watcher, 0, toggle_watch, (void *)toggles);
    writer_enter();
    host_lock(&hook_lock);
    on_map = map;
    on_unmap = unmap;
    on_invalidate = invalidate;
    host_unlock(&hook_lock);
    size_t count = write_count();
    for (size_t i = 0; i < count; ++i)
        if (range_at(i)->kind != RANGE_RESERVED) queue_hook(HOOK_MAP, range_at(i)->start, range_at(i)->end - range_at(i)->start);
    writer_leave();
    run_hooks();
}

void runtime_memory_invalidate(uintptr_t address, uint64_t size) {
    host_lock(&hook_lock);
    RangeHook hook = on_invalidate;
    host_unlock(&hook_lock);
    if (hook && size) hook(address, size);
}

void runtime_memory_set_note_write_hook(RangeHook note) {
    on_note_write = note;
}

void runtime_memory_note_write(uintptr_t address, uint64_t size) {
    RangeHook hook = on_note_write ? on_note_write : on_invalidate;
    if (!hook || !size) return;
    uint64_t starts[8], ends[8];
    int pieces;
    Snapshot s;
    snapshot_begin(&s);
    do {
        pieces = 0;
        for (size_t i = index_in(s.table, s.count, address); i < s.count && pieces < 8; ++i) {
            Range r = load_range(&s.table->items[i]);
            if (r.start >= address + size) break;
            if (r.kind == RANGE_RESERVED || !(r.prot & 0x30)) continue;
            starts[pieces] = higher(r.start, address);
            ends[pieces] = lower(r.end, address + size);
            ++pieces;
        }
    } while (snapshot_retry(&s));
    for (int i = 0; i < pieces; ++i) hook((uintptr_t)starts[i], ends[i] - starts[i]);
}

void runtime_memory_set_cpu_write_hook(RangeHook hook) {
    if (!watch_bits) {
#ifdef _WIN32
        void *bits = VirtualAlloc(NULL, WATCH_END >> 15, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
        void *bits = mmap(NULL, WATCH_END >> 15, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        if (bits == MAP_FAILED) bits = NULL;
#endif
        if (!bits) {
            perror("Lantern Engine: write watch bitmap");
            return;
        }
        watch_bits = bits;
    }
    on_cpu_write = hook;
}

static uint64_t watch_mask(uint64_t word, uint64_t first, uint64_t last) {
    uint64_t low = word == first / 64 ? first % 64 : 0, high = word == last / 64 ? last % 64 : 63;
    return (high == 63 ? ~UINT64_C(0) : (UINT64_C(2) << high) - 1) & ~((UINT64_C(1) << low) - 1);
}

void runtime_memory_set_write_watch(uintptr_t address, uint64_t size, int watch) {
    if (!watch_bits || !size || address >= WATCH_END) return;
    uint64_t first = address / HOST_PAGE, last = lower((address + size - 1) / HOST_PAGE, WATCH_END / HOST_PAGE - 1);
    for (uint64_t word = first / 64; word <= last / 64; ++word) {
        uint64_t mask = watch_mask(word, first, last);
        if (watch) atomic_fetch_or_explicit(&watch_bits[word], mask, memory_order_release);
        else atomic_fetch_and_explicit(&watch_bits[word], ~mask, memory_order_release);
    }
}

void runtime_memory_note_cpu_write(uintptr_t address, uint64_t size) {
    if (!watch_bits || !size || address >= WATCH_END || address + size > WATCH_END) return;
    uint64_t first = address / HOST_PAGE, last = (address + size - 1) / HOST_PAGE;
    for (uint64_t word = first / 64; word <= last / 64; ++word)
        if (atomic_load_explicit(&watch_bits[word], memory_order_acquire) & watch_mask(word, first, last)) {
            on_cpu_write(address, size);
            return;
        }
}

int runtime_memory_direct_phys(uintptr_t address, uint64_t *phys, uintptr_t *end) {
    int found;
    Range r = {0};
    Snapshot s;
    snapshot_begin(&s);
    do {
        size_t i = index_in(s.table, s.count, address);
        found = 0;
        if (i < s.count) {
            r = load_range(&s.table->items[i]);
            found = r.start <= address && r.kind == RANGE_DIRECT;
        }
    } while (snapshot_retry(&s));
    if (found) {
        *phys = r.phys + (address - r.start);
        *end = (uintptr_t)r.end;
    }
    return found;
}

int runtime_memory_vma_info(uintptr_t address, int *prot, int *type, uintptr_t *end) {
    int found;
    Range r = {0};
    Snapshot s;
    snapshot_begin(&s);
    do {
        size_t i = index_in(s.table, s.count, address);
        found = 0;
        if (i < s.count) {
            r = load_range(&s.table->items[i]);
            found = r.start <= address && r.kind != RANGE_RESERVED;
        }
    } while (snapshot_retry(&s));
    if (found) {
        *prot = (int)r.prot;
        *type = r.kind == RANGE_DIRECT ? (int)r.type : -1;
        *end = (uintptr_t)r.end;
    }
    return found;
}

static void store_small(unsigned char *target, const unsigned char *source, uint64_t size) {
    if (size > 64) {
        memcpy(target, source, size);
        return;
    }
    while (size) {
        uintptr_t at = (uintptr_t)target;
        if (size >= 8 && !(at & 7)) {
            uint64_t v;
            memcpy(&v, source, 8);
            __atomic_store_n((uint64_t *)target, v, __ATOMIC_RELEASE);
            target += 8, source += 8, size -= 8;
        } else if (size >= 4 && !(at & 3)) {
            uint32_t v;
            memcpy(&v, source, 4);
            __atomic_store_n((uint32_t *)target, v, __ATOMIC_RELEASE);
            target += 4, source += 4, size -= 4;
        } else if (size >= 2 && !(at & 1)) {
            uint16_t v;
            memcpy(&v, source, 2);
            __atomic_store_n((uint16_t *)target, v, __ATOMIC_RELEASE);
            target += 2, source += 2, size -= 2;
        } else {
            __atomic_store_n(target, *source, __ATOMIC_RELEASE);
            ++target, ++source, --size;
        }
    }
}

int runtime_memory_write_backing(uintptr_t address, const void *data, uint64_t size) {
    int ok = 1;
    reader_enter();
    const RangeTable *table = atomic_load_explicit(&ranges, memory_order_relaxed);
    size_t count = atomic_load_explicit(&range_count, memory_order_relaxed);
    for (uint64_t at = address, end = address + size; at < end;) {
        size_t i = table ? index_in(table, count, at) : 0;
        if (i == count || table->items[i].start > at || table->items[i].kind == RANGE_RESERVED) {
            ok = 0;
            break;
        }
        const Range *r = &table->items[i];
        uint64_t piece = lower(r->end, end) - at;
        store_small(backing + r->phys + (at - r->start), (const unsigned char *)data + (at - address), piece);
        at += piece;
    }
    reader_leave();
    return ok;
}

void runtime_memory_read_backing(uintptr_t address, void *data, uint64_t size) {
    unsigned char *out = data;
    reader_enter();
    const RangeTable *table = atomic_load_explicit(&ranges, memory_order_relaxed);
    size_t count = atomic_load_explicit(&range_count, memory_order_relaxed);
    for (uint64_t at = address, end = address + size; at < end;) {
        size_t i = table ? index_in(table, count, at) : 0;
        uint64_t piece;
        if (i < count && table->items[i].start <= at && table->items[i].kind != RANGE_RESERVED) {
            const Range *r = &table->items[i];
            piece = lower(r->end, end) - at;
            memcpy(out + (at - address), backing + r->phys + (at - r->start), piece);
        } else {
            uint64_t next = i == count ? end : table->items[i].start > at ? table->items[i].start : table->items[i].end;
            piece = lower(next, end) - at;
            memset(out + (at - address), 0, piece);
        }
        at += piece;
    }
    reader_leave();
}

static const RegionHint *hint_for(uint64_t address) {
    if (runtime_disabled_optimizations & 1) return NULL;
    uint64_t generation = atomic_load_explicit(&table_sequence, memory_order_acquire);
    if (generation & 1) return NULL;
    for (unsigned i = 0; i < CACHE_WAYS; ++i) {
        const RegionHint *hint = &hints[i];
        if (hint->generation == generation && hint->end && hint->start <= address && address < hint->end) return hint;
    }
    return NULL;
}

static void hint_remember(uint64_t generation, const Range *r) {
    if (generation & 1) return;
    hints[hint_next++ % CACHE_WAYS] = (RegionHint){generation, r->start, r->end, r->kind};
}

const uint64_t *runtime_memory_generation(void) {
    return (const uint64_t *)&table_sequence;
}

uint64_t runtime_memory_clamp(uintptr_t address, uint64_t size) {
    const RegionHint *hint = hint_for(address);
    if (hint && hint->kind != RANGE_RESERVED && size <= hint->end - address) return size;
    uint64_t length;
    Range first = {0};
    int have_first;
    uint64_t generation;
    Snapshot s;
    snapshot_begin(&s);
    do {
        length = 0;
        have_first = 0;
        generation = s.sequence;
        for (size_t i = index_in(s.table, s.count, address); i < s.count && length < size; ++i) {
            Range r = load_range(&s.table->items[i]);
            if (!have_first && r.start <= address) {
                first = r;
                have_first = 1;
            }
            if (r.start > address + length || r.kind == RANGE_RESERVED) break;
            length = r.end - address;
        }
    } while (snapshot_retry(&s));
    if (have_first && s.mode != 1) hint_remember(generation, &first);
    return lower(length, size);
}

int runtime_memory_region(uintptr_t address, uintptr_t *start, uintptr_t *end, int *mapped) {
    const RegionHint *hint = hint_for(address);
    if (hint) {
        *start = (uintptr_t)hint->start;
        *end = (uintptr_t)hint->end;
        *mapped = hint->kind != RANGE_RESERVED;
        return 1;
    }
    Range r = {0}, left = {0};
    size_t i = 0, count = 0;
    uint64_t generation;
    Snapshot s;
    snapshot_begin(&s);
    do {
        generation = s.sequence;
        count = s.count;
        i = index_in(s.table, s.count, address);
        if (i < s.count) {
            r = load_range(&s.table->items[i]);
            if (i) left = load_range(&s.table->items[i - 1]);
        }
    } while (snapshot_retry(&s));
    if (i >= count) return 0;
    if (r.start <= address) {
        if (s.mode != 1) hint_remember(generation, &r);
        *start = (uintptr_t)r.start;
        *end = (uintptr_t)r.end;
        *mapped = r.kind != RANGE_RESERVED;
    } else {
        *start = i ? (uintptr_t)left.end : 0;
        *end = (uintptr_t)r.start;
        *mapped = 0;
    }
    return 1;
}

void runtime_memory_gpu_protect(uintptr_t address, uint64_t size, int read, int write) {
    reader_enter();
    const RangeTable *table = atomic_load_explicit(&ranges, memory_order_relaxed);
    size_t count = atomic_load_explicit(&range_count, memory_order_relaxed);
    for (size_t i = table ? index_in(table, count, address) : 0; i < count && table->items[i].start < address + size; ++i) {
        const Range *r = &table->items[i];
        if (r->kind == RANGE_RESERVED) continue;
        uint64_t from = higher(r->start, address), to = lower(r->end, address + size);
        int guest = guest_prot_to_host((int)r->prot);
        int wanted = (read ? PROT_READ : 0) | (write ? PROT_WRITE | PROT_READ : 0) | (guest & PROT_EXEC);
#ifdef _WIN32
        win_mem_protect(from, to, guest & wanted);
#else
        mprotect((void *)from, to - from, guest & wanted);
#endif
    }
    reader_leave();
}

int runtime_memory_restore_access(void *context, uintptr_t address, int repeated) {
    int is_write;
#ifdef _WIN32
    const ULONG_PTR access = ((EXCEPTION_POINTERS *)context)->ExceptionRecord->ExceptionInformation[0];
    if (access == 8) return 0;
    is_write = access == 1;
#else
    const greg_t error = ((ucontext_t *)context)->uc_mcontext.gregs[REG_ERR];
    if (error & 0x10) return 0;
    is_write = (error & 2) != 0;
#endif
    uint64_t page = address & ~(HOST_PAGE - 1);
    int restored = 0, usable = 0;
    reader_enter();
    const RangeTable *table = atomic_load_explicit(&ranges, memory_order_relaxed);
    size_t count = atomic_load_explicit(&range_count, memory_order_relaxed);
    size_t i = table ? index_in(table, count, address) : 0;
    if (i < count && table->items[i].start <= address && table->items[i].kind != RANGE_RESERVED) {
        int guest = guest_prot_to_host((int)table->items[i].prot);
        if (guest & (is_write ? PROT_WRITE : PROT_READ)) {
#ifdef _WIN32
            MEMORY_BASIC_INFORMATION info;
            if (VirtualQuery((void *)page, &info, sizeof(info)) && info.State == MEM_COMMIT) {
                const DWORD mode = info.Protect & 0xff;
                const int allowed = is_write ? (mode == PAGE_READWRITE || mode == PAGE_EXECUTE_READWRITE)
                                             : (mode != PAGE_NOACCESS && mode != PAGE_EXECUTE);
                restored = !allowed && !win_mem_protect(page, page + HOST_PAGE, guest);
                usable = allowed || restored;
            }
#else
            restored = usable = !mprotect((void *)page, HOST_PAGE, guest);
#endif
        }
    }
    reader_leave();
    if (restored) {
        static _Atomic unsigned notes;
        const unsigned n = atomic_fetch_add(&notes, 1) + 1;
        if (n <= 8 || !(n & (n - 1))) {
            char line[256];
            int length = snprintf(line, sizeof(line),
                                  "Runtime: %s fault at %#llx: guest page left protected by GPU tracking (%s); "
                                  "access restored (%u so far)\n",
                                  is_write ? "write" : "read", (unsigned long long)address,
                                  repeated ? "the fault repeats" : "not handled", n);
            if (length < 0) length = 0;
            if (length >= (int)sizeof(line)) length = (int)sizeof(line) - 1;
#ifdef _WIN32
            DWORD written;
            WriteFile(GetStdHandle(STD_ERROR_HANDLE), line, (DWORD)length, &written, NULL);
#else
            ssize_t written = write(2, line, (size_t)length);
            (void)written;
#endif
        }
    }
    return usable;
}

int runtime_memory_guest_writable(uintptr_t address, uint64_t size) {
    if (!size) return 0;
    int ok;
    Snapshot s;
    snapshot_begin(&s);
    do {
        ok = 1;
        uint64_t at = address;
        for (size_t i = index_in(s.table, s.count, address); at < address + size; ++i) {
            if (i >= s.count) {
                ok = 0;
                break;
            }
            Range r = load_range(&s.table->items[i]);
            if (r.start > at || r.kind == RANGE_RESERVED || !(guest_prot_to_host((int)r.prot) & PROT_WRITE)) {
                ok = 0;
                break;
            }
            at = r.end;
        }
    } while (snapshot_retry(&s));
    return ok;
}

int runtime_memory_is_mapped(uintptr_t address, uint64_t size) {
    const RegionHint *hint = hint_for(address);
    if (hint && hint->kind != RANGE_RESERVED && size <= hint->end - address) return 1;
    int ok;
    Snapshot s;
    snapshot_begin(&s);
    do {
        ok = 1;
        uint64_t at = address;
        for (size_t i = index_in(s.table, s.count, address); at < address + size; ++i) {
            if (i >= s.count) {
                ok = 0;
                break;
            }
            Range r = load_range(&s.table->items[i]);
            if (r.start > at || r.kind == RANGE_RESERVED) {
                ok = 0;
                break;
            }
            at = r.end;
        }
    } while (snapshot_retry(&s));
    return ok;
}

uint64_t runtime_memory_regions(void) {
    return atomic_load_explicit(&range_count, memory_order_relaxed);
}

void runtime_memory_report(void) {
    printf("Runtime: direct memory allocations=%zu, maps=%zu, live=%" PRIu64 ", budget=%" PRIu64 "\n",
           atomic_load(&count_allocations), atomic_load(&count_direct_maps), direct_live, pool_bytes());
    printf("Runtime: flexible maps=%zu, in use=%" PRIu64 ", protects=%zu, queries=%zu, regions=%zu\n",
           atomic_load(&count_flex_maps), flex_in_use, atomic_load(&count_protects), atomic_load(&count_queries),
           atomic_load(&range_count));
}

static const SysExport memory_exports[] = {
    {"pO96TwzOm5E", "sceKernelGetDirectMemorySize", direct_memory_size},
    {"rTXw65xmLIA", "sceKernelAllocateDirectMemory", direct_allocate},
    {"L-Q3LEjIbgA", "sceKernelMapDirectMemory", direct_map},
    {"MBuItvba6z8", "sceKernelReleaseDirectMemory", direct_release},
    {"cQke9UuBQOk", "sceKernelMunmap", memory_unmap},
    {"UqDGjXA5yUM", "munmap", memory_unmap},
    {"IWIBBdTHit4", "sceKernelMapFlexibleMemory", flexible_map},
    {"mL8NDH86iQI", "sceKernelMapNamedFlexibleMemory", flexible_map_named},
    {"teiItL2boFw", "sceKernelReleaseFlexibleMemory", flexible_release},
    {"7oxv3PPCumo", "sceKernelReserveVirtualRange", reserve_range},
    {"vSMAm3cxYTY", "sceKernelMprotect", memory_protect},
    {"WFcfL2lzido", "sceKernelQueryMemoryProtection", query_protection},
    {"rVjRvHJ0X6c", "sceKernelVirtualQuery", virtual_query},
    {"BC+OG5m9+bw", "sceKernelGetDirectMemoryType", direct_memory_type},
    {"2SKEx6bSq-4", "sceKernelBatchMap", batch_map},
    {"BPE9s9vQQXo", "mmap", posix_mmap},
};
SYS_LIBRARY(sys_memory, "libkernel", memory_exports);
