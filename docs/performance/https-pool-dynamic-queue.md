# HTTPS pool task queue: static ring vs growable ring

Experiments behind commit `8d5a5958` ("grow the pool task ring on demand
instead of a 96 MiB static array"), main `e7568313`.

## What changed

`https_thread_pool_t.queue` was a static `https_pool_task_t[2097152]`. Each
task is 48 bytes, so the array was about 96 MiB. `https_pool_init()` memsets
the whole pool struct, so that 96 MiB became resident in every process that
starts the pool, before the first connection. With `CWIST_C1M_MODE=1` that is
every worker process plus the master.

The queue is now `queue` / `queue_len`: a ring that starts empty, takes
`HTTPS_TASK_QUEUE_INITIAL` (1024) slots on first use, and doubles through
`resize_queue()` when full, up to the old limit `HTTPS_TASK_QUEUE_MAX`
(2097152). At that limit, or if an allocation fails, submitters block on
`cond_not_full` as before. The ring is not shrunk.

## Setup

- AMD Ryzen 5 5600X (6 cores / 12 threads), Linux; client and server on loopback.
- Two non-ASan builds from the same tree, differing only in
  `src/net/http/https.c` (`5f779476` vs `8d5a5958`).
- Server: a minimal `cwist_app` with HTTPS (`GET /` returns `ok`), started
  with `CWIST_C1M_MODE=1`.
- RSS is the sum of `VmRSS` over the master and its workers.

## 1. RSS of `https_pool_init()` alone

| build | VmRSS before init | after init | delta |
|---|---|---|---|
| static ring | 9 876 KiB | 108 360 KiB | **+98 484 KiB** |
| growable ring | 10 104 KiB | 10 280 KiB | **+176 KiB** (thread stacks) |

`size(1)`: the server binary's `.bss` drops from 100 861 061 to 197 765 bytes.

## 2. 4 workers, steady and churn load (3 alternating rounds)

`CWIST_WORKERS=4`. Load in order: `wrk -t4 -c64 -d10s` (keep-alive),
`ab -n 5000 -c 64` (a new TLS connection per request), then
`h2load --h1 -n 3000 -c 3000` (burst).

| round | build | idle RSS | peak RSS | keep-alive req/s | churn req/s | 3000-conn burst |
|---|---|---|---|---|---|---|
| 1 | static | 422 380 KiB | 459 060 KiB | 159 541 | 840 | 3000/3000, 3.62 s |
| 1 | growable | 29 308 KiB | 67 196 KiB | 162 408 | 841 | 3000/3000, 3.73 s |
| 2 | static | 422 324 KiB | 463 564 KiB | 155 651 | 798 | 3000/3000, 4.01 s |
| 2 | growable | 28 892 KiB | 63 920 KiB | 138 338 | 766 | 3000/3000, 3.91 s |
| 3 | static | 420 748 KiB | 460 768 KiB | 146 037 | 857 | 3000/3000, 3.35 s |
| 3 | growable | 29 368 KiB | 65 616 KiB | 153 601 | 842 | 3000/3000, 3.49 s |

- Idle RSS drops by about 393 MiB, which is 5 processes × ~96 MiB less a
  little. Peak RSS under load drops by the same amount.
- Throughput, churn rate, and burst completion are within run-to-run noise.
  Round 2's keep-alive gap goes the other way in rounds 1 and 3.

## 3. Forcing the ring to grow: 1 worker, 2 pool threads, 6000 simultaneous connections

`CWIST_WORKERS=1 CWIST_WORKER_THREADS=2`, then
`h2load --h1 -n 6000 -c 6000 -t 8`.

| round | build | idle RSS | peak RSS | result | finished in |
|---|---|---|---|---|---|
| 1 | static | 112 552 KiB | 161 612 KiB | 6000/6000, 0 failed | 6.61 s |
| 1 | growable | 14 440 KiB | 61 832 KiB | 6000/6000, 0 failed | 6.60 s |
| 2 | static | 112 788 KiB | 189 924 KiB | 6000/6000, 0 failed | 6.49 s |
| 2 | growable | 14 388 KiB | 62 244 KiB | 6000/6000, 0 failed | 6.46 s |

Afterwards, gdb on the worker of the growable build showed
`g_https_pool.queue_len = 4096` and `count = 0`. So the burst did grow the
ring twice (1024 → 2048 → 4096), every task was served, and the queue
drained.

## 4. Order and loss under growth: `tests/test_https_pool_queue.c`

One pool thread is held in a handler while 5000 tasks queue behind it. The
ring first wraps at its initial size, then grows several times while
wrapped. Every task carries a sequence number and must run exactly once, in
submission order. It passes under ASan + UBSan. `test_https`,
`test_https_park`, `test_https_park_full_gc`, `test_https_full_gc`,
`test_http2`, and `test_shutdown` also pass under ASan with the change.

## Not done / open

- **Shrinking.** A process keeps the capacity of its largest burst: 48 bytes
  per slot, so 4096 slots are 192 KiB and the full 2097152 slots are back to
  96 MiB. Shrinking when the ring drains below a quarter would bound this,
  but needs a policy against resize thrash, so it is not part of the minimal
  change.
- **Lock-free queue.** Not explored, and pool-mutex contention was not
  measured here, so these runs do not show whether the lock matters. The
  growable ring keeps the existing mutex + condvar design, so swapping in a
  different queue later stays a separate, measurable change.
