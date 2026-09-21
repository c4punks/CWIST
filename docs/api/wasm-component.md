# CWIST on the WASM component model

**Status:** experimental (issue #203, v3.7 Phase 1). The supported browser
path remains the Emscripten bundle (`docs/api/wasm.md`); this document
describes the component pipeline that is slated to replace it once the WASI
0.3 world stabilizes.

## Why

v3.6 ships three WASM flavors: the Emscripten browser bundle (supported),
a WASI preview1 smoke (retired; WASI 0.2 covers its use), and the WASI 0.2
socket server (`docs/api/wasi.md`). Three toolchains cost more than they
earn, and the Emscripten path carries operational friction: a heavy emsdk,
version-sensitive glue, and the EM_JS formatting incident (#176). The end
state is one pipeline: compile with wasi-sdk, run anywhere, browsers via
a jco-transpiled JS bundle, edge runtimes via wasmtime.

## The boundary

`wit/cwist.wit` defines `world cwist-guest`, the component-model counterpart
of `include/cwist/wasm/wasm_entry.h`:

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

## Stages

1. **WIT + validation (this PR).** The world definition, a `make wit-check`
   gate (runs when `wit-bindgen` is installed; CI pins the toolchain when
   the gate is promoted), and this document.
2. **jco browser spike.** Transpile the wasip2 guest with jco, wrap it in
   the existing `cwist-wasm` npm API, and pass the wasm smoke suite
   alongside the Emscripten build. Emscripten stays supported regardless.
3. **0.3 cutover (conditional).** Only after wasi-sdk ships a stable 0.3
   target, wasmtime runs the 0.3 world without experimental flags, and jco
   transpiles 0.3 components: evaluate native async for the streaming/SSE
   paths (the Asyncify replacement), then drop the Emscripten build from CI.

## Non-goals

- No ABI break for the `cwist-wasm` npm wrapper at any stage.
- No removal of the Emscripten build before stage 3 completes.
