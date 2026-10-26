/**
 * @file port.h
 * @brief Hooks a bare-metal firmware supplies to CWIST (issue #447).
 *
 * A CWIST_BAREMETAL build has no OS to ask for time or randomness, so the
 * firmware defines the functions below (typically on top of SysTick/DWT and
 * the RNG peripheral). They are plain link-time symbols with no default:
 * leaving one out is a link error, never a silent fallback.
 *
 * Everything else the core needs from the OS is either standard newlib
 * (malloc via _sbrk, stdio via _write, time() via _gettimeofday) or provided
 * by CWIST itself in src/sys/port/baremetal.c (single-threaded pthread,
 * clock_gettime, nanosleep, 64-bit atomics for Cortex-M).
 *
 * The bare-metal runtime is single-threaded: call into CWIST from one
 * context only (the main loop or one RTOS task), never from an ISR.
 *
 * Wall clock: CWIST reads it through time(). Without an RTC behind
 * newlib's _gettimeofday, time() fails and responses carry no Date header,
 * as RFC 9110 section 6.6.1 requires for servers without a clock.
 */

#ifndef __CWIST_SYS_PORT_H__
#define __CWIST_SYS_PORT_H__

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Fill a buffer with cryptographically secure random bytes.
 *
 * Seeds the hash-flooding defense (siphash) and generates session secrets
 * and sequence-auth nonces. Back it with a hardware TRNG; a PRNG seeded from
 * a constant or a timer makes those defenses guessable.
 *
 * @param buf Destination buffer.
 * @param len Number of bytes to write.
 * @return 0 when all len bytes were written, non-zero on failure. CWIST
 *         treats failure as fatal where no safe fallback exists.
 */
int cwist_port_entropy(void *buf, size_t len);

/**
 * @brief Monotonic time since an arbitrary epoch (usually boot).
 *
 * Backs clock_gettime(CLOCK_MONOTONIC), which drives timeouts, the GC and
 * metrics. Must never go backwards; resolution of 1 ms or better.
 *
 * @return Nanoseconds since the epoch.
 */
uint64_t cwist_port_monotonic_ns(void);

#ifdef __cplusplus
}
#endif

#endif /* __CWIST_SYS_PORT_H__ */
