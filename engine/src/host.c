#define _GNU_SOURCE
#include "runtime.h"
#include <stdlib.h>
#ifdef _WIN32

static uint64_t qpc_frequency(void) {
    static uint64_t frequency;
    if (!frequency) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); frequency = (uint64_t)f.QuadPart; }
    return frequency;
}
uint64_t host_monotonic_ns(void) {
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    uint64_t f = qpc_frequency(), n = (uint64_t)c.QuadPart;
    return n / f * 1000000000u + n % f * 1000000000u / f;
}
uint64_t host_realtime_ns(void) {
    FILETIME t;
    GetSystemTimePreciseAsFileTime(&t);
    uint64_t ticks = ((uint64_t)t.dwHighDateTime << 32) | t.dwLowDateTime;
    return (ticks - UINT64_C(116444736000000000)) * 100;
}
static __thread HANDLE sleep_timer;
static __thread int sleep_timer_high_resolution;
enum { PRECISE_SPIN_NS = 600000, ADAPTIVE_MARGIN_NS = 100000 };
static int64_t timer_late_ns = 300000;
#define SPIN_ADAPTIVE UINT64_MAX
static void sleep_until_spinning(uint64_t deadline, uint64_t spin_ns) {
    uint64_t now = host_monotonic_ns();
    if (now >= deadline) return;
    if (!sleep_timer) {
        sleep_timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        sleep_timer_high_resolution = sleep_timer != NULL;
        if (!sleep_timer) sleep_timer = CreateWaitableTimerExW(NULL, NULL, 0, TIMER_ALL_ACCESS);
    }
    if (spin_ns == SPIN_ADAPTIVE) {
        int64_t late = __atomic_load_n(&timer_late_ns, __ATOMIC_RELAXED);
        spin_ns = (uint64_t)(late > 0 ? late : 0) + ADAPTIVE_MARGIN_NS;
        if (spin_ns > PRECISE_SPIN_NS) spin_ns = PRECISE_SPIN_NS;
    }
    if (!sleep_timer_high_resolution && spin_ns < PRECISE_SPIN_NS) spin_ns = PRECISE_SPIN_NS;
    if (deadline - now > spin_ns) {
        const uint64_t due_at = deadline - spin_ns;
        LARGE_INTEGER due;
        due.QuadPart = -(LONGLONG)((due_at - now) / 100);
        if (sleep_timer && due.QuadPart < 0 && SetWaitableTimer(sleep_timer, &due, 0, NULL, NULL, FALSE)) {
            WaitForSingleObject(sleep_timer, INFINITE);
            if (sleep_timer_high_resolution) {
                const uint64_t woke = host_monotonic_ns();
                int64_t sample = woke > due_at ? (int64_t)(woke - due_at) : 0;
                if (sample > 2000000) sample = 2000000;
                const int64_t old = __atomic_load_n(&timer_late_ns, __ATOMIC_RELAXED);
                __atomic_store_n(&timer_late_ns, old + (sample - old) / 16, __ATOMIC_RELAXED);
            }
        } else {
            Sleep((DWORD)((deadline - now) / 1000000));
        }
    }
    while (host_monotonic_ns() < deadline) YieldProcessor();
}
void host_sleep_until_ns(uint64_t deadline) { sleep_until_spinning(deadline, PRECISE_SPIN_NS); }
static uint64_t guest_sleep_spin_ns(void) {
    static uint64_t spin = 0;
    uint64_t value = __atomic_load_n(&spin, __ATOMIC_RELAXED);
    if (value == 0) {
        const char *env = getenv("BB_SLEEP_SPIN_US");
        value = env && env[0] ? strtoull(env, NULL, 10) * 1000 : SPIN_ADAPTIVE;
        if (value != SPIN_ADAPTIVE && value > PRECISE_SPIN_NS) value = PRECISE_SPIN_NS;
        if (value == 0) value = 1;
        __atomic_store_n(&spin, value, __ATOMIC_RELAXED);
    }
    return value;
}
void host_sleep_ns(uint64_t ns) {
    if (!ns) { SwitchToThread(); return; }
    sleep_until_spinning(host_monotonic_ns() + ns, guest_sleep_spin_ns());
}
#else
#include <pthread.h>
#include <time.h>

int host_thread_start(HostThread *thread, size_t stack, void *(*entry)(void *), void *argument) {
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    if (stack) pthread_attr_setstacksize(&attr, stack);
    int e = pthread_create(thread, &attr, entry, argument);
    pthread_attr_destroy(&attr);
    return e;
}
int host_thread_join(HostThread thread) { return pthread_join(thread, NULL); }
void host_thread_detach(HostThread thread) { pthread_detach(thread); }
static uint64_t read_clock(clockid_t id) {
    struct timespec t;
    clock_gettime(id, &t);
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}
uint64_t host_monotonic_ns(void) { return read_clock(CLOCK_MONOTONIC); }
uint64_t host_realtime_ns(void) { return read_clock(CLOCK_REALTIME); }
void host_sleep_ns(uint64_t ns) {
    struct timespec t = {.tv_sec = (time_t)(ns / 1000000000), .tv_nsec = (long)(ns % 1000000000)};
    while (nanosleep(&t, &t) && errno == EINTR) {}
}
void host_sleep_until_ns(uint64_t deadline) {
    struct timespec t = {(time_t)(deadline / 1000000000u), (long)(deadline % 1000000000u)};
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &t, NULL)) {}
}
#endif
