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
[P2: DevEx]          ████████░░░░░░░░░░░░  40% (Config loader, test tooling, and wire helpers done)
[P3: Deep Protocols] ███████████████░░░░░  75% (HTTP/3 extension specs and io_uring backend done)
[P4: Ecosystem]      ██████░░░░░░░░░░░░░░  30% (Unary gRPC and Protobuf wire helpers done)
```

### 1) Transport Layer

* **Native protocols ready**: HTTP/1.1 through HTTP/3 (QUIC via `lsquic`), WebTransport, and WebSocket are all implemented in-tree. Low-level socket controls (ECN, 0-RTT, connection migration) are complete.
* **HTTP/3 browser hardening**: Response header emission now normalizes field names to lowercase and rejects CR/LF-bearing values, covering login/logout cookie and redirect paths in strict browsers such as Firefox.
* **Async I/O optimization (`io_uring`)**: `io_uring_backend.c`, SQE/CQE synchronization, demolition safety, and focused tests are complete.
* **Multiport HTTP/3 fan-out**: The `cwist_multiport_t` facade now creates per-port UDP contexts and copies global HTTP/3 settings unless a port is detached into a sub-app.

### 2) Application Layer

* **High-performance router & middleware**: Deterministic resource management with parameterized routes (`/user/:id`), compression (Gzip via zlib), CORS, and rate limiting (libttak token bucket) are integrated.
* **Observability**: Prometheus `/metrics` and a probe-registry health-check system are operational.
* **Per-port sub-applications**: Lifecycle and exception handling for `cwist_multiport_get_app(&app, port)` are hardened; detached ports are separately tunable sub-applications.
* **Unary gRPC services**: Applications can register unary gRPC handlers over HTTP/2 with `cwist_app_grpc_unary()`, decode the gRPC message envelope, and return `application/grpc` responses with explicit gRPC status metadata.

### 3) Security & Data Layer

* **Security specs**: BoringSSL-based TLS 1.3 and hybrid post-quantum KEM (`X25519MLKEM768`) are implemented ahead of time. CSRF and automatic secure-header injection remain planned (`⏳`).
* **Data-layer integrity**: SQLite3 embedded integration, migration system, and a `_Generic` macro-based type-dispatched ORM/query builder are in the build stream. The lock-free work queue (`cwist_io_queue`) and scheduler-backed background jobs are implemented.
* **Protobuf wire helpers**: A lightweight Protobuf runtime supports varint keys, unsigned/signed/bool fields, length-delimited bytes/strings, reader iteration, and ZigZag helpers for hand-written services.

---

## Current Snapshot

- Core HTTP/1.1, HTTP/2, HTTP/3, WebSocket, routing, middleware, validation, metrics, health checks, static-file caching, and graceful shutdown are already implemented in-tree.
- **P0 (must-have) is 100 % complete**: the framework’s core architecture and protocol stack are locked.
- We are now in the **P2–P4 tooling and ecosystem phase**. Completed multiport, scheduler, test-client, `io_uring`, unary gRPC, and Protobuf wire-format work remain covered by focused regression tests.

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
| **io_uring Backend** | ⏳ | Linux-only; `epoll` done, io_uring needs `liburing` or raw syscalls |
| **kqueue Backend** | ⏳ | BSD/macOS; blocked on non-Linux test environment |
| HTTP/2 Server Push | ✅ | `cwist_http2_push_resource` with PUSH_PROMISE frame, HPACK encoding, server-initiated even stream IDs |
| **WebTransport** | ✅ | Basic server handler (`:protocol=webtransport` detection via HTTP/3 CONNECT) |
| HTTP/3 Datagram Extension | ✅ | `send_datagram`, callbacks, `es_datagrams` enabled |
| ECN (Explicit Congestion Notification) | ✅ | UDP socket with `IP_RECVTOS` / `IPV6_RECVTCLASS` |
| Connection Migration | ✅ | `es_allow_migration` enabled |
| 0-RTT Early Data | ✅ | `SSL_CTX_set_early_data_enabled` |
| **Multiport TCP Facade** | 🔄 | Counted `cwist_multiport_t` descriptor and shared accept loop are in progress; per-port tests and docs pending |
| **Multiport HTTP/3 Fan-out** | ⏳ | Needs one UDP socket/context per bound port, with global settings copied unless the port is detached into a sub-app |

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
| **SSE (Server-Sent Events)** | ⏳ | No structured SSE stream API |
| **Access Logging** | ✅ | Common, Combined, and JSON formats implemented |
| **Request ID / Tracing** | ✅ | X-Request-Id middleware injects and propagates request IDs |
| Graceful Shutdown | ✅ | Unified atomic `running` flag + SIGTERM/SIGINT handlers across HTTP/1.1, HTTP/2, HTTP/3 loops |
| **Health Check Endpoint** | ✅ | `/healthz`, `/live`, `/ready` with probe registry and auto-registration |
| **Metrics / Observability** | ✅ | Prometheus `/metrics` endpoint wired; request counter & duration middleware |
| **Per-Status Error Handlers** | ✅ | `cwist_app_register_error_handler` for custom 404, 500, etc. |
| **URL Reverse Routing** | ✅ | `cwist_app_get_named` + `cwist_url_for` with param substitution |
| **Flash Messages** | ✅ | One-time session-scoped messages via `cwist_flash_get/set` |
| **Per-Port Sub-Applications** | 🔄 | `cwist_multiport_get_app(&app, port)` detaches additional ports for independent tuning; public/default port must remain owned by root app |

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
| **gRPC over HTTP/2** | ✅ | Unary server registration via `cwist_app_grpc_unary`, gRPC frame decode/encode, gRPC status metadata, and test-client coverage |
| **Protobuf Runtime Helpers** | ✅ | Wire-format reader/writer for varint, bool, bytes/string, signed integer casting, and ZigZag helpers |
| **GraphQL** | ⏳ | No GraphQL parser or executor |
| **OpenAPI / Swagger Generation** | ⏳ | No automatic spec generation from route definitions |
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
| **1 — Performance** | Close reactor/Classic/Axum latency gaps; validate RX-uring pipelining; HTTPS handshake shards; worker warmup | Mostly done | #293 has the data; #294 raised `CWIST_HTTPS_HS_SHARDS` default floor to 4 and MAX to 16; RX-uring and worker warmup dropped |
| **2 — Rust FFI** | Make CWIST callable from Rust (`cwist-sys` + `cwist`) | Done | Middleware and deferred-async wrappers landed; `example/rust-hello/` and FFI overhead benchmark added; built-in middleware factory wrappers and owned `cwist_async_respond_with` helper done; `cwist-sys` and `cwist` v0.1.0 published to crates.io |
| **3 — HTTP/3 close correctness** | Re-pin lsquic when upstream fixes land; add CONNECTION_CLOSE interop gate | Deferred indefinitely | On hold until upstream lsquic ships WebTransport client support; no separate cutoff |
| **4 — v4.0 scope confirmation** | Enact v3.7 Phase 5 decisions and record v4.0 fate for every experimental item | Done | v3.7 Phase 5 enacted; GraphQL subscriptions, durable queue, Redis RESP2/NATS borrow promoted to supported; WASM component pipeline ships experimental at v4.0 with promotion re-evaluated at v4.1; WebTransport experimental until v4.1; docs audit done |

* Extend gRPC from unary handlers to server streaming, client streaming, and bidirectional streaming.
* Add generated-code bindings from `.proto` descriptors once the runtime ABI settles.
* Add gRPC reflection, health checking, deadlines, cancellation propagation, metadata normalization, and trailer-frame emission.
* Add GraphQL and OpenAPI generation.
* Evaluate persistent job backends separately from the in-process queue/scheduler.

### gRPC / Protobuf Status

Completed:

* `cwist_app_grpc_unary(app, service, method, handler, user_ctx)` registers `POST /Service/Method` routes for HTTP/2 gRPC unary calls.
* `cwist_grpc_decode_message()` and `cwist_grpc_encode_message()` implement the gRPC 5-byte message envelope (`compressed` flag + big-endian payload length).
* `cwist_grpc_set_response()` and `cwist_grpc_set_error()` produce `application/grpc` responses and explicit `grpc-status` / `grpc-message` metadata.
* `cwist_pb_writer` supports varint keys, uint64/int64/bool fields, bytes fields, string fields, and dynamic buffer growth.
* `cwist_pb_reader` iterates Protobuf fields and exposes wire type, field number, varint value, and length-delimited payload slices.
* `cwist_pb_zigzag_encode()` / `cwist_pb_zigzag_decode()` cover signed integer mappings used by `sint32` / `sint64` style fields.
* `test_grpc` verifies Protobuf request construction, gRPC frame handling, unary dispatch, Protobuf response parsing, invalid content type handling, and malformed frame rejection.

Known limits:

* Only unary server handlers are implemented; streaming RPCs remain planned.
* Compressed gRPC messages are rejected with `UNIMPLEMENTED`; compression negotiation is not wired yet.
* Protobuf support is a runtime wire helper, not a `.proto` compiler or generated binding layer.
* HTTP/2 response trailers are represented as gRPC metadata headers for now; dedicated trailer-frame emission is a follow-up.
* No gRPC client, reflection service, health service, deadline propagation, retry policy, or load-balancing policy exists yet.

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
11. **Multiport facade hardening** 🔄: counted port descriptor, per-port sub-app lifecycle, duplicate/default-port validation, and smoke tests

### P2 — Developer Velocity
12. **Hot Reload** for development
13. **CLI Tooling** (project scaffold, route generator)
14. ~~**Configuration** loader (`.env`, `.toml`)~~ ✅
15. **Test Harness** with HTTP mock client 🔄

### P3 — Advanced Protocols
16. ~~**WebTransport** server + client~~ ✅ (basic server handler)
17. ~~**HTTP/2 Server Push**~~ ✅
18. **io_uring** UDP packet loop for HTTP/3 🔄
19. **kqueue** backend for macOS/BSD
20. **Multiport HTTP/3 parity** ⏳: per-port UDP contexts and global setting propagation to non-detached ports

### P4 — Ecosystem
23. ~~**gRPC unary server support**~~ ✅
24. **GraphQL** executor
25. **OpenAPI** generator
26. ~~**Background Jobs / Scheduler**~~ ✅
27. **gRPC streaming, reflection, health checks, and `.proto` codegen**

---

## Contributing

If you want to pick up an item, open an issue referencing this roadmap and the specific feature number.
