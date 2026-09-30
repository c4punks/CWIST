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
[P2: DevEx]          ██████░░░░░░░░░░░░░░  30% (Config loader and test tooling done)
[P3: Deep Protocols] ███████████████░░░░░  75% (HTTP/3 extension specs and io_uring backend done)
[P4: Ecosystem]      ███████████████░░░░░  75% (gRPC wire streaming, trailers, deadlines, gzip, and proto codegen done)
```

### v3.8 Release Progress

| Phase | Theme | Progress |
|-------|-------|----------|
| Phase 1 — Performance | Reactor latency, RX-uring pipelining, HTTPS handshake shards, worker warmup | Mostly done: #293 closed the measurement loop; RX-uring and worker warmup dropped; HTTPS shard tuning remains open |
| Phase 2 — Rust FFI | `bindings/rust/` (`cwist-sys` + `cwist`) | Not started |
| Phase 3 — HTTP/3 Close Correctness | Re-pin lsquic after upstream fixes | Blocked on lsquic #688, #687, #693; cutoff 2026-10-09 |
| Phase 4 — v4.0 Scope Confirmation | Enact v3.7 Phase 5 decisions, record experimental-item fates | Not started |

### 1) Transport Layer

* **Native protocols ready**: HTTP/1.1 through HTTP/3 (QUIC via `lsquic`), WebTransport server support, WebSocket, SSE, and a bounded GraphQL query layer are implemented in-tree. Low-level socket controls (ECN, 0-RTT, connection migration) are complete.
* **HTTP/3 browser hardening**: Response header emission now normalizes field names to lowercase and rejects CR/LF-bearing values, covering login/logout cookie and redirect paths in strict browsers such as Firefox.
* **Async I/O optimization (`io_uring`)**: `io_uring_backend.c`, SQE/CQE synchronization, demolition safety, and focused tests are complete.
* **Multiport HTTP/3 fan-out**: The `cwist_multiport_t` facade now creates per-port UDP contexts and copies global HTTP/3 settings unless a port is detached into a sub-app.

### 2) Application Layer

* **High-performance router & middleware**: Deterministic resource management with parameterized routes (`/user/:id`), compression (Gzip via zlib), CORS, and rate limiting (libttak token bucket) are integrated.
* **Observability**: Prometheus `/metrics` and a probe-registry health-check system are operational.
* **Per-port sub-applications**: Lifecycle and exception handling for `cwist_multiport_get_app(&app, port)` are hardened; detached ports are separately tunable sub-applications.
* **gRPC services**: Applications can register unary and streaming handlers, incrementally decode arbitrarily split gRPC frames, attach transport output sinks, expose standard health/reflection services, and generate C models/method paths with `cwist proto`.
* **Packaging**: `libcwist.a` is a CWIST-only static archive; bundled dependency archives and public headers install side-by-side with `PREFIX`/`DESTDIR` staging support. `cwist.pc` pkg-config metadata, a versioned dist tarball (`dist/cwist-3.2.tar.gz`), and Homebrew (`packaging/homebrew/cwist.rb`) / vcpkg (`packaging/vcpkg/`) packaging drafts are available.
* **Deferred async handlers**: `cwist_async_defer()` hands a request/response pair to any thread (scheduler job, NATS callback, custom worker) for later completion via `cwist_async_respond()` / `respond_with()` / `abort()`, with optional 504 timeout and per-mode completion routing (reactor-posted in C1M, inline in thread-pool mode).

### 3) Security & Data Layer

* **Security specs**: BoringSSL-based TLS 1.3 and hybrid post-quantum KEM (`X25519MLKEM768`) are implemented ahead of time. CSRF uses a 256-bit double-submit token with constant-time validation; WAF-lite uses bounded linear scans and HTML output escaping.
* **Data-layer integrity**: SQLite3 embedded integration, migration system, and a `_Generic` macro-based type-dispatched ORM/query builder are in the build stream. The lock-free work queue (`cwist_io_queue`) protects node reclamation with ttak EBR critical sections and a two-stage retire deferral across global-epoch boundaries, and scheduler-backed background jobs are implemented.
* **Protobuf wire helpers**: A lightweight Protobuf runtime supports varint keys, unsigned/signed/bool fields, length-delimited bytes/strings, reader iteration, and ZigZag helpers for hand-written services.

---

## Current Snapshot

<!-- CI-BENCHMARKS:START -->
Automated OS & Web Server benchmark histories are published in `docs/benchmark-trends.svg` and `docs/webserver-benchmark-trends.svg`. Latest platform: **Linux / Darwin**.
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
- **CWIST v3.8 in progress**: theme is *performance, Rust FFI, and v4.0 scope confirmation*. WebTransport moved to v4.1 on 2026-09-25.
- **Landed on `dev` since v3.7.2**: HTTP close-drain correctness (#292), HTTPS connection-churn optimization (#291), Rust listen-shutdown support (#287).
- **Performance sweep done** (issue #293): CWIST C1M already leads Axum on a single CI run; batch/yield is near-optimal; RX-uring pipelining and worker ttak warmup showed no win; HTTPS handshake shards are a real niche lever (shard=1 is a bottleneck, 4+ saturate).
- **Deferred**: HTTP/3 connection-close correctness and the lsquic re-pin are on hold until upstream lsquic ships WebTransport client support.

---

## 1. Transport Layer

| Feature | Status | Notes |
|---------|--------|-------|
| HTTP/1.1 Server (epoll, threading, forking) | ✅ | Zero-copy sendfile, keep-alive, and supervisor-to-worker shutdown propagation |
| HTTP/2 Server | ✅ | h2 with ALPN |
| HTTP/3 Server (QUIC) | ✅ | lsquic + BoringSSL, QPACK, 0-RTT, migration, push, resilience timeout knobs, lowercase/CRLF-safe response headers |
| HTTP/1.1 + HTTP/2 Client | ✅ | libcurl based, sync & async APIs |
| HTTP/3 Client | ✅ | lsquic based, async stream callbacks, auto-retry with exponential backoff, conn timeout knobs |
| WebSocket Server | ✅ | Upgrade, frame parsing, ping/pong |
| TLS 1.3 / HTTPS | ✅ | BoringSSL, ECH, persistent HTTP/1.1 requests, split header/body writes, and shared prefork session-ticket keys |
| Alt-Svc Header Injection | ✅ | HTTP/3 upgrade advertisement from HTTP/1.1/2 |
| **io_uring Backend** | ✅ | `io_uring_backend.c`, focused smoke tests, demolition tests, SQE/CQE synchronization, and fixed-buffer fallback |
| **kqueue Backend** | 🔄 | macOS GitHub Actions gate builds the kqueue-selected backend and runs focused regressions |
| HTTP/2 Server Push | ✅ | `cwist_http2_push_resource` with PUSH_PROMISE frame, HPACK encoding, server-initiated even stream IDs |
| **WebTransport** | 🔄 | Server sessions, streams, and datagrams are implemented with a browser example; native C client sessions await a client-capable QUIC dependency |
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
| **Caching Layer** | ✅ | ETag, Last-Modified, Cache-Control, 304 Not Modified for static files |
| **Rate Limiting** | ✅ | Per-IP token bucket via libttak; parameter respected |
| **CORS** | ✅ | Permissive CORS + preflight handler implemented |
| **SSE (Server-Sent Events)** | ✅ | Buffered and live structured events, IDs, retry directives, multiline data, comments, and convenience macros |
| **Access Logging** | ✅ | Common, Combined, and JSON formats implemented |
| **Request ID / Tracing** | ✅ | X-Request-Id middleware injects and propagates request IDs |
| Graceful Shutdown | ✅ | Unified atomic `running` flag + SIGTERM/SIGINT handlers across HTTP/1.1, HTTP/2, HTTP/3 loops |
| **Health Check Endpoint** | 🔄 | Basic `/healthz` endpoint added (`healthz.c`, `healthz.h`) |
| **Metrics / Observability** | 🔄 | Metrics module added (`metrics.c`, `metrics.h`, `test_metrics.c`); Prometheus endpoint pending |
| **Per-Status Error Handlers** | ✅ | `cwist_app_register_error_handler` for custom 404, 500, etc. |
| **URL Reverse Routing** | ✅ | `cwist_app_get_named` + `cwist_url_for` with param substitution |
| **Flash Messages** | ✅ | One-time session-scoped messages via `cwist_flash_get/set` |
| **Per-Port Sub-Applications** | ✅ | `cwist_multiport_get_app(&app, port)` detaches additional ports for independent tuning; public/default port remains owned by root app |
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
| **Secure Headers** | ✅ | Automatic injection of HSTS, CSP, X-Frame-Options, Referrer-Policy, CORP via `cwist_http_response_add_security_headers()` |
| **Request Size Limits** | ✅ | HTTP/1.1/2/3 body limits audited and enforced (`CWIST_HTTP_MAX_BODY_SIZE`) |
| **Input Validation** | ✅ | Bind validator added (`bind.c`, `bind.h`, `test_bind.c`) |
| **WAF-lite / Sanitization** | ⏳ | No XSS/SQLi sanitizer middleware |

---

## 4. Data Layer

| Feature | Status | Notes |
|---------|--------|-------|
| SQLite Integration | ✅ | `sqlite3` embedded |
| Database Migration | ✅ | `migrate` system |
| **Connection Pool** | ⏳ | SQLite is direct; no generic connection pool abstraction |
| **ORM / Query Builder** | ✅ | Socket-backed ORM with dialect-aware query builder, `_Generic` type-dispatched RETURNING / scalar helpers |
| **Redis / Key-Value Cache** | ✅ | RESP2 client/pool, binary-safe argv commands, AUTH/SELECT, pub/sub, and app-level pool integration |
| NATS Integration | ✅ | `cwist_nats` wrapper |
| **Message Queue (Job Queue)** | ✅ | `cwist_io_queue` lock-free job queue plus scheduler-backed immediate and delayed jobs |

---

## 5. Developer Experience

| Feature | Status | Notes |
|---------|--------|-------|
| Doxygen Docs | ✅ | Generated HTML docs |
| README / API Reference | ✅ | Markdown docs in `docs/` |
| **Tutorial & Examples** | ⏳ | Few examples; no step-by-step tutorial |
| **CLI Scaffolding** | ✅ | `cwist new project`, `.cwpro` manifests, OpenAPI generation, and include-aware incremental watcher |
| **Hot Reload (Dev Mode)** | ✅ | `cwist watcher` uses inotify/kqueue with snapshot/poll fallback, debounces changes, exports include-graph recompilation scope, preserves the prior process on build failure, and gracefully restarts successful builds |
| **Configuration Management** | ✅ | `.env` file + environment variable loader via `cwist_config` |
| **Static Library Packaging** | ✅ | CWIST-only archive plus separately installed bundled libraries/headers, deterministic static-link order, `PREFIX`/`DESTDIR` staging, `cwist.pc` pkg-config file, versioned dist tarball, and Homebrew/vcpkg packaging drafts |
| **Testing Utilities** | ✅ | In-process test client (`cwist_test_client`), cookie jar, multipart helper, and BDD-style fluent assertions (`CWIST_ASSERT_STATUS`, `CWIST_ASSERT_HEADER`, `CWIST_ASSERT_BODY_CONTAINS`) |
| **Interactive API Documentation** | ✅ | Embedded Swagger UI interactive documentation page (`cwist_app_enable_swagger`) serving `/docs` and `/openapi.json` |
| **Benchmark Suite** | ✅ | GitHub Actions Linux/macOS measurements publish CPU, throughput, RSS, memory-recovery drift, and context-switch SVG trends; `benchmarks/web-frameworks/` contract app pinned to `v3.3` for the-benchmarker harness |
| **Interop Gate** | ✅ | h2spec HTTP/2 conformance diff against a pinned baseline (`scripts/ci/h2spec_gate.sh`); new failures break the build. Builds run with `-Werror`, stack protector, `_FORTIFY_SOURCE=2`, PIE, and full RELRO on Linux |
| **Fuzzing / Hardening** | ✅ | Stateful sequence/auth libFuzzer coverage plus bounded reassembly and strict HTTP chunk framing checks |

---

## 6. Ecosystem & Integrations

| Feature | Status | Notes |
|---------|--------|-------|
| **gRPC over HTTP/2** | ✅ | Unary/stream registration, split-frame incremental decoder and output sink, standard health/reflection service registration, gRPC metadata, and test-client coverage |
| **Protobuf Runtime Helpers** | ✅ | Wire-format reader/writer for varint, bool, bytes/string, signed integer casting, and ZigZag helpers |
| **GraphQL** | ✅ | Full query/mutation engine, field arguments, variables, aliases, nested selection sets, error envelope, and HTTP adapter |
| **OpenAPI / Swagger Generation** | ✅ | OpenAPI 3.1 JSON generated from Doxygen `@openapi.*` annotations on route declarations |
| **Background Jobs / Scheduler** | ✅ | `cwist_scheduler` worker pool with immediate and delayed job execution |
| **WebRTC** | 🔮 | Real-time media; requires separate data channel stack |
| **Serverless / WASM Runtime** | 🔮 | Edge deployment target; WASI 0.3 component pipeline replaces the Emscripten browser bundle once the 0.3 world stabilizes (wasi-sdk / wasmtime / jco); see the v3.7 Phase 1 experiment and the tracked issue |

---

## CWIST v3.8 Roadmap (In Progress)

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
| **1 — Performance** | Close reactor/Classic/Axum latency gaps; validate RX-uring pipelining; HTTPS handshake shards; worker warmup | Mostly done | #293 has the data; decide on `CWIST_HTTPS_HS_SHARDS` default; RX-uring and worker warmup dropped |
| **2 — Rust FFI** | Make CWIST callable from Rust (`cwist-sys` + `cwist`) | Not started | Create `bindings/rust/`; add additive `_ex` route registration; export `static inline` wrappers; add layout assertion tests; write `example/rust-hello/`; integrate `cargo test` into CI |
| **3 — HTTP/3 close correctness** | Re-pin lsquic when upstream fixes land; add CONNECTION_CLOSE interop gate | Blocked on upstream | Track lsquic #688, #687, #693; build h3spec-style gate; add pinned-commit CI check; cutoff 2026-10-09 |
| **4 — v4.0 scope confirmation** | Enact v3.7 Phase 5 decisions and record v4.0 fate for every experimental item | Not started | Promote full GC/malloc interception to supported opt-in; record decisions for GraphQL subscriptions, durable queue, Redis/NATS borrow, WASM component pipeline; audit stale experimental docs |

### Phase 1 — Performance

Every item lands with before/after numbers from the CI benchmark or a checked-in
microbenchmark, or with a recorded negative result.

| Work Item | Result | Decision |
|-----------|--------|----------|
| Reactor vs Classic vs Axum latency gap | CWIST C1M already leads Axum on a single CI run (291k vs 237k RPS, 1.46 ms vs 1.65 ms avg); queue delay dominates the tail | Done for v3.8; further gains need architectural tail-hardening, not tuning |
| #166 tail-latency successor | Opened #293; `CWIST_LATENCY_PROBE` shows callback time is tiny and queue delay drives the tail | Done for v3.8 |
| Batch/yield matrix | Default `(16, 16)` is near-optimal; `(16, 4)` and `(16, 32)` are marginally better on P99 but RPS is flat | Done; no default change |
| RX-uring pipelining (#179) | **Negative**: `CWIST_RX_URING=1` is ~14% slower than `=0` on a 16-depth pipeline workload | **Drop** from v3.8 scope; keep the learn flag |
| HTTPS handshake shards | **Real but niche**: `CWIST_HTTPS_HS_SHARDS=1` is a clear bottleneck (3.1k RPS / 27 ms); 4+ shards saturate (~3.6k RPS / ~13 ms) | **Keep**; consider raising the default shard count |
| Worker ttak calibration warmup | **No cold-start drift**: RPS varies <5% from second 1 to second 10 across three cold starts | **Drop** from v3.8 scope |
| Measurement discipline | Compare within one runner-CPU column, or use checked-in microbenchmark | Ongoing |

### Phase 2 — Rust FFI

Layout: an in-tree `bindings/rust/` workspace with `cwist-sys` (bindgen output
that links `libcwist.a` through `cwist.pc`) and `cwist` (the safe wrapper).

| Work Item | What Changes | C-Side Impact |
|-----------|--------------|---------------|
| Per-route user context | Additive `_ex` registration functions that carry `void *user_ctx` + optional destructor | New symbols only; existing signatures unchanged |
| `static inline` helpers | Export wrappers (or use bindgen `--wrap-static-fns`) so bindgen can see them | New symbols only |
| Struct layout | Decide per field between accessor and bindgen layout access; add layout assertion tests | New layout tests; existing structs unchanged |
| Safe wrapper scope | App lifecycle, routing with closures, request/response access, middleware, graceful shutdown, deferred async | None |
| Memory model | Rust allocations stay outside `cwist_alloc`; full GC and `CWIST_INTERCEPT_MALLOC` do not apply to Rust code | Documentation only |
| CI and measurement | `cargo test` on Linux/macOS, `example/rust-hello/`, FFI overhead measured vs C equivalent | New CI job |

Status at the v3.8 cut: experimental, crate version 0.x, not yet published to
crates.io. The v4.0 decision (publish, or keep in-tree) is recorded before v4.0
cuts.

### Phase 3 — HTTP/3 Connection-Close Correctness

The CWIST-side half is done. The three lsquic fixes are open upstream as
#688 (triggering frame type), #687 (close packet number space selection), and
#693 (pre-handshake fallback); all three are mergeable.

| Work Item | Deadline | Fallback |
|-----------|----------|----------|
| Re-pin `lib/lsquic` to upstream release containing #688, #687, #693 | 2026-10-09 | Mark affected gate cases expected-fail; move re-pin to v4.0 RC |
| Build h3spec-style CONNECTION_CLOSE interop gate | v3.8 | N/A |
| Keep `test_http3` Test 12 pinning peer-abort close path | v3.8 | N/A |
| Add CI gate that fails if pinned lsquic commit lacks required fixes | v3.8 | N/A |

### Phase 4 — v4.0 Scope Confirmation

| Decision Source | Action |
|-----------------|--------|
| v3.7 Phase 5 | Promote full GC and malloc interception to *supported opt-in* (`CWIST_DEFER_FREE`, `CWIST_INTERCEPT_MALLOC`); keep `CWIST_PROFILE` matrix as v4.0 default story; keep latency probe hidden opt-in; keep HTTP batch-shed counter always-on |
| Experimental items | Record v4.0 decision for GraphQL subscriptions (`graphql_ws.h`), durable job queue (`durable_queue.h`), Redis RESP2 reply tree and NATS connection borrow, and WASM component pipeline (#203) |
| WebTransport tutorial | Keep experimental until v4.1 |
| Docs | Audit stale experimental caveats |

### Release Criteria for v3.8

- Every Phase 1 item has before/after numbers or a recorded negative result.
- `bindings/rust` builds and passes tests in CI on Linux and macOS, the Rust
  example serves requests, and the FFI overhead is measured.
- The C API additions for FFI are additive only; no existing symbol changes.
- The HTTP/3 connection-close gate exists, passing or expected-fail per the
  cutoff rule.
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
* `cwist proto input.proto` generates scalar proto3 C models, encoder helpers, and gRPC method-path constants.
* `cwist_grpc_set_response()` and `cwist_grpc_set_error()` produce `application/grpc` responses and explicit `grpc-status` / `grpc-message` metadata.
* `cwist_pb_writer` supports varint keys, uint64/int64/bool fields, bytes fields, string fields, and dynamic buffer growth.
* `cwist_pb_reader` iterates Protobuf fields and exposes wire type, field number, varint value, and length-delimited payload slices.
* `cwist_pb_zigzag_encode()` / `cwist_pb_zigzag_decode()` cover signed integer mappings used by `sint32` / `sint64` style fields.
* `test_grpc` verifies Protobuf request construction, gRPC frame handling, unary dispatch, buffered streaming dispatch, multi-message response parsing, invalid content type handling, and malformed frame rejection.

Known limits:

* Server-side response compression is not implemented (requests only).
* The proto generator covers scalar, enum, nested message, and repeated packed-numeric proto3 fields plus service paths; `oneof`, `map`, fixed-width types, and descriptor-set input remain (v3.4).
* The builtin health `Watch` route stays on the buffered dispatch path.
* No gRPC client, retry policy, or load-balancing policy exists yet.

---

## v3.4 Milestone (Planned)

Theme: gRPC client side and codegen completeness. v3.3 (re-tagged to include the sanitizer/interop test fixes) shipped the wire-level streaming server (DATA-frame wiring, trailers, deadlines, gzip) plus the first proto codegen extension (enums, nested message fields, repeated packed numerics); v3.4 finishes the story.

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

## v3.5 Milestone (Planned)

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

## v3.6 Milestone (In Progress)

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
  * ~~WASI target evaluation~~ (done — decision and prerequisites in
    `docs/api/wasi.md`: a separate `wasm-wasi` workstream, blocked on
    libttak `__wasi__` compat, sqlite header hygiene, and sysroot
    hermeticity; not a v3.6 deliverable).
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
* **Phase 4: reach** (remaining):
  * End-to-end example app (Service Worker or fetch-interception layer).

Landeds alongside the WASM wave, also in scope for v3.6:

* **Per-event latency probe** (issue #166, PR #186, merged): `CWIST_LATENCY_PROBE=1` records arm-to-dispatch queue delay and callback runtime histograms per reactor, dumped at destroy. First measurement at the CI operating point: queue delay p99 = 10 ms while callback p50 = 25 us — the tail lives before dispatch.
* **RX-uring receive path** (issue #179, PR #187, merged): one `IORING_OP_RECV` SQE replaces the POLL_ADD + recv() pair on the C1M async path (Linux io_uring reactors only; `CWIST_RX_URING=0` restores legacy byte-identically). A per-connection learn flag keeps non-pipelining clients at the legacy op count (measured neutral: 342.4k vs 341.7k rps, t12 c400); the pipelining win case is unproven pending a pipelining workload.
* **WebSocket non-blocking I/O** (issue #181, PR #182, merged): reactor-driven WebSocket on C1M (classic mode keeps blocking I/O), callback-style API, `CWIST_WS_ASYNC_IDLE_TIMEOUT_SEC` (default 300 s).
* **SQPOLL evaluation** (closed, negative): kernel SQ-thread wakeup discipline on 6.12 makes producer-side SQPOLL at best equal to the polled ring and at worst a multi-ms stall source; SQ_AFF stabilizes it but never beats the plain submit-enter (issue #179 comments).

Known limits going in (from PR #176 review), updated:

* ~~Bundle size impact of pulling SQLite into `libcwist_wasm.a` is unmeasured~~ — now measured (Phase 2 bundle report in `docs/api/wasm.md`): `libcwist_wasm.a` 328,518 -> 1,703,706 B (5.2x), `wasm_smoke.wasm` 67,267 -> 1,052,405 B (15.7x). The future opt-out or split build decision now has data; it remains open.
* ~~Session behavior under the WASM dispatch model is documented but not yet measured; Phase 3 needs observed behavior, not the current caveats list~~ — now measured (Phase 3: cross-instance verify/reject in `tests/test_wasm_stream.c` and the Emscripten smoke test; model documented in `docs/api/wasm.md`).

---

## v3.7 Milestone (In Progress)

Theme: **Edge deployment and QUIC completion**. v3.6 took WASM from "in-tree target" to "usable from JavaScript"; v3.7 takes it to "deployable on edge runtimes" (WASI), brings WebTransport to the stable line once its upstream dependency lands, folds the HTTP/3 connection-close correctness wave into the release pin, and lands three ecosystem items — gRPC server compression, GraphQL subscriptions, and persistent job backends — as experimental support. Tracked in issue #201.

* **Phase 1: WASI edge deployment** (from 🔮 "Serverless / WASM Runtime"):
  * ~~Promote the WASI 0.2 (`wasm32-wasip2`) socket server from experimental to supported: CI gate (build + wasmtime `wasi:sockets` smoke, mirroring `wasip2-smoke`), and reframe `docs/api/wasi.md` from evaluation to reference documentation~~ (done — `wasip2` job in `.github/workflows/wasm.yml`; `docs/api/wasi.md` reframed to reference).
  * Edge persistence pattern: `cwist_db_serialize()` / `cwist_db_open_memory()` round trip against a host KV-style store, with an example app.
  * Deployment examples and guides for at least one edge runtime (Cloudflare Workers or a wasmtime appliance setup).
  * WASM streaming producer API: response bodies generated chunk-by-chunk inside handlers — the "not covered (yet)" item from `docs/api/wasm.md` (boundary streaming shipped in v3.6 buffers the body in the app; this closes the gap).
  * Component experiment (toward replacing the Emscripten bundle, tracked separately): define the WIT world for the JS dispatch boundary (`wit/`), validate it in CI, and spike a jco-transpiled browser bundle running the existing wasm smoke suite alongside the Emscripten build. Emscripten stays the supported browser path for the whole of v3.7; the swap happens only after the WASI 0.3 world stabilizes.
  * Retire the WASI preview1 target (`wasi-smoke`): 0.2 covers its use, and three flavors cost more than they earn.
* **Phase 2: WebTransport on the stable line** (conditional on upstream, issue #17):
  * Trigger condition: LSQUIC PR #629 (WebTransport) merges to upstream lsquic master.  If it has not merged by the release window, this phase slips — v3.7 ships without WebTransport rather than pinning `main` to a topic branch again.
  * Re-pin `lib/lsquic` to upstream master with WebTransport included; port the dev-branch WebTransport server and native C client to `main` with interop and soak coverage.
* **Phase 3: HTTP/3 client correctness**:
  * Track the lsquic connection-close fixes upstream (triggering-frame-type population, connection-close packet number space selection and pre-handshake fallback) and fold them into the release-line `lib/lsquic` pin at the next re-pin.
  * Add connection-close interop coverage on the CWIST side so the behavior stays pinned by tests.
* **Phase 4: ecosystem experimental support** (shipped behind flags, documented as experimental):
  * ~~gRPC server-side response compression~~ (done 2026-09-07 in `31b44d6f`, pre-v3.7; marked here so Phase 4 tracks only what remains).
  * ~~GraphQL subscriptions over the v3.6 non-blocking WebSocket transport~~ (done — graphql-transport-ws subprotocol in `cwist/graphql_ws.h` with a topic broker (`cwist_graphql_publish`) and reactor-thread-safe fanout; `test_graphql_subscriptions` covers the close-code matrix, streaming, and teardown purge).
  * ~~Persistent job backends: a durable queue over the existing Redis/NATS clients, separate from the in-process scheduler queue~~ (done — `cwist/sys/job/durable_queue.h`: at-least-once queues, Redis streams+consumer groups (XAUTOCLAIM visibility, Lua nack/dead-letter) and NATS JetStream pull consumers; `test_durable_queue` runs against live Redis and skips NATS cleanly without a server).
* **Phase 5: pre-v4 experimental promotion** (new under this retheme — give the dev-only experiments a release-line soak so v4.0 can decide their fate with data):
  * Memory management: full-GC (`CWIST_DEFER_FREE`, EBR path, thread/process-exit sweep) and header-scoped malloc interception (`CWIST_INTERCEPT_MALLOC`) documented as one experimental support tier, with a named v4.0 decision per item (default-on, opt-in, or removed). **Decision (2026-09-24): opt-in, both items.** The overhead of enabling the safety net is real (measured in `tests/bench_malloc_intercept.c`, table and methodology in `docs/GC.md` §5) — it is priced for handler authors who want it, not a tax every stable-line deployment should pay. The revert path is "do not define the macro", so promoting it to *supported opt-in* at v4.0 (out of experimental) carries no API risk. Decision recorded here per the entry criteria; revisit only if the soak turns up correctness regressions.
  * `CWIST_PROFILE` presets and the C1M baseline work: confirm the preset matrix is the v4.0 default story or trim it. **Decision (2026-09-24): keep the matrix as the v4.0 default story, no trim.** The four presets (`performance`/`lowmem`/`lowlat`/`default`) are thin setenv overlays with per-variable escapes (overwrite=0), so the surface is already minimal. On the default: issue #166's tail analysis attributes the extreme-tail gap to neither dispatch model, so C1M-on + drain-chunk 8 remains the defensible throughput-oriented default, with `CWIST_PROFILE=lowlat` as the documented escape for latency-first deployments. `docs/cooperative-queuing.md` documents the matrix for every profile.
  * The env-gated per-event latency probe and HTTP batch shed metrics: promote, hide, or drop. **Decisions (2026-09-24), one per item:**
    * *Latency probe (`CWIST_LATENCY_PROBE=1`, issue #166 tooling):* **hide — stays opt-in.** Disabled cost is one cached atomic load and a branch per arm/record (the `clock_gettime` calls are guarded and never run when off); enabled cost is two clock reads plus a histogram update per request. Its job — attributing the extreme tail between queue and service time — is a diagnostic activity, not steady-state observability; steady-state belongs to the always-on Prometheus metrics. No default-on, and no sampling mode is planned for v4.0 unless #166 (or its successor) asks for one.
    * *HTTP batch shed metric (`cwist_http_continuation_shed_total`):* **promote — already de-facto stable, keep always-on with no gate.** The counter increments only when a pipelined continuation is shed because the reactor post queue is full — a pathological path where the connection is closed anyway. Gating it would add a branch on a path that executes roughly never. It already ships ungated in the Prometheus exposition, so "promote" here means the v4.0 line keeps it unconditional rather than hiding it behind a knob.
  * Each item needs: experimental docs, a revert path, and the promotion decision recorded here before v4.0 cuts. (All three items now have their decision recorded above; the docs and revert paths were already in place.)

---

## v3.8 Release Criteria and v4.0 Preview

The detailed plan for v3.8 lives in the [CWIST v3.8 Roadmap](#cwist-v38-roadmap-in-progress) section above. This section keeps the release gate and the v4.0 transition note in one place.

**Release criteria for v3.8:**
- Every Phase 1 item has before/after numbers or a recorded negative result.
- `bindings/rust` builds and passes tests in CI on Linux and macOS, the Rust
  example serves requests, and the FFI overhead is measured.
- The C API additions for FFI are additive only; no existing symbol changes.
- The HTTP/3 connection-close gate exists, passing or expected-fail per the
  cutoff rule.
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

## v4.1 (Planned): expansion resumes, starting with WebTransport

v4.1 is where expansion resumes after the v4.0 stabilization cycle. The
first item is WebTransport, moved from v3.8 on 2026-09-25 (issue #17). It
arrives as new API; nothing that exists at v4.0 changes.

* Precondition: LSQUIC PR #629 (or its successor) is merged upstream; then
  re-pin `lib/lsquic` to an upstream release. No topic-branch pin.
* Port the dev-branch WebTransport server API (`cwist_http3_transport_*`,
  stream accept/read/write), the native C client, and
  `example/webtransport/`.
* `test_webtransport`: session negotiation, bidirectional streams, datagrams
  if enabled, and graceful teardown.
* Soak against at least one other peer (`aioquic` or Chromium) before the
  experimental flag is removed.

---

## Release Line & Codenames

* The 3.x line is stabilization work on the road to v4.0: release intervals are deliberately long, and each release lands a small number of large, well-tested changes rather than frequent small ones. Expect wide gaps between 3.x tags.
* The first 100% production-compatible stable release is planned as **v4.0**. Until then, minor releases may adjust public APIs (see the versioning note in the README).
* Starting with the stable line (v4.0 onward), each release receives a codename in the form **adjective + color** (e.g. "Steady Amber"). Codenames are assigned at release time and recorded here.

### Versioning rules (as practiced)

The tag history (`v0.1` → `v3.3`) settles into this convention from v3 onward, and it is the rule going forward:

* **Tags**: `v<major>.<minor>` for feature releases (`v3`, `v3.1`, `v3.2`, `v3.3`). Urgent fixes to a released tag get a patch level, `v<major>.<minor>.<patch>` (`v2.5.1`, `v2.4.1`) — patches are for hotfixes only, never for features.
* **Major** (`v2` → `v3`): a generational milestone — a broad capability jump (e.g. v3 = first reliable release, HTTP/2 stabilization). Majors are rare.
* **Minor** (`v3.2` → `v3.3`): one coherent feature theme (v3.2: HTTP/3 standards compliance + security hardening; v3.3: gRPC streaming + deferred async handlers). A minor is cut when its theme is complete, not on a calendar.
* **Release title**: `CWIST vX.Y` followed by an em-dash summary of the headline theme ("CWIST v3.3 — gRPC streaming, deferred async handlers, and stability hardening"). Pre-v3 releases used freeform subtitles ("Firefox Compatibility"); the em-dash form is the standard now.
* **Release body**: "Highlights since vX.(Y−1)" or "Major changes compared to vX.(Y−1)", grouped into numbered/sectioned items with commit references where useful.
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
6. ~~**Access Logs** (Common/JSON format)~~ ✅
7. ~~**Metrics endpoint** (Prometheus text format)~~ ✅
8. ~~**Rate Limiting** middleware~~ ✅
9. ~~**Caching** (ETag generation + in-memory cache)~~ ✅
10. ~~**Health Check** endpoints~~ ✅
11. ~~**Secure Headers** (HSTS, CSP, X-Frame-Options, etc.)~~ ✅
12. ~~**Request Size Limits** (HTTP/1.1/2/3 body limit audit)~~ ✅
13. ~~**Multiport facade hardening**: counted port descriptor, per-port sub-app lifecycle, duplicate/default-port validation, and smoke tests~~ ✅

### P2 — Developer Velocity
14. **Hot Reload** for development
15. ~~**CLI Tooling** (project scaffold, route generator, watcher)~~ ✅
16. ~~**Configuration** loader (`.env`, `.toml`)~~ ✅
17. ~~**Test Harness** with HTTP mock client~~ ✅
18. ~~**Deferred Async Handlers** (`cwist_async_defer` cross-thread completion)~~ ✅

### P3 — Advanced Protocols
19. **Native C WebTransport client stabilization** (experimental `dev` implementation available)
20. ~~**HTTP/2 Server Push**~~ ✅
21. ~~**io_uring** UDP packet loop for HTTP/3~~ ✅
22. ~~**kqueue** backend for macOS/BSD~~ ✅
23. ~~**Multiport HTTP/3 parity**: per-port UDP contexts and global setting propagation to non-detached ports~~ ✅

### P4 — Ecosystem
24. ~~**gRPC unary and buffered streaming server support**~~ ✅
25. ~~**GraphQL** bounded Query executor~~ ✅
26. ~~**OpenAPI** generator~~ ✅
27. ~~**Background Jobs / Scheduler**~~ ✅
28. ~~**Incremental gRPC framing, reflection, health checks, and `.proto` codegen**~~ ✅
29. ~~**gRPC wire streaming**: DATA-frame wiring, trailers, deadlines, gzip negotiation~~ ✅
30. **Distribution**: publish Homebrew formula and vcpkg port beyond the current drafts

---

## Contributing

If you want to pick up an item, open an issue referencing this roadmap and the specific feature number.
