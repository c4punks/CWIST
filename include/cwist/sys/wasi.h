/**
 * @file wasi.h
 * @brief WASI target feature detection shared by all CWIST sources.
 *
 * `__wasi__` is defined by both WASI preview1 (`wasm32-wasi`) and WASI 0.2
 * (`wasm32-wasip2`) targets, but their libc capabilities differ sharply:
 * preview1 has no sockets at all, while wasip2 exposes `wasi:sockets`
 * through the standard <sys/socket.h> API (plus netdb, MSG_NOSIGNAL, poll).
 * Neither has fork(2), signals, or rlimits, and both are single-threaded
 * unless the -threads sysroot variant is used.
 *
 * Feature-gate on what the sysroot actually provides instead of the bare
 * `__wasi__` macro:
 *
 *   - CWIST_WASI_SOCKETS: socket server runtime compiles in (wasip2).
 *   - CWIST_WASI_NO_SOCKETS: in-memory dispatch only (preview1, Emscripten
 *     is covered separately by its own __EMSCRIPTEN__ guards).
 *
 * Guards that exclude fork/signals/rlimits/threads stay on plain
 * `__wasi__` because neither WASI flavour provides those.
 */

#ifndef __CWIST_SYS_WASI_H__
#define __CWIST_SYS_WASI_H__

#if defined(__wasi__)
/* Both preview1 and wasip2 ship a <sys/socket.h> file, but only wasip2's
 * declares anything (wasi:sockets); preview1's is an empty placeholder.
 * wasi-libc pairs the real socket surface with <netdb.h>, which preview1
 * does not ship at all - use it as the discriminator. */
#if defined(__has_include)
#if __has_include(<netdb.h>)
#define CWIST_WASI_SOCKETS 1
#else
#define CWIST_WASI_NO_SOCKETS 1
#endif
#else
#define CWIST_WASI_NO_SOCKETS 1
#endif
#endif

#endif /* __CWIST_SYS_WASI_H__ */
