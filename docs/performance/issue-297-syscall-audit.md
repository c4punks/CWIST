# Issue #297 syscall-reduction audit (local, 12-core Ryzen 5600X)

Date: 2026-10-10. Branch `perf/297-syscall-reduction`. **Result: all four
proposed syscall-reduction items measured negative or were already
implemented; no code landed.** This document preserves the profiling data
and the measured reasons, so the items are not retried blind.

Workload: the-benchmarker contract app (`benchmarks/web-frameworks/main.c`),
plain HTTP, C1M reactor path (default). `wrk -t4` keep-alive at c400, `ab
-n 20000 -c 64` churn. Syscall counts from `strace -f -c` (absolute rps is
depressed under strace; counts and ratios are the signal).

## Baseline syscall profile

Keep-alive c400, 10 s (~1.19M requests):

| syscall | calls | per request | note |
|---|---|---|---|
| sendmsg | 1,193,492 | 1.00 | one coalesced response write per request |
| io_uring_enter | 29,790 | 0.025 | one GETEVENTS wait per CQE batch + deferred-batch flush |
| accept4 | 415 | ~0 | keep-alive: connections are long-lived |
| setsockopt | 814 | ~0 | 2 per accepted connection (TCP_NODELAY + TCP_QUICKACK) |
| clock_gettime | 0 | 0 | vDSO / CLOCK_MONOTONIC_COARSE, never enters the kernel |

Churn (ab, 20k requests, 20k connections):

| syscall | calls | per connection |
|---|---|---|
| io_uring_enter | 48,012 | 2.4 |
| setsockopt | 40,054 | 2.0 |
| accept4 | 26,311 | 1.3 (incl. EAGAIN retries) |
| sendmsg | 20,000 | 1.0 |

## Item results

### 1.1 io_uring multishot accept — NEGATIVE (reverted)

Implemented `cwist_reactor_arm_multishot_accept()` (IORING_OP_ACCEPT +
IORING_ACCEPT_MULTISHOT, accepted fds delivered as CQEs, legacy POLL+accept4
fallback on -EOPNOTSUPP/-EINVAL/-ENOSYS). The syscall win is real: accept4
drops 26.3k -> 0 at churn. Throughput regressed decisively:

| config | c100 | c400 | churn |
|---|---|---|---|
| baseline | 350-355k | 342-354k | 13.4-15.4k |
| multishot, all workers armed | 272-281k | 282-292k | 13.6-13.7k |
| multishot, single worker armed | 279-282k | 289-295k | 13.7-13.9k |

-17-21% on keep-alive in both arming shapes. Cause: each accepted
connection becomes its own CQE and its own dispatch-loop iteration, so the
reactor wakes and re-waits far more often than the legacy path, which drains
many connections per single POLL wake with a tight accept4 loop. Also
visible: io_uring_enter count *rises* (48k -> 54k per 20k requests). The
issue anticipated this ("quirks around SO_REUSEPORT ordering and
backpressure; needs a load-test gate") — the load-test gate failed, so the
change was reverted, not committed.

Implementation note for any retry: `IORING_OP_ACCEPT` is an *enum constant*,
not a preprocessor macro — `#ifdef IORING_OP_ACCEPT` silently disables the
code. Guard on `__linux__` only.

### 1.2 Larger SQE batches / fewer io_uring_enter — ALREADY IMPLEMENTED / BLOCKED

Already in place: re-arms and recvs are staged in a 1024-deep deferred SQE
array and flushed with one `io_uring_enter` per dispatch round
(`queue_deferred`/`flush_deferred`, src/sys/io/reactor.c:852-930); the
remaining enters are completion waits (GETEVENTS), one per CQE batch, which
is the epoll_wait(2) equivalent — no further batching is possible without
busy-polling.

The main reactor ring is deliberately created *without*
`IORING_SETUP_SQPOLL` (src/sys/io/reactor.c:677): on an idle SQPOLL ring the
kernel ignores the enter timeout, so the reactor's timer heap and shutdown
wake would never fire (documented at `cwist_reactor_stop`,
src/sys/io/reactor.c:1405-1416). Moving to SQPOLL requires a timerfd-based
timer source and shutdown path first; deferred.

The SQPOLL *probe* ring at startup (`[io_uring/SQPOLL] verified`) only
proves the kernel supports SQPOLL; it is not the reactor's ring.

### 1.3 Per-accept setsockopt removal — NEGATIVE (reverted)

TCP_NODELAY on accepted sockets *is* inherited from the listen socket on
Linux 6.12 (verified with a probe program), so setting it once on the listen
fd did halve per-connection setsockopt calls (40.1k -> 20.1k at churn).
However the listen-socket TCP_NODELAY set itself — independent of
inheritance, reproduced with per-accept NODELAY kept as well — regressed
keep-alive throughput ~18% (c400: 372-385k -> 305-310k, three interleaved
rounds). Setting TCP_NODELAY on the listening socket changes the
listener/SYN-ACK path enough to cost real throughput; per-accept
setsockopt stays. TCP_QUICKACK was left untouched throughout (it is
load-bearing for the #307 TLS handshake stall fix on the HTTPS path).

### 1.4 Time caching — ALREADY IMPLEMENTED

No `clock_gettime` syscalls appear in either profile: the hot paths use
`cwist_fast_monotonic_sec()` (src/net/http/writer_fast.c:200) which uses
`CLOCK_MONOTONIC_COARSE`, resolved through the vDSO without entering the
kernel. Nothing to cache.

## What the profile says to do instead (ranked)

1. **Zero-syscall response write (serialization-adjacent, biggest count).**
   `sendmsg` is exactly 1 per request — 1.19M calls in a 10 s c400 run, the
   largest syscall count by two orders of magnitude. The SQPOLL send ring
   already exists (`cwist_io_uring_sqpoll_send`, src/sys/io/uring_sqpoll.c)
   and is only probed, never used. Wiring the coalesced `obuf` flush
   (src/net/http/http.c:3368) to an IORING_OP_SEND SQE removes one syscall
   per request, but requires: a completion-queue reader, obuf lifetime
   extension until CQE (obuf is reused across keep-alive requests today),
   and response/close ordering. That is a serialization-path change
   (issue #297 section 2.3 territory) and needs its own branch + gate.
2. **Pre-serialized response cache (#297 2.1).** For the benchmarker
   contract app (`GET /` empty body, `GET /user/:id`) a fully materialized
   200 response would cut the serialize+sendmsg path to a single static
   write; combined with (1) it removes both per-request syscalls that
   remain.
3. **SQPOLL reactor ring (#297 1.2 proper).** Requires timerfd timers and a
   shutdown nudge; eliminates the deferred-batch flush enters (~0.025/req
   today, so the ceiling is small unless (1) lands and the ring carries
   sends too).
4. **Accept path: leave as is.** accept4 is ~0 per request at keep-alive and
   the multishot alternative measured strictly worse.

## Method notes

- strace counts under `-f` follow all worker processes; absolute rps under
  strace is ~2-4x depressed, so only counts and unstraced wrk/ab numbers are
  reported as measurements.
- All A/B runs interleaved base vs experiment, 2-3 rounds, on an otherwise
  idle box; the contract app binary was rebuilt from the branch worktree.
