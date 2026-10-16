/**
 * @file gc.h
 * @brief Epoch-based garbage collection wrapper.
 */

#ifndef __CWIST_CORE_MEM_GC_H__
#define __CWIST_CORE_MEM_GC_H__

#include <stdbool.h>
#include <stddef.h>
#include <ttak/compat/stdatomic.h>
#include <ttak/mem/epoch_gc.h>

/**
 * @brief High-level wrapper over libttak's epoch GC.
 *
 * These helpers keep initialization/destruction scoped to CWIST so
 * applications can toggle GC behavior without touching libttak internals.
 */
typedef struct cwist_gc {
    ttak_epoch_gc_t impl;
    /// Check if CWIST GC has initialized
    bool initialized;
    /// Check if GC should scan and reclaim objects
    _Atomic bool auto_rotated;
} cwist_gc_t;

/**
 * @brief Initialize (if needed) and configure the GC context.
 *
 * @param gc Target context.
 * @param manual_rotation When true, force manual rotations instead of the background thread.
 */
void cwist_gc(cwist_gc_t *gc, bool manual_rotation);

/**
 * @brief Shut down and free internal GC state.
 *
 * Releases every block still registered through cwist_reg_ptr() /
 * cwist_reg_ptr_sized() with cwist_free(); blocks registered directly with
 * ttak via cwist_gc_raw() are released by libttak instead.
 */
void cwist_gc_shutdown(cwist_gc_t *gc);

/**
 * @brief Rotate the GC epoch and reclaim retired nodes.
 */
void cwist_gc_rotate(cwist_gc_t *gc);

/**
 * @brief Toggle whether the GC context reclaims automatically.
 *
 * @param gc Target context.
 * @param enabled When true, mark the context as auto-rotated (full-GC mode).
 */
void cwist_gc_auto_rotate(cwist_gc_t *gc, bool enabled);

/**
 * @brief Check whether the GC context is currently set to auto-rotate.
 */
bool cwist_gc_auto_rotated(cwist_gc_t *gc);

/**
 * @brief Register a pointer without specifying a size (defaults to 0).
 *
 * Hands ownership of the block to the context: cwist_gc_shutdown() releases
 * it with cwist_free(), and an explicit cwist_free() detaches it first.
 * The pointer must come from CWIST's allocator (cwist_alloc() and family);
 * see cwist_reg_ptr_sized() for the full contract.
 */
void cwist_reg_ptr(cwist_gc_t *gc, void *ptr);

/**
 * @brief Register a pointer and its size for better accounting.
 *
 * The pointer must be releasable with cwist_free() (i.e. come from CWIST's
 * allocator: cwist_alloc() and family). The wrapper keeps its own registry
 * of such pointers so that cwist_gc_shutdown() can detach them from
 * libttak's mem_tree and release them with cwist_free(); libttak's own
 * destroy-time pass frees with ttak_mem_free() and aborts on a cwist_alloc
 * block, which carries no ttak block header. Pointers allocated directly
 * from libttak (ttak_mem_alloc_safe()) keep libttak's teardown semantics
 * instead: register them through cwist_gc_raw() with
 * ttak_epoch_gc_register().
 *
 * After registration the context owns the block. An explicit cwist_free()
 * detaches it automatically (staying correct, just unregistering early),
 * and registering the same pointer with another context moves tracking
 * there.
 */
void cwist_reg_ptr_sized(cwist_gc_t *gc, void *ptr, size_t size);

/**
 * @brief Obtain the raw libttak epoch GC pointer (advanced usage).
 *
 * Intended for registering pointers that carry a libttak block header
 * (ttak_mem_alloc_safe() et al.) via ttak_epoch_gc_register(): such blocks
 * are released by ttak_mem_free() when the context is destroyed. Use
 * cwist_reg_ptr() for cwist_alloc() blocks instead.
 */
ttak_epoch_gc_t *cwist_gc_raw(cwist_gc_t *gc);

