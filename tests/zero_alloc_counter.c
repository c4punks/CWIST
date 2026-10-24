/**
 * @file zero_alloc_counter.c
 * @brief LD_PRELOAD shim for the zero-allocation gate (#432).
 *
 * Intercepts malloc/calloc/realloc to count calls that occur between
 * the arm and disarm signals, which the gate script sends via HTTP
 * requests to /zero_alloc_arm and /zero_alloc_count on a server built
 * with CWIST_ZERO_ALLOC_GATE=1.
 *
 * The arm/disarm API is intentionally not in this file: the gate server
 * (rss_gate_server with CWIST_ZERO_ALLOC_GATE=1) calls
 * cwist_zero_alloc_arm() and cwist_zero_alloc_disarm() directly.
 * This file only intercepts libc allocations and maintains the counter.
 *
 * Build: gcc -O2 -fPIC -shared -o zero_alloc_shim.so \
 *             tests/zero_alloc_counter.c -ldl
 */
#define _GNU_SOURCE
#include <stdatomic.h>
#include <stddef.h>
#include <dlfcn.h>

/* Armed flag: 0 = not counting, 1 = counting. */
static _Atomic int  g_armed    = 0;
static _Atomic long g_alloc_count = 0;

/* Public API for the gate server to arm/disarm the counter. */
void cwist_zero_alloc_arm(void)    { atomic_store(&g_armed, 1); }
void cwist_zero_alloc_disarm(void) { atomic_store(&g_armed, 0); }
long cwist_zero_alloc_count(void)  { return atomic_load(&g_alloc_count); }
void cwist_zero_alloc_reset(void)  { atomic_store(&g_alloc_count, 0); }

/* Interposition: only count when armed. */

void *malloc(size_t size) {
    static void *(*real_malloc)(size_t) = NULL;
    if (!real_malloc) real_malloc = dlsym(RTLD_NEXT, "malloc");
    if (atomic_load(&g_armed)) atomic_fetch_add(&g_alloc_count, 1);
    return real_malloc(size);
}

void *calloc(size_t nmemb, size_t size) {
    static void *(*real_calloc)(size_t, size_t) = NULL;
    if (!real_calloc) real_calloc = dlsym(RTLD_NEXT, "calloc");
    if (atomic_load(&g_armed)) atomic_fetch_add(&g_alloc_count, 1);
    return real_calloc(nmemb, size);
}

void *realloc(void *ptr, size_t size) {
    static void *(*real_realloc)(void *, size_t) = NULL;
    if (!real_realloc) real_realloc = dlsym(RTLD_NEXT, "realloc");
    if (atomic_load(&g_armed)) atomic_fetch_add(&g_alloc_count, 1);
    return real_realloc(ptr, size);
}
