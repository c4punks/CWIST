# CWIST on WASM (Emscripten)

CWIST can be compiled to WebAssembly via Emscripten and driven entirely from
JS: an HTTP request goes in as bytes, the router + middleware + handlers run
inside WASM, and the serialized HTTP response comes back as a zero-copy
`Uint8Array` view on the WASM heap.

This page documents what is in scope, how to build it, and the two integration
points (`cwist_app_dispatch_memory` and the TypedArray helpers). See
`tests/wasm_smoke.c` for a complete, runnable low-level example and
`example/wasm-service-worker/` for a full app behind a Service Worker host.

## Building

Requires the Emscripten toolchain (`emcc`/`emar`) and `node` for the smoke
test.

```sh
make wasm               # produces libcwist_wasm.a
make wasm-smoke         # builds and runs tests/wasm_smoke.c under node
```

The WASM archive is a separate, smaller build from the native library. It is
**not** part of `make test` and `make all`.

## What is in the WASM build

Socket-independent subsystems only:

- App dispatch: router (`mux`), middleware pipeline, config, logger,
  in-memory request dispatch (`app.c`)
- HTTP/1.1 parser and serializer, query strings, cookies, sessions
- `sstring`, arena and allocator, siphash
- JSON (`json_builder`, `json_heal`, cJSON)
- Validation (`zod`, `validation/bind`)
- Templates and HTML (`template`, `html/builder`, `html/component`,
  `html/css_composer`)
- `cwist_db` (SQLite) including `cwist_db_open_memory()` and
  `cwist_db_serialize()` for blob-shaped databases

Deliberately excluded: sockets and the accept loop, TLS/BoringSSL,
QUIC/HTTP/3, gRPC (needs HTTP/2), WebSocket transport, threads/scheduler,
compression, and the DB pool/sync clients.

## Request dispatch from JS

`cwist_app_dispatch_memory()` runs one whole request through the app and
returns the serialized HTTP response. It is whole-request-in,
whole-response-out. For streaming delivery see `cwist_app_dispatch_stream()`
and `cwist_stream_req_begin/feed/end` below.

```c
char *res_buf = NULL;
size_t res_len = 0;
cwist_app_dispatch_memory(app, req_buf, req_len, &res_buf, &res_len);
/* res_buf is heap-allocated; free it with cwist_free() when done */
```

## Zero-copy views: `cwist/wasm/typedarray.h`

Include this header (Emscripten builds only) to expose C arrays to JS as
typed-array views and to install the `Module.cwistView` helper bundle:

```c
static const int32_t g_samples[] = {10, -20, 30, 40};
CWIST_WASM_EXPOSE_I32(samples, g_samples, 4)
CWIST_WASM_INSTALL_VIEWS()
```

`CWIST_WASM_INSTALL_VIEWS()` must be expanded once in a single translation
unit and called once from `main()` before any JS callback runs. It installs:

- `Module.cwistView.u8(ptr, n)`  -> `Uint8Array`
- `Module.cwistView.i32(ptr, n)` -> `Int32Array`
- `Module.cwistView.f64(ptr, n)` -> `Float64Array`

All views alias the WASM heap directly (no copy). `CWIST_WASM_EXPOSE_*`
generates `_<name>_ptr()` / `_<name>_len()` accessors callable from JS.

## Databases under WASM

SQLite compiles into `libcwist_wasm.a`, so `cwist_db` works in WASM with two
blob-oriented entry points suited to edge hosts that persist state outside
the WASM instance:

- `cwist_db_serialize(db, &out, &out_len)` dumps the database image to a
  buffer the caller owns (free with `cwist_free()`).
- `cwist_db_open_memory(&db, buf, len, readonly)` opens a database from a
  serialized image; the image is copied, so the caller keeps its buffer.
  Pass a non-NULL image: opening "nothing" is an error by design.

