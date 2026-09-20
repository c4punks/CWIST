# WASI target (issue #93 Phase 3 follow-up)

Date: 2026-09-20 (updated). Verdict: **the WASI (preview1) target is real and
working for the in-memory dispatch surface.** `make wasi-smoke` compiles the
`WASM_SRCS` subset with wasi-sdk, links it, and runs a dispatch + session
smoke under wasmtime.

## Toolchain

The earlier evaluation probed with Debian's clang and no wasi sysroot; every
"blocking finding" from that probe was host-header leakage, not a real porting
problem. With a real sysroot (wasi-sdk 25, `wasm32-wasi`) the picture is:

| unit | result (wasi-sdk 25) |
|---|---|
| `src/core/sstring/sstring.c` | compiles |
| `src/net/http/session.c` | compiles |
| `lib/sqlite3/sqlite3.c` | compiles (no defines needed beyond the standard set) |

Prerequisites: `WASI_SDK` (default `~/toolchains/wasi-sdk-25.0-x86_64-linux`)
and `WASMTIME` on PATH, plus `libwasi-emulated-pthread` (ships with wasi-sdk).

## What the WASI build does differently

- **libttak** (upstream `c4punks/libttak` main): `ttak/compat/pthread.h`
  prefers the sysroot's own `<pthread.h>` when one exists and falls back to
  self-contained single-threaded stubs otherwise; `ttak/mem/mem.h` silences
  `page_base` on targets without `mincore`/`VirtualQuery`.
- **Socket server runtime**: WASI preview1 has no sockets, signals, rlimits,
  or processes. The socket machinery in `http.c`/`app.c` (server loops,
  accept/fork paths, multiport, H3 threads, TLS upgrade stubs) compiles out
  under `__wasi__` — the same code the Emscripten target already excludes or
  stubs. `cwist_app_listen()` returns -1 like on Emscripten; hosts drive
  requests through `cwist_app_dispatch_memory()`.
- **Crypto**: `session.c` and `seq_auth.c` use the bundled header-only
  SHA-256/HMAC (`include/cwist/core/crypto/sha256.h`). Entropy under WASI
  comes from `getentropy()` (wasi-libc maps it to `__wasi_random_get`).
- **Allocation**: `alloc.c`/`arena.c` take the Emscripten-style plain-libc
  path under `__wasi__` as well (no owner-guard bridge, no epoch GC).
- **Link model**: `-fvisibility=hidden -Wl,--gc-sections -Wl,--allow-undefined
  -lwasi-emulated-pthread`, mirroring how the Emscripten link satisfies the
  same server-only references through its system stub libraries plus
  binaryen DCE. `src/sys/wasi/compat.c` provides plain-C stubs for the
  CWIST-internal server symbols that survive gc reachability (reactor,
  metrics, parked-writer clock, HTTP/2 upgrade entry points).

## Verification

`make wasi-smoke` output (wasmtime):

```
WASI dispatch OK (73 bytes)
WASI 404 path handled (rc=-1, no trap)
WASI session secret OK
WASI SMOKE PASS
```

(The 404 rc=-1 with no response buffer is the pre-existing contract on the
Emscripten target too — a missing route is not a dispatch failure.)

## Still out of scope

- Preview1 has no sockets: CWIST cannot *bind* under WASI. Edge-runtime
  reach with real listeners waits for WASI 0.2 / `wasi:sockets` (separate
  workstream, `wasm32-wasip2` in wasi-sdk 25 is probe-ready).
- Threads: single-threaded host. `libwasi-emulated-pthread` supplies the
  pthread symbols server code references; nothing actually spawns.
