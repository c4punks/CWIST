# Deferred HTTP ownership under full GC

Deferred exchanges have explicit destruction owners. A reference count does not
remove an allocation from CWIST's thread-local pending-sweep list:
`cwist_gc_scope_disown` must execute on the allocating thread before publication.
These changes do not disable GC, change the allocator, or register allocations
with the receiving job's scope.

## Handoff sequence

1. `cwist_async_defer` disowns the handle and the existing owned request/response
   graph before setting the response's async/deferred fields and returning it.
2. Synchronous middleware may finish its posthandler work on the dispatch thread.
   `cwist_async_dispatch_ack` walks the graph again, capturing allocations made
   while middleware unwound, refreshes keep-alive policy, then release-publishes
   the acknowledgement. The dispatcher must not touch the exchange afterwards.
3. The winning completion producer acquires that acknowledgement **before**
   mutating, replacing, sending or destroying any exchange object. Thus the
   acknowledgement graph walk cannot race response mutation. Competing losers
   do not walk or take ownership of a caller-supplied response.
4. `cwist_async_finish` disowns the final response's owned graph on the producer
   thread before posting to a reactor or entering the HTTP/2 queue. This covers
   `respond`, `respond_with`, `abort` and timeout-created headers/body strings.
5. Existing completion/destruction paths remain responsible for final release:
   final response when distinct, original response, then request. The ordering
   preserves response storage borrowed from the request arena. Passing the
   original response to `respond_with` does not cause duplicate destruction.

Completion on the dispatch thread claims and records a pending response without
sending or destroying it. After middleware unwinds, dispatch acknowledgement
executes that completion. This preserves an allocation-independent inline abort
when scheduling fails. Do not join or wait for a completing producer
from a handler or posthandler: a foreign winning producer waits for dispatch to
finish. Retained producer/timer reference rules remain unchanged.

## Ownership inventory

The private `src/net/http/async_gc.h` walkers mirror existing destructors:

| Owned resource | Transfer rule |
| --- | --- |
| Request/response shells | Disown when not contained in their arena, including heap fallback with a non-NULL arena |
| sstring storage | Disown the struct only when `owns_storage`; its data only when `!borrows_buffer` |
| Header lists | Walk each node's key/value independently; disown nodes only when `!arena_owned` |
| Query/path/flash maps | For heap maps, disown keys, values, chain nodes, bucket array and map; arena maps have no heap fallback and no individual destructor work |
| Session | Session module disowns the opaque session and its data map, not app/request backreferences |
| Other owned fields | Request CSRF token and response Alt-Svc string |
| Managed pointer body | Disown the payload pointer only when a cleanup callback is installed; keep callback, context and invocation semantics unchanged |

Arenas have their own explicit lifecycle and are not TLS pending allocations.
A non-NULL arena does **not** imply that every descendant is arena-owned.
`scope_disown` does not dereference an arbitrary payload pointer; an untracked
pointer is a no-op. Nulls and already-disowned allocations are harmless.

### Borrowed and external state

The walkers do not follow app/database pointers, protocol shells, route
middleware state, unmanaged pointer bodies, borrowed sstring buffers, or opaque
cleanup contexts. Callers must keep those dependencies alive until final
cleanup. If a cleanup context owns GC-tracked dependencies, their allocating
thread must explicitly transfer them before handing over the response. The
framework cannot infer an opaque graph or change user callback ownership.

Build a caller-supplied response on the thread calling `respond_with`, or transfer
its owned allocations from their actual allocator thread first. A later walk on
another thread cannot remove entries from the original thread's pending list.

## HTTP/2 queue ownership

The queue module, rather than the HTTP graph walker, detaches its explicitly
refcounted queue on creation and completion nodes immediately before linking
them into the shared queue. A producer may exit before the connection drains;
a retained queue may outlive connection teardown. Closed queues discard the
exchange through the existing destructor order. Node-allocation failure also
consumes and destroys the transferred exchange, avoiding a leak after its
producer has relinquished ownership.

## Verification and limits

The initial implementation was source-reviewed before runtime testing. On commit
`5b128f1178df374ee5bc3db1aba8f921a6af7d68`, Linux ASan/UBSan CI passed the
real TCP classic/C1M matrix with GC on/off, repeated under NDEBUG. This is not
cross-platform acceptance by itself. The macOS receive-timeout regression was
then reproduced against the full native library: shutdown had already stopped
the reactor, but the coalesced batch parked its remaining bytes. A flush that
observes shutdown now finishes that batch through the existing send helper.
The native TCP matrix passed in all eight modes after this change. This is not
an absolute shutdown deadline or a guarantee about already-parked callbacks;
the helper retains its per-poll timeout. Final integrated CI remains required.

The verification boundary includes creator exit or scope flush before completion,
heap headers/maps/session/adopted buffers and exhausted-arena fallback, producer
exit before reactor/HTTP2 drain, middleware posthandlers, losing completion,
timeout/abort races, and exactly-once managed cleanup. Do not infer unexecuted
coverage from the ownership inventory.

## Scheduler and IO queue ownership

The scheduler shell, IO queue shell, and sentinel/submitted nodes now leave the
allocating thread's GC scope before publication. Existing explicit destruction
and epoch-retirement paths remain their owners. Opaque callback arguments are
not traversed or transferred by the queue. The caller must transfer a tracked
payload itself before its receiving callback can use or free it.

Unlike `cwist_alloc`, `cwist_alloc_array` uses untracked `cwist_malloc`; the
scheduler worker array already has explicit lifetime. The delayed heap uses
untracked `cwist_realloc`, including initial allocation. Neither needs a new
disown operation.

The macOS/BSD build selects a separate `kqueue.c` job queue. Its queue shell
and kevent job wrappers also leave the creator/donor scope before publication.
The regression waits for its callback before stopping the queue; it does not
assume every backend drains jobs submitted before a stop request. Creator/donor
exit ordering and strict GC scope checks remain unchanged.

`test_gc_job_handoff` exercises creator exit before use/destruction and donor
exit with the consumer paused, for both queues and schedulers. It also checks
opaque payload ownership and delayed-heap growth. Both the common and kqueue
implementations passed normal/NDEBUG/ASan with GC on/off (12 component runs).
The selected macOS backend also passed the full-library normal/NDEBUG target.
Native ASan used leak detection disabled; this is not leak proof or a guarantee
about queued callbacks discarded on shutdown. Integrated CI remains required.
