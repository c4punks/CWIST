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

This document records source-level ownership reasoning, not runtime GREEN.
Parent-owned tests/CI must verify creator exit or scope flush before completion,
heap headers/maps/session/adopted buffers and exhausted-arena fallback, producer
exit before reactor/HTTP2 drain, middleware posthandler allocations, losing
`respond_with`, timeout/abort races, and exactly-once managed cleanup. Local
vendor headers are missing; no build or runtime test was performed for this
implementation.

A separate source-level lifetime risk remains in the existing lazily created
shared timeout scheduler: `cwist_scheduler_create` allocates its scheduler shell
and worker array in the caller's TLS scope without explicit ownership transfer.
This exchange-graph patch does not redesign shared scheduler/IO-queue lifecycle.
Do not interpret coverage of timeout-generated response allocations as proof
that first-time scheduler creation on a short-lived worker is GC-safe.
