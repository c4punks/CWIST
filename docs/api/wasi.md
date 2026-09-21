# WASI targets (issue #93 Phase 3 follow-up)

Date: 2026-09-21 (updated). Verdict: **both WASI flavours work.** Preview1
(`wasm32-wasi`) runs the in-memory dispatch surface under wasmtime, and
**WASI 0.2 (`wasm32-wasip2`) binds real sockets and serves HTTP** through
`wasi:sockets`. Quirks are handled on the CWIST side (see below).

## Flavour detection

`__wasi__` is defined by both flavours but their libc capabilities differ
sharply, so CWIST feature-gates on what the sysroot actually provides
(`include/cwist/sys/wasi.h`):

| | preview1 | wasip2 |
|---|---|---|
| socket runtime gate | `CWIST_WASI_NO_SOCKETS` | `CWIST_WASI_SOCKETS` |
| sockets | placeholder `sys/socket.h` | full `wasi:sockets` surface |
| discriminator | — | `<netdb.h>` (preview1 lacks it) |
| fork / signals / rlimits / threads | no | no |
| `getpid` | no | `libwasi-emulated-getpid` |
| `sendmsg(2)` | — | absent: coalesce to `send()` |
| UDP / TLS | — | UDP optional; TLS not linked (cleartext) |

Guards that exclude fork/signals/rlimits/threads stay on plain `__wasi__`;
guards around the socket server runtime key off `CWIST_WASI_(NO_)SOCKETS`.

## WASI 0.2 socket server

`make wasip2-smoke` builds `libcwist_wasip2.a` — `WASM_SRCS` plus
`src/sys/wasi/compat.c`, `metrics.c`, `writer_fast.c`, `async.c`,
`src/core/log/log.c`, `src/sys/sys_info.c`, and the libttak units the
serving path pulls in (`net/lattice.c`, `net/mols_control.c`,
`shared/shared.c`, `timing/deadline.c`, `mem/epoch.c`, `mem/mem.c`,
`mem/fastpath.c`, `mem/owner.c`, `mem/abstract.c`) — then runs
`tests/wasip2_smoke.c` under:

```
wasmtime run -S preview2=y -S tcp=y -S inherit-network=y \
    --env CWIST_C1M_MODE=0 wasip2_smoke.wasm
```

The smoke binary verifies in-memory dispatch, then `cwist_app_listen()`
serves cleartext HTTP on the blocking accept loop (single-threaded host: no
worker pool, no epoll reactor, no fork). The Makefile target probes it with
curl and kills the wasmtime process afterwards. Measured output:

```
wasip2: in-memory dispatch OK
wasip2: listening on port 18099
wasip2-smoke: PASS (socket server served /hello over wasi:sockets)
```

CWIST-side quirk handling for wasip2:

- `sendmsg`/`recvmsg` do not exist in wasi-libc's socket layer
  (`wasi:sockets` has no scatter-gather): `cwist_http_sendmsg_all()` and
  `cwist_http_sendmsg_speculative()` coalesce the iov into one buffer and
  drive plain `send()`.
- The C1M reactor is epoll/eventfd-based; `cwist_app_listen()` forces the
  blocking accept fallback under `__wasi__` (it ignores `CWIST_C1M_MODE`
  there).
- TLS connection handlers stay compiled out (no BoringSSL); asking for SSL
  on WASI fails with a clear error. HTTP/3 UDP, the static-cache watcher
  thread, and worker fork/reaping are off (no threads/processes).
- The reactor entry points that parked-write/async paths reference at link
  time are plain-C stubs in `src/sys/wasi/compat.c`; the metrics and
  writer-fast stubs in that file are preview1-only since the real units
  join the wasip2 build.

## WASI preview1 (in-memory dispatch)

`make wasi-smoke` compiles the `WASM_SRCS` subset with wasi-sdk and runs a
dispatch + session smoke under wasmtime. Requires `WASI_SDK` (default
`~/toolchains/wasi-sdk-25.0-x86_64-linux`) and `WASMTIME` on PATH, plus
`libwasi-emulated-pthread`.

### What the preview1 build does differently

- **libttak** (upstream `c4punks/libttak` main): `ttak/compat/pthread.h`
  prefers the sysroot's own `<pthread.h>` when one exists and falls back to
  self-contained single-threaded stubs otherwise; `ttak/mem/mem.h` silences
  `page_base` on targets without `mincore`/`VirtualQuery`.
- **Socket server runtime**: the socket machinery in `http.c`/`app.c`
  compiles out under `CWIST_WASI_NO_SOCKETS`. `cwist_app_listen()` returns
  -1 like on Emscripten; hosts drive requests through
  `cwist_app_dispatch_memory()`.
- **Crypto**: `session.c` and `seq_auth.c` use the bundled header-only
  SHA-256/HMAC (`include/cwist/core/crypto/sha256.h`). Entropy comes from
  `getentropy()` (`__wasi_random_get`).
- **Allocation**: `alloc.c`/`arena.c` take the Emscripten-style plain-libc
  path under `__wasi__`.
- **Link model**: `-fvisibility=hidden -Wl,--gc-sections -Wl,--allow-undefined
  -lwasi-emulated-pthread`, mirroring how the Emscripten link satisfies the
  same server-only references through its system stub libraries plus
  binaryen DCE.

### Verification

`make wasi-smoke` output (wasmtime):

```
WASI dispatch OK (73 bytes)
WASI 404 path handled (rc=-1, no trap)
WASI session secret OK
WASI SMOKE PASS
```

(The 404 rc=-1 with no response buffer is the pre-existing contract on the
Emscripten target too — a missing route is not a dispatch failure.)
