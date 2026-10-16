#include <cwist/core/mem/gc.h>
#include <cwist/core/mem/alloc.h>
#include <ttak/mem/epoch.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#if !defined(__EMSCRIPTEN__) && !defined(__wasi__)
#include <sys/mman.h>
#endif
#include <unistd.h>

/**
 * @file gc.c
 * @brief Thin implementation wrapper around libttak epoch-based reclamation.
 */

/* --- cwist_reg_ptr registry: CWIST-side bookkeeping for pointers handed to
 * a GC context via cwist_reg_ptr()/cwist_reg_ptr_sized().
 *
 * libttak's epoch GC releases whatever is still in its mem_tree at destroy
 * time with ttak_mem_free(), which validates the ttak block header and
 * aborts on any pointer that does not carry one (lib/libttak/src/mem/mem.c's
 * V_HEADER). cwist_alloc() blocks have no such header when the owner guard
 * is off (the default), so the wrapper must detach every CWIST-registered
 * block from the tree before destruction and release it with cwist_free()
 * itself. This table records exactly which pointers went through the
 * wrapper (and which context owns them) so cwist_gc_shutdown(), an explicit
 * cwist_free(), and cross-context re-registration can do that detachment.
 *
 * Pointers registered directly with ttak via cwist_gc_raw() are
 * intentionally absent here: they keep libttak's native teardown semantics
 * (freed by ttak_mem_free() at destroy), e.g. example/rps-showcase's
 * ttak_mem_alloc_safe() payloads. Never mix the two registration paths for
 * the same pointer.
 *
 * The table, like the other registries in this file, uses the raw
 * allocator: it is the GC's own bookkeeping and must never sit on a
 * full-GC pending list. */

typedef struct {
    void *ptr;         /* NULL marks an empty slot */
    cwist_gc_t *owner; /* context responsible for releasing the block */
} cwist_reg_entry_t;

static pthread_mutex_t g_reg_lock = PTHREAD_MUTEX_INITIALIZER;
static cwist_reg_entry_t *g_reg_slots = NULL; /* open addressing, power-of-two cap */
static size_t g_reg_cap = 0;
static size_t g_reg_count = 0;

static size_t cwist_reg_slot(const void *ptr, size_t mask) {
    uint64_t h = (uint64_t)((uintptr_t)ptr >> 4) * UINT64_C(0x9E3779B97F4A7C15);
    return (size_t)(h >> 32) & mask;
}

/** @brief Entry for @p ptr, or NULL. Caller holds g_reg_lock. */
static cwist_reg_entry_t *cwist_reg_find_locked(const void *ptr) {
    if (g_reg_count == 0) return NULL;
    size_t mask = g_reg_cap - 1;
    for (size_t i = cwist_reg_slot(ptr, mask);; i = (i + 1) & mask) {
        if (!g_reg_slots[i].ptr) return NULL;
        if (g_reg_slots[i].ptr == ptr) return &g_reg_slots[i];
    }
}

/** @brief Backward-shift delete of the entry at @p e. Caller holds g_reg_lock. */
static void cwist_reg_remove_locked(cwist_reg_entry_t *e) {
    size_t mask = g_reg_cap - 1;
    size_t hole = (size_t)(e - g_reg_slots);
    for (size_t j = (hole + 1) & mask; g_reg_slots[j].ptr; j = (j + 1) & mask) {
        size_t home = cwist_reg_slot(g_reg_slots[j].ptr, mask);
        if (((j - home) & mask) >= ((j - hole) & mask)) {
            g_reg_slots[hole] = g_reg_slots[j];
            hole = j;
        }
    }
    g_reg_slots[hole] = (cwist_reg_entry_t){0};
    g_reg_count--;
}

/** @brief Grow so one more entry fits at <= 50% load. Caller holds g_reg_lock. */
static bool cwist_reg_reserve_locked(void) {
    if ((g_reg_count + 1) * 2 <= g_reg_cap) return true;
    size_t new_cap = g_reg_cap ? g_reg_cap * 2 : 64;
    cwist_reg_entry_t *slots = (cwist_reg_entry_t *)calloc(new_cap, sizeof(*slots));
    if (!slots) return false;
    size_t mask = new_cap - 1;
    for (size_t i = 0; i < g_reg_cap; i++) {
        if (!g_reg_slots[i].ptr) continue;
        size_t j = cwist_reg_slot(g_reg_slots[i].ptr, mask);
        while (slots[j].ptr) j = (j + 1) & mask;
        slots[j] = g_reg_slots[i];
    }
    free(g_reg_slots);
    g_reg_slots = slots;
    g_reg_cap = new_cap;
    return true;
}

/**
 * @brief Detach @p ptr from whichever GC context tracks it, without freeing.
 *
 * Removes the pointer from the registry and from its owner's libttak
 * mem_tree (when that context is still initialized) so a later
 * cwist_gc_shutdown() of the context no longer touches the block. Called by
 * cwist_free() so an explicit free of a registered pointer is always safe,
 * and by cwist_reg_ptr_sized() when registration moves to another context.
 *
 * @param ptr Block to detach; a no-op when NULL.
 * @return true when @p ptr was registered and has been detached.
 */
bool cwist_gc_reg_untrack(void *ptr) {
    if (!ptr) return false;
    pthread_mutex_lock(&g_reg_lock);
    cwist_reg_entry_t *e = cwist_reg_find_locked(ptr);
    if (!e) {
        pthread_mutex_unlock(&g_reg_lock);
        return false;
    }
    cwist_gc_t *owner = e->owner;
    cwist_reg_remove_locked(e);
    pthread_mutex_unlock(&g_reg_lock);

    if (owner && owner->initialized) {
        ttak_mem_node_t *node = ttak_mem_tree_find_node(&owner->impl.tree, ptr);
        if (node) ttak_mem_tree_remove(&owner->impl.tree, node);
    }
    return true;
}

