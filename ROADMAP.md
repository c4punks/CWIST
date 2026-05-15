# CWIST Roadmap

> CWIST is a high-performance C web framework supporting HTTP/1.1, HTTP/2, HTTP/3, and WebSocket. This document tracks what exists, what is actively being built, and what is still missing for a competitive modern web framework.

---

## Legend

| Symbol | Meaning |
|--------|---------|
| ✅ | Done |
| 🔄 | In Progress |
| ⏳ | Planned / Not Started |
| 🔮 | Research / Future |

---

## Section Progress Summary

```
[P0: Critical]      ████████████████████ 100% (Completed)
[P1: Production]     ████████████████████ 100% (Multiport hardening complete)
[P2: DevEx]          ████████████████████ 100% (30 English hands-on tutorials, CLI, hot reload, and test client complete)
[P3: Deep Protocols] ███████████████░░░░░  75% (HTTP/3 extension specs and io_uring backend done)
[P4: Ecosystem]      ████████████████░░░░  80% (gRPC client retry policy and load balancing done)
```

### v3.8 Release Progress

| Phase | Theme | Progress |
|-------|-------|----------|
| Phase 1 — Performance | Reactor latency, RX-uring pipelining, HTTPS handshake shards, worker warmup | Mostly done: #293 closed the measurement loop; RX-uring and worker warmup dropped; HTTPS shard tuning remains open |
| Phase 2 — Rust FFI | `bindings/rust/` (`cwist-sys` + `cwist`) | Done: middleware + async wrappers, example, FFI overhead benchmark; `cwist-sys` and `cwist` published to crates.io as v0.1.0 |
| Phase 3 — HTTP/3 Close Correctness | Re-pin lsquic after upstream fixes | Deferred indefinitely — waiting for upstream lsquic to ship WebTransport client support |
| Phase 4 — v4.0 Scope Confirmation | Enact v3.7 Phase 5 decisions, record experimental-item fates | Done: v3.7 Phase 5 enacted; GraphQL subscriptions, durable queue, Redis RESP2/NATS borrow promoted to supported; WASM component deferred to v4.0; WebTransport stays experimental until v4.1 |

### 1) Transport Layer

* **Native protocols ready**: HTTP/1.1 through HTTP/3 (QUIC via `lsquic`), WebSocket, SSE, and a bounded GraphQL query layer are implemented in-tree (WebTransport server/client evaluation is isolated to `dev`). Low-level socket controls (ECN, 0-RTT, connection migration) are complete.
* **HTTP/3 browser hardening**: Response header emission now normalizes field names to lowercase and rejects CR/LF-bearing values, covering login/logout cookie and redirect paths in strict browsers such as Firefox.
* **HTTPS hot-path optimization**: HTTP/1.1 connections now remain alive across requests, TLS writes stream headers and bodies separately without an intermediate response blob, and prefork workers share session-ticket keys for cross-worker resumption.
* **Request-memory optimization**: Parsed request strings and static response headers use arena-backed or borrowed storage with copy-on-write detachment, while received bodies can transfer ownership without a second copy.
* **Async I/O optimization (`io_uring`)**: io_uring serves as the readiness/wait layer inside `reactor.c` (epoll_wait replacement); its ring setup, teardown, and free-stack slot infrastructure were absorbed from the retired completion-based backend.
* **Multiport HTTP/3 fan-out**: The `cwist_multiport_t` facade now creates per-port UDP contexts and copies global HTTP/3 settings unless a port is detached into a sub-app.

### 2) Application Layer

* **High-performance router & middleware**: Deterministic resource management with parameterized routes (`/user/:id`), compression (Gzip via zlib), CORS, and rate limiting (libttak token bucket) are integrated.
* **Observability**: Prometheus `/metrics` and a probe-registry health-check system are operational.
* **Per-port sub-applications**: Lifecycle and exception handling for `cwist_multiport_get_app(&app, port)` are hardened; detached ports are separately tunable sub-applications. "Independently tunable" covers app-level config (routes, middleware, TLS, size limits) -- it does not extend to process-wide subsystems. `cwist_full_gc()` (see docs/GC.md) is one process-wide switch: enabling it for one sub-app enables it for every `cwist_app` instance sharing that process, with no per-app opt-out. The cJSON allocator hook installed at process start is the same shape. A deployment that needs different memory-management behavior per sub-app needs separate processes, not separate `cwist_app` instances in one process.
* **gRPC services**: Applications can register unary and streaming handlers, incrementally decode arbitrarily split gRPC frames, attach transport output sinks, expose standard health/reflection services, and generate C models/method paths with `cwist proto`. Clients use `cwist_grpc_channel`: dns/ipv4/ipv6 target resolution, `pick_first`/`round_robin` load balancing over per-address subchannels, and the gRFC A6 retry engine (exponential backoff, retryable codes, server pushback, throttling, transparent retries); error responses go out Trailers-Only so conforming clients can retry.
* **Packaging**: `libcwist.a` is a CWIST-only static archive; bundled dependency archives and public headers install side-by-side with `PREFIX`/`DESTDIR` staging support. `cwist.pc` pkg-config metadata, a versioned dist tarball (`make dist VERSION=X.Y.Z`), and Homebrew (`packaging/homebrew/cwist.rb`) / vcpkg (`packaging/vcpkg/`) packaging drafts are available.
* **Deferred async handlers**: `cwist_async_defer()` hands a request/response pair to any thread (scheduler job, NATS callback, custom worker) for later completion via `cwist_async_respond()` / `respond_with()` / `abort()`, with optional 504 timeout and per-mode completion routing (reactor-posted in C1M, inline in thread-pool mode).

### 3) Security & Data Layer

* **Security specs**: BoringSSL-based TLS 1.3 and hybrid post-quantum KEM (`X25519MLKEM768`) are implemented ahead of time. CSRF uses a 256-bit double-submit token with constant-time validation; WAF-lite compiles its signature set into an Aho-Corasick automaton (single O(n) pass per input, ~10.7M checks/s benchmarked) plus HTML output escaping.
* **Data-layer integrity**: SQLite3 embedded integration, migration system, and a `_Generic` macro-based type-dispatched ORM/query builder are in the build stream. The lock-free work queue (`cwist_io_queue`) protects node reclamation with ttak EBR critical sections and a two-stage retire deferral across global-epoch boundaries, and scheduler-backed background jobs are implemented.
* **Protobuf wire helpers**: A lightweight Protobuf runtime supports varint keys, unsigned/signed/bool fields, length-delimited bytes/strings, reader iteration, and ZigZag helpers for hand-written services.

---

## Current Snapshot

<!-- CI-BENCHMARKS:START -->
Automated OS benchmark history is published in `docs/benchmark-trends.svg`. Latest platform: **Darwin**.
<!-- CI-BENCHMARKS:END -->

