/**
 * @file shutdown.c
 * @brief Graceful shutdown implementation.
 */

#define _POSIX_C_SOURCE 200809L
#include <cwist/sys/app/shutdown.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#ifndef __wasi__
#include <sys/socket.h>
#endif

atomic_int g_cwist_running = 1;
int g_cwist_listen_fd = -1;
int g_cwist_udp_fd = -1;
/* Upper bound for the post-stop connection drain. Overridable with
 * CWIST_DRAIN_TIMEOUT (seconds; 0 skips the drain wait entirely). The drain
 * also exits early once no C1M connection is left, so an idle server no
 * longer sits out the full timeout on every shutdown. */
int g_cwist_drain_timeout_sec = 5;

/**
 * @brief Constructor that initializes the drain timeout from the environment.
 *
 * Runs automatically before main(). If CWIST_DRAIN_TIMEOUT is set to a value
 * between 0 and 3600 (inclusive) it overrides g_cwist_drain_timeout_sec;
 * missing, empty, or malformed values leave the default in place, and an
 * invalid value is reported on stderr.
 */
static void __attribute__((constructor)) cwist_drain_timeout_init(void) {
    const char *env = getenv("CWIST_DRAIN_TIMEOUT");
    if (!env || !*env) return;
    char *end = NULL;
    long v = strtol(env, &end, 10);
    if (end && *end == '\0' && v >= 0 && v <= 3600) {
        g_cwist_drain_timeout_sec = (int)v;
    } else {
        fprintf(stderr, "[CWIST] Ignoring invalid CWIST_DRAIN_TIMEOUT=\"%s\" (using default %d)\n",
                env, g_cwist_drain_timeout_sec);
    }
}

/**
 * @brief Request a graceful shutdown of the server.
 *
 * Clears the g_cwist_running flag and closes the listening and UDP
 * descriptors (if open). Closing the listen descriptor also calls
 * shutdown(SHUT_RDWR) so accept() loops blocked in other threads wake up
 * with EINVAL. Safe to call more than once and from any thread: each
 * descriptor is claimed atomically, so a second call cannot close a
 * descriptor number that has been reused for an unrelated file.
 */
void cwist_shutdown_request(void) {
    atomic_store(&g_cwist_running, 0);
    /* Take each descriptor atomically so a second request (another thread,
     * a signal, a repeated call) cannot close a number that has meanwhile
     * been reused for an unrelated file. */
    int fd = __atomic_exchange_n(&g_cwist_listen_fd, -1, __ATOMIC_SEQ_CST);
    if (fd >= 0) {
#ifndef __wasi__
        /* close() alone does not interrupt an accept() already blocked in
         * another thread on Linux; shutdown() makes it return (EINVAL),
         * which the accept loops treat as the end of the server. */
        shutdown(fd, SHUT_RDWR);
#endif
        close(fd);
    }
    int udp = __atomic_exchange_n(&g_cwist_udp_fd, -1, __ATOMIC_SEQ_CST);
    if (udp >= 0) {
        close(udp);
    }
}

#ifndef __wasi__
/**
 * @brief Signal handler for SIGTERM/SIGINT; forwards to cwist_shutdown_request().
 * @param sig Signal number (ignored).
 */
static void cwist_shutdown_handler(int sig) {
    (void)sig;
    cwist_shutdown_request();
}
#endif

#ifndef __wasi__
/* What SIGTERM/SIGINT did before CWIST installed its handlers, so
 * cwist_shutdown_restore_handlers() can put it back. Saved only on the first
 * install after a restore: a second install would otherwise record CWIST's
 * own handler as the previous one. */
static struct sigaction g_prev_sigterm;
static struct sigaction g_prev_sigint;
static bool g_prev_saved = false;

/**
 * @brief Install CWIST's SIGTERM/SIGINT handlers.
 *
 * Both signals are routed to cwist_shutdown_handler() with SA_RESTART. The
 * pre-existing handlers are saved on the first install after a restore so
 * cwist_shutdown_restore_handlers() can put them back; a second install
 * without a restore does not overwrite the saved handlers. The previous
 * handlers are only recorded when both sigaction() calls succeed. No-op on
 * WASI hosts, which own the instance lifecycle.
 */
void cwist_shutdown_install_handlers(void) {
    struct sigaction sa, old_term, old_int;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sa.sa_handler = cwist_shutdown_handler;
    bool term_ok = sigaction(SIGTERM, &sa, &old_term) == 0;
    bool int_ok = sigaction(SIGINT, &sa, &old_int) == 0;
    if (!g_prev_saved && term_ok && int_ok) {
        g_prev_sigterm = old_term;
        g_prev_sigint = old_int;
        g_prev_saved = true;
    }
}

/**
 * @brief Restore the SIGTERM/SIGINT handlers saved by the first
 * cwist_shutdown_install_handlers() call.
 *
 * Does nothing if no previous handlers were saved. After restoring, the saved
 * state is cleared so a later install records the handlers then in effect.
 * No-op on WASI hosts.
 */
void cwist_shutdown_restore_handlers(void) {
    if (!g_prev_saved) return;
    sigaction(SIGTERM, &g_prev_sigterm, NULL);
    sigaction(SIGINT, &g_prev_sigint, NULL);
    g_prev_saved = false;
}
#else
/**
 * @brief Install CWIST's SIGTERM/SIGINT handlers.
 *
 * Both signals are routed to cwist_shutdown_handler() with SA_RESTART. The
 * pre-existing handlers are saved on the first install after a restore so
 * cwist_shutdown_restore_handlers() can put them back; a second install
 * without a restore does not overwrite the saved handlers. The previous
 * handlers are only recorded when both sigaction() calls succeed. No-op on
 * WASI hosts, which own the instance lifecycle.
 */
void cwist_shutdown_install_handlers(void) {
    /* WASI hosts own the instance lifecycle; there are no signals to
     * install. Shutdown is driven by the host dropping the context. */
}

/**
 * @brief Restore the SIGTERM/SIGINT handlers saved by the first
 * cwist_shutdown_install_handlers() call.
 *
 * Does nothing if no previous handlers were saved. After restoring, the saved
 * state is cleared so a later install records the handlers then in effect.
 * No-op on WASI hosts.
 */
void cwist_shutdown_restore_handlers(void) {}
#endif

/**
 * @brief Reset shutdown state so the process can serve again.
 *
 * Sets g_cwist_running back to 1 and marks the listening and UDP descriptors
 * as closed (-1). Does not close any open descriptors or touch signal
 * handlers.
 */
void cwist_shutdown_reset(void) {
    atomic_store(&g_cwist_running, 1);
    __atomic_store_n(&g_cwist_listen_fd, -1, __ATOMIC_SEQ_CST);
    __atomic_store_n(&g_cwist_udp_fd, -1, __ATOMIC_SEQ_CST);
}