/**
 * @brief Allocate a block meant to be released through cwist_ebr_free().
 *
 * Identical to cwist_alloc() today: a freshly allocated block is not yet
 * shared, so nothing needs epoch protection until it is published to other
 * threads. The distinct name exists to mark call sites that pair with
 * cwist_ebr_free() instead of cwist_free().
 *
 * @param size Number of bytes to allocate.
 * @return Newly allocated block, or NULL on failure.
 */
void *cwist_ebr_alloc(size_t size);

/**
 * @brief Defer release of a block shared across threads via epoch reclamation.
 *
 * Unlike cwist_free(), this does not release @p ptr immediately. It hands
 * the pointer to libttak's EBR (ttak_epoch_retire) so the actual free only
 * runs once every thread that might still be inside a ttak_epoch_enter() /
 * ttak_epoch_exit() critical section touching @p ptr has left it.
 *
 * Callers on the reading side MUST wrap their access to the shared pointer
 * in ttak_epoch_enter()/ttak_epoch_exit() for this guarantee to hold; this
 * function alone does not protect readers that skip that discipline.
 *
 * The actual free only happens once something calls ttak_epoch_reclaim()
 * (see cwist_gc_pipeline_tick()) and observes it is safe.
 *
 * @param ptr Block previously returned by cwist_ebr_alloc() (or cwist_alloc()).
 */
void cwist_ebr_free(void *ptr);

/**
 * @brief Drive the EBR pipeline: attempt to advance the epoch and run any
 *        cwist_ebr_free() callbacks that are now safe to execute.
 *
 * Call this periodically (e.g. once per event-loop tick) from a single
 * well-known place; nothing retired via cwist_ebr_free() is ever actually
 * released unless this runs.
 */
void cwist_gc_pipeline_tick(void);

/**
 * @brief Enable or disable full-GC mode process-wide.
 *
 * Hardened, boot-time-only setter: the first call wins and is latched in;
 * every later call -- from any thread, deliberate re-invocation or
 * otherwise -- is a silent no-op, and the backing storage is mprotect()'d
 * read-only after that first call so a raw memory-corruption write can't
 * flip it either. See gc.c's cwist_full_gc_guard_t for the full threat
 * model this defends against.
 *
 * @param enable When true, cwist_alloc() starts registering blocks with the
 *               per-thread pending-sweep list and cwist_free() starts
 *               unregistering them; a worker thread that exits without
 *               freeing everything it allocated has the remainder swept via
 *               cwist_ebr_free() automatically.
 */
void cwist_full_gc(bool enable);

/**
 * @brief Check whether full-GC mode is currently enabled.
 */
bool cwist_full_gc_enabled(void);

/**
 * @brief Check whether the full-GC toggle has been latched (i.e.
 *        cwist_full_gc() has been called once already and every further
 *        call will be ignored). Diagnostic/testing use.
 */
bool cwist_full_gc_locked(void);

/**
 * @brief Raw pointer to the full-GC toggle's guard page, or NULL if the
 *        mmap() backing it failed at process startup. Advanced/testing use
 *        only -- e.g. to confirm the page is actually read-only after
 *        cwist_full_gc() has been called once.
 */
void *cwist_full_gc_guard_page(void);

/**
 * @brief Register a cwist_alloc() block with the current thread's
 *        pending-sweep list (full-GC mode only).
 *
 * Called by cwist_alloc() itself; not meant to be called directly.
 */
void cwist_gc_scope_track(void *ptr);

/**
 * @brief Remove a block from the current thread's pending-sweep list.
 *
 * Called by cwist_free() itself before it releases @p ptr, so a block that
 * was freed explicitly is never swept a second time at thread exit.
 *
 * @return true if @p ptr was pending (and has been removed); false if it
 *         was never tracked (e.g. it came from cwist_strdup()/cwist_realloc()
 *         instead of cwist_alloc(), or full-GC mode was off at alloc time).
 *         Either way the caller should proceed to free @p ptr normally.
 */
