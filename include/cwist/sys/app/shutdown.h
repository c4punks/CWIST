/**
 * @file shutdown.h
 * @brief Unified Graceful Shutdown support for CWIST.
 *
 * Provides a global atomic 'running' flag and signal handling for SIGTERM/SIGINT.
 * All protocol server loops (HTTP/1.1, HTTP/2, HTTP/3) check this flag.
 */

#ifndef __CWIST_SHUTDOWN_H__
#define __CWIST_SHUTDOWN_H__

#include <stdatomic.h>
/* WASI preview1 has no signals (wasi-libc's <signal.h> is a hard #error
 * without emulation); signal-based shutdown is compiled out there. */
#ifndef __wasi__
#include <signal.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Global running flag. Checked by all server event loops. */
extern atomic_int g_cwist_running;

/** @brief Registered TCP listening socket (for signal-handler closure). */
extern int g_cwist_listen_fd;

/** @brief Registered UDP socket for HTTP/3 (for signal-handler closure). */
extern int g_cwist_udp_fd;

/** @brief Drain timeout in seconds before process exits. */
extern int g_cwist_drain_timeout_sec;

/**
 * @brief Install SIGTERM and SIGINT handlers for graceful shutdown.
 *
 * The handlers in place before the first install are saved for
 * cwist_shutdown_restore_handlers().
 */
void cwist_shutdown_install_handlers(void);

/**
 * @brief Put back the SIGTERM and SIGINT handlers that were in place before
 * cwist_shutdown_install_handlers(). Does nothing if they are not installed.
 * cwist_app_listen_ex() calls it before returning, so the signals request a
 * graceful stop only while a server runs.
 */
void cwist_shutdown_restore_handlers(void);

/**
 * @brief Request a graceful shutdown, as SIGTERM/SIGINT do: clear the running
 * flag and shut down the listening sockets, so cwist_app_listen() drains and
 * returns. Safe from any thread, a handler or a signal handler; repeated
 * calls are harmless.
 */
void cwist_shutdown_request(void);

/**
 * @brief Reset shutdown state (useful in test suites).
 */
void cwist_shutdown_reset(void);

#ifdef __cplusplus
}
#endif

#endif