/**
 * @brief Release every block still registered with @p gc through the
 *        cwist_reg_ptr()/cwist_reg_ptr_sized() wrapper.
 *
 * Each block is first detached from the context's libttak mem_tree (so the
 * upcoming ttak destroy-time pass, which frees with ttak_mem_free() and
 * requires a ttak block header, never sees it) and then released with
 * cwist_free(). Entries are taken one at a time: cwist_free() itself calls
 * cwist_gc_reg_untrack(), which takes the same registry lock.
 *
 * @param gc Context being shut down.
 */
static void cwist_reg_drain(cwist_gc_t *gc) {
    for (;;) {
        pthread_mutex_lock(&g_reg_lock);
        cwist_reg_entry_t *e = NULL;
        for (size_t i = 0; i < g_reg_cap; i++) {
            if (g_reg_slots[i].ptr && g_reg_slots[i].owner == gc) {
                e = &g_reg_slots[i];
                break;
            }
        }
        if (!e) {
            pthread_mutex_unlock(&g_reg_lock);
            return;
        }
        void *ptr = e->ptr;
        cwist_reg_remove_locked(e);
        pthread_mutex_unlock(&g_reg_lock);

        ttak_mem_node_t *node = ttak_mem_tree_find_node(&gc->impl.tree, ptr);
        if (node) ttak_mem_tree_remove(&gc->impl.tree, node);
        cwist_free(ptr);
    }
}

/**
 * @brief Lazily initialize and optionally configure manual rotation mode.
 * @param gc GC context owned by the caller.
 * @param manual_rotation When true, keep epoch advancement under explicit caller control.
 */
void cwist_gc(cwist_gc_t *gc, bool manual_rotation) {
    if (!gc) return;
    if (!gc->initialized) {
        ttak_epoch_gc_init(&gc->impl);
        gc->initialized = true;
    }
    ttak_epoch_gc_manual_rotate(&gc->impl, manual_rotation);
}

/**
 * @brief Destroy the wrapped libttak GC state when it has been initialized.
 *
 * Blocks still registered through cwist_reg_ptr()/cwist_reg_ptr_sized() are
 * detached from the libttak tree and released with cwist_free() first;
 * anything registered directly with ttak via cwist_gc_raw() is left for
 * ttak_epoch_gc_destroy() to release with ttak_mem_free().
 *
 * @param gc GC context to shut down.
 */
void cwist_gc_shutdown(cwist_gc_t *gc) {
    if (!gc || !gc->initialized) return;
    cwist_reg_drain(gc);
    ttak_epoch_gc_destroy(&gc->impl);
    gc->initialized = false;
}

/**
 * @brief Advance the current epoch and reclaim retired nodes when eligible.
 * @param gc GC context to rotate.
 */
void cwist_gc_rotate(cwist_gc_t *gc) {
    if (!gc || !gc->initialized) return;
    ttak_epoch_gc_rotate(&gc->impl);
}

/**
 * @brief Enable auto-rotation of epoch GC.
 * @param gc GC context to enable auto-rotation.
 */
void cwist_gc_auto_rotate(cwist_gc_t *gc, bool enabled) {
    if (!gc || !gc->initialized) return;
    bool gc_status = atomic_load(&gc->auto_rotated);
    while (!atomic_compare_exchange_weak(&gc->auto_rotated, &gc_status, enabled));
}

/**
 * @brief Return if epoch GC is auto-rotated.
 * @ param gc GC context to get status.
 */
bool cwist_gc_auto_rotated(cwist_gc_t *gc) {
    return atomic_load(&gc->auto_rotated);
}

/**
 * @brief Register a pointer with zero-size accounting metadata.
 *
 * See cwist_reg_ptr_sized() for the full contract: registering hands
 * ownership of the block to the context, which releases it with
 * cwist_free() at cwist_gc_shutdown().
 *
 * @param gc GC context that tracks the pointer.
 * @param ptr Pointer to retire through the epoch GC.
 */
void cwist_reg_ptr(cwist_gc_t *gc, void *ptr) {
    cwist_reg_ptr_sized(gc, ptr, 0);
}

/**
 * @brief Register a pointer and its approximate size with the epoch GC.
 *
 * The pointer must be releasable with cwist_free() (i.e. come from CWIST's
 * allocator: cwist_alloc() and family). The wrapper records the pointer in
 * its own registry so that cwist_gc_shutdown() can detach it from libttak's
 * mem_tree and release it with cwist_free(); libttak's destroy-time pass
 * frees with ttak_mem_free(), which aborts on headerless cwist_alloc
 * blocks. Pointers allocated directly from libttak (ttak_mem_alloc_safe())
 * must NOT go through here; register them with the raw context instead —
 * ttak_epoch_gc_register(cwist_gc_raw(gc), ptr, size) — so libttak's own
 * teardown releases them.
 *
 * Ownership: after registration the context owns the block and will free it
 * at shutdown. An explicit cwist_free() first detaches it automatically;
 * registering the same pointer with another context moves tracking there.
 *
 * @param gc GC context that tracks the pointer.
 * @param ptr Pointer to retire through the epoch GC.
 * @param size Optional size hint associated with @p ptr.
 */