- Core HTTP/1.1, HTTP/2, HTTP/3, WebSocket, routing, middleware, validation, metrics, health checks, static-file caching, and graceful shutdown are fully implemented in-tree.
- **Developer Ecosystem & Tutorials**: 30 comprehensive hands-on tutorial modules with C source (`main.c`), `CMakeLists.txt`, and English documentation guides (`README.md`) are available under `tutorials/`.
- **CI Automated Web Server Benchmark**: Inline CI job dynamically generates and measures CWIST, Axum, and Spring Boot web servers using `wrk`, rendering real-time RPS, Latency, Peak RSS, and Context Switch metrics. A `the-benchmarker/web-frameworks` contract app lives in `benchmarks/web-frameworks/`, pinned to the `v3.3` tag with the uriparser lib path wired in.
- **P0 (must-have) is 100% complete**: the framework’s core architecture and protocol stack are locked.
- **Resolved**: HTTP/3 header-set objects for streams still open at engine destroy are now tracked per `cwist_http3_context` and swept on the engine/context teardown path; the `leak:cwist_h3_hsi_create` entry was removed from `tests/lsan.supp`.
- **Resolved**: Deferred async completions on the reactor path now park unsent bytes in an owned buffer and resume via one-shot POLLOUT (`cwist_reactor_add_out`), so slow clients no longer occupy a worker/reactor thread for the send budget; a deadline (keep-alive timeout, refreshed on progress) bounds parked writers.
- **Resolved**: gRPC handler-thread sends now wait for WINDOW_UPDATE credit via a condvar rendezvous signalled by the dispatcher (`h2_fc_wait_credit`), instead of failing a zero-credit send with UNAVAILABLE; RST/teardown/stall-timeout still fail fast.
- **Resolved (v3.4 perf wave)**: C1M reactor tail latency — request batches now yield cooperatively (`CWIST_HTTP_YIELD_BATCH`), batch responses coalesce into one writev per turn (256 KiB stash), reactor wake eventfds are registered with the ring (`IORING_REGISTER_EVENTFD`), and post bursts coalesce to a single wake. P99.999 CI measurement fixed end-to-end (lua output format + `$GITHUB_WORKSPACE` script path).
- **Resolved (v3.4 perf wave)**: BDR cache learn/read paths are lock-free (CAS-published entries, atomic blob swaps, EBR reclamation) and entries support hit-time revalidation hooks with zero-copy pointer swaps (`cwist_bdr_put_revalidatable`); classic pool gained `CWIST_POOL_PREWARM` / `CWIST_POOL_IDLE_TIMEOUT_MS` tunables; the HTTPS handshake shepherd shard count is tunable via `CWIST_HTTPS_HS_SHARDS`.
- **Resolved (v3.4 gRPC client wave)**: gRPC client retry policy and client-side load balancing are done — `cwist_grpc_channel` resolves dns/ipv4/ipv6 targets into per-address subchannels with connection backoff (doc/connection-backoff.md), balances calls with `pick_first`/`round_robin` (doc/load-balancing.md), and runs the gRFC A6 retry engine (jittered exponential backoff, retryable codes, server pushback, token-bucket throttling, transparent retries, commit-on-headers) configured via C structs or JSON service config. Error responses are Trailers-Only on both unary and streaming server paths so retries can actually happen; `test_grpc_channel` covers the matrix against loopback backends plus a raw GOAWAY fake.
- **CWIST v3.7.2 released 2026-09-29**: WASI 0.2 formally supported, WASM component pipeline evaluated, durable queue and GraphQL subscriptions shipped behind flags.
- **CWIST v3.8 released 2026-10-01**: theme was *performance, Rust FFI, and v4.0 scope confirmation*. WebTransport moved to v4.1 on 2026-09-25.
- **CWIST v3.9 released 2026-10-05**: TLS observability in `/metrics`, CI HTTPS performance gates, and WebRTC DataChannel support.
- **CWIST v4.0 in preparation**: open decisions and work are listed under [CWIST v4.0 Readiness](#cwist-v40-readiness).
- **Landed on `dev` since v3.7.2**: HTTP close-drain correctness (#292), HTTPS connection-churn optimization (#291), Rust listen-shutdown support (#287).
- **Performance sweep done** (issue #293): CWIST C1M already leads Axum on a single CI run; batch/yield is near-optimal; RX-uring pipelining and worker ttak warmup showed no win; HTTPS handshake shards are a real niche lever (shard=1 is a bottleneck, 4+ saturate).
- **Deferred**: HTTP/3 connection-close correctness and the lsquic re-pin are on hold until upstream lsquic ships WebTransport client support.

---

## 1. Transport Layer

| Feature | Status | Notes |
|---------|--------|-------|
| HTTP/1.1 Server (epoll, threading, forking) | ✅ | Zero-copy sendfile, keep-alive, and supervisor-to-worker shutdown propagation |
| HTTP/2 Server | ✅ | h2 with ALPN |
| HTTP/3 Server (QUIC) | ✅ | lsquic + BoringSSL, QPACK, 0-RTT, migration, push, resilience timeout knobs |
| HTTP/1.1 + HTTP/2 Client | ✅ | libcurl based, sync & async APIs |
| HTTP/3 Client | ✅ | lsquic based, async stream callbacks, auto-retry with exponential backoff, conn timeout knobs |
| WebSocket Server | ✅ | Upgrade, frame parsing, ping/pong |
| TLS 1.3 / HTTPS | ✅ | BoringSSL, ECH, persistent HTTP/1.1 requests, split header/body writes, and shared prefork session-ticket keys |
| Alt-Svc Header Injection | ✅ | HTTP/3 upgrade advertisement from HTTP/1.1/2 |
| **io_uring Backend** | 🔄 | Initial implementation added (`io_uring_backend.c`, `test_io_uring.c`) |
| **kqueue Backend** | ⏳ | BSD/macOS; blocked on non-Linux test environment |
| HTTP/2 Server Push | ✅ | `cwist_http2_push_resource` with PUSH_PROMISE frame, HPACK encoding, server-initiated even stream IDs |
| **WebTransport** | ⏳ / 🔮 | Excluded from `main`; experimental WebTransport server/native C client work stays on `dev` until upstream lsquic ships WebTransport client support; no topic-branch pin |
| HTTP/3 Datagram Extension | ✅ | `send_datagram`, callbacks, `es_datagrams` enabled |
| ECN (Explicit Congestion Notification) | ✅ | UDP socket with `IP_RECVTOS` / `IPV6_RECVTCLASS` |
| Connection Migration | ✅ | `es_allow_migration` enabled |
| 0-RTT Early Data | ✅ | Client: `cwist_http3_client_enable_0rtt`; Server: opt-in via `cwist_http3_set_early_data` (default OFF), shared session-ticket keys for resumption, and a default-ON replay guard restricting early-data requests to idempotent methods |
| **Multiport TCP Facade** | ✅ | Counted `cwist_multiport_t` descriptor, shared accept loop, duplicate/default-port validation, and per-port smoke tests |
| **Multiport HTTP/3 Fan-out** | ✅ | One UDP socket/context per bound port, with global settings copied unless the port is detached into a sub-app |

---

## 2. Application Layer

| Feature | Status | Notes |
|---------|--------|-------|
| Basic Router / Mux | ✅ | Path-based dispatch |
| **Advanced Router** | ✅ | Parameterized routes (`/user/:id`), route groups, per-route middleware, wildcards |
| **Middleware Pipeline** | ✅ | Global + per-route middleware chains supported |
| Static File Serving | ✅ | Fast path via `sendfile`, zero-copy `ptr_body` |
| Template Engine | ✅ | Custom template syntax |
| JSON Builder / Healer | ✅ | `json_builder`, `json_heal` |
| HTML / CSS Builder | ✅ | Programmatic HTML/CSS generation |
| **Form / Multipart Parser** | ✅ | `multipart/form-data` via `multipart-parser-c` submodule |
| **Compression (gzip / brotli)** | ✅ | Compression middleware with swappable backend (`gzip` via zlib), `cwist_mw_compress` factory, tests added |
| **Caching Layer** | ⏳ | No HTTP cache (ETag, Last-Modified, Cache-Control) or in-memory cache |
| **Rate Limiting** | ⏳ | Token bucket or leaky bucket not implemented |
| **CORS** | 🔄 | Basic CORS test exists; configurable CORS middleware missing |
| **SSE (Server-Sent Events)** | ⏳ | No structured SSE stream API |
| **Access Logging** | 🔄 | Macro-based internal logging added; standardized Common/Combined/JSON access format pending |
| **Request ID / Tracing** | ⏳ | No distributed tracing or request correlation ID injection |
| Graceful Shutdown | ✅ | Unified atomic `running` flag + SIGTERM/SIGINT handlers across HTTP/1.1, HTTP/2, HTTP/3 loops |
| **Health Check Endpoint** | 🔄 | Basic `/healthz` endpoint added (`healthz.c`, `healthz.h`) |
| **Metrics / Observability** | 🔄 | Metrics module added (`metrics.c`, `metrics.h`, `test_metrics.c`); Prometheus endpoint pending |
| **Per-Status Error Handlers** | ✅ | `cwist_app_register_error_handler` for custom 404, 500, etc. |
| **URL Reverse Routing** | ✅ | `cwist_app_get_named` + `cwist_url_for` with param substitution |
| **Flash Messages** | ✅ | One-time session-scoped messages via `cwist_flash_get/set` |
| **Per-Port Sub-Applications** | ✅ | `cwist_multiport_get_app(&app, port)` detaches additional ports for independent tuning; public/default port remains owned by root app. Independent tuning is per-app config only -- `cwist_full_gc()` and the cJSON allocator hook are process-wide singletons shared by every sub-app in the process (docs/GC.md) |
| **Standard Status Codes** | ✅ | Full 1xx–5xx `cwist_http_status_t` enum (RFC 9110 + WebDAV extensions), `cwist_http_status_reason()` table, and automatic reason phrases when handlers only set the numeric code |
| **Async / Deferred Handlers** | ✅ | `cwist_async_defer` parks the request for cross-thread completion (`respond` / `respond_with` / `abort`), optional 504 timeout, reactor-posted completion in C1M mode with keep-alive re-arm; covered by `test_async_defer` |

---

## 3. Security

| Feature | Status | Notes |
|---------|--------|-------|
| JWT (encode/decode/verify) | ✅ | HS256 / RS256 |
| Database Encryption | ✅ | `db_crypt` layer |
| ECH (Encrypted Client Hello) | ✅ | BoringSSL ECH |
| **CSRF Protection** | ⏳ | No double-submit cookie or synchronizer token |
| **Secure Headers** | ⏳ | No automatic HSTS, CSP, X-Frame-Options injection |
| **Request Size Limits** | 🔄 | HTTP/3 has body limit; HTTP/1.1/2 limits need audit |
| **Input Validation** | ✅ | Bind validator added (`bind.c`, `bind.h`, `test_bind.c`) |
| **WAF-lite / Sanitization** | ⏳ | No XSS/SQLi sanitizer middleware |

---

## 4. Data Layer

| Feature | Status | Notes |
|---------|--------|-------|
| SQLite Integration | ✅ | `sqlite3` embedded |
| Database Migration | ✅ | `migrate` system |
| **Connection Pool** | ⏳ | SQLite is direct; no generic connection pool abstraction |
| **ORM / Query Builder** | ✅ | Socket-backed ORM with dialect-aware query builder, _Generic type-dispatched RETURNING / scalar helpers |
| **Redis / Key-Value Cache** | ⏳ | No Redis client integration |
| NATS Integration | ✅ | `cwist_nats` wrapper |
| **Message Queue (Job Queue)** | ✅ | `cwist_io_queue` lock-free job queue plus scheduler-backed immediate and delayed jobs |

---

## 5. Developer Experience

| Feature | Status | Notes |
|---------|--------|-------|
| Doxygen Docs | ✅ | Generated HTML docs |
| README / API Reference | ✅ | Markdown docs in `docs/` |
| **Tutorial & Examples** | ✅ | 30 comprehensive hands-on tutorial modules with C source, CMakeLists, and English guides in `tutorials/` |
| **CLI Scaffolding** | ✅ | `cwist new project`, `.cwpro` manifests, OpenAPI generation, and include-aware incremental watcher |
| **Hot Reload (Dev Mode)** | ✅ | `cwist watcher` uses inotify/kqueue with snapshot/poll fallback, debounces changes, exports include-graph scope, preserves prior process on build failure, and performs zero-downtime SO_REUSEPORT overlap process hot-swapping |
| **Configuration Management** | ✅ | `.env` file + environment variable loader via `cwist_config` |
| **Static Library Packaging** | ✅ | CWIST-only archive plus separately installed bundled libraries/headers, deterministic static-link order, `PREFIX`/`DESTDIR` staging, `cwist.pc` pkg-config file, versioned dist tarball, and Homebrew/vcpkg packaging drafts |
| **Testing Utilities** | ✅ | In-process test client (`cwist_test_client`), cookie jar, multipart helper, and BDD-style fluent assertions (`CWIST_ASSERT_STATUS`, `CWIST_ASSERT_HEADER`, `CWIST_ASSERT_BODY_CONTAINS`) |
| **Interactive API Documentation** | ✅ | Embedded Swagger UI interactive documentation page (`cwist_app_enable_swagger`) serving `/docs` and `/openapi.json` |
| **Benchmark Suite** | ✅ | GitHub Actions Linux/macOS measurements publish CPU, throughput, RSS, memory-recovery drift, and context-switch SVG trends; `benchmarks/web-frameworks/` contract app pinned to the `v3.3` tag for the-benchmarker harness |
| **Interop Gate** | ✅ | h2spec HTTP/2 conformance diff against a pinned baseline (`scripts/ci/h2spec_gate.sh`); new failures break the build. Builds run with `-Werror`, stack protector, `_FORTIFY_SOURCE=2`, PIE, and full RELRO on Linux |
| **Fuzzing / Hardening** | ✅ | Stateful sequence/auth libFuzzer coverage plus bounded reassembly and strict HTTP chunk framing checks |

---

## 6. Ecosystem & Integrations

| Feature | Status | Notes |
|---------|--------|-------|
| **gRPC over HTTP/2** | ✅ | Unary/stream registration, split-frame incremental decoder and output sink, standard health (streaming Watch)/reflection service registration, gRPC metadata, h2c/TLS client with unary and server-streaming calls, channel client with dns/ipv4/ipv6 resolution, `pick_first`/`round_robin` LB, and gRFC A6 retries, and test-client coverage |
| **Protobuf Runtime Helpers** | ✅ | Wire-format reader/writer for varint, bool, bytes/string, signed integer casting, and ZigZag helpers |
| **GraphQL** | ✅ | Full query/mutation engine, field arguments, variables, aliases, nested selection sets, error envelope, and HTTP adapter |
| **OpenAPI / Swagger Generation** | ✅ | OpenAPI 3.1 JSON generated from Doxygen `@openapi.*` annotations on route declarations |
| **Background Jobs / Scheduler** | ✅ | `cwist_scheduler` worker pool with immediate and delayed job execution |
| **WebRTC** | 🔮 | Real-time media; requires separate data channel stack |
| **Serverless / WASM Runtime** | 🔮 | Edge deployment target; WASI 0.3 component pipeline replaces the Emscripten browser bundle once the 0.3 world stabilizes (wasi-sdk / wasmtime / jco); see the v3.7 Phase 1 experiment and the tracked issue |

---

## CWIST v3.8 Roadmap (Released 2026-10-01)

v3.7 is released. WebTransport moved to v4.1 on 2026-09-25 (see the v4.1
section below), so v3.8 spends its cycle on work CWIST controls end to end:
closing the measured latency gaps, making CWIST callable from Rust, and
confirming the v4.0 scope. Discussion: https://github.com/c4punks/CWIST/discussions/268.

Entry criteria for v3.8: every item must (a) move a measured performance
number, (b) serve the Rust bindings, (c) close a correctness gap carried over
from v3.7, or (d) resolve a v4.0 blocker. New public C API is allowed only when
it is additive. Scope does not grow; anything not ready slips.

### v3.8 Status at a Glance

| Phase | Goal | Status | Next Actions |
|-------|------|--------|--------------|
| **1 — Performance** | Close reactor/Classic/Axum latency gaps; validate RX-uring pipelining; HTTPS handshake shards; worker warmup | Mostly done | #293 has the data; #294 raised `CWIST_HTTPS_HS_SHARDS` default floor to 4 and MAX to 16; RX-uring and worker warmup dropped |
| **2 — Rust FFI** | Make CWIST callable from Rust (`cwist-sys` + `cwist`) | Done | Middleware and deferred-async wrappers landed; `example/rust-hello/` and FFI overhead benchmark added; built-in middleware factory wrappers and owned `cwist_async_respond_with` helper done; `cwist-sys` and `cwist` v0.1.0 published to crates.io |
| **3 — HTTP/3 close correctness** | Re-pin lsquic when upstream fixes land; add CONNECTION_CLOSE interop gate | Deferred indefinitely | On hold until upstream lsquic ships WebTransport client support; no separate cutoff |
| **4 — v4.0 scope confirmation** | Enact v3.7 Phase 5 decisions and record v4.0 fate for every experimental item | Done | v3.7 Phase 5 enacted; GraphQL subscriptions, durable queue, Redis RESP2/NATS borrow promoted to supported; WASM component pipeline deferred to v4.0; WebTransport experimental until v4.1; docs audit done |

### Phase 1 — Performance

Every item lands with before/after numbers from the CI benchmark or a checked-in
microbenchmark, or with a recorded negative result.

| Work Item | Result | Decision |
|-----------|--------|----------|
| Reactor vs Classic vs Axum latency gap | CWIST C1M already leads Axum on a single CI run (291k vs 237k RPS, 1.46 ms vs 1.65 ms avg); queue delay dominates the tail | Done for v3.8; further gains need architectural tail-hardening, not tuning |
| #166 tail-latency successor | Opened #293; `CWIST_LATENCY_PROBE` shows callback time is tiny and queue delay drives the tail | Done for v3.8 |
| Batch/yield matrix | Default `(16, 16)` is near-optimal; `(16, 4)` and `(16, 32)` are marginally better on P99 but RPS is flat | Done; no default change |
| RX-uring pipelining (#179) | **Negative**: `CWIST_RX_URING=1` is ~14% slower than `=0` on a 16-depth pipeline workload | **Drop** from v3.8 scope; keep the learn flag |
| HTTPS handshake shards | **Real but niche**: `CWIST_HTTPS_HS_SHARDS=1` is a clear bottleneck (3.1k RPS / 27 ms); 4+ shards saturate (~3.6k RPS / ~13 ms) | **Raise default floor to 4 and MAX to 16** (#294); explicit override still available |
| Worker ttak calibration warmup | **No cold-start drift**: RPS varies <5% from second 1 to second 10 across three cold starts | **Drop** from v3.8 scope |
| Measurement discipline | Compare within one runner-CPU column, or use checked-in microbenchmark | Ongoing |

### Phase 2 — Rust FFI

Layout: an in-tree `bindings/rust/` workspace with `cwist-sys` (bindgen output
that links `libcwist.a` through `cwist.pc`) and `cwist` (the safe wrapper).

| Work Item | Status | What Changes | C-Side Impact |
|-----------|--------|--------------|---------------|
| Per-route user context | Done | Additive `_ex` registration functions that carry `void *user_ctx` + optional destructor | New symbols only; existing signatures unchanged |
| `static inline` helpers | Done | Export wrappers (or use bindgen `--wrap-static-fns`) so bindgen can see them | New symbols only |
| Struct layout | Done | Decide per field between accessor and bindgen layout access; add layout assertion tests | New layout tests; existing structs unchanged |
| Safe wrapper scope | Done | App lifecycle, routing with closures, request/response access, middleware, graceful shutdown, deferred async | Middleware `_ex` API added; otherwise none |
| Memory model | Done | Rust allocations stay outside `cwist_alloc`; full GC and `CWIST_INTERCEPT_MALLOC` do not apply to Rust code | Documentation only |
| CI and measurement | Done | `cargo test` on Linux/macOS, `example/rust-hello/`, FFI overhead measured vs C equivalent | New CI step |
| Built-in middleware factories | Done | Wrap `cwist_mw_*` factories in `cwist` crate (`cwist::middleware`) and expose via `App::use_builtin_middleware` | None |
| `cwist_async_respond_with` | Done | `OwnedResponse` in `cwist::http` builds a CWIST-allocated response; `AsyncResponse::respond_with` transfers ownership | New `cwist_http_response_create/destroy` C helpers |

Status at the v3.8 cut: `cwist-sys` and `cwist` v0.1.0 are published to
crates.io. The bindings remain experimental in API stability; breaking changes
may still land while the surface matures, signaled by 0.x crate versions.

### Phase 3 — HTTP/3 Connection-Close Correctness (Deferred)

The CWIST-side half is done, but the lsquic re-pin is now tied to upstream
WebTransport client support rather than to the three standalone close-fix PRs.
Until lsquic ships that support, CWIST stays on its current pin and does not
chase #688, #687, or #693 separately.

| Work Item | Status | Rationale |
|-----------|--------|-----------|
| Re-pin `lib/lsquic` to upstream release containing #688, #687, #693 | Deferred | Rolled into the WebTransport-client-support re-pin; no separate lsquic update before then |
| Build h3spec-style CONNECTION_CLOSE interop gate | Deferred | Gate needs the re-pinned lsquic behavior as the reference |
| Keep `test_http3` Test 12 pinning peer-abort close path | Maintained | Existing regression coverage stays; no new gate until re-pin |
| Add CI gate that fails if pinned lsquic commit lacks required fixes | Dropped | Replaced by the WebTransport-client-support precondition |

### Phase 4 — v4.0 Scope Confirmation

| Decision Source | Action |
|-----------------|--------|
| v3.7 Phase 5 | **Done** — full GC and malloc interception promoted to *supported opt-in* (`docs/GC.md` updated, defaults unchanged: full-GC off); `CWIST_PROFILE` matrix stays the v4.0 default story; latency probe stays hidden opt-in; HTTP batch-shed counter stays always-on |
| Experimental items | **Promoted to supported in v3.8** — GraphQL subscriptions (`graphql_ws.h`), durable job queue (`durable_queue.h`), Redis RESP2 reply tree, and NATS connection borrow (`cwist_nats_native()`). **Deferred to v4.0** — WASM component pipeline (#203) stays experimental (gated on WASI 0.3 / unflagged JSPI); WebTransport stays experimental until v4.1 |
| WebTransport tutorial | Keep experimental until v4.1 |
| Docs | **Done** — stale experimental caveats removed from promoted headers/docs; WASM component and WebTransport caveats refreshed with explicit v4.0/v4.1 deferral |

### Release Criteria for v3.8

- Every Phase 1 item has before/after numbers or a recorded negative result.
- `bindings/rust` builds and passes tests in CI on Linux and macOS, the Rust
  example serves requests, and the FFI overhead is measured.
- The C API additions for FFI are additive only; no existing symbol changes.
- Phase 3 (HTTP/3 connection-close correctness) remains deferred and is
  explicitly not a v3.8 release blocker.
- Every experimental item has a recorded v4.0 decision, and the v3.7 Phase 5
  decisions are enacted in code and docs.
- CI is green on the exact release commit.


### gRPC / Protobuf Status

Completed:

* `cwist_app_grpc_unary(app, service, method, handler, user_ctx)` registers `POST /Service/Method` routes for HTTP/2 gRPC unary calls.
* `cwist_app_grpc_stream(app, service, method, handler, user_ctx)` registers buffered streaming handlers that can consume multiple request messages and append multiple response messages in order.
* `cwist_grpc_decode_message()` and `cwist_grpc_encode_message()` implement the gRPC 5-byte message envelope (`compressed` flag + big-endian payload length).
* `cwist_grpc_decode_next_message()` iterates concatenated gRPC message envelopes for client-streaming and bidi-style buffered request bodies.
* `cwist_grpc_stream_send()` and `cwist_grpc_stream_close()` build ordered multi-message gRPC responses with final status metadata.
* `cwist_grpc_decoder_feed()` recovers gRPC envelopes split across arbitrary DATA payload boundaries; `cwist_grpc_stream_set_writer()` permits immediate transport-frame output.
* `cwist_app_grpc_health()` and `cwist_app_grpc_health_set_status()` register and control `grpc.health.v1.Health`; `cwist_app_grpc_reflection()` registers the v1alpha reflection stream.
* `cwist proto input.proto` (or a `protoc --descriptor_set_out` binary, auto-detected or via `--descriptor-set`) generates scalar proto3 C models, encoder helpers, and gRPC method-path constants.
* `cwist_grpc_set_response()` and `cwist_grpc_set_error()` produce `application/grpc` responses and explicit `grpc-status` / `grpc-message` metadata.
* `cwist_pb_writer` supports varint keys, uint64/int64/bool fields, bytes fields, string fields, and dynamic buffer growth.
* `cwist_pb_reader` iterates Protobuf fields and exposes wire type, field number, varint value, and length-delimited payload slices.
* `cwist_pb_zigzag_encode()` / `cwist_pb_zigzag_decode()` cover signed integer mappings used by `sint32` / `sint64` style fields.
* `test_grpc` verifies Protobuf request construction, gRPC frame handling, unary dispatch, buffered streaming dispatch, multi-message response parsing, invalid content type handling, and malformed frame rejection.

* HTTP/2 streaming calls are wired end-to-end: inbound DATA frames feed `cwist_grpc_decoder_feed()` directly (client-streaming handlers see messages as they arrive via the blocking `cwist_grpc_stream_recv()`), and `cwist_grpc_stream_send()` flushes DATA frames immediately from the handler thread. The app server wires this via `cwist_http2_serve_connection_ex()` + `cwist_grpc_http2_hooks()`.
* `grpc-status` / `grpc-message` are emitted in a dedicated HTTP/2 trailer HEADERS frame (END_STREAM) for both unary and streaming responses.
* `grpc-timeout` is parsed (`cwist_grpc_parse_timeout()`), enforced on streaming calls (DEADLINE_EXCEEDED trailers plus handler cancellation), and client RST_STREAM propagates to handlers via `cwist_grpc_stream_cancelled()` / a `-1` recv.
* Request metadata is normalized: header names are lowercased at the HTTP/2 layer, lookups are case-insensitive (`cwist_grpc_metadata_get()`), and `*-bin` values are base64-decoded (`cwist_grpc_metadata_get_binary()`).
* Compression negotiation: `grpc-encoding: gzip` request messages are inflated (zlib), `grpc-accept-encoding: gzip, identity` is advertised, response messages are compressed with gzip when client advertises `grpc-accept-encoding: gzip` (with `grpc-encoding: gzip` header and frame compressed flag set), and unsupported encodings are rejected with `UNIMPLEMENTED`.
* `test_grpc` covers the buffered path (unary/streaming dispatch, metadata, gzip request decompression & response compression, recv replay, timeout parsing); `test_grpc_stream` covers the wire path over h2c (split DATA delivery, trailer frames, deadline, RST cancellation, gzip request/response compression, unsupported-encoding rejection).

Known limits:

* The proto generator covers scalar, enum, repeated packed-numeric, `oneof`, `map`, and fixed-width/`double` proto3 fields plus service paths, with descriptor-set input (nested types flatten on that path); text input still lacks nested message definitions.
* The builtin health `Watch` route streams status changes over the HTTP/2 transport path and falls back to a single snapshot on the buffered dispatch path.
* Client-side hedging (`hedgingPolicy`) is not implemented — gRPC C-core does not implement it either (gRFC A6). The channel resolver supports `dns` (default), `ipv4`, and `ipv6` targets; `unix`/`unix-abstract`/`vsock` transports, `grpclb`/xDS policies, and `perAttemptRecvTimeout` remain unimplemented.

### gRPC Client Channel (v3.4)

Completed:

* `cwist_grpc_channel_connect(target, options)` resolves `dns:[//authority/]host[:port]` (default scheme, default port 443, every resolved address used), `ipv4:`/`ipv6:` literal lists, and bare `host[:port]` targets (doc/naming.md); unsupported schemes fail cleanly.
* One lazily-connected subchannel per backend address; failed dials back off per doc/connection-backoff.md (1s initial, ×1.6, 120s cap, ±0.2 jitter, reset when the SETTINGS handshake completes).
* `pick_first` (default) sticks to the connected address and fails over in resolver order; `round_robin` dials every address up front and rotates READY subchannels per call (doc/load-balancing.md). `cwist_grpc_channel_get_state()` aggregates subchannel states per the spec rules.
* gRFC A6 retry engine between the channel and the LB pick: `maxAttempts` (client-capped at 5), `initialBackoff`/`maxBackoff`/`backoffMultiplier` with ±0.2 jitter, `retryableStatusCodes` bitmask, overall call deadline shared by all attempts, per-attempt remaining `grpc-timeout`, and the `grpc-previous-rpc-attempts` header on every retry.
* Commit semantics: Response-Headers commit the RPC (no further retries); Trailers-Only failures stay retryable. Transparent retries cover RPCs that never left the client (until the deadline) and RPCs refused before server application logic (RST_STREAM REFUSED_STREAM or GOAWAY with a lower last-stream-id, one immediate retry) — neither counts against `maxAttempts` nor the throttle.
* Server pushback: `grpc-retry-pushback-ms` ≥ 0 delays the retry exactly that long (backoff restarts at `initialBackoff` afterwards); negative or unparseable values stop retries.
* `retryThrottling` token bucket per channel: retries need `token_count > maxTokens/2`; failed attempts with retryable codes (or do-not-retry pushback) cost one token, successful calls refund `tokenRatio`.
* `cwist_grpc_channel_apply_service_config_json()` parses the doc/service_config.md subset — `loadBalancingConfig`/`loadBalancingPolicy`, `methodConfig` (name matching, `retryPolicy`, `waitForReady`, `timeout`), and `retryThrottling` — with gRFC A6 validation rules.
* Error responses are Trailers-Only end-to-end (gRFC A6 / PROTOCOL-HTTP2): unary error replies are a single HEADERS frame with END_STREAM, and the streaming server delays Response-Headers until the first message, so conforming clients can retry failed calls. Servers can emit `grpc-retry-pushback-ms` via a response header (unary) or `cwist_grpc_stream_set_retry_pushback()` (streaming).
* `test_grpc_channel` covers LB stickiness/failover/rotation, retryable/non-retryable codes, `maxAttempts` exhaustion, pushback both ways, throttling, deadlines spanning attempts, JSON service config validation, GOAWAY transparent retries against a raw-socket fake, and resolver schemes; `test_grpc_stream` verifies the Trailers-Only wire forms.

---

## v3.4 Milestone (Released 2026-09-12, hotfix v3.4.1 on 2026-09-14)

Theme: gRPC client side, codegen completeness, and the first WASM client-side support wave. v3.3 shipped the wire-level streaming server (DATA-frame wiring, trailers, deadlines, gzip) plus the first proto codegen extension (enums, nested message fields, repeated packed numerics); v3.4 finishes the story instead of moving the already-pushed v3.3 tag.

* **`cwist proto` completion**: ~~`oneof`, `map`, fixed-width types (`fixed32/64`, `sfixed32/64`, `double`), and `protoc --descriptor_set_out` input bindings~~ (all done). CLI-only work (`tools/cli/cwist`), no library ABI impact.
* **gRPC client**: ~~h2/h2c client with unary/streaming calls, retry policy, and client-side load balancing~~ (all done: `cwist_grpc_client_*` with deadlines and cancellation, plus `cwist_grpc_channel_*` with resolver, `pick_first`/`round_robin` LB, and the gRFC A6 retry engine).
* **gRPC server leftovers**: ~~moving health `Watch` onto the streaming dispatch path~~ (done).
* **Distribution**: ~~publish the Homebrew formula~~ (done: `brew tap c4punks/cwist`, `brew install c4punks/cwist/cwist`, verified end-to-end on Linuxbrew). vcpkg stays an in-tree draft under `packaging/vcpkg/`; upstream submission postponed.
* **WASM client-side support**: bring CWIST handlers into the browser WASM ecosystem.
  * ~~In-memory HTTP dispatcher~~ ✅ (`cwist_app_dispatch_memory()`): run `cwist_app` routing and handlers directly on request/response memory buffers, with no sockets — so the same C handlers run inside a Service Worker or a JS fetch-interception layer.
  * ~~`libcwist_wasm.a` target~~ ✅ (`make wasm`): an Emscripten build that excludes the native transport (sockets, TLS, QUIC) and ships the socket-independent core — app dispatch, mux/middleware, HTTP/1 parser/serializer, query map, cJSON, memory utilities.
  * ~~`cwist_db` in-memory/blob abstraction~~ ✅ (`cwist_db_open_memory()` / `cwist_db_serialize()`): official API over `sqlite3_deserialize`-style memory buffers, so WASM apps don't need to parse custom binary blobs.
  * ~~TypedArray zero-copy serialization helpers~~ ✅ (`<cwist/wasm/typedarray.h>`): Emscripten-side macros/headers that map C struct arrays directly onto `HEAP` TypedArrays instead of round-tripping through `snprintf` JSON.

---

## v3.5 Milestone (Released 2026-09-15)

Theme: **Full GC — automatic resource reclamation**. Today CWIST relies on explicit destroy-family calls (`cwist_app_destroy`, connection/session teardown, `cwist_free`) paired with arena discipline. v3.5 makes cleanup automatic where it matters: threads and processes ending must close connections, and `cwist_alloc` objects must evaporate without manual frees. Design document: `docs/GC.md`.

* **`cwist_full_gc(bool)` global toggle**: ~~when enabled, connections are closed automatically on worker-thread exit and process exit even without explicit destroy calls; when disabled (default), the current explicit model runs unchanged at zero cost~~ (done — `src/core/mem/gc.c`, see Tutorial 30).
* **Connection reclamation via libttak epoch GC** (`ttak_epoch_gc`, already wrapped by `src/core/mem/gc.c`): ~~per-thread connection registration with epoch-deferred teardown, so a connection swept at thread exit can never UAF a thread still referencing it~~ (done — `test_conn_registry`, `test_gc_ebr_release`, `test_full_gc_sweep`).
* **`cwist_alloc` registration**: ~~allocations route into the full-GC context (internal changes to `src/core/mem/alloc.c`) so objects are reclaimed by epoch rotation instead of manual `cwist_free` calls~~ (done — gated on `cwist_full_gc_enabled()`, registered via `cwist_reg_ptr_sized`).
* **Pseudo-RAII**: ~~scoped guards via GCC/Clang `__attribute__((cleanup))` for handle-like locals, plus raw borrowing of LibTTAK RAII primitives~~ (done — `CWIST_DEFER_FREE`/`CWIST_SCRATCH_DEFER` in `include/cwist/core/mem/alloc.h`, used in tutorials 07/28).
* **Transparent `malloc` interception**: ~~users will habitually write `malloc`, not `cwist_alloc` — so handler-thread `malloc` calls should evaporate the same way `cwist_alloc()` calls do~~ (done — `include/cwist/core/mem/intercept.h`, opt-in per translation unit via `#define CWIST_INTERCEPT_MALLOC` before including it). Header-scoped `#define malloc cwist_malloc_shim` (never `-Wl,--wrap=malloc` — link-level wrapping was considered and rejected, since it would also intercept vendored dependencies like BoringSSL/lsquic that never include CWIST headers and have no reason to expect a non-libc allocator underneath them, e.g. BoringSSL's `OPENSSL_cleanse()`-then-free assumes plain heap semantics). Covers `calloc`/`realloc`/`free` consistently from the same seam (`src/core/mem/alloc.c`); reclaim cadence reuses the existing `cwist_gc_scope_track`/`cwist_gc_scope_flush` pipeline unchanged; cross-thread handoff reuses the existing `cwist_gc_scope_disown()` escape hatch. Measured overhead (`tests/bench_malloc_intercept.c`): the measured overhead lives in `docs/GC.md` section 5 (table and methodology, refreshed when the mechanism changes).
* **Thread/process exit sweeps**: ~~thread-local connection registry with pthread TLS destructors for worker exit, and an `atexit` sweep for process exit~~ (done — `test_io_queue_full_gc`, `test_full_gc_ownership_handoff`).
* **Evaluate a `mimalloc` backend for `cwist_alloc`/the epoch-GC heap** (tracked in #25): ~~evaluate via `LD_PRELOAD` A/B against jemalloc and tcmalloc, then vendor mimalloc if the data holds up~~ (done, result: **negative** — real CI A/B (PR #34) showed mimalloc regressing every metric on CWIST's prefork C1M model: RPS −1.8%, P99.999 +97%, RSS +120%, root-caused to mimalloc's eager per-process arena reservation being a poor fit for many short-lived low-allocation forked processes. `mallopt(M_ARENA_MAX, 1)` (`CWIST_MALLOC_ARENA_MAX`, PR #35, merged) targets the same "N processes × M arenas" mechanism without mimalloc's reservation cost and won on every metric instead — see issue #25 for the full writeup. Note: the P99.999-vs-Axum gap that originally motivated this item was measured via the-benchmarker's public `percentile99999` field, which turned out to be mislabeled P99.99, not true P99.999 (found during PR #48's validation, also on #25) — the qualitative direction (Axum ahead on tail latency) likely still holds, but the "2.8–3.6x" figure specifically should not be cited as P99.999 going forward).
* ~~**Full-GC malloc interception overhead** (tracked in #65)~~ (done: the pending-sweep list was scanned linearly on every free, so a free cost time proportional to the blocks the thread held; it is now a per-thread hash set reached through a thread-local pointer, flat across live-set sizes. Measurements and method in `docs/GC.md`, "Known performance caveat"; `tests/bench_full_gc_tracking.c` reproduces them).

---

## v3.6 Milestone (Released 2026-09-21)

Theme: **WASM client-side support**. v3.4 shipped the gRPC client wave; v3.5 shipped full GC; v3.6 takes the WASM client-side support wave from "in-tree target" to "usable from JavaScript". Tracked in issue #93.

* **Phase 1: in-tree WASM correctness** (issue #93 gaps 1-4, PR #176) — ~~done~~ (merged 2026-09-17):
  * ~~`cwist_db` in the WASM build (`src/core/db/db.c` + `lib/sqlite3/sqlite3.c` in `WASM_SRCS`), so `cwist_db_open_memory()` / `cwist_db_serialize()` work under Emscripten as the roadmap has long claimed~~ (done).
  * ~~Fix the EM_JS corruption from the tree-wide clang-format pass (`=>` rewritten to `= >` inside brace-block JS bodies); `clang-format off/on` guards plus `make format-check` coverage so it cannot recur~~ (done).
  * ~~Emscripten build + smoke test as a CI gate (`.github/workflows/wasm.yml`)~~ (done; scoped to the wasm files after runner clang-format version skew produced false tree-wide failures).
  * ~~`docs/api/wasm.md`: build, scope, the `dispatch_memory` pattern, TypedArray helpers, the db round trip, session caveats~~ (done).
* **Phase 2: JavaScript consumption** (issues #183/#184, PR #185) — ~~done~~ (merged 2026-09-18; both issues auto-closed):
  * ~~npm/release packaging for `libcwist_wasm.a` and the smoke-tested artifact~~ (done — `wasm/npm/` publishes the `cwist-wasm` package: `index.js` fetch-style API, `index.d.ts`, README; `make wasm-dist` builds the tarball, CI verifies a clean install).
  * ~~First-party JS wrapper exposing the `dispatch_memory` request path and TypedArray views without requiring consumers to write Emscripten glue~~ (done — `include/cwist/wasm/wasm_entry.h` `CWIST_WASM_DEFINE_ENTRY`, plus the `_main` export pitfall documented: without it the linker dead-code-eliminates `main`).
* **Phase 3: streaming + session model** (done on `feat/wasm-phase3`):
  * ~~WASI target evaluation~~ (done — the blockers recorded in
    `docs/api/wasi.md` turned out to be CWIST-side quirks, so both WASI
    flavours landed in-tree instead of staying a separate workstream:
    preview1 (`wasm32-wasi`) runs the in-memory dispatch surface under
    wasmtime, and WASI 0.2 (`wasm32-wasip2`) binds real sockets and serves
    cleartext HTTP through `wasi:sockets`; experimental, see
    `docs/api/wasi.md`).
  * ~~Streaming request/response bodies through the WASM boundary~~ (done:
    `cwist_app_dispatch_stream` + `cwist_stream_req_begin/feed/end` in
    app.h/app.c; boundary streaming, not a chunked-producer handler API.
    The `CWIST_WASM_DEFINE_ENTRY` macro exports
    `_cwist_wasm_dispatch_stream`, pumping chunks through a
    `Module.cwistStreamChunk` JS hook).
  * ~~Session persistence model for WASM apps~~ (done and measured: the
    model is "pin the signing secret, let the signed client-side cookie
    carry the state" - instance lifetime is irrelevant. The WASM build now
    actually links sessions via a bundled header-only SHA-256/HMAC
    (`include/cwist/core/crypto/sha256.h`; OpenSSL is not in WASM_SRCS, so
    pre-Phase-3 session.o could never link), with a
    `crypto.getRandomValues` entropy fallback when /dev/urandom is absent.
    Verified across app instances in `tests/test_wasm_stream.c` and the
    Emscripten smoke test; documented in `docs/api/wasm.md`).
* **Phase 4: reach** (issue #93, PR #198) — ~~done~~ (merged 2026-09-21):
  * ~~End-to-end example app (Service Worker or fetch-interception layer)~~ (done — `example/wasm-service-worker/`: a full CWIST app (routing + zod validation + template rendering + `cwist_db` + pinned-secret sessions) served by a Service Worker fetch-interception layer with its own cookie jar; CI-gated via the WASM job's example build + node smoke).

Landeds alongside the WASM wave, also in scope for v3.6:

* **Per-event latency probe** (issue #166, PR #186, merged): `CWIST_LATENCY_PROBE=1` records arm-to-dispatch queue delay and callback runtime histograms per reactor, dumped at destroy. First measurement at the CI operating point: queue delay p99 = 10 ms while callback p50 = 25 us — the tail lives before dispatch.
* **RX-uring receive path** (issue #179, PR #187, merged): one `IORING_OP_RECV` SQE replaces the POLL_ADD + recv() pair on the C1M async path (Linux io_uring reactors only; `CWIST_RX_URING=0` restores legacy byte-identically). A per-connection learn flag keeps non-pipelining clients at the legacy op count (measured neutral: 342.4k vs 341.7k rps, t12 c400); the pipelining win case is unproven pending a pipelining workload.
* **WebSocket non-blocking I/O** (issue #181, PR #182, merged): reactor-driven WebSocket on C1M (classic mode keeps blocking I/O), callback-style API, `CWIST_WS_ASYNC_IDLE_TIMEOUT_SEC` (default 300 s).
* **SQPOLL evaluation** (closed, negative): kernel SQ-thread wakeup discipline on 6.12 makes producer-side SQPOLL at best equal to the polled ring and at worst a multi-ms stall source; SQ_AFF stabilizes it but never beats the plain submit-enter (issue #179 comments).

Known limits going in (from PR #176 review), updated:

* ~~Bundle size impact of pulling SQLite into `libcwist_wasm.a` is unmeasured~~ — measured and decided (2026-09-21): `libcwist_wasm.a` 328,518 -> 1,703,706 B (5.2x), but the linked-size cost is paid only by apps that reference `cwist_db_*` — a db-less app still links ~64.8 KB, the pre-db size — and for db users the ~1 MB is SQLite's reachable core (`-ffunction-sections`/`--gc-sections` recover ~0; emcc -O2 already DCEs cross-module). Decision: no opt-out/split build; the remaining lever if ever needed is `SQLITE_OMIT_*`, not packaging. Details in `docs/api/wasm.md`.
* ~~Session behavior under the WASM dispatch model is documented but not yet measured; Phase 3 needs observed behavior, not the current caveats list~~ — now measured (Phase 3: cross-instance verify/reject in `tests/test_wasm_stream.c` and the Emscripten smoke test; model documented in `docs/api/wasm.md`).

---

## v3.7 Milestone (Released 2026-09-24, emergency patch v3.7.1 on 2026-09-29)

Theme: **the last experimental train before v4 stabilization**. v3.6 took
WASM from "in-tree target" to "usable from JavaScript"; v3.7 is the last
release where experimental capability lands before the v4.0 scope is
confirmed. Everything experimental rides here first, behind flags and
documented as experimental, so that v3.8 can confirm the v4.0 scope (which
of these items v4.0 supports, keeps opt-in, or removes) and v4.0 (the first
stable, production-compatible line) can spend its cycle on stabilization,
semver commitments, and soak-driven promotion decisions. Stabilization
through v4.0 builds the base for expansion, which resumes in v4.1. Tracked
in issue #201.

Entry criteria for anything joining v3.7 after this retheme: shipped
behind a flag or clearly marked experimental in the docs, revertible, and
with a named v4.0 promotion decision (default-on, supported, or removed).
The release-window rule is unchanged: v3.7 ships on schedule, and what is
not ready slips — scope grows only by pulling validation forward, never by
delaying the cut.

* **Phase 1: WASI edge deployment** (from 🔮 "Serverless / WASM Runtime"):
  * ~~Promote the WASI 0.2 (`wasm32-wasip2`) socket server from experimental to supported: CI gate (build + wasmtime `wasi:sockets` smoke, mirroring `wasip2-smoke`), and reframe `docs/api/wasi.md` from evaluation to reference documentation~~ (done — `wasip2` job in `.github/workflows/wasm.yml`; `docs/api/wasi.md` reframed to reference).
  * ~~Edge persistence pattern: `cwist_db_serialize()` / `cwist_db_open_memory()` round trip against a host KV-style store, with an example app~~ (done — `example/wasip2-kv/` round-trips the blob through a preopened dir; `smoke.sh` proves it survives a wasmtime restart).
  * ~~Deployment examples and guides for at least one edge runtime (Cloudflare Workers or a wasmtime appliance setup)~~ (done — [Wasmtime appliance guide](docs/deployment/wasmtime-appliance.md): isolated state, restart verification, backup, and rollback; production limitations documented).
  * ~~WASM streaming producer API~~ (done — `cwist_http_response_stream_begin/write/end`; immediate per-chunk delivery under `cwist_app_dispatch_stream()`, buffered chunked serialization under `cwist_app_dispatch_memory()`; `test_stream_producer`).
  * ~~Component experiment (toward replacing the Emscripten bundle)~~ (done, beyond the original ask — WIT world validated in CI, jco guests over preview2-shim and preview3-shim under JSPI, browser-bundle packaging gate, and the async `host.send-chunk` streaming world; see `docs/api/wasm-component.md`. The Emscripten swap itself remains gated on WASI 0.3 stabilization and unflagged JSPI — a v4.0 decision, not a v3.7 one).
  * ~~Retire the WASI preview1 target (`wasi-smoke`)~~ (done — target, archive rule, smoke source, CI reference, and docs removed; 0.2 covers its use).
* **Phase 2: WebTransport on the stable line**: ~~slipped to v3.8, then to v4.1~~ (see the v4.1
  section below): LSQUIC PR #629 has not merged, and the release-window rule
  says v3.7 ships without WebTransport rather than pinning to a topic branch.
* **Phase 3: HTTP/3 connection-close correctness** — ~~slipped to v3.8~~ (deferred
  indefinitely): the CWIST-side half is done, but the lsquic re-pin is on hold
  until upstream ships WebTransport client support; the three close-fix PRs fold
  in with that re-pin rather than separately.
* **Phase 4: ecosystem experimental support** (shipped behind flags, documented as experimental):
  * ~~gRPC server-side response compression~~ (done 2026-09-07 in `31b44d6f`, pre-v3.7; marked here so Phase 4 tracks only what remains).
  * ~~GraphQL subscriptions over the v3.6 non-blocking WebSocket transport~~ (done — graphql-transport-ws subprotocol in `cwist/graphql_ws.h` with a topic broker (`cwist_graphql_publish`) and reactor-thread-safe fanout; `test_graphql_subscriptions` covers the close-code matrix, streaming, and teardown purge).
  * ~~Persistent job backends: a durable queue over the existing Redis/NATS clients, separate from the in-process scheduler queue~~ (done — `cwist/sys/job/durable_queue.h`: at-least-once queues, Redis streams+consumer groups (XAUTOCLAIM visibility, Lua nack/dead-letter) and NATS JetStream pull consumers; `test_durable_queue` runs against live Redis and skips NATS cleanly without a server).
* **Phase 5: pre-v4 experimental promotion** (new under this retheme — give the dev-only experiments a release-line soak so v4.0 can decide their fate with data):
  * Memory management: full-GC (`CWIST_DEFER_FREE`, EBR path, thread/process-exit sweep) and header-scoped malloc interception (`CWIST_INTERCEPT_MALLOC`) **promoted to supported opt-in** in `docs/GC.md`. The v3.7 decision was already *opt-in, both items*; v3.8 enacts that promotion out of experimental. The overhead of enabling the safety net is real (measured in `tests/bench_malloc_intercept.c`, table and methodology in `docs/GC.md` §5) — it is priced for handler authors who want it, not a tax every stable-line deployment should pay. The revert path is "do not define the macro" / "do not call `cwist_full_gc(true)`"; defaults remain unchanged (full-GC off). Revisit only if the soak turns up correctness regressions.
  * `CWIST_PROFILE` presets and the C1M baseline work: confirm the preset matrix is the v4.0 default story or trim it. **Decision (2026-09-24): keep the matrix as the v4.0 default story, no trim.** The four presets (`performance`/`lowmem`/`lowlat`/`default`) are thin setenv overlays with per-variable escapes (overwrite=0), so the surface is already minimal. On the default: issue #166's tail analysis attributes the extreme-tail gap to neither dispatch model, so C1M-on + drain-chunk 8 remains the defensible throughput-oriented default, with `CWIST_PROFILE=lowlat` as the documented escape for latency-first deployments. `docs/cooperative-queuing.md` documents the matrix for every profile.
  * The env-gated per-event latency probe and HTTP batch shed metrics: promote, hide, or drop. **Decisions (2026-09-24), one per item:**
    * *Latency probe (`CWIST_LATENCY_PROBE=1`, issue #166 tooling):* **hide — stays opt-in.** Disabled cost is one cached atomic load and a branch per arm/record (the `clock_gettime` calls are guarded and never run when off); enabled cost is two clock reads plus a histogram update per request. Its job — attributing the extreme tail between queue and service time — is a diagnostic activity, not steady-state observability; steady-state belongs to the always-on Prometheus metrics. No default-on, and no sampling mode is planned for v4.0 unless #166 (or its successor) asks for one.
    * *HTTP batch shed metric (`cwist_http_continuation_shed_total`):* **promote — already de-facto stable, keep always-on with no gate.** The counter increments only when a pipelined continuation is shed because the reactor post queue is full — a pathological path where the connection is closed anyway. Gating it would add a branch on a path that executes roughly never. It already ships ungated in the Prometheus exposition, so "promote" here means the v4.0 line keeps it unconditional rather than hiding it behind a knob.
  * Each item needs: experimental docs, a revert path, and the promotion decision recorded here before v4.0 cuts. (All three items now have their decision recorded above; the docs and revert paths were already in place.)

---

## v3.8 Release Criteria and v4.0 Preview

The detailed plan for v3.8 lives in the [CWIST v3.8 Roadmap](#cwist-v38-roadmap-released-2026-10-01) section above. This section keeps the release gate and the v4.0 transition note in one place.

**Release criteria for v3.8:**
- Every Phase 1 item has before/after numbers or a recorded negative result.
- `bindings/rust` builds and passes tests in CI on Linux and macOS, the Rust
  example serves requests, and the FFI overhead is measured.
- The C API additions for FFI are additive only; no existing symbol changes.
- Phase 3 (HTTP/3 connection-close correctness) remains deferred and is
  explicitly not a v3.8 release blocker.
- Every experimental item has a recorded v4.0 decision, and the v3.7 Phase 5
  decisions are enacted in code and docs.
- CI is green on the exact release commit.

**v4.0 preview:** v4.0 starts the API stability guarantee. From v4.0 on,
existing public API is not changed, and new features keep being implemented
as new API in any release. Deprecated APIs and flags are resolved (promoted or removed) before the cut.
The v4.0 cycle itself focuses on correctness, soak, docs, and the promotion
decisions, so that expansion can resume in v4.1 on a stable base. See "API stability from v4.0" under the versioning
rules.

---

## CWIST v3.9 Roadmap (Released 2026-10-05)

v3.8 spends its cycle on performance, Rust FFI, and v4.0 scope confirmation.
The follow-up TLS investigation for issue #306 (PR #307 shipped the
TCP_QUICKACK handshake fix; shard/teardown/buffer follow-ups all measured
no-gain) leaves one clear gap: **TLS observability & performance
governance**. There is no way today to see handshake health in production
or to stop a TLS regression from landing silently. v3.9 closes that, and
also brings WebRTC DataChannel support into the release scope.

### v3.9 Status at a Glance

| Goal | Status | Notes |
|------|--------|-------|
| (a) CI TLS performance gates (HTTPS churn / keep-alive / large-transfer / RTT) | ✅ Done | Gates landed in #309 but never passed: the absolute backstops came from fast CPUs and healthy code failed on the EPYC 7763 runner, so no history accumulated either. Recalibrated on https/http ratios plus same-CPU history; disabling the #307 fix fails the RTT gate (0.08 → 43 ms) on any CPU. The churn-ratio backstop had to be loosened after the first Intel runner (healthy at 0.071 vs 0.11-0.13 on AMD). Green on `dev` at 610fe55b (https://github.com/c4punks/CWIST/actions/runs/37303818384). Tracked in #306 |
| (b) TLS observability in Prometheus `/metrics` (handshake counts, TLS version/cipher counters, resumption vs full-handshake ratio) | ✅ Done | `cwist_tls_handshakes_total`, `cwist_tls_handshakes_resumed_total`, `cwist_tls_connections_active`, `cwist_tls_handshakes_tls12_total`, `cwist_tls_handshakes_tls13_total`, `cwist_tls_ciphers_{aes128_gcm,aes256_gcm,chacha20,other}_total`; covered by `test_https_metrics` |
| (c) Settle issue #294 with measured data | ✅ Done | #294 closed as completed. An earlier comment there claimed `cwist_app_listen` never uses the sharded handshake shepherds; that was wrong (it confused `cwist_app_multiport`'s inline path with `cwist_app_listen`). Measured: `cwist_app_listen` adds one thread per `CWIST_HTTPS_HS_SHARDS` shard on the first HTTPS request, in both C1M and classic mode. The default floor of 4 shards / cap of 16 is already on dev, and #294's data shows no gain beyond 4 |
| (d) WebRTC DataChannel support | ✅ Done | #310: SDP offer/answer, ICE-lite, DTLS (vendored BoringSSL), SCTP DataChannels (`lib/usrsctp`) on the cwist reactor. Echo verified against headless Chromium (`make test_webrtc_browser`); Firefox not yet tested |

Entry criteria for v3.9: every item must move a measured metric (handshake
throughput, resumption ratio, connection-churn latency, or regression
detection latency) or retire a mismeasured premise. New public API is
additive only, matching the v3.8 rule.

Exit criteria for v3.9:

- The CI TLS gate PR (#306) is green on the release commit and fails on a
  re-introduced #307-class regression (measured by the churn benchmark).
- `/metrics` exposes the full TLS counter set above on a live app, and the
  resumption-vs-full ratio is observable across prefork workers.
- #294 is closed with measured data (done: closed as completed; the
  shepherds do serve `cwist_app_listen`, and the shipped default needs no
  change).

---

## CWIST v4.0 Readiness

v4.0 starts the API stability guarantee described under "API stability from
v4.0" in the versioning rules below. This section lists what is still open
before the v4.0 cut, as checked on `dev` at 4cd05e21 (2026-10-07). It records
open work and pending decisions, not results.

### API surface decisions

After v4.0 an existing public API can no longer change, so each item below
needs a recorded decision before the cut.

| Item | Current state | Decision needed |
|------|---------------|-----------------|
| WASM component pipeline (#203) | Experimental; deferred to v4.0 in v3.8 Phase 4; gated on WASI 0.3 / unflagged JSPI | Promote at v4.0, or ship v4.0 with it marked experimental and outside the guarantee |
| `cwist_http3_set_stream_priority()` | `@deprecated` in `http3.h`; kept for ABI compatibility; always logs a warning and returns -1 | Remove before the cut, or keep it with the always-refuse behavior as its permanent contract |
| `cwist_http_stringify_response()` | Declared in `http.h` without a deprecation marker; a source comment in `http.c` calls it "Deprecated / Debug only"; covered by `test_http_stringify` | Keep it as supported API and drop the comment, or mark it deprecated and remove it before the cut |
| WebTransport client API (`http3_client.h`) | Marked experimental (LSQUIC PR #629) | None for v4.0: stays experimental and outside the guarantee until v4.1 |
| Public API baseline | No recorded list of public symbols and public struct layouts exists | Record the v4.0 baseline. Possible follow-up: a CI check that diffs headers against it |

The guarantee as written covers the C public API. The Rust crates
(`cwist-sys`, `cwist`, 0.1.0, documented as experimental in Phase 2 above)
and the Zig bindings (`bindings/zig`, 0.1.0) are versioned separately.

### Performance target (#319)

* Target: at least 1.05x Actix-web throughput and a lower P99.999 than
  Actix-web on every CI runner architecture (AMD EPYC, Intel Xeon).
* Done: Actix-web is in the CI webserver benchmark matrix and charts, and
  the measurement contract is in `docs/webserver-benchmark.md`.
* Open: #297 (syscall and serialization reduction: io_uring multishot
  accept with a fallback for kernels without it, SQE batching, per-accept
  `setsockopt`, time caching), #293 (tail latency; queue delay drives the
  tail), and #322 (libttak v3.4.0 benchmark comparison). Each item lands
  with an A/B measurement or a recorded negative result.

### Carry-over and housekeeping

| Item | State on `dev` at 4cd05e21 | Remaining |
|------|----------------------------|-----------|
| #306 TLS performance | Fixed by #307; v3.9 HTTPS gates green on `dev` | Close the issue |
| #286 raw-allocator gate | `https.c` parked-connection allocations use `cwist_alloc`/`cwist_free`; `cwist audit --gate` passes | Close the issue |
| Soak testing | Named in the v4.0 preview; no soak job or plan exists in the tree | Define the soak run and its pass criteria |
| Docs good first issues | #272 landed in #320; PRs open for #273 (#324) and #275 (#325, #326); #271 open | Review and merge |

### Not in v4.0 scope

* WebTransport (#17) and the lsquic re-pin: v4.1, see below.

### Exit criteria for v4.0

- Every API surface decision above is recorded and enacted in code and
  docs; deprecated APIs and flags are resolved (promoted or removed).
- The #319 performance target holds on the CI benchmark on every runner
  architecture.
- The soak run is defined and passes on the release commit.
- The release rules in `CONTRIBUTING.md` hold on the release commit (every
  CI workflow green; `make dist` archive builds and tests clean).

---

## v4.1 (Planned): expansion resumes, starting with WebTransport

v4.1 is where expansion resumes after the v4.0 stabilization cycle. The
first item is WebTransport, moved from v3.8 on 2026-09-25 (issue #17). It
arrives as new API; nothing that exists at v4.0 changes.

* Precondition: upstream lsquic ships WebTransport client support (previously
  tracked as LSQUIC PR #629); then re-pin `lib/lsquic` to that upstream release.
  No topic-branch pin. The same re-pin also folds in the HTTP/3 connection-close
  correctness fixes deferred from v3.8.
* Port the dev-branch WebTransport server API (`cwist_http3_transport_*`,
  stream accept/read/write), the native C client, and
  `example/webtransport/`.
* `test_webtransport`: session negotiation, bidirectional streams, datagrams
  if enabled, and graceful teardown.
* Soak against at least one other peer (`aioquic` or Chromium) before the
  experimental flag is removed.

---

## Release Line & Codenames

* The 3.x line is stabilization work on the road to v4.0, the stable base that expansion resumes from in v4.1: release intervals are deliberately long, and each release lands a small number of large, well-tested changes rather than frequent small ones. Expect wide gaps between 3.x tags.
* The first 100% production-compatible stable release is planned as **v4.0**. Until then, minor releases may adjust public APIs (see the versioning note in the README).
* Starting with the stable line (v4.0 onward), each release receives a codename in the form **adjective + color** (e.g. "Steady Amber"). Codenames are assigned at release time and recorded here.

### Versioning rules (as practiced)

The tag history (`v0.1` → `v3.3`) settles into this convention from v3 onward, and it is the rule going forward:

* **Tags**: `v<major>.<minor>` for feature releases (`v3`, `v3.1`, `v3.2`, `v3.3`). Urgent fixes to a released tag get a patch level, `v<major>.<minor>.<patch>` (`v2.5.1`, `v2.4.1`) — patches are for hotfixes only, never for features.
* **Major** (`v2` → `v3`): a generational milestone — a broad capability jump (e.g. v3 = first reliable release, HTTP/2 stabilization). Majors are rare.
* **Minor** (`v3.2` → `v3.3`): one coherent feature theme (v3.2: HTTP/3 standards compliance + security hardening; v3.3: gRPC streaming + deferred async handlers). A minor is cut when its theme is complete, not on a calendar.
* **Release title**: `CWIST vX.Y` followed by an em-dash summary of the headline theme ("CWIST v3.3 — gRPC streaming, deferred async handlers, and stability hardening"). Pre-v3 releases used freeform subtitles ("Firefox Compatibility"); the em-dash form is the standard now.
* **Release body**: "Highlights since vX.(Y−1)" or "Major changes compared to vX.(Y−1)", grouped into numbered/sectioned items with commit references where useful.
* **API stability from v4.0**:
  * Existing public API is not changed: public symbols, signatures,
    documented behavior, and public struct layouts stay as they are.
  * New features keep being implemented. They land as new API in any minor
    release (patches stay hotfix-only). A new API marked experimental is
    outside the guarantee until it is promoted.
  * When an existing API needs different behavior, a new API is added next
    to it; the existing one stays as it is.
* The 0.x line was pre-1.0 experimentation; the 1.x–2.x lines were feature accretion with themed minors. None of that constrains the 3.x rules above.

---

## Priority Queue (Suggested)

### P0 — Framework Gap (Must Have) ✅ COMPLETE
1. ~~**Advanced Router** with parameterized routes and route groups~~ ✅
2. ~~**Multipart / File Upload** parser~~ ✅
3. ~~**Graceful Shutdown** unified across HTTP/1.1, HTTP/2, HTTP/3~~ ✅
4. ~~**Compression** (gzip at minimum, brotli preferred)~~ ✅
5. ~~**Form / Request Validation** middleware~~ ✅

### P1 — Production Readiness
6. **Access Logs** (Common/JSON format)
7. **Metrics endpoint** (Prometheus text format) 🔄
8. **Rate Limiting** middleware
9. **Caching** (ETag generation + in-memory cache)
10. **Health Check** endpoints 🔄

### P2 — Developer Velocity
14. ~~**Hot Reload** for development~~ ✅
15. ~~**CLI Tooling** (project scaffold, route generator, watcher)~~ ✅
16. ~~**Configuration** loader (`.env`, `.toml`)~~ ✅
17. ~~**Test Harness** with HTTP mock client~~ ✅
18. ~~**Deferred Async Handlers** (`cwist_async_defer` cross-thread completion)~~ ✅

### P3 — Advanced Protocols
15. ~~**WebTransport** server + client~~ ✅ (basic server handler)
16. ~~**HTTP/2 Server Push**~~ ✅
17. **io_uring** UDP packet loop for HTTP/3 🔄
18. **kqueue** backend for macOS/BSD

### P4 — Ecosystem
24. ~~**gRPC unary and buffered streaming server support**~~ ✅
25. ~~**GraphQL** bounded Query executor~~ ✅
26. ~~**OpenAPI** generator~~ ✅
27. ~~**Background Jobs / Scheduler**~~ ✅
28. ~~**Incremental gRPC framing, reflection, health checks, and `.proto` codegen**~~ ✅
29. ~~**gRPC wire streaming**: DATA-frame wiring, trailers, deadlines, gzip negotiation~~ ✅
30. ~~**Distribution**~~ ✅ (Homebrew tap published at `c4punks/homebrew-cwist`; vcpkg kept as in-tree draft, upstream submission postponed)

---

## Contributing

If you want to pick up an item, open an issue referencing this roadmap and the specific feature number.