bool cwist_gc_scope_untrack(void *ptr);

/**
 * @brief Remove @p ptr from the current thread's pending-sweep list without
 *        freeing it, because ownership is being handed off to other code
 *        that will call cwist_free() (or cwist_ebr_free()) on it later,
 *        possibly from a different thread.
 *
 * Without this call, a block allocated by one unit of work (e.g. a queued
 * job) and stored somewhere for a later unit of work to release would be
 * auto-swept as soon as the allocating unit of work finishes -- the sweep
 * logic cannot otherwise tell "nobody will ever free this" apart from
 * "someone else will free this later". Call cwist_gc_scope_disown() at the
 * point of handoff, before the pointer becomes visible to whatever will
 * eventually free it.
 *
 * This is exactly cwist_gc_scope_untrack() under a name that documents the
 * handoff use case; the two are interchangeable.
 *
 * @return true if @p ptr was pending and has been removed; false if it was
 *         never tracked (full-GC mode was off at alloc time, or it was
 *         already disowned/freed).
 */
bool cwist_gc_scope_disown(void *ptr);

/**
 * @brief Retire every block still on the current thread's pending-sweep
 *        list right now, instead of waiting for the thread to exit.
 *
 * "Every block" means every block, live or not: the flush cannot tell a
 * forgotten allocation from one that is still in use, so it retires both.
 *
 * **Contract.** Call this only at a point where the calling thread holds
 * no tracked block that anything will touch again:
 *  - OK: the end of a self-contained job on a dedicated worker thread, after
 *    everything the job hands on has been disowned (cwist_gc_scope_disown())
 *    or handed to a CWIST API that disowns it (e.g. cwist_async_respond_with()).
 *  - Not OK: inside a request handler or middleware (the response is still
 *    being built and sent after next() returns), or on any thread that serves
 *    connections (HTTP/2 session state and other per-connection objects live
 *    across requests on that thread).
 *
 * **Reclamation.** Retired blocks go to cwist_ebr_free(), and the flush
 * then drives reclamation itself (ttak_epoch_reclaim()), so they are freed
 * as soon as no thread is inside an older epoch, normally before this call
 * returns. The thread-exit sweep does the same. Request-serving threads
 * never enter an epoch, so reclaim treats a retired block as unreferenced
 * even if a thread is still using it: a block flushed while still in use is
 * freed while still in use. (Before #347 nothing drove reclaim unless the
 * app ran an io_queue, so misuse leaked instead of crashing.)
 *
 * Misuse is a use-after-free, so the `cwist` CLI
 * (`cwist audit`, `cwist watcher`) warns on every call site that is not
 * marked `cwist-ack: cwist_gc_scope_flush`. See docs/GC.md, "Explicit
 * flushes and reclamation", and c4punks/CWIST#347.
 */
void cwist_gc_scope_flush(void);

/**
 * @brief Number of blocks currently on the calling thread's pending-sweep
 *        list. Introspection/testing helper.
 */
size_t cwist_gc_scope_pending_count(void);

/**
 * @brief One-shot guard ensuring a shared teardown path runs exactly once.
 *
 * Use this when the same release path (e.g. detaching two sibling
 * references and freeing what they both point to) may be entered
 * concurrently from more than one thread, and exactly one of them must
 * win. Initialize with cwist_release_guard_init(); every racer then calls
 * cwist_release_guard_acquire() and only the caller that gets `true` back
 * performs the detach + free.
 */
typedef atomic_bool cwist_release_guard_t;

/**
 * @brief Initialize a release guard to its unclaimed state.
 */
void cwist_release_guard_init(cwist_release_guard_t *guard);

/**
 * @brief Attempt to claim the guard.
 * @return true exactly once across all racing callers; false to every
 *         other caller (including ones arriving after the winner finishes).
 */
bool cwist_release_guard_acquire(cwist_release_guard_t *guard);