void cwist_reg_ptr_sized(cwist_gc_t *gc, void *ptr, size_t size) {
    if (!gc || !gc->initialized || !ptr) return;

    pthread_mutex_lock(&g_reg_lock);
    cwist_reg_entry_t *e = cwist_reg_find_locked(ptr);
    if (e) {
        if (e->owner == gc) {
            /* Already tracked by this context: skip a duplicate ttak tree
             * node, which destroy would hand to ttak_mem_free() twice. */
            pthread_mutex_unlock(&g_reg_lock);
            return;
        }
        /* Registration moves to a new context: drop the pointer from the
         * previous owner first, so that context's shutdown neither frees
         * the block nor trips over it. */
        cwist_gc_t *prev = e->owner;
        cwist_reg_remove_locked(e);
        pthread_mutex_unlock(&g_reg_lock);
        if (prev && prev->initialized) {
            ttak_mem_node_t *node = ttak_mem_tree_find_node(&prev->impl.tree, ptr);
            if (node) ttak_mem_tree_remove(&prev->impl.tree, node);
        }
        pthread_mutex_lock(&g_reg_lock);
        if (cwist_reg_find_locked(ptr)) {
            /* Another thread registered it in the meantime; that
             * registration stands. */
            pthread_mutex_unlock(&g_reg_lock);
            return;
        }
    }
    bool tracked = cwist_reg_reserve_locked();
    if (tracked) {
        size_t mask = g_reg_cap - 1;
        size_t i = cwist_reg_slot(ptr, mask);
        while (g_reg_slots[i].ptr) i = (i + 1) & mask;
        g_reg_slots[i] = (cwist_reg_entry_t){.ptr = ptr, .owner = gc};
        g_reg_count++;
    }
    pthread_mutex_unlock(&g_reg_lock);

    /* Best-effort, matching cwist_gc_scope_track(): when the registry
     * itself cannot record the pointer, leave it untracked rather than
     * register it with ttak, whose destroy-time free would abort on a
     * headerless cwist_alloc block. */
    if (!tracked) return;
    ttak_epoch_gc_register(&gc->impl, ptr, size);
}

/**
 * @brief Expose the underlying libttak epoch GC structure for advanced integrations.
 *
 * Use this to register pointers that carry a libttak block header
 * (ttak_mem_alloc_safe() et al.) via ttak_epoch_gc_register(): such blocks
 * keep libttak's native teardown semantics and are released by
 * ttak_mem_free() at destroy. cwist_reg_ptr() is the right API for
 * cwist_alloc() blocks instead.
 *
 * @param gc GC wrapper owned by CWIST.
 * @return Raw libttak GC handle, or NULL when the wrapper is unavailable.
 */
ttak_epoch_gc_t *cwist_gc_raw(cwist_gc_t *gc) {
    if (!gc || !gc->initialized) return NULL;
    return &gc->impl;
}

/**
 * @brief Allocate a block that will later be released through cwist_ebr_free().
 * @param size Number of bytes to allocate.
 * @return Newly allocated block, or NULL on failure.
 */
void *cwist_ebr_alloc(size_t size) {
    /* Nothing to protect yet: a block nobody else can see does not need
     * epoch coverage until it is published to another thread. */
    return cwist_alloc(size);
}

/**
 * @brief Callback libttak invokes once it is safe to actually free @p ptr.
 * @param ptr Block previously retired via cwist_ebr_free().
 */
static void cwist_ebr_free_cb(void *ptr) {
    cwist_free(ptr);
}

/**
 * @brief Defer release of a block until every thread that might still be
 *        reading it (inside ttak_epoch_enter()/ttak_epoch_exit()) has left.
 * @param ptr Block to retire; a no-op when NULL.
 */
void cwist_ebr_free(void *ptr) {
    if (!ptr) return;
    ttak_epoch_retire(ptr, cwist_ebr_free_cb);
}

/**
 * @brief Advance the EBR epoch and run any callbacks that are now safe.
 */
void cwist_gc_pipeline_tick(void) {
    ttak_epoch_reclaim();
}

/**
 * @brief Reset a release guard to its unclaimed state.
 * @param guard Guard to initialize.
 */
void cwist_release_guard_init(cwist_release_guard_t *guard) {
    if (!guard) return;
    atomic_store_explicit(guard, false, memory_order_relaxed);
}

/**
 * @brief Claim a release guard.
 * @param guard Guard shared by every racing caller.
 * @return true for exactly one caller across all racers; false otherwise.
 */
bool cwist_release_guard_acquire(cwist_release_guard_t *guard) {
    if (!guard) return false;
    bool expected = false;
    return atomic_compare_exchange_strong_explicit(guard, &expected, true, memory_order_acq_rel,
                                                   memory_order_relaxed);
}

/* --- Full-GC mode: process-wide toggle + per-thread pending-sweep list --- */

/**
 * @brief Hardened backing storage for the full-GC toggle.
 *
 * Two attacks this defends against, since flipping this flag mid-run
 * silently disables the EBR deferred-free safety net full-GC mode
 * provides (opening a use-after-free window for anything it was
 * protecting): (1) a later call to cwist_full_gc() -- an accidental
 * re-invocation, or an attacker who has gained the ability to call
 * arbitrary exported functions -- and (2) a raw arbitrary-write
 * primitive that overwrites this flag's memory directly, bypassing the
 * API entirely.
 *
 * Defense: the struct lives alone on its own mmap()'d page. cwist_full_gc()
 * claims the right to set it exactly once via an atomic CAS on `locked`
 * (defeats (1): every later call, benign or hostile, is a silent no-op);
 * once set, the page is mprotect()'d PROT_READ (defeats (2): any write
 * that bypasses the CAS check and lands on this memory directly hard-faults
 * instead of silently succeeding).
 */
