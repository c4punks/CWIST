/**
 * @file baremetal.c
 * @brief OS services the core expects but newlib on bare metal lacks.
 *
 * Built only into the CWIST_BAREMETAL archive (make baremetal). newlib
 * declares the POSIX thread and timer APIs once the build enables them
 * (-D_POSIX_THREADS, -D_POSIX_TIMERS, ...; see BAREMETAL_CFLAGS) but ships
 * no implementation, so this file provides one for a single-threaded
 * runtime, on top of the firmware hooks in <cwist/sys/port.h>:
 *
 *   - pthread: mutexes and rwlocks cannot contend, condition variables never
 *     block (callers re-check their predicate), pthread_create() fails with
 *     EAGAIN so every background-thread path takes its no-thread fallback.
 *     Same contract as libttak's WASI preview1 stubs.
 *   - clock_gettime(), nanosleep() and timegm() live in baremetal_time.c,
 *     which the host unit test (tests/test_baremetal_port.c) also builds.
 *   - sched_yield() is a no-op.
 *   - 64-bit __atomic_* helpers: ARMv7-M/ARMv8-M have no LDREXD/STREXD, so
 *     GCC emits libcalls that libgcc does not implement for Cortex-M.
 */

#include <cwist/sys/platform.h>

#if defined(CWIST_BAREMETAL)

#include <cwist/sys/port.h>

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

/* --- pthread: one thread, nothing contends --------------------------------- */

int pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *attr) {
    (void)attr;
    *m = 0;
    return 0;
}
int pthread_mutex_destroy(pthread_mutex_t *m) { (void)m; return 0; }
int pthread_mutex_lock(pthread_mutex_t *m) { (void)m; return 0; }
int pthread_mutex_trylock(pthread_mutex_t *m) { (void)m; return 0; }
int pthread_mutex_unlock(pthread_mutex_t *m) { (void)m; return 0; }

int pthread_rwlock_init(pthread_rwlock_t *rw, const pthread_rwlockattr_t *attr) {
    (void)attr;
    *rw = 0;
    return 0;
}
int pthread_rwlock_destroy(pthread_rwlock_t *rw) { (void)rw; return 0; }
int pthread_rwlock_rdlock(pthread_rwlock_t *rw) { (void)rw; return 0; }
int pthread_rwlock_wrlock(pthread_rwlock_t *rw) { (void)rw; return 0; }
int pthread_rwlock_unlock(pthread_rwlock_t *rw) { (void)rw; return 0; }

int pthread_cond_init(pthread_cond_t *c, const pthread_condattr_t *attr) {
    (void)attr;
    *c = 0;
    return 0;
}
int pthread_cond_destroy(pthread_cond_t *c) { (void)c; return 0; }
int pthread_cond_signal(pthread_cond_t *c) { (void)c; return 0; }
int pthread_cond_broadcast(pthread_cond_t *c) { (void)c; return 0; }
int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m) {
    (void)c;
    (void)m;
    return 0; /* never block: no other thread could signal */
}
int pthread_cond_timedwait(pthread_cond_t *c, pthread_mutex_t *m, const struct timespec *abstime) {
    (void)c;
    (void)m;
    (void)abstime;
    return ETIMEDOUT;
}
int pthread_condattr_init(pthread_condattr_t *a) { (void)a; return 0; }
int pthread_condattr_destroy(pthread_condattr_t *a) { (void)a; return 0; }
int pthread_condattr_setclock(pthread_condattr_t *a, clockid_t id) {
    (void)a;
    (void)id;
    return 0;
}

int pthread_attr_init(pthread_attr_t *a) {
    memset(a, 0, sizeof(*a));
    return 0;
}
int pthread_attr_destroy(pthread_attr_t *a) { (void)a; return 0; }
int pthread_attr_setdetachstate(pthread_attr_t *a, int state) {
    (void)a;
    (void)state;
    return 0;
}
int pthread_attr_setstacksize(pthread_attr_t *a, size_t size) {
    (void)a;
    (void)size;
    return 0;
}

int pthread_create(pthread_t *t, const pthread_attr_t *attr, void *(*fn)(void *), void *arg) {
    (void)t;
    (void)attr;
    (void)fn;
    (void)arg;
    return EAGAIN; /* no threads: callers fall back to inline/no worker */
}
int pthread_join(pthread_t t, void **ret) {
    (void)t;
    (void)ret;
    return ESRCH;
}
int pthread_detach(pthread_t t) {
    (void)t;
    return ESRCH;
}
pthread_t pthread_self(void) { return (pthread_t)1; }
int pthread_equal(pthread_t a, pthread_t b) { return a == b; }

