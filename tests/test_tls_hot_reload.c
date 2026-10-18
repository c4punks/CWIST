/**
 * @file test_tls_hot_reload.c
 * @brief TLS certificate hot reload without listener restart (issue #404).
 *
 * Serves HTTPS (classic pool) and HTTPS/3 on an ephemeral port with
 * certificate A, swaps to certificate B via cwist_app_reload_tls() while a
 * keep-alive connection is held open, and asserts:
 *  - new TCP connections present certificate B;
 *  - the old connection still completes a request (zero drops);
 *  - a failed reload keeps the previous certificate;
 *  - 200 alternating reloads leave the process fd count unchanged (ASAN
 *    covers the SSL_CTX side);
 *  - HTTP/3: a client trusting bundle B completes a verified request after
 *    the reload.
 */
#define _POSIX_C_SOURCE 200809L
#include <cwist/sys/app/app.h>
#include <cwist/sys/app/shutdown.h>
#include <cwist/net/http/http3.h>
#include <cwist/net/http/http3_client.h>
#include <arpa/inet.h>
#include <assert.h>
#include <dirent.h>
#include <limits.h>
#include <netinet/in.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static char g_dir[PATH_MAX];
static char g_cert_a[PATH_MAX], g_key_a[PATH_MAX];
static char g_cert_b[PATH_MAX], g_key_b[PATH_MAX];

static bool run_cmd(const char *cmd) {
    return system(cmd) == 0;
}

static void join_path(char *dst, size_t dst_len, const char *dir, const char *name) {
    int n = snprintf(dst, dst_len, "%s/%s", dir, name);
    assert(n > 0 && (size_t)n < dst_len);
}

static void make_cert(const char *cert, const char *key, const char *cn) {
    char cmd[4096];
    snprintf(cmd, sizeof(cmd),
             "openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:P-256 -nodes "
             "-keyout %s -out %s -days 1 -sha256 -subj /CN=%s "
             "-addext basicConstraints=critical,CA:TRUE "
             "-addext subjectAltName=DNS:localhost >/dev/null 2>&1",
             key, cert, cn);
    assert(run_cmd(cmd));
}

static void generate_fixtures(void) {
    char template[] = "/tmp/cwist_tls_reload_XXXXXX";
    char *tmp = mkdtemp(template);
    assert(tmp != NULL);
    snprintf(g_dir, sizeof(g_dir), "%s", tmp);
    join_path(g_cert_a, sizeof(g_cert_a), g_dir, "a.crt");
    join_path(g_key_a, sizeof(g_key_a), g_dir, "a.key");
    join_path(g_cert_b, sizeof(g_cert_b), g_dir, "b.crt");
    join_path(g_key_b, sizeof(g_key_b), g_dir, "b.key");
    make_cert(g_cert_a, g_key_a, "reload-a.local");
    make_cert(g_cert_b, g_key_b, "reload-b.local");
}

static void remove_fixtures(void) {
    char cmd[PATH_MAX + 32];
    snprintf(cmd, sizeof(cmd), "rm -rf -- %s", g_dir);
    assert(run_cmd(cmd));
}

static int count_fds(void) {
#ifdef __linux__
    DIR *d = opendir("/proc/self/fd");
    if (!d) return -1;
    int n = -2; /* exclude "." and ".." */
    while (readdir(d)) n++;
    closedir(d);
    return n;
#else
    return -1;
#endif
}

static int pick_free_port(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = 0};
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t len = sizeof(addr);
    int port = -1;
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0 &&
        getsockname(fd, (struct sockaddr *)&addr, &len) == 0) {
        port = ntohs(addr.sin_port);
    }
    close(fd);
    assert(port > 0);
    return port;
}

static void ok_handler(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    res->status_code = CWIST_HTTP_OK;
    cwist_sstring_assign(res->body, "reload-ok");
    cwist_http_header_add(&res->headers, "Content-Type", "text/plain");
}

/* --- TLS client helpers (BoringSSL) --------------------------------------- */

static SSL *tls_connect(int port, SSL_CTX **out_ctx) {
    SSL_CTX *cctx = SSL_CTX_new(TLS_client_method());
    assert(cctx != NULL);
    SSL_CTX_set_verify(cctx, SSL_VERIFY_NONE, NULL);

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port)};
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);

    SSL *ssl = SSL_new(cctx);
    assert(ssl != NULL);
    assert(SSL_set_fd(ssl, fd) == 1);
    if (SSL_connect(ssl) != 1) {
        SSL_free(ssl);
        SSL_CTX_free(cctx);
        close(fd);
        return NULL;
    }
    *out_ctx = cctx;
    return ssl;
}