typedef struct {
    _Atomic bool enabled;
    _Atomic bool locked;
} cwist_full_gc_guard_t;

/**
 * @brief The guard page itself, or NULL if the mmap() below failed
 * (treated as full-GC being permanently unavailable, never as a security
 * regression -- see cwist_full_gc_enabled()).
 */
static cwist_full_gc_guard_t *g_full_gc_guard = NULL;

/**
 * @brief Map the guard page before main() runs, so cwist_full_gc_enabled()
 * -- called on every cwist_alloc()/cwist_free() -- never needs a
 * pthread_once/lazy-init check on its hot path (that check itself
 * regressed the C1M reactor latency gate in CI once already).
 */
__attribute__((constructor)) static void cwist_full_gc_guard_init(void) {
#if defined(__EMSCRIPTEN__) || defined(__wasi__)
    static cwist_full_gc_guard_t s_wasm_guard;
    atomic_init(&s_wasm_guard.enabled, false);
    atomic_init(&s_wasm_guard.locked, false);
    g_full_gc_guard = &s_wasm_guard;
#else
    long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) page_size = 4096;
    void *page =
        mmap(NULL, (size_t)page_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (page == MAP_FAILED) return;
    cwist_full_gc_guard_t *guard = (cwist_full_gc_guard_t *)page;
    atomic_init(&guard->enabled, false);
    atomic_init(&guard->locked, false);
    g_full_gc_guard = guard;
#endif
}

/** @brief Process-wide GC instance backing full-GC's epoch-retire pipeline. */
static cwist_gc_t g_full_gc;
static pthread_once_t g_full_gc_once = PTHREAD_ONCE_INIT;

/** @brief pthread_once body: initialize the full-GC instance in manual-rotation mode. */
static void cwist_full_gc_lazy_init(void) {
    cwist_gc(&g_full_gc, true);
}

/**
 * @brief Lazily initialized process-wide GC instance used by full-GC mode.
 * @return Shared full-GC context, initialized on first call.
 */
static cwist_gc_t *cwist_full_gc_instance(void) {
    pthread_once(&g_full_gc_once, cwist_full_gc_lazy_init);
    return &g_full_gc;
}

/**
 * @brief Serializes claiming the "first caller" slot in cwist_full_gc().
 *
 * NOT a CAS on g_full_gc_guard->locked: a CAS is a hardware read-modify-
 * WRITE (x86 cmpxchg asserts write intent even when the comparison
 * fails), so trying one on the guard page after it's been mprotect()'d
 * PROT_READ faults immediately -- on every later caller, exactly the
 * "harmless no-op" case this is supposed to handle gracefully. This
 * mutex lives off the guarded page, so contending for it is always safe;
 * only the thread that wins it ever writes to the page, and only before
 * that same thread mprotect()s it.
 */
static pthread_mutex_t g_full_gc_claim_mu = PTHREAD_MUTEX_INITIALIZER;

/**
 * @brief Claim full-GC mode permanently, exactly once, then harden the toggle.
 *
 * The first caller stores @p enable into the guard page, marks the page
 * locked, and mprotect()s it read-only; every later call is a silent no-op.
 * Fails safe (no-op) when the guard page was never mapped. Also toggles
 * auto-rotation on the process-wide full-GC instance.
 *
 * @param enable true to enable full-GC mode, false to claim-and-disable.
 */
void cwist_full_gc(bool enable) {
    if (!g_full_gc_guard) return; /* guard page unavailable; fail safe, not silently unhardened */

    /* Fast path: once locked, this plain load is the only thing every
     * later caller ever does -- safe indefinitely, even after the page
     * below becomes read-only. */
    if (atomic_load_explicit(&g_full_gc_guard->locked, memory_order_acquire)) return;

    pthread_mutex_lock(&g_full_gc_claim_mu);
    if (atomic_load_explicit(&g_full_gc_guard->locked, memory_order_acquire)) {
        /* Someone else claimed it while we were waiting for the mutex. */
        pthread_mutex_unlock(&g_full_gc_claim_mu);
        return;
    }

    /* We hold the mutex and locked is still false: nobody has mprotect()'d
     * the page yet, so it is still writable and we are its sole writer. */
    atomic_store_explicit(&g_full_gc_guard->enabled, enable, memory_order_relaxed);
    atomic_store_explicit(&g_full_gc_guard->locked, true, memory_order_release);
#if !defined(__EMSCRIPTEN__) && !defined(__wasi__)
    long page_size = sysconf(_SC_PAGESIZE);
    mprotect(g_full_gc_guard, (size_t)(page_size > 0 ? page_size : 4096), PROT_READ);
#endif
    pthread_mutex_unlock(&g_full_gc_claim_mu);

    cwist_gc_auto_rotate(cwist_full_gc_instance(), enable);
}

/**
 * @brief Query whether full-GC mode was enabled by the (single) cwist_full_gc() claim.
 * @return Current enabled state; false when the guard page was never mapped.
 */
bool cwist_full_gc_enabled(void) {
    if (!g_full_gc_guard) return false;
    return atomic_load_explicit(&g_full_gc_guard->enabled, memory_order_relaxed);
}

/**
 * @brief Query whether the full-GC toggle has already been claimed and locked.
 * @return true once cwist_full_gc() has run its claiming path exactly once.
 */
bool cwist_full_gc_locked(void) {
    if (!g_full_gc_guard) return false;
    return atomic_load_explicit(&g_full_gc_guard->locked, memory_order_acquire);
}

