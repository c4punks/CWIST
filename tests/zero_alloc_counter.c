/**
 * @file zero_alloc_counter.c
 * @brief LD_PRELOAD shim for scripts/ci/zero_alloc_gate.sh (#432).
 *
 * Counts heap allocations (malloc, calloc, realloc and the aligned
 * allocators) made while armed.  tests/zero_alloc_server.c finds the control
 * functions with dlsym(RTLD_DEFAULT) and drives them from HTTP routes.
 *
 * The real allocators are reached through glibc's __libc_* entry points, not
 * dlsym(RTLD_NEXT): dlsym itself allocates, which would recurse into the
 * shim before it is resolved.
 *
 * Build: gcc -O2 -fPIC -shared -o zero_alloc_shim.so tests/zero_alloc_counter.c
 */
#define _GNU_SOURCE
#include <errno.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>

extern void *__libc_malloc(size_t size);
extern void *__libc_calloc(size_t nmemb, size_t size);
extern void *__libc_realloc(void *ptr, size_t size);
extern void *__libc_memalign(size_t alignment, size_t size);

static atomic_bool g_armed;
static atomic_long g_count;

void cwist_zero_alloc_arm(void) {
    atomic_store(&g_count, 0);
    atomic_store(&g_armed, true);
}

long cwist_zero_alloc_disarm(void) {
    atomic_store(&g_armed, false);
    return atomic_load(&g_count);
}

static inline void count(void) {
    if (atomic_load_explicit(&g_armed, memory_order_relaxed))
        atomic_fetch_add_explicit(&g_count, 1, memory_order_relaxed);
}

void *malloc(size_t size) {
    count();
    return __libc_malloc(size);
}

void *calloc(size_t nmemb, size_t size) {
    count();
    return __libc_calloc(nmemb, size);
}

void *realloc(void *ptr, size_t size) {
    count();
    return __libc_realloc(ptr, size);
}

void *memalign(size_t alignment, size_t size) {
    count();
    return __libc_memalign(alignment, size);
}

void *aligned_alloc(size_t alignment, size_t size) {
    count();
    return __libc_memalign(alignment, size);
}

int posix_memalign(void **out, size_t alignment, size_t size) {
    count();
    void *p = __libc_memalign(alignment, size);
    if (!p) return ENOMEM;
    *out = p;
    return 0;
}
