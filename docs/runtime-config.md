# Runtime configuration

Every `CWIST_*` environment variable the framework reads is listed here.
`scripts/ci/check_env_knobs.py` (`make env-check`, run in CI) fails when
`src/` reads a variable this page does not list, or when this page lists one
`src/` no longer reads, so the table stays complete.

Most variables are read once, on first use, and cached: set them before the
process starts (or before `cwist_app_listen()`), not while it is serving.
An empty or malformed value falls back to the default unless the row says
otherwise.

## Status

Each variable has one status. The status says what v4 promises about it.

| Status | Promise for the v4 series |
|--------|---------------------------|
| `supported` | The name and meaning stay. A supported variable is not removed or repurposed before v5; the CI check fails if one disappears after the `v4` tag. |
| `tuning` | The name stays, but the effect and the default may change between minor releases as the engine changes. Measure before relying on a value. |
| `diagnostic` | For debugging only. Output format and behavior are not stable and may change or go away in any release. |

## Processes and server mode

| Variable | Status | Values | Default | Effect |
|----------|--------|--------|---------|--------|
| `CWIST_WORKERS` | supported | integer | `1` | Worker processes forked before the event loop. |
| `CWIST_C1M_MODE` | supported | `0` or `false` disables | `1` | `1` serves on the CWIST reactor (C1M); `0` selects the classic thread pool. |
| `CWIST_PROFILE` | supported | `default`, `performance`, `lowmem`, `lowlat` | `default` | Preset that fills in other variables the environment leaves unset (explicit values win). `default`: `CWIST_C1M_MODE=1`, `CWIST_REACTOR_DRAIN_CHUNK=8`. `performance`: adds `CWIST_MALLOC_ARENA_MAX=1`, `CWIST_HTTP_BATCH=64`. `lowmem`: `CWIST_C1M_MODE=1`, `CWIST_MALLOC_ARENA_MAX=1`, `CWIST_WORKERS=2`. `lowlat`: `CWIST_C1M_MODE=0`. An unknown value warns and uses `default`. |
| `CWIST_DRAIN_TIMEOUT` | supported | seconds, `0`–`3600` | `5` | How long shutdown waits for in-flight requests before the app is destroyed. Out of range values are reported on stderr and ignored. |
| `CWIST_FD_LIMIT_TARGET` | supported | integer > 0 | `1050000` | Open-file soft limit the server raises itself to at start (capped by the hard limit). See [C1M file limits](c1m-file-limits.md). |
| `CWIST_MALLOC_ARENA_MAX` | tuning | integer ≥ 0 | unset (glibc default) | glibc only: `mallopt(M_ARENA_MAX)`. `1` trades some allocator concurrency for a much lower RSS. |
| `CWIST_MALLOC_TRIM_IDLE_SEC` | tuning | seconds, `0`–`86400` | `30` | glibc only (no-op elsewhere): seconds of cumulative event-free reactor idleness after which the reactor calls `malloc_trim(0)` on a zero-event wake, returning the free arena top to the OS. Runs only on idle wakes, so the hot path pays nothing. Measured ≈1.2 MiB median per worker after a load round. `0` disables the trim. |

## Classic thread pool

| Variable | Status | Values | Default | Effect |
|----------|--------|--------|---------|--------|
| `CWIST_WORKER_THREADS` | supported | integer > 0 | CPU cores | Base number of pool threads. |
| `CWIST_POOL_PREWARM` | supported | integer | base pool size | Threads spawned at start, when larger than the base pool (capped by the pool maximum). Avoids spawning during a connection ramp. |
| `CWIST_POOL_IDLE_TIMEOUT_MS` | supported | milliseconds ≥ 0 | `2000` | How long a thread above the base pool idles before it exits. `0` keeps surplus threads forever. |

## HTTP/1.1

| Variable | Status | Values | Default | Effect |
|----------|--------|--------|---------|--------|
| `CWIST_HTTP_KEEP_ALIVE_TIMEOUT` | supported | seconds > 0 | `15` | Idle time before a keep-alive connection is closed. |
| `CWIST_ASYNC_STASH_IDLE_SEC` | tuning | seconds ≥ 0 | `10` | C1M async HTTP: seconds an oversized keep-alive rbuf/obuf stash must go unneeded before it is shrunk toward the 4 KiB floor at re-arm. Busy connections keep their capacity (no per-request shrink→regrow realloc); idle ones still converge to the floor, delayed by the window. `0` restores the legacy immediate shrink. |
| `CWIST_HTTP_BATCH` | tuning | `1`–`1024` | `16` | Pipelined requests dispatched per event-loop turn; the rest continue on the next turn. |
| `CWIST_HTTP_YIELD_BATCH` | tuning | `1`–`1024` | `CWIST_HTTP_BATCH` | Requests served within a batch before the connection yields back to the reactor queue. |
| `CWIST_USE_TCP_CORK` | tuning | integer > 0 | off | Linux, cleartext HTTP/1.1: cork the socket so headers, body and file bytes go out in full segments. |
| `CWIST_TCP_CORK_BURST` | tuning | bytes ≥ 16384 | `262144` | With `CWIST_USE_TCP_CORK`, a file body is flushed every this many bytes instead of in one clump. |

