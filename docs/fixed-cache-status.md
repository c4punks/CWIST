# Public FIXED response cache (ADR-0001)

`CWIST_ENDPOINT_PUBLIC_FIXED` is a new, independent route flag. Bare
`CWIST_ENDPOINT_FIXED` and `CWIST_DYNAMIC` still dispatch every request.
Automatic HTTP replay/learning of legacy BDR wire blobs has been removed in
both classic and C1M. The standalone BDR storage API remains available; manually
preseeded entries no longer bypass routing or middleware.

## Opt in only for public constants

```c
static void public_version(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    cwist_http_header_add(&res->headers, "Content-Type", "text/plain");
    cwist_sstring_assign(res->body, "service-v1");
}
cwist_app_get_opt(app, "/public-version", public_version,
                  CWIST_ENDPOINT_PUBLIC_FIXED);
```

The author asserts that the representation is public and independent of
credentials, user, User-Agent, client address, connection, and time. The handler
must have no required per-request side effects and must not perform authorization
that a hit would skip. Do not opt in personalized pages, counters, health checks,
authenticated endpoints or mutable database results. Existing FIXED declarations
are **not** proof of this contract and have not been bulk-migrated.

## Initial admission profile

- Cleartext HTTP/1.1, exact GET route, no middleware at all (even middleware that
  normally does nothing). Parameter, fallback/static, WebSocket, TLS, H2/H3 and
  in-memory dispatch do not use this cache.
- No nonempty query or request body, framing/Expect/upgrade flags, session,
  CSRF/flash/private state. An empty query is equivalent to no query, matching
  the parser's representation and ADR-0001; delimiter presence is not invented.
- Exactly one valid Host. The exact accepted Host bytes, authoritative route
  identity, method and exact path form the key; case/port differences separate
  entries. The initial conservative Host syntax accepts DNS-style names,
  IPv4-shaped names and bracketed IPv6 with an optional decimal port <=65535;
  unsupported forms are safe misses, not parser rejections.
- The only request headers are unique Host, User-Agent, `Accept: */*`, and
  Connection with a single `keep-alive` or `close` value. Names are checked
  case-insensitively on **every** node. Unknown headers, duplicates, auth/cookies,
  cache directives, range/conditional/encoding negotiation, and even explicit
  `Content-Length: 0` miss safely.
- Complete synchronous 200, default HTTP/1.1 version / `OK` reason / keep-alive
  capability; no deferred/file/upgrade/private state or `alt_svc`. The only
  permitted response header is one nonempty, validated Content-Type <=1 KiB.
  Set-Cookie, Vary, cache directives, Date, framing overrides, custom headers and
  connection-closing responses are not cached.
- PUBLIC_FIXED may be combined with the compatibility FIXED bit. Combining it
  with DYNAMIC, FILE or unknown bits conservatively disables admission.

## Storage, framing and failures

Bodies are binary, bounded at 64 KiB, and copied **before** the ordinary sender
can release a managed body. Cache entries and per-hit snapshots own their bytes
outside worker GC. A mutex protects copy/expiry/eviction; response sending happens
outside the lock. There is no cache pin borrowed across sends.

Across all application caches in a worker process: at most 256 entries and
16 MiB accounted entry/key/body/metadata storage, guarded by one process-wide
mutex. Entries have a 60-second monotonic maximum age. Allocator overhead and
transient per-response snapshots are not an RSS bound. Pressure evicts the
inserting cache's oldest entries, or misses if that cache is empty; it never
borrows another application's allowance. Clear, expiry and destruction release
the shared reservation. Hits do not renew age. There is no background worker.

Every hit creates an ordinary response. The normal classic or coalesced/parked
C1M sender derives Content-Length and this request's keep-alive/close/shutdown
framing. A first request with `Connection: close` can populate an otherwise
keep-alive-capable representation without forcing later responses to close.

Allocation failure, expiry, capacity eviction and any unsupported condition use
ordinary routing. They **never** fall back to legacy BDR replay. Simultaneous
cold misses may run the handler more than once; this cache is not single-flight.

## Reconfiguration and shutdown

Configure routes/middleware before serving. Supported route registration,
WebSocket/group registration, middleware/static registration, BDR reconfiguration
and server restart invalidate the entire cache. After changing public constant
data, call `cwist_app_clear_public_fixed_cache(app)` while workers are quiescent.
Do not mutate exposed router/app fields concurrently; cache locking does not make
the existing router safe for live mutation. Stop/join workers and connections
before `cwist_app_destroy`, which frees all entries and the cache owner.

## Verification status

`make test_public_fixed_cache` runs the independent guard/storage matrix normally
and with NDEBUG, without vendored runtime dependencies. Its only test seams are
opaque DB/cJSON types and a private clock/free-compatible allocator; HTTP layouts
and cache implementation are production source. `make test_public_fixed_cache_sanitize
CC=clang` adds ASan/UBSan, including concurrent get/put/clear and snapshot lifetime.

`make test_public_fixed_http` builds the real TCP classic/C1M fixture and runs each
with full GC enabled and disabled. It exercises handler counts, binary bodies,
Host separation, close/keep-alive pipelines, backpressure, managed cleanup,
legacy-preseed rejection, request/response misses, deterministic expiry/OOM,
capacity and quiescent reconfiguration. **This fixture has not been built/run on
the current dependency-free macOS worktree.** Native Linux/BSD/full-library CI,
TCP sanitizer runs and integration review remain release gates. Unit PASS is not
TCP PASS and handler counts are not tail-latency evidence.

### Rebuild applications

The application structure gains an opaque cache pointer at its end. Existing field order is preserved, but its size grows. Recompile and relink applications against the matching headers and library; do not treat this as a binary-only drop-in update.
