# WASI target evaluation (issue #93 Phase 3)

Date: 2026-09-20. Verdict: **scope it as a separate workstream; do not block
v3.6 on it.** The current Emscripten/browser target and the WASI/edge-runtime
target share the `WASM_SRCS` subset philosophy but diverge on toolchain,
host APIs, and session secret plumbing enough that they should be built and
gated separately.

## What was probed

Local toolchain: Debian clang 19.1.7, which ships a `wasm32-wasi`
(wasi-libc preview1) sysroot. Probe: compile representative `WASM_SRCS`
translation units with

```
clang --target=wasm32-wasi -std=c17 -O2 \
    -I./include -I./lib -I./lib/cjson -I./lib/boringssl/include \
    -I./lib/libttak/include -I./lib/sqlite3 -c <file>
```

A trivial program compiles and links. The CWIST units do not (yet):

| unit | result |
|---|---|
| `src/core/sstring/sstring.c` | fails |
| `src/net/http/session.c` | fails (13 errors) |
| `lib/sqlite3/sqlite3.c` | fails (1 error) |

## Blocking findings, in order of effort

1. **Host glibc header leakage through libttak compat shims.** The root
   failure is `/usr/include/pthread.h` (`regparm is not valid on this
   platform`) pulled in via `ttak/compat/pthread.h`. Under the wasi target
   the host include directories must not be searched at all: the build
   needs a hermetic sysroot setup (`--sysroot` / `-nostdinc` + explicit
   wasi-libc include path) or libttak compat guards for `__wasi__`. The
   Emscripten build does not hit this because emcc's sysroot already
   isolates headers.
2. **Linux-only headers inside the dependency tree.** sqlite3.c reaches
   `<linux/types.h>` (`asm/types.h` missing) through a syscall-adjacent
   include path; WASI preview1 has no such headers. Needs a `__wasi__`
   guard or a sqlite compile-define that keeps it on pure wasi-libc APIs
   (sqlite already supports `SQLITE_OS_UNIX`-style ports; the
   `SQLITE_ENABLE_DESERIALIZE` in-memory path we rely on is OS-agnostic
   once the headers resolve).
3. **Session crypto is now WASM-portable, secret plumbing is not.** The
   bundled header-only SHA-256/HMAC added for Phase 3
   (`include/cwist/core/crypto/sha256.h`) removes the OpenSSL link blocker
   for WASI the same way it did for Emscripten. What remains host-specific
   is entropy and secret persistence: `/dev/urandom` does not exist on
   WASI and there is no `crypto.getRandomValues` global. A WASI target
   would need an explicit `cwist_app_use_session(app, secret)` contract
   (the host injects the secret) - which is the recommended production
   pattern for Emscripten too.
4. **Preview1 has no sockets, threads, or clocks beyond WASI's.** That
   matches the existing `WASM_SRCS` exclusion list, so the in-memory
   dispatch surface (`dispatch_memory`, `dispatch_stream`, the streaming
   request assembly) is expected to port once the headers resolve.

## Recommendation

Treat WASI as issue #93's "decide and scope" outcome rather than a v3.6
deliverable: the decision is **yes, eventually, as a separate target**
(`wasm-wasi` Makefile target + its own CI job), with the concrete
prerequisite list above. The blocker work (libttak `__wasi__` compat,
sqlite header hygiene, sysroot hermeticity) is independent of the
browser/Emscripten path and can proceed in parallel without touching it.
Until then, edge-runtime reach goes through the Emscripten build embedded
in a JS host (Service Worker/fetch interception), which Phase 4 covers.