/**
 * @brief Expose the hardened full-GC guard page for introspection/tests.
 * @return Pointer to the guard page, or NULL if mmap() failed at startup.
 */
void *cwist_full_gc_guard_page(void) {
    return g_full_gc_guard;
}

/**
 * @brief One thread's set of cwist_alloc() blocks not yet cwist_free()'d.
 *
 * An open-addressing hash set keyed by the block pointer: linear probing,
 * a power-of-two capacity kept at most half full, and backward-shift
 * deletion, so there are no tombstones and a lookup stops at the first
 * empty slot. With full-GC on, cwist_free() untracks on every free,
 * including frees of blocks that were never tracked (cwist_strdup(),
 * cwist_realloc(), cJSON's hook allocations). A list had to be scanned end
 * to end for every one of those, so each free cost time proportional to
 * the number of blocks the thread still held; here a hit and a miss are
 * both O(1) on average.
 *
 * The table itself uses the raw allocator: it is the tracker's own
 * bookkeeping, and allocating it through cwist_alloc() would recurse into
 * cwist_gc_scope_track().
 */
typedef struct {
    void **slots;
    size_t cap; /* 0 or a power of two */
    size_t count;
} cwist_gc_pending_t;

/** Initial table size. */
#define CWIST_GC_PENDING_MIN_CAP 16
/** A flush keeps a table up to this size for reuse and frees larger ones,
 *  so one burst of allocations does not pin a big table to the thread. */
#define CWIST_GC_PENDING_KEEP_CAP 1024

/**
 * @brief Home slot for @p ptr. Fibonacci hashing of the address without
 *        its low bits, which malloc alignment keeps constant.
 */
static inline size_t cwist_gc_pending_slot(const void *ptr, size_t mask) {
    uint64_t h = (uint64_t)((uintptr_t)ptr >> 4) * UINT64_C(0x9E3779B97F4A7C15);
    return (size_t)(h >> 32) & mask;
}

/** @brief Double the table (or create it). false on allocation failure. */
static bool cwist_gc_pending_grow(cwist_gc_pending_t *pending) {
    size_t new_cap = pending->cap ? pending->cap * 2 : CWIST_GC_PENDING_MIN_CAP;
    void **slots = (void **)calloc(new_cap, sizeof(void *));
    if (!slots) return false;
    size_t mask = new_cap - 1;
    for (size_t i = 0; i < pending->cap; i++) {
        void *key = pending->slots[i];
        if (!key) continue;
        size_t j = cwist_gc_pending_slot(key, mask);
        while (slots[j]) j = (j + 1) & mask;
        slots[j] = key;
    }
    free(pending->slots);
    pending->slots = slots;
    pending->cap = new_cap;
    return true;
}

/**
 * @brief Retire every pending block and reset the set to empty.
 *
 * The table is detached before anything is retired: cwist_ebr_free() may
 * run reclaim callbacks that end in cwist_free() -> cwist_gc_scope_untrack()
 * on this same thread, and those must see an empty set rather than the one
 * being walked.
 *
 * @param pending Set to drain; a no-op when NULL.
 */
static void cwist_gc_pending_flush(cwist_gc_pending_t *pending) {
    if (!pending) return;
    void **slots = pending->slots;
    size_t cap = pending->cap;
    size_t count = pending->count;
    if (count == 0 && cap <= CWIST_GC_PENDING_KEEP_CAP) return;

    pending->slots = NULL;
    pending->cap = 0;
    pending->count = 0;
    size_t retired = 0;
    for (size_t i = 0; i < cap && count > 0; i++) {
        if (!slots[i]) continue;
        cwist_ebr_free(slots[i]);
        count--;
        retired++;
    }
    if (cap <= CWIST_GC_PENDING_KEEP_CAP && !pending->slots) {
        memset(slots, 0, cap * sizeof(void *));
        pending->slots = slots;
        pending->cap = cap;
    } else {
        free(slots);
    }
    /* Drive reclaim here: the full-GC instance rotates manually, so nothing
     * else frees what was just retired unless the app happens to run an
     * io_queue (#347). Every retired block was tracked by this thread and,
     * by the cwist_gc_scope_flush() contract, is no longer in use, so
     * freeing it once the grace period has passed is safe.
     * Skipped when nothing was retired, so an io_queue job that freed
     * everything itself pays nothing. */
    if (retired > 0) {
        /* libttak keeps a retired block for TTAK_EPOCH_GRACE epochs, and
         * each reclaim advances at most one: with no reader inside an epoch
         * this many calls free what was just retired. */
        for (unsigned i = 0; i < TTAK_EPOCH_GRACE; i++) ttak_epoch_reclaim();
    }
}

#if (defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L)
#define CWIST_GC_PENDING_TLS 1
static _Thread_local cwist_gc_pending_t *t_gc_pending = NULL;
#elif defined(__GNUC__) || defined(__clang__)
#define CWIST_GC_PENDING_TLS 1
static __thread cwist_gc_pending_t *t_gc_pending = NULL;
#endif

/**
 * @brief pthread TSD destructor: flush and free a thread's pending set on exit.
 * @param arg Thread-local cwist_gc_pending_t allocated by cwist_gc_pending_get().
 */
static void cwist_gc_pending_destroy(void *arg) {
    cwist_gc_pending_t *pending = (cwist_gc_pending_t *)arg;
    if (!pending) return;
#ifdef CWIST_GC_PENDING_TLS
    /* Destructors run on the exiting thread. Drop the cached pointer first,
     * so an allocation made by a later destructor creates a fresh set (and
     * re-registers it) instead of touching this one after it is freed. */
    t_gc_pending = NULL;
#endif
    cwist_gc_pending_flush(pending);
    free(pending->slots);
    free(pending);
}

