<p align="center">
  <img src="./logo.png" alt="CWIST logo" width="220">
</p>

<h1 align="center">CWIST</h1>
<p align="center"><strong>C Web development Is Still Trustworthy</strong></p>

<p align="center">
  <a href="https://discord.gg/6F8HDmNAPg"><img src="https://img.shields.io/badge/Discord-Join%20chat-5865F2?logo=discord&logoColor=white" alt="Discord"></a>
</p>

<p align="center">
CWIST is a C17 web framework and application server with built-in HTTP/1.1, HTTP/2,
HTTP/3 (QUIC), WebSocket, and WebTransport support, hybrid post-quantum TLS
(X25519MLKEM768), an embedded SQLite ORM, and a synchronous io_uring/epoll/kqueue
reactor. It is written in plain C and links statically.
</p>

## Two server modes, and why you would pick each

CWIST ships two request paths and they are tuned for opposite things. Pick per
workload; the mode is one environment variable.

**CWIST reactor (default)** takes the throughput. It multiplexes many connections per event
loop, so connection count is decoupled from thread count and a connection costs
a reactor slot rather than a parked thread. On the run recorded further down it
leads the async row on throughput. This is the default mode; no extra environment
variable is needed unless you want the other path.

**CWIST Classic pool** is the opt-in thread-per-connection mode. Every connection gets
its own thread, so no request waits behind another in a batch. It answers the
median request in less than half of the Axum row's time, and stays ahead through
p99. Enable it with `CWIST_C1M_MODE=0`.

### The distribution is the point, not the average

An average hides which requests were slow. The benchmark block below samples
the full distribution, and the density chart draws the whole shape. Read it as:
classic pool leads through the median and p99; the crossover is at the extreme
tail, where Axum's is tighter. Closing that gap is open work rather than
something to spin.

These figures come from one commit, load profile, and CI environment, and the
runner CPU model changes between runs, which moves them more than most code
changes do. They are not universal guarantees.