int pthread_once(pthread_once_t *once, void (*init)(void)) {
    if (!once->init_executed) {
        once->init_executed = 1;
        init();
    }
    return 0;
}

#define CWIST_BAREMETAL_KEYS 32
static void *g_key_values[CWIST_BAREMETAL_KEYS];
static unsigned g_key_count;

int pthread_key_create(pthread_key_t *key, void (*destructor)(void *)) {
    (void)destructor; /* the only thread never exits */
    if (g_key_count >= CWIST_BAREMETAL_KEYS) return EAGAIN;
    *key = (pthread_key_t)g_key_count++;
    return 0;
}
void *pthread_getspecific(pthread_key_t key) {
    return (unsigned)key < g_key_count ? g_key_values[key] : NULL;
}
int pthread_setspecific(pthread_key_t key, const void *value) {
    if ((unsigned)key >= g_key_count) return EINVAL;
    g_key_values[key] = (void *)value;
    return 0;
}

int sched_yield(void) { return 0; }

/* --- 64-bit atomics (Cortex-M: mask interrupts around the access) ------------ */

#if defined(__ARM_ARCH_7M__) || defined(__ARM_ARCH_7EM__) || defined(__ARM_ARCH_8M_MAIN__) || \
    defined(__ARM_ARCH_8_1M_MAIN__)

static inline uint32_t atomic_irq_save(void) {
    uint32_t primask;
    __asm volatile("mrs %0, primask\n\tcpsid i" : "=r"(primask)::"memory");
    return primask;
}
static inline void atomic_irq_restore(uint32_t primask) {
    __asm volatile("msr primask, %0" ::"r"(primask) : "memory");
}

/* GCC's libcall ABI for 8-byte atomics; the memory-order argument is moot
 * with interrupts masked on a single core. */
uint64_t __atomic_load_8(const volatile void *mem, int order) {
    (void)order;
    uint32_t s = atomic_irq_save();
    uint64_t v = *(const volatile uint64_t *)mem;
    atomic_irq_restore(s);
    return v;
}
void __atomic_store_8(volatile void *mem, uint64_t val, int order) {
    (void)order;
    uint32_t s = atomic_irq_save();
    *(volatile uint64_t *)mem = val;
    atomic_irq_restore(s);
}
uint64_t __atomic_exchange_8(volatile void *mem, uint64_t val, int order) {
    (void)order;
    uint32_t s = atomic_irq_save();
    uint64_t old = *(volatile uint64_t *)mem;
    *(volatile uint64_t *)mem = val;
    atomic_irq_restore(s);
    return old;
}
bool __atomic_compare_exchange_8(volatile void *mem, void *expected, uint64_t desired, bool weak,
                                 int success, int failure) {
    (void)weak;
    (void)success;
    (void)failure;
    uint32_t s = atomic_irq_save();
    uint64_t cur = *(volatile uint64_t *)mem;
    bool ok = cur == *(uint64_t *)expected;
    if (ok)
        *(volatile uint64_t *)mem = desired;
    else
        *(uint64_t *)expected = cur;
    atomic_irq_restore(s);
    return ok;
}

#define CWIST_BAREMETAL_FETCH_OP(name, op)                                                         \
    uint64_t __atomic_fetch_##name##_8(volatile void *mem, uint64_t val, int order) {             \
        (void)order;                                                                               \
        uint32_t s = atomic_irq_save();                                                            \
        uint64_t old = *(volatile uint64_t *)mem;                                                  \
        *(volatile uint64_t *)mem = old op val;                                                    \
        atomic_irq_restore(s);                                                                     \
        return old;                                                                                \
    }
CWIST_BAREMETAL_FETCH_OP(add, +)
CWIST_BAREMETAL_FETCH_OP(sub, -)
CWIST_BAREMETAL_FETCH_OP(and, &)
CWIST_BAREMETAL_FETCH_OP(or, |)
CWIST_BAREMETAL_FETCH_OP(xor, ^)
#undef CWIST_BAREMETAL_FETCH_OP

#endif /* ARMv7-M / ARMv8-M mainline */

#endif /* CWIST_BAREMETAL */