static pthread_key_t g_pending_key;
static pthread_once_t g_pending_key_once = PTHREAD_ONCE_INIT;

static void cwist_gc_pending_key_init(void) {
    pthread_key_create(&g_pending_key, cwist_gc_pending_destroy);
}

/**
 * @brief Lazily create (or return) this thread's pending set.
 *
 * Hot path: one thread-local load. pthread_once() and pthread_getspecific()
 * only run the first time a thread tracks something; the pthread key is
 * still what runs the exit-time sweep.
 */
static cwist_gc_pending_t *cwist_gc_pending_get(void) {
#ifdef CWIST_GC_PENDING_TLS
    if (t_gc_pending) return t_gc_pending;
#endif
    pthread_once(&g_pending_key_once, cwist_gc_pending_key_init);
    cwist_gc_pending_t *pending = (cwist_gc_pending_t *)pthread_getspecific(g_pending_key);
    if (!pending) {
        pending = (cwist_gc_pending_t *)calloc(1, sizeof(*pending));
        if (pending) pthread_setspecific(g_pending_key, pending);
    }
#ifdef CWIST_GC_PENDING_TLS
    t_gc_pending = pending;
#endif
    return pending;
}

/**
 * @brief Track a live cwist_alloc() block so full-GC can retire it later.
 *
 * Best-effort: on allocation failure the block is left untracked rather
 * than failing the caller's allocation. Duplicate pointers are stored once.
 *
 * @param ptr Block to track; a no-op when NULL.
 */
void cwist_gc_scope_track(void *ptr) {
    if (!ptr) return;
    cwist_gc_pending_t *pending = cwist_gc_pending_get();
    if (!pending) return;
    /* Best-effort, as before: if the table cannot grow, leave ptr untracked
     * rather than fail the allocation. */
    if ((pending->count + 1) * 2 > pending->cap && !cwist_gc_pending_grow(pending)) return;
    size_t mask = pending->cap - 1;
    size_t i = cwist_gc_pending_slot(ptr, mask);
    while (pending->slots[i]) {
        if (pending->slots[i] == ptr) return; /* already tracked: keep one entry */
        i = (i + 1) & mask;
    }
    pending->slots[i] = ptr;
    pending->count++;
}

/**
 * @brief Stop tracking a block without retiring it.
 *
 * Used when the caller hands ownership elsewhere (e.g. frees through a
 * different path); the block is removed from this thread's pending set
 * and will not be epoch-retired by a later flush.
 *
 * @param ptr Block to untrack.
 * @return true when @p ptr was tracked and removed; false otherwise.
 */
bool cwist_gc_scope_untrack(void *ptr) {
    if (!ptr) return false;
    cwist_gc_pending_t *pending = cwist_gc_pending_get();
    if (!pending || pending->count == 0) return false;
    size_t mask = pending->cap - 1;
    size_t i = cwist_gc_pending_slot(ptr, mask);
    while (pending->slots[i] != ptr) {
        if (!pending->slots[i]) return false;
        i = (i + 1) & mask;
    }

    /* Backward-shift deletion: pull each following entry of the probe run
     * back into the hole unless the hole lies before that entry's home
     * slot, so every remaining key stays reachable without tombstones. */
    size_t hole = i;
    for (size_t j = (i + 1) & mask; pending->slots[j]; j = (j + 1) & mask) {
        size_t home = cwist_gc_pending_slot(pending->slots[j], mask);
        if (((j - home) & mask) >= ((j - hole) & mask)) {
            pending->slots[hole] = pending->slots[j];
            hole = j;
        }
    }
    pending->slots[hole] = NULL;
    pending->count--;
    return true;
}

/**
 * @brief Alias of cwist_gc_scope_untrack(): drop tracking without retiring.
 * @param ptr Block to untrack.
 * @return true when @p ptr was tracked and removed; false otherwise.
 */
bool cwist_gc_scope_disown(void *ptr) {
    return cwist_gc_scope_untrack(ptr);
}

/**
 * @brief Retire every pending block this thread is still tracking.
 *
 * Each tracked block is handed to cwist_ebr_free(), so reclamation is
 * deferred until no thread can still be reading inside an epoch.
 */
void cwist_gc_scope_flush(void) {
    cwist_gc_pending_flush(cwist_gc_pending_get());
}

/**
 * @brief Count this thread's not-yet-retired tracked blocks.
 * @return Number of pending entries; 0 when nothing is tracked.
 */
size_t cwist_gc_scope_pending_count(void) {
    cwist_gc_pending_t *pending = cwist_gc_pending_get();
    return pending ? pending->count : 0;
}

/* --- Connection registry: thread/process-exit sweep for non-memory
 * resources (sockets, TLS sessions, protocol state). See gc.h's
 * cwist_conn_registry_* doc comments for the contract.
 *
 * One process-wide table maps each tracked handle to its close_fn and its
 * owner, the thread whose exit sweep closes it (#345). The owner starts as
 * the tracking thread and moves with cwist_conn_registry_adopt() when
 * another thread takes over serving the connection, so a thread that exits
 * no longer closes connections other threads are still using. Untrack is a
 * hash delete instead of a walk over every thread's list, and close_fn
 * always runs with no registry lock held, so a close_fn may track or
 * untrack other handles.
 *
 * The table, batches and owner tokens use the raw allocator: they are the
 * GC's own bookkeeping and must never sit on a full-GC pending list. */

