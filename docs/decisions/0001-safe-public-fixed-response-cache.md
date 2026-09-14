# ADR-0001: Restore HTTP response caching through an explicit public FIXED contract

- **Status:** Accepted. The implementation is the separate PUBLIC_FIXED API; release notes identify shipped versions. This ADR is not a performance or test-pass claim.
- **Date:** 2026-09-13
- **Scope:** Cleartext HTTP/1 in classic and C1M modes.
- **Source baseline:** `7e19b461c5772f4e46e7d58fb96827bbb954cf97`.
- **Related:** [issue #25](https://github.com/c4punks/CWIST/issues/25), [PR #84](https://github.com/c4punks/CWIST/pull/84).

## Context

The existing `CWIST_ENDPOINT_FIXED` documentation promises RAM caching after the first request. Public TCP observations contradict that promise: in both classic and C1M modes, 16 FIXED requests invoked the handler 16 times. The observation included eight keep-alive requests and eight fresh connections, a DYNAMIC control, and independently preseeded cache responses. Adaptive latency-based learning was suppressed for this experiment. This establishes a population failure, not its contribution to P99.999.

Two ordering defects explain the failure: the common serving function samples endpoint options before routing assigns them, and the C1M branch returns before the learning block. Moving that block alone is unsafe:

- Existing cache hits precede routing and middleware, including authorization.
- Parsed cache keys omit query and other representation distinctions. The classic raw shortcut has different key and framing behavior.
- Whole-wire blobs preserve the first response's connection headers. Some hits use a single unchecked `send`, and a classic lookup returns an already-unpinned pointer.
- Normal sending may release managed pointer bodies before post-send learning reads them.
- The stringification helper treats a non-NUL-terminated header span as a C string. PR #84 fixes that independent defect; it does not make cached replay safe.

Native backend/thread screening has not established a universal runtime default or resolved the original tail-latency issue. A correct response is mandatory even when a cache miss costs performance.

## Decision

Introduce a separately named opt-in, `CWIST_ENDPOINT_PUBLIC_FIXED`, for a restricted public, request-invariant representation cache. The symbol is separate from legacy FIXED declarations. It uses its own single flag bit: the current `cwist_endpoint_has` helper tests whether **any** supplied bit is present, so a composite mask must not be mistaken for an all-bits requirement.

The opt-in is an application-author assertion that the handler performs no authorization, personalization, required per-request side effect, or request-dependent selection within the admitted profile. The framework cannot infer that assertion from a function pointer, absence of credentials, or a previously successful response. Existing FIXED declarations are not silently upgraded to this stronger contract.

### 1. One parsed eligibility boundary

Both classic and C1M must parse and validate the complete request before cache access. Resolve the current exact route using the same routing authority as ordinary dispatch. Remove the alternate classic raw-cache ingress path; it must not remain a way around the policy.

The first profile admits only:

- cleartext HTTP/1.1 GET on an exact, non-parameterized route explicitly carrying the new opt-in, without FILE or conflicting options;
- an application with no configured middleware; otherwise run ordinary dispatch on every request, including both middleware pre- and post-handler work;
- no non-empty query, body, transfer coding, Expect, upgrade, session or other authentication context;
- one valid Host header, with its exact accepted value included in the key;
- request headers limited to Host, User-Agent, `Accept: */*`, and ordinary keep-alive/close Connection semantics. Unknown headers, duplicate instances, credentials, cookies, conditional/range requests, cache directives, and encoding negotiation are safe misses.

An empty query is explicitly equivalent to no query in this profile. The current request model does not preserve a separate query-delimiter-presence bit; implementations must not pretend that a zero-length query proves the delimiter was absent. A later profile that needs that distinction must preserve it during parsing.

The author contract requires independence from User-Agent, client address, time, and connection identity. Host and route identity remain separate key dimensions. Membership and value checks must examine every header node case-insensitively, not just the first value returned by a lookup helper. Invalid or unsupported values yield ordinary dispatch, never a best-effort cache hit.

### 2. Cache representations, not wire responses

Store an owned binary body, an optional validated Content-Type, and authoritative route/key identity. Only complete synchronous 200 responses with ordinary keep-alive capability, the default HTTP/1.1 version and standard reason phrase are eligible. Reject deferred, upgraded, file-stream, partial/non-200, or connection-closing responses. Reject connection-dependent overrides held outside the header list as well, including `alt_svc` and custom version/status-text fields. Any response header other than a single bounded Content-Type makes the initial profile ineligible. This deliberately excludes Set-Cookie, Vary, cache-control policy, Date, Alt-Svc, and framing overrides rather than partially implementing their semantics.

A hit constructs an ordinary response for the current request. Derive Content-Length and connection/shutdown behavior anew and use the existing normal classic or coalesced/parked C1M sender. Never send the cache's storage with a one-shot raw `send`. Regenerate any transport-owned headers; do not replay the first request's close decision.

Take a bounded owned snapshot after synchronous dispatch and its deferred-response guard, but before sending can consume a pointer body. Publication means successful eligible representation generation, not proof of network delivery. Allocation or insertion failure must fall back to sending the original response. A failed hit reconstruction falls back to ordinary dispatch before any cached bytes are emitted.

### 3. Isolate ownership, provenance and capacity

Use cache-owned immutable entries protected by a bounded lookup/update critical section. Copy a hit into request-owned storage while the entry is protected, then release the cache lock before sending. No cache lock, thread-local epoch pin, or borrowed body pointer crosses a blocking send, reactor park, deferred completion, or response destruction.

Cache allocations must have application-cache lifetime, not request-arena or thread-GC lifetime. Copies and managed-body release hooks must retain exactly-once ownership under both enabled and disabled full GC. Cache destruction follows worker/connection shutdown and releases every entry.

Initial limits are 64 KiB per body, 1 KiB for copied Content-Type, 256 entries, and 16 MiB of accounted entry/key/body/metadata storage per worker process, with a 60-second monotonic maximum age. These are conservative bounds, not measured optimal values. Accounted storage excludes allocator overhead and is not an RSS guarantee. On capacity pressure or expiry, evict safely or miss; do not add an unbounded queue or background worker. Check arithmetic before allocation.

Key by the current exact route identity, method, path, and accepted Host value. Never treat an old heuristic or manually injected whole-wire BDR blob as a validated representation entry. Routes and middleware are configured before serving; live concurrent route mutation is outside this profile. The implementation must invalidate affected entries on supported reconfiguration and must not add a route-mutation safety claim that the router itself cannot support.

### 4. Make the compatibility change explicit

The recommended migration replaces automatic HTTP BDR lookup/learning with the restricted opt-in path. Existing BDR storage APIs remain available to applications, but legacy heuristic or manually preseeded wire blobs no longer automatically bypass HTTP routing. Bare FIXED and DYNAMIC routes continue through ordinary dispatch until explicitly migrated; document the resulting loss of legacy automatic cache hits.

This is an intentional compatibility change requiring maintainer acceptance and release notes. It is not hidden inside the serializer fix. Do not expose an unsafe legacy-fast-path fallback as the cache's failure behavior. Migrate only demonstrated public constant-response examples and benchmark fixtures; do not bulk-convert application routes based on their existing FIXED bit.

## Alternatives considered

1. **Move learning and resample FIXED only.** Small diff, but activates bypass, framing, partial-write and lifetime defects. Rejected.
2. **Treat existing FIXED as proof of a public invariant handler.** Avoids a new flag, but silently strengthens the contract of deployed declarations and cannot prove handler-local authorization absent. Rejected as the default migration.
3. **Implement general shared HTTP caching.** Variant-aware keys, middleware-preserving execution, validators, cache directives and broader transports could preserve more use cases. Deferred: substantially larger protocol and API scope than the current investigation.
4. **Leave caching dormant and investigate scheduler contention only.** Safe short-term fallback and still a valid parallel investigation. It does not repair the demonstrated cache contract failure.

## Consequences

- Unsupported traffic takes a safe miss; cache hit rate is secondary to confidentiality and valid framing.
- Explicit migration is required, and some applications lose legacy automatic hits. The new API and removal of automatic legacy replay must be reviewed together.
- Body copies and synchronization may add overhead. A lower handler count alone does not establish a tail-latency improvement.
- No new dependency, reactor-count default, backend default, TLS/H2/H3 cache path, deferred-response caching, or C1M heuristic caching is introduced by this decision.

## Implementation and release gates

Implement in separately reviewable slices: eligibility/provenance and bounded storage; parsed lookup plus pre-send snapshot and normal sending; public API/documentation migration; then controlled performance validation. Keep PR #84 independent and verify the actual prerequisite revision rather than assuming it has merged.

Before release, require:

- public TCP tests in classic and C1M proving first miss/second hit, exact binary body, keep-alive and fresh connections, Host separation, and the DYNAMIC/bare-FIXED behavior;
- independent negative tests for each request and response guard, including duplicate headers, middleware with missing credentials, query variants, private responses, errors, streaming and deferred ownership;
- forced partial writes and backpressure, ordered pipelines with framed bodies, current close/shutdown behavior, pointer-body cleanup, replacement/expiry and allocation/capacity failure;
- tests showing legacy raw ingress and preseeded wire entries cannot bypass the new boundary;
- full-GC lifetime tests, sanitizers, relevant normal/native suites, and independent correctness/security review of the exact candidate;
- paired same-runner baseline/candidate measurements on unchanged load and verified runtime topology, retaining every error and outlier. Report corrected P99.999 separately from throughput and do not equate closed-loop screening with a fixed-rate production SLO.

Do not enable the cache if a confidentiality, framing or ownership gate is unresolved. Rollback disables the new automatic cache and keeps normal dispatch; it does not reactivate unsafe raw replay. Issue #25 remains open until its performance objective is demonstrated independently.

## Evidence and source anchors

- [Cache observation and serializer prerequisite](https://github.com/c4punks/CWIST/issues/25#issuecomment-5653665938).
- [Native backend/thread screening](https://github.com/c4punks/CWIST/issues/25#issuecomment-5653554218).
- At the pinned baseline: `src/sys/app/app.c`, `internal_route_handler`, `app_serve_parsed_request`, and the classic raw BDR ingress loop.
- `src/net/http/http.c`, `cwist_http_stringify_response`, normal/coalesced senders, pointer-body release, and header serialization.
- `src/sys/app/big_dumb_reply.c`, pinned/unpinned lookup and fixed/adaptive publication.
- `include/cwist/sys/app/endpoint_opts.h` and `include/cwist/net/http/http.h`, existing option and request/response contracts.