## HTTPS

| Variable | Status | Values | Default | Effect |
|----------|--------|--------|---------|--------|
| `CWIST_HTTPS_IDLE_TIMEOUT_MS` | supported | milliseconds > 0 | `30000` | Idle time before an HTTPS connection is closed. |
| `CWIST_HTTPS_PARK` | tuning | `0`/`false` disables | on | Park idle TLS connections in the reactor instead of holding a pool thread. |
| `CWIST_HTTPS_HS_SHARDS` | tuning | `1`–`16` | CPU cores, clamped to `4`–`16` | Handshake shards. For connect-burst tuning. |

## HTTP/2

| Variable | Status | Values | Default | Effect |
|----------|--------|--------|---------|--------|
| `CWIST_HTTP2_IDLE_TIMEOUT_MS` | supported | milliseconds > 0 | `300000` | Idle time before an HTTP/2 connection is closed. |
| `CWIST_HTTP2_GOAWAY_GRACE_MS` | supported | milliseconds ≥ 0 | `2000` | Time between GOAWAY and closing the connection, so a client can finish streams it already started. |
| `CWIST_HTTP2_MAX_RST_BURST` | supported | integer > 0 | `100` | Stream resets a client may send in a burst before the connection is closed (rapid reset defence, CVE-2023-44487). |
| `CWIST_HTTP2_MAX_RST_RATE` | supported | resets per second > 0 | `100` | Refill rate of that reset budget. |
| `CWIST_H2_BATCH_BYTES` | tuning | bytes ≥ 4096 | `65536` | Outgoing frames are coalesced and written once this many bytes are pending (or before any blocking wait). |

## HTTP/3

| Variable | Status | Values | Default | Effect |
|----------|--------|--------|---------|--------|
| `CWIST_H3_NO_GSO` | tuning | `1` | off | Linux: send each QUIC datagram separately instead of with UDP GSO. |
| `CWIST_H3_DEBUG` | diagnostic | set | unset | Routes lsquic's own log to stderr (CONNECTION_CLOSE codes and so on). Read when the server loop starts. |

## WebSocket

| Variable | Status | Values | Default | Effect |
|----------|--------|--------|---------|--------|
| `CWIST_WS_ASYNC_IDLE_TIMEOUT_SEC` | supported | seconds ≥ 0 | `300` | Idle time before an async (reactor) WebSocket is closed. `0` never closes idle sockets. |

## Reactor

| Variable | Status | Values | Default | Effect |
|----------|--------|--------|---------|--------|
| `CWIST_REACTOR_BACKEND` | tuning | `epoll` | io_uring | Linux: force the epoll backend, for A/B measurement. |
| `CWIST_RX_URING` | tuning | `0` disables | on | Receive with `IORING_OP_RECV` instead of POLL + `recv`. |
| `CWIST_RX_EPOLL` | tuning | `0` disables | on | On the epoll reactor (no io_uring ring, or `CWIST_REACTOR_BACKEND=epoll`), keep keep-alive connections registered edge-triggered and receive from the reactor, instead of re-arming a one-shot poll with `epoll_ctl` per request. |
| `CWIST_REACTOR_DRAIN_CHUNK` | tuning | integer ≥ 0 | `8` (set by `CWIST_PROFILE`; `64` without it) | Connection callbacks handled between drains of posted work, on every reactor backend (io_uring, epoll, kqueue). `0` disables the mid-batch drain (legacy). See [cooperative queuing](cooperative-queuing.md). |
| `CWIST_REACTOR_ROUND_BUDGET_US` | tuning | microseconds ≥ 0 | `2000` | Once a reactor round runs longer than this, parked re-arms are flushed to the kernel every 64 callbacks instead of at the end of the round. The round still runs to completion. `0` disables the bound. |
| `CWIST_REACTOR_CQ_GRACE_NS` | tuning | nanoseconds ≥ 0 | `20000` | How long the reactor spins on the completion ring before it sleeps in the kernel. `0` disables the spin. |

## Diagnostics

| Variable | Status | Values | Default | Effect |
|----------|--------|--------|---------|--------|
| `CWIST_ASYNC_DEBUG` | diagnostic | set | unset | Log rare failures of the C1M async path and the reactor (rearm, submit, full SQ) to stderr. Silent in normal operation. |
| `CWIST_ASYNC_STRICT` | diagnostic | `1` | unset | Deferred-response misuse aborts (leaving a core) instead of printing one warning. |
| `CWIST_LATENCY_PROBE` | diagnostic | `1` | unset | Record arm-to-dispatch and callback-time histograms per reactor and print them to stderr when the reactor is destroyed (issue #166). |