/**
 * @brief Full-GC's connection registry: thread/process-exit sweep for
 * resources that aren't plain cwist_alloc() memory -- sockets, TLS
 * sessions, protocol-level state -- so a worker thread (or the whole
 * process) ending without an explicit destroy-family call still closes
 * them, instead of leaving them for the peer/OS to notice. See
 * docs/GC.md sections 2-3. There is no cwist_conn_t in this codebase
 * (connection lifecycle is protocol-specific -- http.c/http2.c/http3.c/
 * https.c/grpc.c/grpc_client.c each own their own), so this tracks
 * (handle, close_fn) pairs instead of a concrete type.
 */
typedef void (*cwist_conn_close_fn)(void *handle);

/**
 * @brief Register a connection handle for the full-GC exit sweep, owned by
 *        the calling thread (full-GC mode only; a no-op otherwise, so
 *        callers can call this unconditionally at connection-open time).
 *
 * The registry is one process-wide table. Each handle has an owner thread;
 * that thread's exit sweep (or cwist_conn_registry_flush()) closes it. The
 * owner is the tracking thread until cwist_conn_registry_adopt() moves it.
 *
 * @param handle Opaque connection handle, passed back to @p close_fn.
 * @param close_fn Called with @p handle to close/tear it down; must be
 *                  safe to call from whichever thread ends up sweeping it
 *                  (the owning thread at its own exit, or any thread
 *                  running the atexit sweep at process exit). It is called
 *                  with no registry lock held and may track or untrack
 *                  other handles.
 */
void cwist_conn_registry_track(void *handle, cwist_conn_close_fn close_fn);

/**
 * @brief Make the calling thread the owner of a tracked handle.
 *
 * Call this when a thread takes over serving a connection another thread
 * tracked (pool resubmit, parked-connection resume, async completion that
 * keeps the connection). Without it, the tracking thread's exit would
 * close a connection the new thread is still using (#345).
 *
 * @return true if @p handle is tracked and now owned by the caller; false
 *         if it is not tracked or full GC is off.
 */
bool cwist_conn_registry_adopt(void *handle);

/**
 * @brief Remove a handle from the registry before closing it yourself
 *        through the normal explicit path.
 *
 * Works from any thread, whichever thread owns the handle: a connection is
 * usually tracked on the thread that accepted it but closed on another one
 * (async completion, keep-alive resubmit, parked-connection expiry), and a
 * handle left behind would be closed a second time by a sweep (#344).
 * Untrack must still happen before the handle is freed, and only one party
 * may close it; coordinate racing closers yourself (e.g.
 * cwist_release_guard_t).
 *
 * @return true if @p handle was pending and has been removed; false if
 *         it was never tracked or already swept. Either way the caller
 *         should proceed to close it.
 */
bool cwist_conn_registry_untrack(void *handle);

/**
 * @brief Close every handle the calling thread owns right now, without
 *        waiting for thread exit. Analogous to cwist_gc_scope_flush() but
 *        for connections.
 *
 * Ownership follows cwist_conn_registry_adopt(), but a connection this
 * thread tracked and another thread serves without adopting it (a parked
 * or deferred connection) is still owned here, so call this only when none
 * of them can still be in use. The `cwist` CLI warns on call sites not
 * marked `cwist-ack: cwist_conn_registry_flush`.
 */
void cwist_conn_registry_flush(void);

/**
 * @brief Sweep every thread's pending-sweep list right now. This is what
 *        the process-exit (atexit) hook calls; exposed directly for
 *        testing, or for callers that want a deterministic sweep point
 *        earlier than actual process exit (e.g. cwist_app_destroy()).
 */
void cwist_conn_registry_sweep_all(void);

/**
 * @brief Number of tracked connections the calling thread owns.
 *        Introspection/testing helper.
 *
 * After fork() the child starts with an empty registry: the inherited
 * connections belong to the parent, and a child that exits must not shut
 * them down.
 */
size_t cwist_conn_registry_pending_count(void);

#endif