/** Per-thread owner token; its address identifies the owning thread. */
typedef struct cwist_conn_owner {
    char unused;
} cwist_conn_owner_t;

typedef struct {
    void *handle; /* NULL marks an empty slot */
    cwist_conn_close_fn close_fn;
    cwist_conn_owner_t *owner;
} cwist_conn_entry_t;

static pthread_key_t g_conn_key;
static pthread_once_t g_conn_key_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t g_conn_registry_lock = PTHREAD_MUTEX_INITIALIZER;
static cwist_conn_entry_t *g_conn_slots = NULL; /* open addressing, power-of-two cap */
static size_t g_conn_cap = 0;
static size_t g_conn_count = 0;
static _Atomic bool g_conn_hooks_registered = false;

static size_t cwist_conn_slot(const void *handle, size_t mask) {
    uintptr_t x = (uintptr_t)handle;
    x ^= x >> 33;
    x *= (uintptr_t)0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    return (size_t)x & mask;
}

/** @brief Index of @p handle, or SIZE_MAX. Caller holds the lock. */
static size_t cwist_conn_find_locked(const void *handle) {
    if (g_conn_count == 0) return SIZE_MAX;
    size_t mask = g_conn_cap - 1;
    for (size_t i = cwist_conn_slot(handle, mask);; i = (i + 1) & mask) {
        if (g_conn_slots[i].handle == handle) return i;
        if (!g_conn_slots[i].handle) return SIZE_MAX;
    }
}

/** @brief Backward-shift delete of slot @p i. Caller holds the lock. */
static void cwist_conn_remove_at_locked(size_t i) {
    size_t mask = g_conn_cap - 1;
    size_t hole = i;
    for (size_t j = (i + 1) & mask; g_conn_slots[j].handle; j = (j + 1) & mask) {
        size_t home = cwist_conn_slot(g_conn_slots[j].handle, mask);
        if (((j - home) & mask) >= ((j - hole) & mask)) {
            g_conn_slots[hole] = g_conn_slots[j];
            hole = j;
        }
    }
    g_conn_slots[hole] = (cwist_conn_entry_t){0};
    g_conn_count--;
}

/** @brief Grow so one more entry fits at <= 50% load. Caller holds the lock. */
static bool cwist_conn_reserve_locked(void) {
    if ((g_conn_count + 1) * 2 <= g_conn_cap) return true;
    size_t new_cap = g_conn_cap ? g_conn_cap * 2 : 64;
    cwist_conn_entry_t *slots = (cwist_conn_entry_t *)calloc(new_cap, sizeof(*slots));
    if (!slots) return false;
    size_t mask = new_cap - 1;
    for (size_t i = 0; i < g_conn_cap; i++) {
        if (!g_conn_slots[i].handle) continue;
        size_t j = cwist_conn_slot(g_conn_slots[i].handle, mask);
        while (slots[j].handle) j = (j + 1) & mask;
        slots[j] = g_conn_slots[i];
    }
    free(g_conn_slots);
    g_conn_slots = slots;
    g_conn_cap = new_cap;
    return true;
}

/**
 * @brief Remove every entry owned by @p owner (every entry when NULL) and
 *        close each one with no lock held.
 *
 * Entries are moved out in batches under the lock, then closed after it is
 * dropped. If the batch buffer cannot be allocated, entries are taken one
 * at a time instead (slower, but nothing is left open).
 */
static void cwist_conn_close_owned(cwist_conn_owner_t *owner) {
    for (;;) {
        pthread_mutex_lock(&g_conn_registry_lock);
        size_t want = 0;
        for (size_t i = 0; i < g_conn_cap; i++) {
            if (g_conn_slots[i].handle && (!owner || g_conn_slots[i].owner == owner)) want++;
        }
        if (want == 0) {
            pthread_mutex_unlock(&g_conn_registry_lock);
            return;
        }
        cwist_conn_entry_t one;
        cwist_conn_entry_t *heap = (cwist_conn_entry_t *)calloc(want, sizeof(*heap));
        cwist_conn_entry_t *batch = heap ? heap : &one;
        size_t room = heap ? want : 1;
        size_t n = 0;
        for (size_t i = 0; i < g_conn_cap && n < room;) {
            if (g_conn_slots[i].handle && (!owner || g_conn_slots[i].owner == owner)) {
                batch[n++] = g_conn_slots[i];
                cwist_conn_remove_at_locked(i); /* may shift a later entry into i */
            } else {
                i++;
            }
        }
        pthread_mutex_unlock(&g_conn_registry_lock);
        for (size_t k = 0; k < n; k++) batch[k].close_fn(batch[k].handle);
        free(heap);
    }
}

static void cwist_conn_registry_atexit(void) {
    cwist_conn_close_owned(NULL);
}

/* fork(): the child inherits the parent's table, but the connections are
 * the parent's. A child that later runs exit() would otherwise shut down
 * TLS sessions on sockets the parent is still serving. The lock is held
 * across fork so the child never inherits it mid-update. */
static void cwist_conn_atfork_prepare(void) {
    pthread_mutex_lock(&g_conn_registry_lock);
}

static void cwist_conn_atfork_parent(void) {
    pthread_mutex_unlock(&g_conn_registry_lock);
}

static void cwist_conn_atfork_child(void) {
    if (g_conn_slots) memset(g_conn_slots, 0, g_conn_cap * sizeof(*g_conn_slots));
    g_conn_count = 0;
    pthread_mutex_unlock(&g_conn_registry_lock);
}

