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
- Streaming is intentionally absent for now. The Emscripten path pushes
  chunks through an EM_JS callback; the component equivalent should be a
  host import built on `wasi:io` streams, which lands with the 0.3 async
  world. Adding a polling export now would bake in the wrong shape.

## Toolchain status (measured 2026-09-21)

- WASI 0.3: ratified by the WASI subgroup; `wasi:cli 0.3.0` is official
  with component-model async native.
- wasi-sdk 34 (2026-08-25): ships a `wasm32-wasip3` sysroot. CWIST builds
  and links unchanged via `make wasip2-smoke WASIP2_TARGET=wasm32-wasip3`;
  the recipe drops `-lwasi-emulated-pthread` automatically because the
  wasip3 libc covers those symbols (the p1/p2 sysroots keep the library).
- wasmtime 48: runs the resulting component. Startup, in-memory dispatch,
  and socket listen all work.
- jco 1.34.0: transpiles the 0.2 component to a JS module today (see
  `make jco-transpile`); 0.3 host bindings live in its `preview3-shim`
  package and need evaluation.

## Known issue: socket request path under wasip3

The first HTTP request to a wasip3 socket server traps with an out-of-bounds
read in `serialize_headers` (`res->version->data`, http.c:1885): response
arena memory is corrupt by the time the response serializes. Reproduced
with a DWARF backtrace under wasmtime 48; in-memory dispatch is unaffected,
and the identical code passes on wasip2 and native, so this is not a CWIST
regression. Prime suspect is the wasip3 socket shim in wasi-libc, which
wasi-sdk 34 labels work in progress ("more work towards a wasip3 target").
Needs a wasi-libc-side audit before the socket server can move to 0.3; the
0.2 target remains the supported WASI build meanwhile.

## Stages

1. **WIT + validation (landed).** The world definition, `make wit-check`,
   `make jco-transpile`, and this document.
2. **jco browser spike.** Transpile the wasip2 guest with jco, wrap it in
   the existing `cwist-wasm` npm API, and pass the wasm smoke suite
   alongside the Emscripten build. Emscripten stays supported regardless.
3. **0.3 cutover (conditional).** After the wasip3 socket runtime issue
   above is resolved (wasi-libc fix or newer wasi-sdk), evaluate jco
   `preview3-shim` for the browser bundle and native async for the
   streaming/SSE paths (the Asyncify replacement), then drop the
   Emscripten build from CI.

## Non-goals

- No ABI break for the `cwist-wasm` npm wrapper at any stage.
- No removal of the Emscripten build before stage 3 completes.
