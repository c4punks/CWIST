# Cooperative queuing in the C1M reactor's CQE drain (issue #25)

Status: **implemented, on by default** (`CWIST_REACTOR_DRAIN_CHUNK`,
`src/sys/io/reactor.c`). Unset defaults to 64; set to `0` to restore
legacy behavior (no mid-batch drain).

## Background

Issue #25 tracks CWIST's P99.999 tail latency. Two rounds of real fixes
already landed against it (see the issue thread): the yield-batch fix
(`ce970136`, bounding how many pipelined requests one connection serves
inline per turn) and response coalescing (`0bcc4f5a`). Both target
**intra-connection** head-of-line blocking -- one connection's own
pipelined backlog monopolizing the reactor.

This fix targets a different, previously-unaddressed layer:
**inter-connection** ordering within a single io_uring wake round.

## The gap

`cwist_reactor_run()`'s io_uring branch drains a whole CQE batch per wake
synchronously:

```c
while (head != tail) {
    ev_ctx->cb(ev_ctx->fd, ev_ctx->ctx);   /* runs the connection's handler inline */
    head++;
}
queue_deferred(reactor);   /* re-arms accumulated during this round */
```

The ring is 4096 entries deep, so a burst of ready connections (a load
spike, or simply enough concurrency that many sockets become readable in
the same round) can put hundreds of connections in one batch. Each is
served **inline, synchronously, to completion** before the next one even
starts.

Separately, `cwist_reactor_post()` lets foreign threads (background
DB/job completions via `cwist_async_defer`) hand a finished continuation
back to the reactor's owner thread. That handoff is delivered by
`reactor_drain_posts()`, which was only ever called **once per round, at
the top of the loop** -- i.e. after the *entire previous* CQE batch had
already finished draining. A background job that completes early in a
512-connection batch waits behind the other 511 connections' full handler
execution before its continuation ever runs, even though nothing about
that continuation itself is slow.

This reproduces the shape issue #25 actually reports: P99.999 scaling
sharply with concurrency while the average stays flat. A slow request
isn't the cause -- an unlucky *arrival time* relative to a big batch is.

## The fix

`CWIST_REACTOR_DRAIN_CHUNK=<N>` interleaves `reactor_drain_posts()` every
`N` connection callbacks instead of only at the end of the batch:

```c
if (drain_chunk && ++since_drain >= drain_chunk && head != tail) {
    since_drain = 0;
    __atomic_store_n(reactor->impl.cq_head, head, __ATOMIC_RELEASE);
    reactor_drain_posts(reactor);
}
```

`0` skips this entirely, restoring legacy behavior -- the loop is identical
to before the fix. Unset defaults to 64. This only ever reorders *when* a
foreign-thread post is drained
relative to the rest of the batch; it does not change how many
connections get served, how requests within one connection are ordered,
or classic-pool behavior at all (io_uring-branch-only, matching where the
problem was found).

## Measured effect

`tests/bench_cooperative_queuing.c`: 512 pipes all made readable before
the reactor is entered (so all 512 land as CQEs in one drain round), each
callback simulating an 80us handler; a background thread posts one
`cwist_reactor_post()` 200us after the round starts and its own
post-to-callback latency is measured. 10 runs each, this machine:

| configuration | post-to-callback latency |
|---|---:|
| legacy (unset) | 41.1–41.4 ms (~= 512 × 80us, i.e. waits for the whole batch) |
| `CWIST_REACTOR_DRAIN_CHUNK=32` | 2.32 ms (~18x) |
| `CWIST_REACTOR_DRAIN_CHUNK=8` | 0.38 ms (~107x) |

`tests/test_reactor_drain_chunk.c` is the pass/fail twin of this
benchmark, wired into `make test`: it asserts a chunked post lands in
under half the time an unchunked whole batch would take, and under half
of what the same run measured with the env var unset.

**Cost side**: the interleaved drain point is one atomic exchange
(`reactor_drain_posts` popping the MPSC stack, a no-op walk when nothing
is queued) plus one atomic store per chunk boundary -- for chunk=8 over a
512-connection batch, 64 extra cheap atomic ops against ~41ms of handler
work already being spent in that batch. `test_http_fairness` and
`test_reactor_wake` both still pass unchanged (env var unset by default in
their test runs), including under ASan+UBSan.

## What this does not fix

This only shortens the wait for **foreign-thread continuations**
(`cwist_async_defer`, background job completions) relative to other
connections in the same batch. It does nothing for a plain synchronous
`GET /` benchmark that never uses `cwist_async_defer` -- that workload has
no foreign-thread posts to interleave, so the the-benchmarker-style
numbers already in issue #25 are not expected to move from this alone.
It is a real, separate, previously-unaddressed gap in the same
dispatch-round-serialization family the issue has been tracking, not a
replacement for further work on plain-request tail latency within a
batch (which would require either bounding total inline work per batch by
time, or moving handler execution off the reactor thread entirely -- both
weighed and neither attempted here; see the issue thread's own caution
that a "traditional completion queue" hand-off might regress average
latency).

## Tuning

The default is 64, set unconditionally after the header-serialization and
response-coalescing work landed (PR #180). At chunk=64 the interleaved
drain fires at most 8 times per 512-connection batch -- a small number of
cheap atomic ops against tens of milliseconds of handler work. The plain
benchmark workload the CI runs does not use `cwist_async_defer`, so it is
not exercised there; the improvement is visible only on deployments that
actually use background jobs or deferred responses.

Set `CWIST_REACTOR_DRAIN_CHUNK=0` to disable the mid-batch drain entirely
and restore legacy behavior. A smaller value (8-32) tightens the
foreign-thread post latency further at the cost of more atomic ops per
round; 8 reduces the worst-case wait from ~41ms (whole batch) to ~0.38ms
in the bench above.
