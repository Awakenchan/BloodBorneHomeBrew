#include "runtime.h"
#include "sys/sys.h"
#include <stdio.h>
#ifdef _WIN32
enum { SLOTS = 1024 };
typedef struct {
    volatile LONG tid;
    volatile LONG kind;
    volatile uintptr_t object;
} Blocked;
static Blocked slots[SLOTS];

int runtime_block_begin(int kind, uintptr_t object) {
    const LONG tid = (LONG)GetCurrentThreadId();
    for (unsigned i = 0; i < SLOTS; ++i) {
        unsigned at = ((unsigned)tid / 4 + i) % SLOTS;
        if (InterlockedCompareExchange(&slots[at].tid, tid, 0) == 0) {
            slots[at].object = object;
            slots[at].kind = kind;
            return (int)at;
        }
    }
    return -1;
}

void runtime_block_end(int slot) {
    if (slot < 0) return;
    slots[slot].kind = 0;
    InterlockedExchange(&slots[slot].tid, 0);
}

void runtime_blocked_report(void) {
    for (unsigned i = 0; i < SLOTS; ++i) {
        const LONG tid = slots[i].tid, kind = slots[i].kind;
        const uintptr_t object = slots[i].object;
        if (!tid || !kind) continue;
        char what[160];
        if (kind == RUNTIME_BLOCK_MUTEX) sys_mutex_describe(object, what, sizeof what);
        else if (kind == RUNTIME_BLOCK_SEMA) sys_sema_describe((uint32_t)object, what, sizeof what);
        else snprintf(what, sizeof what, "condition variable %#llx", (unsigned long long)object);
        printf("Freeze: thread %ld waits for %s\n", (long)tid, what);
    }
}
#else
int runtime_block_begin(int kind, uintptr_t object) { (void)kind; (void)object; return -1; }
void runtime_block_end(int slot) { (void)slot; }
void runtime_blocked_report(void) {}
#endif