/** @brief pthread TLS destructor: close what this thread still owns. */
static void cwist_conn_owner_destroy(void *arg) {
    cwist_conn_owner_t *owner = (cwist_conn_owner_t *)arg;
    if (!owner) return;
    cwist_conn_close_owned(owner);
    free(owner);
}

static void cwist_conn_key_init(void) {
    pthread_key_create(&g_conn_key, cwist_conn_owner_destroy);
}

/** @brief This thread's owner token, created on first use. */
static cwist_conn_owner_t *cwist_conn_owner_get(bool create) {
    pthread_once(&g_conn_key_once, cwist_conn_key_init);
    cwist_conn_owner_t *owner = (cwist_conn_owner_t *)pthread_getspecific(g_conn_key);
    if (owner || !create) return owner;

    owner = (cwist_conn_owner_t *)calloc(1, sizeof(*owner));
    if (!owner) return NULL;
    if (pthread_setspecific(g_conn_key, owner) != 0) {
        free(owner);
        return NULL;
    }

    /* Register the process-exit sweep and the fork handler exactly once,
     * the first time any thread tracks a connection (not eagerly at load
     * time: most processes never enable full-GC). */
    if (!atomic_exchange_explicit(&g_conn_hooks_registered, true, memory_order_acq_rel)) {
        atexit(cwist_conn_registry_atexit);
        pthread_atfork(cwist_conn_atfork_prepare, cwist_conn_atfork_parent, cwist_conn_atfork_child);
    }
    return owner;
}

/**
 * @brief Track a non-memory resource for the full-GC exit sweep.
 *
 * No-op unless full-GC mode is enabled. Best-effort: if the table cannot
 * grow, the handle is dropped rather than failing the caller. Tracking a
 * handle that is already tracked updates its close_fn and makes the caller
 * its owner.
 *
 * @param handle Resource handle to close later (e.g. socket, TLS session).
 * @param close_fn Callback invoked with @p handle during a sweep or flush.
 */
void cwist_conn_registry_track(void *handle, cwist_conn_close_fn close_fn) {
    if (!handle || !close_fn) return;
    if (!cwist_full_gc_enabled()) return;
    cwist_conn_owner_t *owner = cwist_conn_owner_get(true);
    if (!owner) return;

    pthread_mutex_lock(&g_conn_registry_lock);
    size_t i = cwist_conn_find_locked(handle);
    if (i != SIZE_MAX) {
        g_conn_slots[i].close_fn = close_fn;
        g_conn_slots[i].owner = owner;
    } else if (cwist_conn_reserve_locked()) {
        size_t mask = g_conn_cap - 1;
        size_t j = cwist_conn_slot(handle, mask);
        while (g_conn_slots[j].handle) j = (j + 1) & mask;
        g_conn_slots[j] = (cwist_conn_entry_t){.handle = handle, .close_fn = close_fn, .owner = owner};
        g_conn_count++;
    }
    pthread_mutex_unlock(&g_conn_registry_lock);
}

/**
 * @brief Remove a handle from the registry, whichever thread owns it.
 * @param handle Handle previously passed to cwist_conn_registry_track().
 * @return true when found and removed; false otherwise.
 */
bool cwist_conn_registry_untrack(void *handle) {
    if (!handle) return false;
    pthread_mutex_lock(&g_conn_registry_lock);
    size_t i = cwist_conn_find_locked(handle);
    if (i != SIZE_MAX) cwist_conn_remove_at_locked(i);
    pthread_mutex_unlock(&g_conn_registry_lock);
    return i != SIZE_MAX;
}

/**
 * @brief Make the calling thread the owner of a tracked handle.
 * @param handle Handle previously passed to cwist_conn_registry_track().
 * @return true when @p handle is tracked (and now owned by the caller);
 *         false when it is not tracked or full GC is off.
 */
bool cwist_conn_registry_adopt(void *handle) {
    if (!handle || !cwist_full_gc_enabled()) return false;
    cwist_conn_owner_t *owner = cwist_conn_owner_get(true);
    if (!owner) return false;
    pthread_mutex_lock(&g_conn_registry_lock);
    size_t i = cwist_conn_find_locked(handle);
    if (i != SIZE_MAX) g_conn_slots[i].owner = owner;
    pthread_mutex_unlock(&g_conn_registry_lock);
    return i != SIZE_MAX;
}

/**
 * @brief Close every handle the calling thread owns, right now.
 *
 * The registry stays usable afterwards; later tracks work as before.
 */
void cwist_conn_registry_flush(void) {
    cwist_conn_owner_t *owner = cwist_conn_owner_get(false);
    if (owner) cwist_conn_close_owned(owner);
}

/**
 * @brief Close every tracked handle, whichever thread owns it.
 *
 * Intended for process-exit paths. Each handle is removed before it is
 * closed, so racing sweeps (atexit vs. a thread's own exit) close it once.
 */
void cwist_conn_registry_sweep_all(void) {
    cwist_conn_close_owned(NULL);
}

/**
 * @brief Count the tracked handles the calling thread owns.
 * @return Number of owned entries; 0 when nothing is tracked.
 */
size_t cwist_conn_registry_pending_count(void) {
    cwist_conn_owner_t *owner = cwist_conn_owner_get(false);
    if (!owner) return 0;
    size_t n = 0;
    pthread_mutex_lock(&g_conn_registry_lock);
    for (size_t i = 0; i < g_conn_cap; i++) {
        if (g_conn_slots[i].handle && g_conn_slots[i].owner == owner) n++;
    }
    pthread_mutex_unlock(&g_conn_registry_lock);
    return n;
}
