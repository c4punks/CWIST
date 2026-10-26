/**
 * @file platform.h
 * @brief Target capability macros shared by all CWIST sources.
 *
 * Guards test what the target provides instead of which toolchain built it,
 * so a new target (bare metal, issue #447) only has to define its row here:
 *
 *   - CWIST_PLATFORM_HOSTED: a native POSIX host (Linux, macOS, BSD) with
 *     processes, signals, rlimits, threads, a filesystem and sockets. The
 *     server-only subsystems (listen loop, worker pool, HTTPS/H2/H3, static
 *     file cache, nuke DB) compile only here.
 *   - CWIST_PLATFORM_NO_PROC: no fork(2), signals, rlimits, getpid() or
 *     threads (WASI and bare metal). Emscripten keeps its own guards.
 *   - CWIST_PLATFORM_NO_SOCKETS: no BSD socket API; requests reach the app
 *     through cwist_app_dispatch_memory() only (WASI sysroots without
 *     wasi:sockets, and bare metal).
 *   - CWIST_BAREMETAL: no OS at all (arm-none-eabi + newlib on Cortex-M).
 *     Inferred for ARM EABI toolchains that define no OS macro; pass
 *     -DCWIST_BAREMETAL to force it elsewhere. The firmware supplies the
 *     hooks declared in <cwist/sys/port.h>.
 *
 * WASI flavour detection (CWIST_WASI_SOCKETS / CWIST_WASI_NO_SOCKETS) stays
 * in <cwist/sys/wasi.h>; guards that select a WASI-only API (getentropy via
 * wasi-libc, wasi:sockets quirks) keep testing it or __wasi__ directly.
 */

#ifndef __CWIST_SYS_PLATFORM_H__
#define __CWIST_SYS_PLATFORM_H__

#include <cwist/sys/wasi.h>

#if !defined(CWIST_BAREMETAL) && defined(__ARM_EABI__) && !defined(__linux__) && \
    !defined(__unix__) && !defined(__APPLE__) && !defined(_WIN32)
#define CWIST_BAREMETAL 1
#endif

#if !defined(__EMSCRIPTEN__) && !defined(__wasi__) && !defined(CWIST_BAREMETAL)
#define CWIST_PLATFORM_HOSTED 1
#endif

#if defined(__wasi__) || defined(CWIST_BAREMETAL)
#define CWIST_PLATFORM_NO_PROC 1
#endif

#if defined(CWIST_WASI_NO_SOCKETS) || defined(CWIST_BAREMETAL)
#define CWIST_PLATFORM_NO_SOCKETS 1
#endif

#endif /* __CWIST_SYS_PLATFORM_H__ */
