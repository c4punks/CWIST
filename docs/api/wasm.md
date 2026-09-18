# CWIST on WASM (Emscripten)

CWIST can be compiled to WebAssembly via Emscripten and driven entirely from
JS: an HTTP request goes in as bytes, the router + middleware + handlers run
inside WASM, and the serialized HTTP response comes back as a zero-copy
`Uint8Array` view on the WASM heap.

This page documents what is in scope, how to build it, and the two integration
points (`cwist_app_dispatch_memory` and the TypedArray helpers). See
`tests/wasm_smoke.c` for a complete, runnable example.

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
- Templates and HTML (`template`, `html/builder`, `html/css_composer`)
- `cwist_db` (SQLite) including `cwist_db_open_memory()` and
  `cwist_db_serialize()` for blob-shaped databases

Deliberately excluded: sockets and the accept loop, TLS/BoringSSL,
QUIC/HTTP/3, gRPC (needs HTTP/2), WebSocket transport, threads/scheduler,
compression, and the DB pool/sync clients.

## Request dispatch from JS

`cwist_app_dispatch_memory()` runs one whole request through the app and
returns the serialized HTTP response. It is whole-request-in,
whole-response-out; there is no streaming entry point yet.

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

The archive grows 5x but the linked smoke binary 16x because the db round
trip also pulls cJSON query-result building into the link. If this is too
heavy for db-less consumers, the future opt-out/split build decision (issue
#93 Phase 3) has these numbers as its input.

## Sessions and cookies

`cookie.c` and `session.c` are compiled in, but persistence across requests
is the host's responsibility: if the host does not keep the same WASM
instance (and heap) alive between requests, in-memory session state does not
survive. Pass session data through signed cookies or serialize/deserialize
explicitly until a documented story lands.

## Not covered (yet)

- WASI target for non-Emscripten edge runtimes (Cloudflare Workers, wasmtime,
  Fastly Compute) - everything here assumes an Emscripten `Module` host.
- Streaming responses from `cwist_app_dispatch_memory()`.
- Published npm package / release artifact; today every consumer builds from
  source with `make wasm`.
- WASM CI; `wasm-smoke` is a manual check, so run it before touching
  `WASM_SRCS`, `typedarray.h`, or anything under `EM_JS`.