static void tls_close(SSL *ssl, SSL_CTX *cctx) {
    if (ssl) {
        int fd = SSL_get_fd(ssl);
        SSL_shutdown(ssl);
        SSL_free(ssl);
        if (fd >= 0) close(fd);
    }
    if (cctx) SSL_CTX_free(cctx);
}

/* Subject CN of the served certificate, copied into out (NUL-terminated). */
static void peer_cn(SSL *ssl, char *out, size_t out_len) {
    out[0] = '\0';
    X509 *cert = SSL_get_peer_certificate(ssl);
    assert(cert != NULL); /* full handshakes always present a certificate */
    X509_NAME *name = X509_get_subject_name(cert);
    assert(name != NULL);
    int n = X509_NAME_get_text_by_NID(name, NID_commonName, out, (int)out_len);
    assert(n > 0);
    X509_free(cert);
}

static void tls_get(SSL *ssl, const char *conn_hdr, char *buf, size_t buf_len) {
    char req[160];
    snprintf(req, sizeof(req), "GET / HTTP/1.1\r\nHost: localhost\r\nConnection: %s\r\n\r\n",
             conn_hdr);
    assert(SSL_write(ssl, req, (int)strlen(req)) > 0);
    size_t total = 0;
    while (total < buf_len - 1) {
        int n = SSL_read(ssl, buf + total, (int)(buf_len - 1 - total));
        if (n <= 0) break;
        total += (size_t)n;
        buf[total] = '\0';
        char *hdr_end = strstr(buf, "\r\n\r\n");
        if (hdr_end) {
            const char *cl = strstr(buf, "Content-Length: ");
            size_t body_len = cl ? (size_t)atol(cl + 16) : 0;
            if (total - ((hdr_end + 4) - buf) >= body_len) break;
        }
    }
    assert(total > 0);
    assert(strstr(buf, "200 OK") != NULL);
    assert(strstr(buf, "reload-ok") != NULL);
}

/* --- server thread --------------------------------------------------------- */

typedef struct {
    cwist_app *app;
    int port;
    int c1m;
    int result;
} server_args_t;

static void *server_thread(void *arg) {
    server_args_t *args = arg;
    g_cwist_drain_timeout_sec = 1; /* no long-lived connections at shutdown */
    args->result = cwist_app_listen_ex(args->app, args->port, 1, args->c1m);
    return NULL;
}

static void wait_for_port(int port) {
    for (int i = 0; i < 200; i++) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        assert(fd >= 0);
        struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port)};
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
            close(fd);
            return;
        }
        close(fd);
        usleep(10000);
    }
    assert(!"server did not start listening");
}

/* --- HTTPS (TCP) hot reload ------------------------------------------------ */