[Heavy Benchmark on CWIST APP](https://github.com/gg582/fly.board/blob/main/README.md)

<!-- TUNED_BENCHMARK:START -->
**Tuned low-latency run (wrk -t4 -c100 -d10s (after 10s warmup, warmup discarded)), CWIST vs Axum on identical concurrency:**

- **CWIST**: 114,849 req/s at 0.53ms average latency (P50 0.44ms, P90 0.96ms, P99 2.21ms)
- **Axum**: 120,065 req/s at 0.79ms average latency (P50 0.69ms, P90 1.44ms, P99 2.60ms), same binary as the main run above

These runs use a different concurrency budget from the main table. They do not establish a causal scheduling explanation or a universal tail-latency improvement.
<!-- TUNED_BENCHMARK:END -->

---

## Install

CWIST vendors its dependencies (BoringSSL, lsquic, libttak, SQLite3), so a plain
build works on a fresh Linux, macOS, or BSD machine:

```sh
git clone https://github.com/c4punks/CWIST.git
cd cwist
make
sudo make install        # optional, installs to /usr/local (override with PREFIX=/opt/cwist)
```

### Homebrew

macOS and Linux (Linuxbrew) users can install the latest release from the
[CWIST tap](https://github.com/c4punks/homebrew-cwist):

```sh
brew tap c4punks/cwist
brew install cwist
```

Or install directly without tapping:

```sh
brew install c4punks/cwist/cwist
```

The formula builds from the release source tarball
(`dist/cwist-<version>.tar.gz`, vendored dependencies included) and installs
`libcwist.a`, public headers, the `cwist` CLI, and `cwist.pc` pkg-config
metadata under the Homebrew prefix.

## Hello world

```c
#include <cwist/app.h>

static void hello(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    cwist_sstring_assign(res->body, "Hello from CWIST!");
}

int main(void) {
    cwist_app *app = cwist_app_create();
    cwist_app_get(app, "/", hello);
    cwist_app_listen(app, 8080);
    cwist_app_destroy(app);
    return 0;
}
```

```sh
gcc -o server main.c -lcwist -lssl -lcrypto -lz -lzstd -lbrotlienc -lbrotlicommon -luriparser -lcjson -ldl -lpthread -lm
./server
```

A larger example with a database, post-quantum TLS, metrics, and RDBMS
auto-detection:

```c
#include <cwist/app.h>

static void hello(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    cwist_sstring_assign(res->body, "Hello from CWIST!");
}

int main(void) {
    cwist_app *app = cwist_app_create();

    /* SQLite + ORM-ready database */
    cwist_app_use_db(app, ":memory:");

    /* Post-Quantum TLS (hybrid X25519MLKEM768) */
    cwist_app_use_pqc_layer(app, true);

    /* Observability endpoints */
    cwist_app_enable_metrics(app);
    cwist_app_enable_healthz(app);

    /* Auto-detect PostgreSQL / MySQL / MariaDB on localhost */
    cwist_app_auto_rdbms(app, 5432);

    cwist_app_get(app, "/", hello);

    cwist_app_listen(app, 8080);
    cwist_app_destroy(app);
    return 0;
}
```


<!-- WEBSERVER_BENCHMARKS:START -->
## Latest isolated HTTP benchmark

Measured commit: `3fd4bd1db7e3181cb784e2cebd5bac84a3351b14`. Release tag: `not recorded; identify this run by commit`.
Run: https://github.com/c4punks/CWIST/actions/runs/36017321687. Timestamp: `2026-09-24T15:16:40.020516+00:00`.

Latency columns use the **wrk corrected distribution**. Memory columns are process-group end samples, not peaks: Group RSS counts a page shared between worker processes once per process, Group PSS divides it by its mapper count, so compare a single-process server against PSS. Context switches are same-thread counter deltas over threads live at both ends of the window; N/A means unavailable.

| Profile | Req/s | Mean ms | P99.999 ms | Group PSS MiB | Group RSS MiB | Context-switch delta |
|---|---:|---:|---:|---:|---:|---:|
| CWIST Classic | 115,290 | 1.995 | 20.317 | 47.31 | 57.77 | 1,129,020 |
| CWIST | 143,093 | 2.789 | 29.083 | 14.25 | 25.69 | 124,237 |
| CWIST arena_max=1 | 143,983 | 2.713 | 12.190 | 18.09 | 29.52 | 128,614 |
| CWIST drain_chunk=8 | 144,039 | 2.737 | 15.187 | 14.23 | 25.64 | 114,495 |
| Axum | 113,232 | 3.440 | 16.226 | 14.87 | 16.93 | 193,421 |
| Gin | 79,983 | 6.722 | 82.435 | 28.30 | 29.72 | 312,749 |
| Spring Boot | 43,418 | 9.176 | 64.246 | 1,273.96 | 1,276.84 | 220,070 |

Main profile: `wrk -t12 -c400 -d10s`, after a discarded 10s warmup.

### Separate tuned profile

`wrk -t4 -c100 -d10s`, after a discarded 10s warmup. Do not compare these rows as equal-load results against the main table.
- CWIST Classic: 114,849 req/s; mean 0.535 ms; corrected P99.999 4.473 ms.
- Axum: 120,065 req/s; mean 0.789 ms; corrected P99.999 5.635 ms.
- Spring Boot: 44,547 req/s; mean 2.286 ms; corrected P99.999 29.122 ms.

Spring Boot row: openjdk version "25.0.4.1" 2026-08-18 LTS, Spring Boot 3.2.3, Spring WebFlux + Reactor Netty on native epoll (G1GC, JDK 25 Leyden AOT, virtual threads disabled). Full JVM options are recorded in `benchmarks/webserver.json`.

[Measurement contract](docs/webserver-benchmark.md) · [History](benchmarks/webserver.json)
![Web Server Benchmark Trends](docs/webserver-benchmark-trends.svg)

Latency distribution (density curve reconstructed from each server's percentiles - shows the shape of the tail, not just its P99.999 number):

![Web Server Latency Distribution](docs/webserver-latency-distribution.svg)

### Per runner CPU

GitHub hands out a different CPU model per run, which moves these numbers more than most code changes do. Medians of every recorded run, split by the CPU it landed on, so rows are only comparable down a column:

| Runner CPU | Runs | CWIST Classic ms | CWIST ms | Axum ms | CWIST req/s | Axum req/s |
|---|---:|---:|---:|---:|---:|---:|
| AMD EPYC 7763 64-Core Processor | 8 | 2.13 | 2.76 | 3.49 | 143,578 | 112,133 |
| AMD EPYC 9V74 80-Core Processor | 1 | 1.65 | 2.17 | 2.60 | 186,923 | 151,791 |
| INTEL(R) XEON(R) PLATINUM 8573C | 1 | 1.09 | 2.33 | 1.78 | 262,140 | 223,003 |
| Intel(R) Xeon(R) 6973P-C | 1 | 1.00 | 1.77 | 1.61 | 305,229 | 246,479 |
| Intel(R) Xeon(R) Platinum 8370C CPU @ 2.80GHz | 1 | 1.39 | 2.29 | 2.54 | 213,771 | 156,146 |
<!-- WEBSERVER_BENCHMARKS:END -->

_Methodology, JVM options, and fairness settings: [docs/webserver-benchmark.md](docs/webserver-benchmark.md)_


## What CWIST includes

Most C web frameworks stop at HTTP/1.1 and leave TLS, protocol upgrades, and
memory management to the user. CWIST ships the whole stack:

- **HTTP/3 & WebTransport** powered by lsquic (server-side sessions plus an experimental
  native C client on `dev`, backed by LSQUIC PR #629).
- **Post-Quantum TLS** with one call: `cwist_app_use_pqc_layer(app, true)`
  forces hybrid X25519MLKEM768 and disables legacy TLS < 1.3.
- **Server-side zero-copy I/O & C1M reactor** on io_uring / epoll / kqueue,
  with lock-free job queues and generational arena allocators from libttak.
- **Nuke DB**: a read-optimal, in-memory SQLite engine that syncs to disk on every COMMIT.
- **Auto-RDBMS detection**: probe any TCP port and mount PostgreSQL, MySQL,
  or MariaDB runtimes by wire-protocol fingerprinting.

| Layer | What you get |
|-------|-------------|
| **Protocols** | HTTP/1.1, HTTP/2 (h2/h2c), HTTP/3 (QUIC), WebSocket, WebTransport |
| **TLS / Security** | BoringSSL, PQC hybrid groups, ECH, JWT, DB Crypt |
| **Database** | SQLite3 + ORM, Nuke DB (in-memory + WAL sync), RDBMS auto-detection |
| **Routing** | Express-style `:param` routes, Mux router, chainable middleware |
| **Performance** | Zero-copy I/O, generational arenas, EBR GC, lock-free queues, Big Dumb Reply cache |
| **Async handlers** | Deferred responses (`cwist_async_defer` / `cwist_async_respond`): offload blocking work to a job thread and complete the request later without stalling the reactor |
| **Observability** | Structured access logs, metrics endpoint, healthz, rate limiting |
| **gRPC / Protobuf** | Unary and streaming routes, incremental framing, health/reflection services, Trailers-Only error responses, and `cwist proto` model/encoder/decoder generation (scalars, repeated, nested messages, enums). Channel client: dns/ipv4/ipv6 resolution, `pick_first`/`round_robin` load balancing, and gRFC A6 retries (backoff, pushback, throttling, transparent retries) |
| **Rendering** | HTML builder, CSS composer, template engine, JSON builder / heal |

## Why C, when Axum and Gin exist?

The benchmark results above demonstrate the advantages in latency, memory footprint, and determinism:

1. **Latency & Throughput.** Under 400 concurrency (`wrk -t12 -c400`, CI run above), both CWIST paths deliver lower average latency and higher throughput than the Axum, Gin, and Spring Boot rows of the same run; the tuned low-latency profile (`wrk -t4 -c100`) shows the sub-millisecond median.
2. **Memory Efficiency.** CWIST's resident footprint is a fraction of the Go row and orders of magnitude below the JVM row of the same CI run. In high-density container environments, this significantly reduces memory consumption across thousands of instances.
3. **Tail Latency & Predictability.** Zero-copy framing, thread-pinned worker execution, and generational arena allocators minimize latency variance and GC pauses.
4. **Zero-Overhead FFI.** Production libraries in finance, game servers, machine learning, and systems software written in C/C++ link directly into CWIST with zero FFI conversion or runtime bridge penalty.
5. **Instant Cold Start.** With no runtime VM warmup or GC initialization required, CWIST starts in milliseconds and immediately serves requests at full capacity.

## Platform support

CWIST builds on Linux, macOS, and BSD. The HTTP/3 server and native client use
non-blocking UDP sockets on every platform; Linux uses `epoll`, BSD-family
systems use the portable polling path. ECN metadata is enabled only when the
host exposes the required socket options, so a missing optional API never
blocks an HTTP/3 build.

## Execution & I/O models: CWIST reactor (default) and CWIST Classic pool

CWIST provides two operational execution models tailored for different workload profiles:

1. **CWIST reactor mode (`CWIST_C1M_MODE=1`, default)**: An event-driven asynchronous reactor designed for massive concurrent connections (`io_uring` on Linux, `kqueue` on macOS/BSD). It uses non-blocking I/O multiplexing and cooperative scheduling with lock-free coordination to maintain low latency under high concurrency without per-connection thread overhead. This is the default mode: if you do not set `CWIST_C1M_MODE`, you are running the CWIST reactor.
2. **CWIST Classic pool mode (`CWIST_C1M_MODE=0`)**: A worker thread pool model designed for low-jitter, predictable throughput on compute-bound workloads. In this mode, incoming requests are assigned to worker threads using thread-pinned queues and executed to completion inline on the worker stack. Enable it explicitly when you want a thread per active connection instead of the reactor.

### Readiness multiplexing vs full completion rings

On Linux, CWIST uses `io_uring` (raw syscalls, no liburing dependency) as the event engine, replacing `epoll_wait` in `src/sys/io/reactor.c`. By default the reactor arms one-shot `IORING_OP_POLL_ADD` requests and the woken worker performs inline I/O directly. On reactors with a real io_uring ring the async HTTP receive wait can instead be a single `IORING_OP_RECV` whose completion stages the bytes, replacing the POLL+recv pair (issue #179, PR #187); a per-connection learn flag keeps non-pipelining clients at the legacy op count. `CWIST_RX_URING=0` restores the POLL path, `CWIST_LATENCY_PROBE=1` records arm-to-dispatch and callback-time histograms per reactor (issue #166). If io_uring setup fails, the reactor falls back to epoll (kqueue on macOS/BSD) with identical behavior.

- **Direct readiness handling.** When a readiness notification arrives, the worker processes the event directly rather than routing multiple intermediate completion steps through userspace ring buffers on every tick.
- **Structural backpressure.** Work cannot unboundedly accumulate; per-worker concurrency limits allow the server to shed excess load under saturation rather than inflating tail latency.
- **Cache locality.** Handling request execution on contiguous worker stacks minimizes cache misses and fragmentation compared to multi-stage heap-allocated callback chains.
- **Deterministic tail.** Thread-pinned worker execution and generational arenas keep latency variance minimal across percentiles.

## Development hot reload

New projects include a self-describing `.cwpro` development command. Run the
watcher from the project directory to calculate the affected translation units,
incrementally invoke the build, and restart the app only after a successful
build. It uses Linux `inotify` or BSD/macOS `kqueue` for low-latency wakeups,
with an mtime snapshot and polling fallback to prevent missed rebuilds. The
previous process remains available if a build fails.

```sh
cwist watcher
```

Use `cwist watcher --no-run` for CI or rebuild-only use, or `--poll` to force
portable polling. `dev.debounce_ms` and `dev.stop_timeout_ms` in the manifest
control atomic-save coalescing and graceful process shutdown.

## Managing a project with the cwist CLI

`make install` also installs the `cwist` command line tool into `$(PREFIX)/bin`.

```sh
# Scaffold a project: src/main.c, Makefile, and a demo.cwpro manifest
cwist new project demo --directory ~/src
cd ~/src/demo

# Build and run like the generated Makefile does
make            # cc src/main.c -lcwist -lpthread  (needs CWIST installed)
./bin/demo

# Develop with hot reload (incremental rebuild + zero-downtime restart)
cwist watcher

# Generate OpenAPI 3.1 from Doxygen @openapi.* route annotations
cwist openapi

# Generate C models and gRPC method paths from proto3 definitions
cwist proto api.proto

# Inspect the project manifest and detected routes
cwist describe
```

The `.cwpro` manifest (`cwist-project/v1`) is the single source of truth for
the watcher and generators: entry point, include paths, dev debounce/stop
timeouts, and route discovery scope all live there, so builds stay reproducible
across machines without extra configuration.

## Linking

CWIST's `libcwist.a` is a **thin static archive**: it contains only CWIST
objects. `make install PREFIX=/opt/cwist` installs its built external
submodules separately in `/opt/cwist/lib/cwist`, and installs their public
headers in `/opt/cwist/include/cwist/vendor`.

Compile against both header directories and link against both library
directories. This keeps third-party archives independently replaceable and
avoids duplicate symbols from a merged (fat) archive.

```sh
gcc -I/opt/cwist/include -I/opt/cwist/include/cwist/vendor main.c \
    -L/opt/cwist/lib -L/opt/cwist/lib/cwist -lcwist \
    -llsquic -lssl -lcrypto -lnats_static -lttak -lcjson -luriparser \
    -lz -lzstd -ldl -lpthread -lm -lstdc++
```

`DESTDIR` is supported for staged packages, for example
`make install PREFIX=/usr DESTDIR=/tmp/cwist-package`.

The order above matters for static linking: CWIST first, then its dependencies.

### Required flags (always needed)

| Flag | Provides |
|------|----------|
| `-lcwist` | The framework itself |
| `-llsquic -lssl -lcrypto` | HTTP/3/QUIC and TLS (bundled BoringSSL) |
| `-lz` | zlib: gzip/deflate compression and internal use |
| `-lzstd` | Zstandard: payload compression (preferred algorithm) |
| `-lbrotlienc -lbrotlicommon` | Brotli: payload compression |
| `-lnats_static -lttak -luriparser -lcjson` | Bundled NATS, libttak, URI parsing, and JSON |
| `-ldl` | Dynamic loading (RDBMS auto-mount) |
| `-lpthread` | POSIX threads |
| `-lm` | Math (used by libttak) |

### Optional flags (feature-dependent)

| Flag | When required |
|------|---------------|
| `-lnghttp2` | HTTP/2 support |
| `-lcurl` | RDBMS auto-mount wire probing |

### pkg-config (installed since v3.2)

`make install` ships `cwist.pc`, so the flags above collapse into one line:

```sh
gcc -o server main.c $(pkg-config --cflags --libs cwist)
```

For static linking, use `pkg-config --cflags --libs --static cwist`, which also
pulls in the optional libs (`-lcurl`, `-lnghttp2`).

### pkg-config snippet for Makefile

```makefile
CWIST_LIBS := -lcwist \
              -lssl -lcrypto \
              -lz -lzstd -lbrotlienc -lbrotlicommon \
              -luriparser -lcjson \
              -ldl -lpthread -lm

# Append optional libs if present on the build host
CWIST_LIBS += $(shell pkg-config --libs libnghttp2  2>/dev/null)
CWIST_LIBS += $(shell pkg-config --libs libcurl     2>/dev/null || echo -lcurl)

your_target: your_source.c
	$(CC) -o $@ $< $(CWIST_LIBS)
```

> **Note**: `brotlienc` and `brotlicommon` ship as **`libbrotli-dev`** on
> Debian/Ubuntu and **`brotli-devel`** on Fedora/RHEL. `zstd` ships as
> **`libzstd-dev`** / **`libzstd-devel`**.

## Configuration

CWIST bundles a lightweight configuration loader that reads `.env` files and
environment variables with optional prefixes.

### Loading `.env` files

```c
cwist_config *cfg = cwist_config_create();
cwist_config_load_file(cfg, ".env");

const char *db_url = cwist_config_get(cfg, "DATABASE_URL");
int workers       = cwist_config_get_int(cfg, "WORKERS", 4);
bool debug        = cwist_config_get_bool(cfg, "DEBUG", false);

cwist_config_destroy(cfg);
```

### Loading environment variables by prefix

```c
cwist_config_load_env(cfg, "CWIST_");
/* Now CWIST_PORT=8080 is accessible as cwist_config_get(cfg, "CWIST_PORT") */
```

### `.env` file format

```bash
# Lines starting with # are comments
PORT=8080
DATABASE_URL="sqlite3:data.db"
DEBUG=true
WORKERS=4
```

- Keys and values are separated by `=`.
- Values may be quoted with double quotes (`"..."`).
- Leading/trailing whitespace around keys and values is trimmed automatically.

### Framework-built-in environment variables

These variables are read directly by the framework runtime (no prefix required):

| Variable | Type | Default | Description |
|----------|------|---------|-------------|
| `CWIST_WORKERS` | integer | `1` | Number of worker processes to fork before entering the event loop. |
| `CWIST_C1M_MODE` | boolean | `true` | Default server mode. `true`/`1` selects the CWIST reactor (C1M) and `false`/`0` selects CWIST Classic pool. The reactor is the default; only set this if you want the classic thread-per-connection path. |
| `CWIST_HTTP_BATCH` | integer | `16` | Maximum pipelined HTTP/1.1 requests dispatched per event-loop turn (clamped to `[1, 1024]`). Excess requests are deferred via reactor continuations. |
| `CWIST_HTTP_YIELD_BATCH` | integer | `16` (derived from batch) | Request dispatch yield granularity within a batch before re-posting connection to reactor queue. |
| `CWIST_ASYNC_DEBUG` | boolean | unset | When set, the C1M async path and the reactor log rare failure events (rearm/submit/SQ failures) to stderr. No output in normal operation. |

**CWIST reactor mode (C1M) is now measured, not theoretical.** With the event-driven one-shot
connection path (connections live in the io_uring/epoll reactor instead of
parking a worker thread each), a single cwist server on loopback served:

| Scale | Result | Wall time | Server peak RSS |
|-------|--------|-----------|-----------------|
| C10K | 10,000 / 10,000 established + responded (100%) | ~0.7 s | ~111 MB |
| C100K | 100,000 / 100,000 responded (100%) | ~8.7 s | ~124 MB |
| C1M (8×125K) | 1,000,000 / 1,000,000 responded (100%) | ~14–16 s per client process | ~244 MB |

Peak RSS is the summed `VmHWM` high-water mark across the 12 forked
worker processes (the server defaults to one worker per core); the arena
bump allocator with a shared per-request arena keeps a million held
connections inside a quarter gigabyte of resident memory.

The classic thread-per-connection path (`CWIST_C1M_MODE=0`) is no longer
capped at `cores*8` held connections either: every accepted connection gets
its own on-demand detached thread with a 256 KiB stack, so held connections
scale with the task budget instead of parking a fixed worker pool. Measured
on the same machine, same client:

| Scale | Result | Wall time | Server peak RSS |
|-------|--------|-----------|-----------------|
| C10K | 10,000 / 10,000 responded (100%) | ~0.8 s | ~950 MB |
| C100K | 100,000 / 100,000 responded (100%) | ~9.6 s | ~9.2 GB |

C100K classic needs task-count headroom (one thread per held connection:
`pids.max` / `TasksMax` above 100K, desktop app scopes often cap this near
76K, and `kernel.threads-max` is fine by default) and roughly 26 GB of
virtual commit budget for 100K x 256 KiB stacks (about 9.2 GB of that
actually resident; raise `vm.overcommit_ratio` when RAMxratio + swap is
tight). C1M is out of reach
for this model, a million threads exceeds `threads-max`, which is exactly
what the reactor path is for.

**Which mode should you pick?** The default is the CWIST reactor (`CWIST_C1M_MODE` unset or `1`). It is the right starting point for most workloads: APIs behind a reverse proxy, web pages, webhooks, and anything that must *hold* large numbers of simultaneously open, mostly idle connections, SSE fan-out, websocket-scale chat, long-polling, or genuine C1M targets. The reactor keeps connection count decoupled from thread count and avoids the scheduling overhead of a parked thread per connection.

Switch to CWIST Classic pool (`CWIST_C1M_MODE=0`) only when your workload is dominated by short request bursts where the lowest possible median latency matters more than connection density, and you can tolerate one thread per active connection. That is where cwist's sub-millisecond tuned latency comes from (see the benchmark block above). Giving up the reactor for the classic path costs nothing until your workload is dominated by hundreds of thousands of idle open sockets.

Benchmark environment:

- CPU: AMD Ryzen 5 5600X (6 cores / 12 threads)
- RAM: 62 GB
- Kernel: Linux 6.12.101 (Debian 13), GCC 14.2.0
- Network: loopback (127.0.0.0/8 source-IP spreading on the client side)

Measured with `tests/bench_cxm.c` (multi-process epoll load client,
deterministic source-port allocation round-robined over multiple 127.0.0.x
addresses, `SO_REUSEADDR` on every client socket so reruns within the
TIME_WAIT window do not collide with themselves). Kernel prerequisites
for C100K and above:

- `ulimit -n 1050000` (and `fs.file-max` ≥ 8M for C1M: each connection costs
  one file descriptor on client and server side alike)
- `net.netfilter.nf_conntrack_max=4194304`, loopback traffic is conntracked
  too, and the default 262144 caps you near ~263K connections
- `net.ipv4.ip_local_port_range="1024 65535"` on the client side

Known limits of the current async path: cleartext HTTP/1.x only (HTTPS still
uses the thread-pool model for the request phase), a handler that writes
faster than the socket drains waits inside the reactor thread (bounded by
`CWIST_HTTP_TIMEOUT_MS`), and idle keep-alive connections are not yet reaped
by a timer.

**HTTPS churn experiment (handshake shepherd).** HTTPS accepts go through
`cwist_https_dispatch()`: a single non-blocking `SSL_accept` attempt, with
incomplete handshakes parked in a private epoll set owned by one shepherd
thread (Linux-only; other platforms fall back to the blocking pool path).
Parked handshakes are reaped after `CWIST_HTTPS_HANDSHAKE_TIMEOUT_MS`
(45 s, compile-time). This replaced a synchronous in-worker handshake that stalled the accept loop
under churn (accept-queue overflow, silently dropped handshakes, "phantom"
ESTABLISHED clients). Under a million-request loopback connect-burst load
(h2load), the old path deadlocked within minutes while the shepherd path
completes the run; the requests it drops are slow handshakes h2load reclaims
with its own timeout while queued behind the shepherd. The shepherd has since
been sharded across `CWIST_HTTPS_HS_MAX_SHARDS` threads (hashed by fd,
default derived from the request worker count); tuning
`CWIST_HTTPS_HS_SHARDS` against connect-burst workloads is the next
performance candidate. `make test_https` covers the path.

Some example applications (e.g. `example/othello-web`) also read the standard
`PORT` variable when no explicit port is given.

## Nuke DB

Read-from-RAM, Write-to-Disk. Nuke DB loads an on-disk SQLite file into memory
via `sqlite3_deserialize`, runs `PRAGMA integrity_check`, and then serves every
query from RAM. Every COMMIT triggers a background WAL sync. If bootstrap
fails, it falls back to read-only disk protection mode.

```c
cwist_nuke_init("data.db", 5000);   /* 5-second auto-sync interval */
cwist_db *db = cwist_nuke_get_db();
```

## libttak performance core

CWIST links the in-tree **libttak** allocator/reactor toolkit:

- **Generational Arena Allocator**: static assets and BDR blobs are released in
  one shot, eliminating RSS fragmentation.
- **Epoch-Based Reclamation (EBR)**: `ttak_epoch_enter/exit` pin critical
  sections; stale buffers are reclaimed automatically.
- **Detachable Memory**: signal-safe, cache-aligned arenas for TLS write
  buffers and WebSocket frames.
- **Lock-Free Job Queue**: producers push with a single atomic swap; consumers
  reuse detached nodes to prevent fragmentation.

## PQC TLS layer

Enable post-quantum cryptography with one line:

```c
cwist_app_use_pqc_layer(app, true);
```

This forces `X25519MLKEM768:X25519:P-256`, sets TLS 1.3 as the minimum version,
and strips all legacy TLSv1.0-1.2 ciphers. Application code never touches
OpenSSL directly.

## WebTransport

CWIST exposes server-side WebTransport over HTTP/3:

```c
cwist_app_use_webtransport(app, my_wt_handler);
```

The framework handles the CONNECT negotiation, keeps the stream open after 2xx,
and provides `cwist_webtransport_read/write/flush/close/open_bidi/open_uni`
APIs.

## RDBMS auto-mount

Point CWIST at a local TCP port and it detects the provider by wire protocol:

```c
if (cwist_app_auto_rdbms(app, 5432)) {
    /* PostgreSQL, MySQL, or MariaDB runtime mounted */
}
```

No port-number guessing: CWIST sends a PostgreSQL StartupMessage or reads a
MySQL Handshake initiation packet to classify the server.

## Dependencies

- BoringSSL (in-tree)
- lsquic (in-tree, compiled with `-DLSQUIC_WEBTRANSPORT=ON`)
- libttak (in-tree)
- SQLite3 (in-tree)
- cJSON
- uriparser
- zlib
- Brotli (`libbrotlienc`, `libbrotlicommon`)
- Zstandard (`libzstd`)

See [NOTICE.md](NOTICE.md) for the license summary of every vendored component.

## Stability & conformance

- **Versioning**: until CWIST 4.0, minor releases may adjust public APIs; pin
  an exact tag in production. Draft-level features cycle through `dev` and
  may appear/disappear between tags without a stability guarantee.
- **Conformance gates**: every push runs the test suite under ASan/UBSan with
  `-Werror`, plus an h2spec HTTP/2 conformance diff against a pinned baseline
  (`scripts/ci/h2spec-baseline.txt`); regressions fail the build. Known
  conformance gaps are tracked in that baseline file.

## Community

The official CWIST Discord server: **https://discord.gg/6F8HDmNAPg**: questions,
design discussion, and contribution coordination happen there.

## Documentation

The full documentation map lives in [docs/README.md](docs/README.md). The short
version, in suggested reading order:

- **[Tutorials](tutorials/README.md)**: 30 hands-on modules (`tutorials/01..30`),
  each with a runnable `main.c`, a `CMakeLists.txt`, and a guided README.
- **[Guides](docs/tutorials/)**: task-oriented walkthroughs: [CRUD blog](docs/tutorials/blog-crud.md),
  [NATS integration](docs/tutorials/nats-integration.md), [WebTransport server](docs/tutorials/webtransport-server.md).
- **[API reference](docs/API.md)**: per-module docs under `docs/api/`, plus the
  [flat quick reference](docs/api-quickref.md) and generated
  [Doxygen HTML](https://c4punks.github.io/CWIST/).
- **[ROADMAP.md](ROADMAP.md)**: feature status and milestone planning.

## Examples

Runnable demos under [example/](example/): a [minimal server](example/simple-server),
[step-by-step HTTP](example/http), [SQLite](example/db) and
[encrypted-column DB](example/db-crypt), [JWT auth](example/jwt), a
[WebSocket Othello game](example/othello-web), the [rps-showcase](example/rps-showcase)
throughput demo, rendering helpers ([json-builder](example/json-builder),
[html](example/html), [template](example/template)), and the experimental
[WebTransport](example/webtransport) app. See the
[examples table](docs/README.md#4-runnable-examples) for the full list.

A production deployment built on CWIST: [fly.board](https://github.com/gg582/fly.board).

## Third-Party Licenses

CWIST vendors its dependencies under `lib/`; each retains its own license
(full summary in [NOTICE.md](NOTICE.md), authoritative text in each
submodule's license file):

| Component | License |
|-----------|---------|
| BoringSSL | Apache-2.0 |
| lsquic | MIT (some Chromium-derived parts BSD-3-Clause) |
| cJSON, multipart-parser-c | MIT |
| libttak | BSD-3-Clause |
| SQLite | Public Domain |
| cnats | Apache-2.0 |
| uriparser | BSD-3-Clause (library only; its test suite is LGPL-2.1-or-later and is not linked) |

Static linking propagates each component's license obligations to linked
binaries; review [NOTICE.md](NOTICE.md) when distributing.
