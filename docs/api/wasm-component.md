# CWIST on the WASM component model

**Status:** experimental (issue #203, v3.7 Phase 1). The supported browser
path remains the Emscripten bundle (`docs/api/wasm.md`); this document
describes the component pipeline that is slated to replace it once the WASI
0.3 world is usable end to end.

## Why

v3.6 shipped three WASM flavors: the Emscripten browser bundle (supported),
a WASI preview1 smoke (retired by this work; WASI 0.2 covers its use), and
the WASI 0.2 socket server (`docs/api/wasi.md`). Three toolchains cost more
than they earn, and the Emscripten path carries operational friction: a
heavy emsdk, version-sensitive glue, and the EM_JS formatting incident
(#176). The end state is one pipeline: compile with wasi-sdk, run anywhere,
browsers via a jco-transpiled JS bundle, edge runtimes via wasmtime.

## The boundary

`wit/cwist.wit` defines `world cwist-guest`, the component-model
counterpart of `include/cwist/wasm/wasm_entry.h`:

- `dispatch(request: list<u8>) -> result<list<u8>, dispatch-error>`:
  serialized HTTP/1.1 in and out, exactly what `_cwist_wasm_dispatch`
  handles today. The app-side parser needs no changes.
- `use-session(secret: option<string>)`: host-injected session signing
  secret, same contract as `_cwist_wasm_use_session`.
- Buffer disposal that today requires an explicit `_cwist_wasm_dispose`
  round-trip becomes implicit: the canonical ABI copies the result out of
  guest linear memory, and the guest frees its own buffer after return.
- Streaming lives in the separate `cwist-guest-stream` world (0.3 only):
  `host.send-chunk` delivers chunks with backpressure, and
  `dispatch-stream` mirrors `_cwist_wasm_dispatch_stream`. Landed and
  smoke-tested; see below.

## Toolchain status (measured 2026-09-21)

- WASI 0.3: ratified by the WASI subgroup; `wasi:cli 0.3.0` is official
  with component-model async native.
- wasi-sdk 34 (2026-08-25): ships a `wasm32-wasip3` sysroot. CWIST builds
  and links unchanged via `make wasip2-smoke WASIP2_TARGET=wasm32-wasip3`;
  the recipe drops `-lwasi-emulated-pthread` automatically because the
  wasip3 libc covers those symbols (the p1/p2 sysroots keep the library).
- wasmtime 48: runs the resulting component. Startup, in-memory dispatch,
  and socket listen all work.
- jco 1.34.0: transpiles both 0.2 and 0.3 components to JS. 0.3 exports
  lower to async functions; WASI 0.3 imports are satisfied by
  `@bytecodealliance/preview3-shim` (evaluated below).

## Evaluated: preview3-shim (2026-09-23)

`make component-smoke-p3` runs the full dispatch guest as a 0.3 component:
same guest C source, built for `wasm32-wasip3` with wasi-sdk >= 34,
componentized with `wasm-tools component embed`, transpiled by jco 1.34,
hosted in node by preview3-shim 0.6.1, and driven through the same five
assertions as the 0.2 smoke. PASS. Caveats, all host-side:

- node needs `--experimental-wasm-jspi` (JSPI drives the canonical-ABI
  async lowering). The Makefile passes it; no browser flag decision is
  baked in.
- jco lowers every 0.3 export to an async function, so against a wasip3
  guest `handle()` returns a Promise; `wasm/npm/component.js` detects
  this and `await` works uniformly for both targets.
- preview3-shim keeps the node event loop alive after completion, so the
  smoke exits explicitly.

## Landed: native async for streaming/SSE (2026-09-23)

The 0.3 shape for the SSE path (today an EM_JS chunk callback on
Emscripten) is the `cwist-guest-stream` world in wit/cwist.wit: the async
host import `host.send-chunk` plus the sync export `dispatch-stream`.
`make component-stream-smoke-p3` proves it end to end: an SSE route
(`/events`) dispatches through `cwist_app_dispatch_stream()`, each
serialized chunk crosses to a JS host implementation of `sendChunk`, and
the head-first chunk order is asserted.

The pump needed no continuation rewrite. The chunk sink initiates the
async import and, unless it returns immediately, blocks on a waitable
set: the export task suspends mid-pump and resumes when the host
resolves the call, so the synchronous `write_fn` loop in
`cwist_app_dispatch_stream()` runs unchanged. Backpressure falls out of
the host delaying resolution. What the spike established before this
landed: `async func` parses and validates in WIT; wit-bindgen 0.62 emits
waitable-set/callback bindings that compile for wasm32-wasip3 with
wasi-sdk 34; jco surfaces custom host imports as plain ESM imports, which
is how the smoke (and any bundler consumer) wires `sendChunk`.

Only wasm32-wasip3 builds this world: 0.2 components cannot lower async
imports. Bindings are generated per world (`wit-bindgen --world`), and
`wasm-tools component embed` picks the world explicitly
(`--world cwist-guest[-stream]`).

## Resolved: socket request path under wasip3 (audited 2026-09-22)

The first HTTP request to a wasip3 socket server used to trap with an
out-of-bounds read in `serialize_headers` (`res->version->data`,
http.c:1885) at a wild negative address (~`0xffff9c00`), with the response
object intact right up to the send call. The audit (wasi-sdk 34,
wasmtime 49, `-O1`/`-O2`, buffer sizes from 1 KiB/2 KiB to 8 KiB/16 KiB)
established by elimination:

- Not a CWIST memory bug: the response object is valid at the send call
  site; the faulting access misbehaves only after the p3-switched socket
  path runs.
- Not buffer sizes: identical fault across a 16x range.
- Codegen-sensitive (an entry print made it vanish once), which sent the
  audit down a sibling-call-elimination dead end; that flag alone does
  not fix `-O2`.
- Root cause: **stack exhaustion.** wasm-ld's default 64 KiB stack is too
  small for the socket-serving chain once p3's async lowering is in the
  frame mix. `-z stack-size=131072` passes at `-O1` and `-O2`
  deterministically; the Makefile links every wasip2/wasip3 guest with
  `-Wl,-z,stack-size=$(WASIP2_STACK_BYTES)` (default 1 MiB), which covers
  wasip3 with headroom.

A plain BSD-socket minimal repro (accept/read/write, no CWIST) passes
unchanged on the same toolchain, so small-frame guests are unaffected;
CWIST's ~24 KiB of request/response stack buffers plus the p3 lowering
overhead is what crosses the default limit.

## Stages

1. **WIT + validation (landed).** The world definition, `make wit-check`,
   `make jco-transpile`, and this document.
2. **jco browser spike (landed).** `tests/wasm_component_guest.c` is a dispatch guest exporting
   the cwist-guest world through wit-bindgen's canonical ABI shims
   (`include/cwist/wasm/wasm_component.h` holds the shared helpers);
   `make component-smoke` builds it for wasm32-wasip2, componentizes with
   `wasm-tools component embed`, transpiles with jco, and drives it from
   node through the `createCwistFromComponent` adapter (`wasm/npm/component.js`)
   against the same assertions as the Emscripten wrapper test, including the
   signed-cookie session roundtrip and the dispatch-error variant. WASI
   imports are satisfied by `@bytecodealliance/preview2-shim`. Browser
   packaging is gated by `make component-browser-smoke`: a
   `--no-nodejs-compat` re-transpile plus the adapter must bundle under
   esbuild with `--platform=browser` and no node-only imports, which is
   the property a bundler consumer needs (the bundle itself loads its wasm
   shards via fetch and is not executed in the smoke). The npm package
   exposes the adapter through an exports map (`cwist-wasm/component`).
   Emscripten stays supported regardless.
3. **0.3 cutover (conditional).** Both preconditions held up under
   evaluation and the streaming boundary has landed in
   `cwist-guest-stream`. Remaining before the cutover: track JSPI
   shipping unflagged in node and browsers, and port the SSE examples
   off the EM_JS callback. Only then does the Emscripten build leave CI.

## Measured while building stage 2

- The WIT never passed wit-bindgen validation as written: the error
  variants sat at package top level. They moved into the `guest`
  interface; `make wit-check` now actually validates the world.
- `wasm-tools component embed` (1.259) emits the final component in one
  step: it merges the cwist-guest world into wasi-sdk's component-type
  section, so no separate `component new` pass is needed.
- wit-bindgen 0.62 lowers guest exports to plain C functions returning
  bool (ok/err out-params). The returned `list<u8>` must be a libc
  allocation because the generated `cabi_post` hook frees it; `cwist_alloc`
  is libc-backed under `__wasi__`, so the dispatch response hands over
  directly and the explicit dispose entry point disappears as designed.
- With two worlds in wit/, both wit-bindgen and `wasm-tools component
  embed` require an explicit `--world`; bindings land in per-world
  directories under .wit-bindings and each guest links its own
  component-type object.

## Non-goals

- No ABI break for the `cwist-wasm` npm wrapper at any stage.
- No removal of the Emscripten build before stage 3 completes.