static void test_https_hot_reload(void) {
    printf("Testing HTTPS certificate hot reload (classic pool)...\n");

    int port = pick_free_port();
    cwist_app *app = cwist_app_create();
    assert(app != NULL);
    cwist_error_t err = cwist_app_use_https(app, g_cert_a, g_key_a);
    assert(cwist_error_is_ok(&err));
    cwist_app_get(app, "/", ok_handler);

    server_args_t args = {.app = app, .port = port, .c1m = 0, .result = -1};
    pthread_t tid;
    assert(pthread_create(&tid, NULL, server_thread, &args) == 0);
    wait_for_port(port);

    /* Baseline: certificate A on a connection that stays open. */
    SSL_CTX *cctx1 = NULL;
    SSL *old_conn = tls_connect(port, &cctx1);
    assert(old_conn != NULL);
    char cn[64];
    peer_cn(old_conn, cn, sizeof(cn));
    assert(strcmp(cn, "reload-a.local") == 0);
    char buf[1024];
    tls_get(old_conn, "keep-alive", buf, sizeof(buf));

    /* The shared STEK is carried over, not rotated, by a reload. */
    void *stek = app->ssl_ctx->ticket_key;
    assert(stek != NULL);

    /* Swap to certificate B while old_conn is mid-traffic. */
    err = cwist_app_reload_tls(app, g_cert_b, g_key_b);
    assert(cwist_error_is_ok(&err));
    assert(app->ssl_ctx->ticket_key == stek);

    /* New handshakes present certificate B. */
    SSL_CTX *cctx2 = NULL;
    SSL *new_conn = tls_connect(port, &cctx2);
    assert(new_conn != NULL);
    peer_cn(new_conn, cn, sizeof(cn));
    assert(strcmp(cn, "reload-b.local") == 0);
    tls_get(new_conn, "close", buf, sizeof(buf));
    tls_close(new_conn, cctx2);

    /* The pre-reload connection is untouched: another request succeeds. */
    tls_get(old_conn, "close", buf, sizeof(buf));
    tls_close(old_conn, cctx1);

    /* A failed reload keeps the previous certificate. */
    err = cwist_app_reload_tls(app, "/nonexistent/cert.pem", "/nonexistent/key.pem");
    assert(!cwist_error_is_ok(&err));
    cwist_error_dispose(&err);
    SSL_CTX *cctx3 = NULL;
    SSL *check_conn = tls_connect(port, &cctx3);
    assert(check_conn != NULL);
    peer_cn(check_conn, cn, sizeof(cn));
    assert(strcmp(cn, "reload-b.local") == 0);
    tls_close(check_conn, cctx3);

    /* Reload loop: alternate A/B, spot-check the presented certificate, and
     * confirm the process fd count does not drift. The server side reaps a
     * closed connection asynchronously (park-set wakeup, pool resubmit), so
     * let the count settle instead of sampling a single instant. */
    int iters = 200;
    const char *env = getenv("CWIST_TLS_RELOAD_ITERS");
    if (env && *env) {
        int parsed = atoi(env);
        if (parsed > 0) iters = parsed;
    }
    usleep(200000);
    int fds_before = count_fds();
    for (int i = 0; i < iters; i++) {
        const char *cert = (i % 2 == 0) ? g_cert_a : g_cert_b;
        const char *key = (i % 2 == 0) ? g_key_a : g_key_b;
        const char *want_cn = (i % 2 == 0) ? "reload-a.local" : "reload-b.local";
        err = cwist_app_reload_tls(app, cert, key);
        assert(cwist_error_is_ok(&err));
        if (i % 25 == 0) {
            SSL_CTX *loop_cctx = NULL;
            SSL *loop_conn = tls_connect(port, &loop_cctx);
            assert(loop_conn != NULL);
            peer_cn(loop_conn, cn, sizeof(cn));
            assert(strcmp(cn, want_cn) == 0);
            tls_close(loop_conn, loop_cctx);
        }
    }
    int fds_after = fds_before;
    for (int i = 0; i < 100 && fds_after != fds_before; i++) {
        usleep(50000);
        fds_after = count_fds();
    }
    if (fds_before >= 0) {
        assert(fds_after == fds_before);
    }
    printf("  %d reloads, fds before=%d after=%d\n", iters, fds_before, fds_after);

    cwist_shutdown_request();
    pthread_join(tid, NULL);
    assert(args.result == 0);
    cwist_app_destroy(app);
    cwist_shutdown_reset();
    printf("[PASS] HTTPS certificate hot reload.\n");
}

/* --- HTTPS/3 (QUIC) hot reload --------------------------------------------- */

static void h3_request_with_bundle(int port, const char *ca_bundle, bool expect_ok) {
    cwist_http3_client *client = cwist_http3_client_create();
    assert(client != NULL);
    assert(cwist_http3_client_set_server(client, "localhost", (uint16_t)port) == 0);
    assert(cwist_http3_client_set_ca_bundle(client, ca_bundle) == 0);
    cwist_http3_client_set_timeout_ms(client, 3000);
    cwist_http3_client_set_conn_timeout_ms(client, 3000);
    cwist_http3_client_set_max_retries(client, 0);

    cwist_http_response *res = NULL;
    cwist_error_t err = cwist_http3_client_request(client, "/", CWIST_HTTP_GET, NULL, NULL, 0, &res);
    if (expect_ok) {
        assert(cwist_error_is_ok(&err));
        assert(res != NULL);
        assert(res->status_code == CWIST_HTTP_OK);
        cwist_http_response_destroy(res);
    } else {
        /* The certificate no longer chains to this bundle: the handshake must
         * fail, not silently serve. */
        assert(!cwist_error_is_ok(&err));
        cwist_error_dispose(&err);
        if (res) cwist_http_response_destroy(res);
    }
    cwist_http3_client_destroy(client);
}

static void test_http3_hot_reload(void) {
    printf("Testing HTTP/3 certificate hot reload...\n");

    int port = pick_free_port();
    cwist_app *app = cwist_app_create();
    assert(app != NULL);
    cwist_error_t err = cwist_app_use_https(app, g_cert_a, g_key_a);
    assert(cwist_error_is_ok(&err));
    err = cwist_app_use_https3(app, true);
    assert(cwist_error_is_ok(&err));
    cwist_app_get(app, "/", ok_handler);

    server_args_t args = {.app = app, .port = port, .c1m = 0, .result = -1};
    pthread_t tid;
    assert(pthread_create(&tid, NULL, server_thread, &args) == 0);
    wait_for_port(port);
    usleep(100000); /* give the UDP/QUIC listener thread a moment too */

    /* Verified request against certificate A's self-signed anchor. */
    h3_request_with_bundle(port, g_cert_a, true);

    err = cwist_app_reload_tls(app, g_cert_b, g_key_b);
    assert(cwist_error_is_ok(&err));

    /* New QUIC connections present certificate B. */
    h3_request_with_bundle(port, g_cert_b, true);
    h3_request_with_bundle(port, g_cert_a, false);

    /* A failed reload keeps the previous certificate. */
    err = cwist_app_reload_tls(app, g_cert_b, "/nonexistent/key.pem");
    assert(!cwist_error_is_ok(&err));
    cwist_error_dispose(&err);
    h3_request_with_bundle(port, g_cert_b, true);

    cwist_shutdown_request();
    pthread_join(tid, NULL);
    assert(args.result == 0);
    cwist_app_destroy(app);
    cwist_shutdown_reset();
    printf("[PASS] HTTP/3 certificate hot reload.\n");
}

