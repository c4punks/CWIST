/* CWIST_ASYNC_DEBUG, read once (issue #340).
 *
 * The reactor and the C1M async path log rare failures when the variable is
 * set. Those failure paths can fire in bursts, so the flag is cached instead
 * of calling getenv() each time. The cache is per translation unit; the
 * first-use race is benign (every thread computes the same value).
 *
 * Private to the build; not installed. */
#ifndef CWIST_SYS_IO_ASYNC_DEBUG_H
#define CWIST_SYS_IO_ASYNC_DEBUG_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>

static inline bool cwist_async_debug_enabled(void) {
    static _Atomic int cached = -1;
    int v = atomic_load_explicit(&cached, memory_order_relaxed);
    if (v < 0) {
        v = getenv("CWIST_ASYNC_DEBUG") != NULL;
        atomic_store_explicit(&cached, v, memory_order_relaxed);
    }
    return v != 0;
}

#endif /* CWIST_SYS_IO_ASYNC_DEBUG_H */
