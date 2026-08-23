#define _POSIX_C_SOURCE 200809L
#include <cwist/sys/app/app.h>
#include <cwist/sys/io/reactor.h>
#include <cwist/net/http/http.h>
#include <cwist/net/http/https.h>
#include <cwist/net/http/http2.h>
#include <cwist/core/mem/alloc.h>
#include <cwist/sys/app/shutdown.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>
#if defined(__linux__)
#include <cwist/sys/io/uring_sqpoll.h>
#endif

static cwist_reactor_t *g_reactor = NULL;

/**
 * @brief Decide whether the app is fully configured for HTTPS serving.
 *
 * Checks that the app pointer is non-NULL and that SSL mode, an SSL context,
 * and an HTTPS request handler are all present. No side effects; read-only.
 *
 * @param app Application object, may be NULL.
 * @return true if HTTPS dispatch can be used for accepted connections.
 */
static bool app_use_https(const cwist_app *app) {
    return app && app->use_ssl && app->ssl_ctx && app->https_request_handler;
}

/**
 * @brief Reactor callback: accept pending connections on a listening socket.
 *
 * Accepts all currently queued connections in a loop (non-blocking), disables
 * Nagle (and enables TCP_QUICKACK on Linux where available) on each accepted
 * socket, then hands it either to the HTTPS dispatcher or to the HTTP
 * connection pool, depending on the app configuration. Connections accepted
 * while SSL is requested but not fully configured are closed with a warning.
 *
 * After the accept loop drains, the listening socket is re-armed so the worker
 * keeps accepting: transient reactor re-add failures (e.g. a full io_uring SQ)
 * are retried up to 1000 times with a 10 ms backoff before giving up with a
 * fatal message; if the process is shutting down (@c g_cwist_running cleared),
 * the reactor is stopped instead. Runs on a reactor worker thread; the
 * accepted sockets are owned by the HTTP/HTTPS pools once dispatched.
 *
 * @param fd    Listening socket fd that became readable.
 * @param ctx   Reactor slot payload holding the `cwist_app *` pointer.
 */
static void async_accept_cb(int fd, void *ctx) {
    /* ctx is the reactor slot's inline payload holding the app pointer. */
    cwist_app *app = *(cwist_app **)ctx;
    int client_fd;
#if defined(__linux__)
    while ((client_fd = accept4(fd, NULL, NULL, SOCK_CLOEXEC)) >= 0) {
#else
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    while ((client_fd = accept(fd, (struct sockaddr*)&addr, &len)) >= 0) {
#endif
        int nodelay = 1;
        setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
#if defined(__linux__) && defined(TCP_QUICKACK)
        int quickack = 1;
        setsockopt(client_fd, IPPROTO_TCP, TCP_QUICKACK, &quickack, sizeof(quickack));
#endif

        if (app_use_https(app)) {
            cwist_https_dispatch(client_fd, app->ssl_ctx, app->https_request_handler, app);
        } else if (app && !app->use_ssl) {
            cwist_http_pool_submit(client_fd, cwist_app_http_handler, app);
        } else {
            fprintf(stderr, "[async] SSL request accepted but HTTPS not ready (use_ssl=%d ssl_ctx=%p handler=%p), closing fd=%d\n",
                    app ? app->use_ssl : -1,
                    app ? (void*)app->ssl_ctx : NULL,
                    app ? (void*)app->https_request_handler : NULL,
                    client_fd);
            close(client_fd);
        }
    }

    /* Re-arm the listening socket so we can accept the next batch. */
    if (g_reactor) {
        if (atomic_load(&g_cwist_running)) {
            cwist_reactor_add(g_reactor, fd, async_accept_cb, &app, sizeof(app));
        } else {
            cwist_reactor_stop(g_reactor);
        }
    }
}

/**
 * @brief Run the async (reactor-based) HTTP/HTTPS server loop for C1M scale.
 *
 * Initializes the HTTPS thread pool when the app serves HTTPS, otherwise the
 * HTTP thread pool (without re-reading CWIST_C1M_MODE, which
 * cwist_app_listen_ex() may have overridden). Sets the server socket
 * non-blocking, enables SO_REUSEPORT when available, creates the reactor, and
 * registers the accept callback on @p server_fd. Blocks in cwist_reactor_run()
 * until the reactor is stopped (e.g. on shutdown), then tears down the reactor
 * and the thread pool that was initialized.
 *
 * @param server_fd Listening socket to serve; set non-blocking on entry.
 * @param app       Application object; NULL or partially configured apps fall
 *                  back to plain HTTP.
 * @return cwist_error_t with @c err_i16 set to 0 on orderly shutdown, or -1 if
 *         setup failed (pool init, fcntl, or reactor creation); on the -1 path
 *         any initialized resources are cleaned up first and a message is
 *         printed to stderr.
 */
cwist_error_t cwist_async_server_loop(int server_fd, cwist_app *app) {
    cwist_error_t err;
    memset(&err, 0, sizeof(err));
    err.errtype = CWIST_ERR_INT16;
    err.error.err_i16 = -1;

    bool use_https = app_use_https(app);
    if (use_https) {
        if (https_pool_init() != 0) {
            fprintf(stderr, "[async] Failed to init HTTPS thread pool\n");
            return err;
        }
    } else {
        /* This loop only drives reactor workers: do not re-read
         * CWIST_C1M_MODE, which cwist_app_listen_ex() may have overridden. */
        if (cwist_http_pool_init_mode(true) != 0) {
            fprintf(stderr, "[async] Failed to init HTTP thread pool\n");
            return err;
        }
    }

    int flags = fcntl(server_fd, F_GETFL, 0);
    if (flags < 0 || fcntl(server_fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        perror("[async] Failed to set server socket non-blocking");
        if (use_https) https_pool_destroy();
        else cwist_http_pool_destroy();
        return err;
    }

#if defined(SO_REUSEPORT)
    int reuseport = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEPORT, &reuseport, sizeof(reuseport));
#endif

    g_cwist_listen_fd = server_fd;

    g_reactor = cwist_reactor_create();
    if (!g_reactor) {
        fprintf(stderr, "[async] Failed to create reactor\n");
        if (use_https) https_pool_destroy();
        else cwist_http_pool_destroy();
        return err;
    }

    cwist_reactor_add(g_reactor, server_fd, async_accept_cb, &app, sizeof(app));
    printf("[io_uring/kqueue/epoll] Reactor started for C1M scale.\n");

    cwist_reactor_run(g_reactor);

    cwist_reactor_destroy(g_reactor);
    g_reactor = NULL;

    if (use_https) {
        https_pool_destroy();
    } else {
        cwist_http_pool_destroy();
    }

    err.error.err_i16 = 0;
    return err;
}