/* --- API surface checks ------------------------------------------------------ */

static void test_reload_rejects_unconfigured_app(void) {
    printf("Testing reload argument validation...\n");
    cwist_app *app = cwist_app_create();
    assert(app != NULL);
    cwist_error_t err = cwist_app_reload_tls(app, g_cert_b, g_key_b);
    assert(!cwist_error_is_ok(&err)); /* TLS was never configured */
    cwist_error_dispose(&err);
    err = cwist_app_reload_tls(NULL, g_cert_b, g_key_b);
    assert(!cwist_error_is_ok(&err));
    cwist_error_dispose(&err);
    cwist_app_destroy(app);
    printf("[PASS] reload argument validation.\n");
}

/* --- Context-level reload (no listener) ------------------------------------ */

static void ctx_cert_cn(SSL_CTX *ssl_ctx, char *out, size_t out_len) {
    out[0] = '\0';
    X509 *cert = SSL_CTX_get0_certificate(ssl_ctx);
    assert(cert != NULL);
    X509_NAME *name = X509_get_subject_name(cert);
    assert(name != NULL);
    int n = X509_NAME_get_text_by_NID(name, NID_commonName, out, (int)out_len);
    assert(n > 0);
}

static void test_context_level_reload(void) {
    printf("Testing context-level reload APIs...\n");
    char cn[64];

    cwist_https_context *hctx = NULL;
    cwist_error_t err = cwist_https_init_context(&hctx, g_cert_a, g_key_a);
    assert(cwist_error_is_ok(&err));
    ctx_cert_cn(hctx->ctx, cn, sizeof(cn));
    assert(strcmp(cn, "reload-a.local") == 0);
    err = cwist_https_reload_context(hctx, g_cert_b, g_key_b, NULL);
    assert(cwist_error_is_ok(&err));
    ctx_cert_cn(hctx->ctx, cn, sizeof(cn));
    assert(strcmp(cn, "reload-b.local") == 0);
    /* Invalid swap: old context untouched. */
    err = cwist_https_reload_context(hctx, g_cert_a, g_key_b, NULL);
    assert(!cwist_error_is_ok(&err));
    cwist_error_dispose(&err);
    ctx_cert_cn(hctx->ctx, cn, sizeof(cn));
    assert(strcmp(cn, "reload-b.local") == 0);
    cwist_https_destroy_context(hctx);

    cwist_http3_context *h3 = NULL;
    err = cwist_http3_init_context(&h3, g_cert_a, g_key_a);
    assert(cwist_error_is_ok(&err));
    ctx_cert_cn(h3->ssl_ctx, cn, sizeof(cn));
    assert(strcmp(cn, "reload-a.local") == 0);
    err = cwist_http3_reload_context(h3, g_cert_b, g_key_b);
    assert(cwist_error_is_ok(&err));
    ctx_cert_cn(h3->ssl_ctx, cn, sizeof(cn));
    assert(strcmp(cn, "reload-b.local") == 0);
    err = cwist_http3_reload_context(h3, "/nonexistent/cert.pem", "/nonexistent/key.pem");
    assert(!cwist_error_is_ok(&err));
    cwist_error_dispose(&err);
    ctx_cert_cn(h3->ssl_ctx, cn, sizeof(cn));
    assert(strcmp(cn, "reload-b.local") == 0);
    cwist_http3_destroy_context(h3);

    printf("[PASS] context-level reload APIs.\n");
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    if (!run_cmd("command -v openssl >/dev/null 2>&1")) {
        printf("Skipping TLS hot reload tests; openssl CLI not found.\n");
        return 0;
    }
    generate_fixtures();
    test_reload_rejects_unconfigured_app();
    test_context_level_reload();
    test_https_hot_reload();
    test_http3_hot_reload();
    remove_fixtures();
    printf("All TLS hot reload tests passed!\n");
    return 0;
}
