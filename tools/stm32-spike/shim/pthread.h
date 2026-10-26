/* spike shim: single-threaded pthread stubs (lifted from libttak WASI fallback) */
#ifndef SPIKE_PTHREAD_H
#define SPIKE_PTHREAD_H
#include <sys/types.h>
#include <time.h>
/* newlib sys/types.h already declares pthread types; rename ours */
#define pthread_attr_t spike_pthread_attr_t
#define pthread_t spike_pthread_t
#define pthread_key_t spike_pthread_key_t
#define pthread_cond_t spike_pthread_cond_t
#define pthread_condattr_t spike_pthread_condattr_t
#define pthread_mutex_t spike_pthread_mutex_t
#define pthread_mutexattr_t spike_pthread_mutexattr_t
#define pthread_once_t spike_pthread_once_t
#define pthread_rwlock_t spike_pthread_rwlock_t
#define pthread_rwlockattr_t spike_pthread_rwlockattr_t
/* -----------------------------------------------------------------------
 * WASI (preview1) fallback: the sysroot ships no <pthread.h>. Searching
 * host include directories would leak glibc headers, so this branch
 * provides self-contained stubs. Synchronisation primitives cannot
 * contend on a single-threaded host: mutexes track state only to catch
 * misuse, cond vars never block, and pthread_create fails with ENOSYS.
 *
 * Note: sysroots whose <sys/types.h> defines pthread types (musl-style
 * bits/alltypes.h, as in wasi-libc) cannot use this fallback - the
 * typedefs would collide. Such sysroots are expected to provide their
 * own <pthread.h>, which is consumed via include_next above.
 * --------------------------------------------------------------------- */

#include <errno.h>   /* EBUSY / EDEADLK / ENOSYS */
#include <stddef.h>  /* size_t */

typedef int pthread_mutex_t;
typedef int pthread_cond_t;
typedef int pthread_t;
typedef int pthread_once_t;
typedef int pthread_attr_t;
typedef int pthread_mutexattr_t;
typedef int pthread_condattr_t;
typedef int pthread_rwlockattr_t;
typedef struct { int exclusive; } pthread_rwlock_t;

#define PTHREAD_MUTEX_INITIALIZER 0
#define PTHREAD_ONCE_INIT         0
#define PTHREAD_RWLOCK_INITIALIZER   { 0 }
#define PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP  1  /* stub value */

static __inline int pthread_mutex_init(pthread_mutex_t *m,
                                        const pthread_mutexattr_t *attr) {
    (void)attr; *m = 0; return 0;
}
static __inline int pthread_mutex_lock(pthread_mutex_t *m) {
    if (*m) return EDEADLK;
    *m = 1; return 0;
}
static __inline int pthread_mutex_trylock(pthread_mutex_t *m) {
    if (*m) return EBUSY;
    *m = 1; return 0;
}
static __inline int pthread_mutex_unlock(pthread_mutex_t *m) {
    *m = 0; return 0;
}
static __inline int pthread_mutex_destroy(pthread_mutex_t *m) {
    (void)m; return 0;
}

static __inline int pthread_attr_init(pthread_attr_t *attr) {
    (void)attr; return 0;
}
static __inline int pthread_attr_destroy(pthread_attr_t *attr) {
    (void)attr; return 0;
}
static __inline int pthread_attr_setstacksize(pthread_attr_t *attr,
                                              size_t stacksize) {
    (void)attr; (void)stacksize; return 0;
}
static __inline int pthread_attr_getstacksize(const pthread_attr_t *attr,
                                              size_t *stacksize) {
    (void)attr; if (stacksize) *stacksize = 0; return 0;
}

static __inline int pthread_cond_init(pthread_cond_t *c,
                                       const pthread_condattr_t *attr) {
    (void)c; (void)attr; return 0;
}
static __inline int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m) {
    /* Single-threaded host: never block; the caller re-checks state. */
    (void)c; (void)m; return 0;
}
static __inline int pthread_cond_timedwait(pthread_cond_t *c,
                                            pthread_mutex_t *m,
                                            const struct timespec *abstime) {
    (void)c; (void)m; (void)abstime; return 0;
}
static __inline int pthread_cond_signal(pthread_cond_t *c) {
    (void)c; return 0;
}
static __inline int pthread_cond_broadcast(pthread_cond_t *c) {
    (void)c; return 0;
}
static __inline int pthread_cond_destroy(pthread_cond_t *c) {
    (void)c; return 0;
}

static __inline int pthread_rwlock_init(pthread_rwlock_t *rw,
                                         const pthread_rwlockattr_t *attr) {
    (void)attr; rw->exclusive = 0; return 0;
}
static __inline int pthread_rwlock_rdlock(pthread_rwlock_t *rw) {
    if (rw->exclusive) return EDEADLK;
    return 0;
}
static __inline int pthread_rwlock_wrlock(pthread_rwlock_t *rw) {
    if (rw->exclusive) return EDEADLK;
    rw->exclusive = 1; return 0;
}
static __inline int pthread_rwlock_unlock(pthread_rwlock_t *rw) {
    rw->exclusive = 0; return 0;
}
static __inline int pthread_rwlock_destroy(pthread_rwlock_t *rw) {
    (void)rw; return 0;
}

static __inline int pthread_rwlockattr_init(pthread_rwlockattr_t *a) {
    (void)a; return 0;
}
static __inline int pthread_rwlockattr_destroy(pthread_rwlockattr_t *a) {
    (void)a; return 0;
}
static __inline int pthread_rwlockattr_setkind_np(pthread_rwlockattr_t *a,
                                                   int pref) {
    (void)a; (void)pref; return 0;
}

static __inline int pthread_once(pthread_once_t *once_ctrl,
                                  void (*init_routine)(void)) {
    /* States mirror the MSVC branch: 0=new, 1=running, 2=done. A second
     * call cannot arrive mid-initialisation on a single-threaded host. */
    if (*once_ctrl != 2) {
        *once_ctrl = 1;
        init_routine();
        *once_ctrl = 2;
    }
    return 0;
}

static __inline int pthread_create(pthread_t *thread,
                                    const void *attr,
                                    void *(*start_routine)(void *),
                                    void *arg) {
    (void)attr; (void)start_routine; (void)arg;
    if (thread) *thread = 0;
    return ENOSYS;
}
static __inline int pthread_join(pthread_t thread, void **retval) {
    (void)thread; (void)retval; return 0;
}
static __inline pthread_t pthread_self(void) {
    return 0;
}


/* spike additions: TLS keys are plain slots on a single thread */
#ifndef PTHREAD_COND_INITIALIZER
#define PTHREAD_COND_INITIALIZER 0
#endif
typedef int spike_pthread_key_t;
static void *spike_key_slots[64];
static int spike_key_next;
static __inline int pthread_key_create(pthread_key_t *k, void (*d)(void *)) { (void)d; if (spike_key_next >= 64) return 11; *k = spike_key_next++; return 0; }
static __inline void *pthread_getspecific(pthread_key_t k) { return spike_key_slots[k]; }
static __inline int pthread_setspecific(pthread_key_t k, const void *v) { spike_key_slots[k] = (void *)v; return 0; }
static __inline int pthread_equal(pthread_t a, pthread_t b) { return a == b; }
static __inline int pthread_attr_setdetachstate(pthread_attr_t *a, int s) { (void)a; (void)s; return 0; }
static __inline int pthread_detach(pthread_t t) { (void)t; return 0; }
#ifndef PTHREAD_CREATE_DETACHED
#define PTHREAD_CREATE_DETACHED 1
#endif
#endif