Typical round trip: open `:memory:`, apply migrations, serve reads, then
either serialize back to the host or open a read-only copy of the host's
image. `cwist_db_query()` returns rows as a cJSON array of objects with all
values as strings (SQLite `exec` callback semantics).

## Bundle size impact of `cwist_db`

Adding `src/core/db/db.c` + `lib/sqlite3/sqlite3.c` pulls all of SQLite into
the archive. Measured with Emscripten 5.0.0, this branch vs the pre-db tree:

| artifact | before | after | ratio |
|---|---|---|---|
| `libcwist_wasm.a` | 328,518 B | 1,703,706 B | 5.2x |
| linked `wasm_smoke.wasm` | 67,267 B | 1,052,405 B | 15.7x |

The archive grows 5x but the linked smoke binary 16x because the smoke test
itself does the db round trip (which also pulls cJSON query-result building
into the link). Measured consequence for consumers (2026-09-21): archive
linking is per-object and no core WASM object references `cwist_db_*`, so a
db-less app links ~64.8 KB — the pre-db size. Only apps that call
`cwist_db_*` pull in the ~1 MB amalgamation, and that is SQLite's reachable
core: `-ffunction-sections` + `--gc-sections` recover ~300 B because emcc
-O2 already performs cross-module DCE.

**Decision (issue #93): no opt-out/split build.** The linked-size cost
falls only on db users, where it is inherent to SQLite, and a split archive
would not reduce it — while making db consumers link two archives. If the
~1 MB ever matters, the lever is `SQLITE_OMIT_*` feature omission, not
packaging.

## Sessions and cookies

Sessions are **client-side signed cookies** (HMAC-SHA256 over a base64 JSON
payload), so session state travels with the request and instance lifetime
is irrelevant - what must survive across WASM instances is only the signing
secret. The Phase 3 persistence model (issue #93):

- **Pin the secret.** Call `cwist_app_use_session(app, secret)` with a
  host-persisted secret (e.g. a constant baked into the fetch layer, or a
  value stored in KV/localStorage fetched at startup). A generated-per-boot
  secret invalidates every session whenever the host recycles the module;
  auto-generation is a convenience for native servers with a persistent
  process, and on WASM it falls back to `crypto.getRandomValues` /
  `getentropy()` when `/dev/urandom` is absent - fine for demos, wrong for
  production.
- **JS injection points.** Modules built with `CWIST_WASM_DEFINE_ENTRY`
  export `_cwist_wasm_use_session(secret)`. The npm wrapper exposes it as
  `handle.useSession(secret)` (call before the first session-bearing
  dispatch; pass `null` for the random dev-mode secret) and also applies a
  declarative `Module.cwistSessionSecret` string once at binding time.
  Rotating the secret invalidates every existing session, which is how
  forced sign-out is implemented.
- **Crypto is bundled.** The WASM build verifies cookie signatures with the
  header-only SHA-256/HMAC in `include/cwist/core/crypto/sha256.h` (OpenSSL
  is not linked into `libcwist_wasm.a`), so sessions now actually work
  under Emscripten; before Phase 3 nothing could link them.
- **The host carries the cookie.** A Service Worker or fetch-interception
  host must copy the `Set-Cookie` header from the dispatch response into
  its cookie store and send the stored `Cookie` header on subsequent
  requests - the browser's document cookie jar does not feed fetch events
  handled by a SW automatically.

Verified end to end by `tests/wasm_stream.c` (native), the Emscripten
smoke test, and `make wasm-wrapper-test` (JS-side `useSession` roundtrip +
rotated-secret rejection): a session set on one app instance reads back on
a second instance with the same pinned secret, and is rejected under a
different secret.

## Streaming through the boundary

`cwist_app_dispatch_memory()` is whole-request-in, whole-response-out.
Phase 3 adds streaming at the **boundary** (the handler still builds the
response body in memory; the chunked-producer handler API now exists as
`cwist_http_response_stream_begin/write/end`, see "Streaming producer"
below):

- `cwist_app_dispatch_stream(app, req, req_len, write_fn, ctx)` delivers
  the serialized response through a sink callback: the head (status line +
  headers) first, then the body in slices of at most `CWIST_STREAM_CHUNK`
  (64 KiB). Returning nonzero from the sink aborts the dispatch (-2).
- `cwist_stream_req_begin/feed/end` assembles a request body incrementally
  (large uploads without one contiguous host buffer); `Content-Length` is
  required and enforced, then `cwist_stream_req_dispatch` dispatches with
  a streaming response.

For the standard entry macro, `CWIST_WASM_DEFINE_ENTRY` also exports
`_cwist_wasm_dispatch_stream`, which pumps each chunk through
`Module.cwistStreamChunk(ptr, len)` when the host defines it - assemble
the chunks into a `ReadableStream` or accumulate them in JS.

## Runnable example: Service Worker app

`example/wasm-service-worker/` (issue #93 Phase 4) is an end-to-end app
that uses routing, zod validation, template rendering, `cwist_db`, and
sessions together, behind a Service Worker fetch-interception host:

- `app.c` - CWIST app compiled to `app.js`/`app.wasm` by `build.sh`:
  `GET /` renders a template page with the session visit counter and the
  item list, `POST /items` zod-validates a JSON body before inserting it
  into an in-memory `cwist_db`, `GET /items` returns the rows as JSON,
  `GET /items/image` returns the serialized SQLite image
  (`cwist_db_serialize`, the blob an edge host would persist), and
  unknown routes fall through to the router's 404.
- `sw.js` - the host: intercepts same-origin GET/POST fetches, dispatches
  them through the module's entry points (serialization mirrors
  `wasm/npm/index.js`, inlined to stay self-contained), pins the session
  secret via `_cwist_wasm_use_session`, and carries the session cookie
  itself - a Service Worker does not see the document cookie jar, so
  `Set-Cookie` from a dispatch response is captured into an in-memory jar
  and replayed as the `Cookie` header on later requests.
- `smoke.js` - node smoke over the same module through the cwist-wasm
  wrapper; covers the 200/400/201/404 paths, the db image endpoint, and
  session survival across module instances. The Service Worker itself is
  verified manually in a browser (steps in the example's README).

Built and run in CI (`.github/workflows/wasm.yml`). See the example's
README.md for build and local serving instructions.

## Not covered (yet)

- WASI 0.2 (`wasm32-wasip2`) is now supported and CI-gated; see
  `docs/api/wasi.md`. Cloudflare Workers and Fastly Compute are not yet
  evaluated — everything here still assumes an Emscripten `Module` host.
- Published npm package / release artifact; today every consumer builds from
  source with `make wasm`.
- WASM CI; `wasm-smoke` is a manual check, so run it before touching
  `WASM_SRCS`, `typedarray.h`, or anything under `EM_JS`.

## Streaming producer (handlers generate the body chunk-by-chunk)

`cwist_http_response_stream_begin/write/end()` (issue #201 Phase 1) close
the buffered-producer gap: a handler switches the response to chunked
mode, writes the body incrementally, and ends the stream.

- Under `cwist_app_dispatch_memory()` the framed bytes buffer and
  serialize like any other body (`Transfer-Encoding: chunked`, chunk
  framing applied at write time).
- Under `cwist_app_dispatch_stream()` each write reaches the host sink
  immediately, head first (sent lazily on the first write), so SSE-style
  handlers deliver chunks while still running; backpressure is the sink
  rejecting a write (-2 from the dispatch).
- A body assigned before `begin` is discarded; writes after `end` fail;
  a handler that returns without `end` is finalized implicitly.
- `cwist_http_response_stream_write(res, data, 0)` is a no-op (an empty
  chunk is the chunked terminator, never emitted mid-stream).
