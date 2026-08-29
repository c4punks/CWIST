#include <cwist/net/http/http3.h>
#include <cwist/net/http/http3_client.h>
#include <cwist/sys/err/cwist_err.h>
#include <lsquic.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <pthread.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>

#define TEST_CERT "example/othello-web/server.crt"
#define TEST_KEY "example/othello-web/server.key"

int cwist_http3_normalize_response_header_name(const char *name, char *out, size_t out_len);
int cwist_http3_response_header_value_is_safe(const char *value);
int cwist_http3_method_is_idempotent(const char *method_str);

int cwist_http3_normalize_response_header_name(const char *name,
                                               char *out,
                                               size_t out_len);
int cwist_http3_response_header_value_is_safe(const char *value);
int cwist_http3_method_is_idempotent(const char *method_str);

int cwist_http3_normalize_response_header_name(const char *name,
                                               char *out,
                                               size_t out_len);
int cwist_http3_response_header_value_is_safe(const char *value);
int cwist_http3_method_is_idempotent(const char *method_str);

static volatile int g_handler_called = 0;

static void http3_test_handler(void *user_ctx, cwist_http_request *req, cwist_http_response *res) {
    (void)user_ctx;
    (void)req;
    (void)res;
    g_handler_called = 1;
}

typedef struct {
    int udp_fd;
    cwist_http3_context *ctx;
} server_thread_args_t;

/* --- Minimal raw-lsquic client -----------------------------------------
 * Used by Test 12 to exercise a peer-initiated CONNECTION_CLOSE.  The
 * high-level cwist client has no connection-close API (its engine destroy
 * drops connections silently), so drive lsquic directly: complete the
 * handshake, then lsquic_conn_abort().  The pinned lsquic turns the abort
 * into a transport-level CONNECTION_CLOSE (NO_ERROR, "user aborted
 * connection"), which the server records on its context.
 * ---------------------------------------------------------------------- */
typedef struct {
    int udp_fd;
    lsquic_engine_t *engine;
    SSL_CTX *ssl_ctx;
    lsquic_conn_t *conn;
    struct sockaddr_storage local_addr;
    socklen_t local_addr_len;
    int conn_closed;
} raw_h3_client_t;

static lsquic_conn_ctx_t *raw_on_new_conn(void *stream_if_ctx, lsquic_conn_t *conn) {
    (void)conn;
    return (lsquic_conn_ctx_t *)stream_if_ctx;
}

static void raw_on_conn_closed(lsquic_conn_t *conn) {
    raw_h3_client_t *cl = (raw_h3_client_t *)lsquic_conn_get_ctx(conn);
    if (cl) cl->conn_closed = 1;
}

static lsquic_stream_ctx_t *raw_on_new_stream(void *stream_if_ctx, lsquic_stream_t *stream) {
    (void)stream_if_ctx;
    lsquic_stream_wantread(stream, 1);
    return NULL;
}

static void raw_on_read(lsquic_stream_t *stream, lsquic_stream_ctx_t *st_h) {
    (void)st_h;
    lsquic_stream_wantread(stream, 0);
}

static void raw_on_write(lsquic_stream_t *stream, lsquic_stream_ctx_t *st_h) {
    (void)stream;
    (void)st_h;
}

static void raw_on_close(lsquic_stream_t *stream, lsquic_stream_ctx_t *st_h) {
    (void)stream;
    (void)st_h;
}

static const struct lsquic_stream_if raw_stream_if = {
    .on_new_conn = raw_on_new_conn,
    .on_conn_closed = raw_on_conn_closed,
    .on_new_stream = raw_on_new_stream,
    .on_read = raw_on_read,
    .on_write = raw_on_write,
    .on_close = raw_on_close,
};

static int raw_packets_out(void *ctx, const struct lsquic_out_spec *specs, unsigned n_specs) {
    raw_h3_client_t *cl = ctx;
    unsigned i;
    for (i = 0; i < n_specs; ++i) {
        const struct lsquic_out_spec *spec = &specs[i];
        struct msghdr msg = {0};
        msg.msg_name = (void *)spec->dest_sa;
        msg.msg_namelen = spec->dest_sa->sa_family == AF_INET ? sizeof(struct sockaddr_in)
                                                              : sizeof(struct sockaddr_in6);
        msg.msg_iov = (struct iovec *)spec->iov;
        msg.msg_iovlen = spec->iovlen;
        if (sendmsg(cl->udp_fd, &msg, MSG_DONTWAIT) < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            return -1;
        }
    }
    return (int)i;
}

static SSL_CTX *raw_get_ssl_ctx(void *peer_ctx, const struct sockaddr *local) {
    (void)local;
    raw_h3_client_t *cl = peer_ctx;
    return cl ? cl->ssl_ctx : NULL;
}

static void raw_process_io(raw_h3_client_t *cl, int timeout_ms) {
    int diff = timeout_ms * 1000;
    if (diff <= 0) diff = 1000;
    if (lsquic_engine_earliest_adv_tick(cl->engine, &diff)) {
        if (diff <= 0) diff = 0;
        else if (diff > 1000000) diff = 1000000;
    }

    struct pollfd pfd = {.fd = cl->udp_fd, .events = POLLIN};
    int pret = poll(&pfd, 1, diff / 1000);
    if (pret > 0 && (pfd.revents & POLLIN)) {
        unsigned char buf[65535];
        struct sockaddr_storage peer_addr;
        socklen_t peer_addr_len = sizeof(peer_addr);
        ssize_t nr = recvfrom(cl->udp_fd, buf, sizeof(buf), 0, (struct sockaddr *)&peer_addr,
                              &peer_addr_len);
        if (nr > 0) {
            lsquic_engine_packet_in(cl->engine, buf, (size_t)nr,
                                    (struct sockaddr *)&cl->local_addr,
                                    (struct sockaddr *)&peer_addr, NULL, 0);
        }
    }
    lsquic_engine_process_conns(cl->engine);
}

static void *http3_server_thread(void *arg) {
    int udp_fd = *(int *)arg;
    cwist_http3_context *ctx = NULL;
    cwist_error_t err = cwist_http3_init_context(&ctx, TEST_CERT, TEST_KEY);
    assert(err.error.err_i16 == 0);

    cwist_http3_server_loop(udp_fd, ctx, http3_test_handler, NULL);

    cwist_http3_destroy_context(ctx);
    return NULL;
}
#endif

int main(void) {
    printf("Testing HTTP/3 (BoringSSL + lsquic) infrastructure...\n");

    cwist_http3_context *ctx = NULL;
    cwist_error_t err = cwist_http3_init_context(&ctx, TEST_CERT, TEST_KEY);

#if CWIST_HAVE_OPENSSL_QUIC
    assert(err.error.err_i16 == 0);
    assert(ctx != NULL);
    assert(ctx->ssl_ctx != NULL);
    cwist_http3_destroy_context(ctx);
    printf("[PASS] HTTP/3 context initialization with PEM files.\n");

    int udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
    assert(udp_fd >= 0);

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(0);

    assert(bind(udp_fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);

    ctx = NULL;
    err = cwist_http3_init_context_ephemeral(&ctx);
    assert(err.error.err_i16 == 0);

    /* Invalid: NULL handler should fail */
    err = cwist_http3_server_loop(udp_fd, ctx, NULL, NULL);
    assert(err.error.err_i16 == -1);
    printf("[PASS] HTTP/3 server_loop rejects NULL handler.\n");

    /* --- Test 5: server_loop runs and stops gracefully --- */
    server_thread_args_t args = {.udp_fd = udp_fd, .ctx = ctx};
    pthread_t tid;
    int rc = pthread_create(&tid, NULL, http3_server_thread, &args);
    assert(rc == 0);

    usleep(100000);

    struct sockaddr_in server_addr;
    socklen_t server_addr_len = sizeof(server_addr);
    getsockname(udp_fd, (struct sockaddr *)&server_addr, &server_addr_len);

    /* The server loop must force the UDP socket into non-blocking mode */
    int fl = fcntl(udp_fd, F_GETFL, 0);
    assert(fl >= 0 && (fl & O_NONBLOCK));
    printf("[PASS] HTTP/3 server_loop sets O_NONBLOCK on the UDP socket.\n");

    /* The server loop must force the UDP socket into non-blocking mode */
    int fl = fcntl(udp_fd, F_GETFL, 0);
    assert(fl >= 0 && (fl & O_NONBLOCK));
    printf("[PASS] HTTP/3 server_loop sets O_NONBLOCK on the UDP socket.\n");

    /* Stop the server */
    ctx->running = 0;

    usleep(100000);
    close(udp_fd);

    printf("HTTP/3 server loop skeleton test finished.\n");
#else
    assert(err.error.err_i16 == -1);
    assert(ctx == NULL);
    printf("HTTP/3 QUIC support unavailable in this OpenSSL build; TCP HTTP/1.1 fallback remains available.\n");
#endif

    /* --- Test 7: response header normalization for browser strictness --- */
    char h3_name[64];
    assert(cwist_http3_normalize_response_header_name("Set-Cookie",
                                                       h3_name,
                                                       sizeof(h3_name)) == 0);
    assert(strcmp(h3_name, "set-cookie") == 0);
    assert(cwist_http3_normalize_response_header_name("Location",
                                                       h3_name,
                                                       sizeof(h3_name)) == 0);
    assert(strcmp(h3_name, "location") == 0);
    assert(cwist_http3_normalize_response_header_name(":bad",
                                                       h3_name,
                                                       sizeof(h3_name)) == -1);
    assert(cwist_http3_normalize_response_header_name("Bad Header",
                                                       h3_name,
                                                       sizeof(h3_name)) == -1);
    assert(cwist_http3_response_header_value_is_safe("sid=gone; Path=/; Max-Age=0"));
    assert(!cwist_http3_response_header_value_is_safe("ok\r\nbad: value"));
    printf("[PASS] HTTP/3 response headers are lowercased and CRLF-safe.\n");

    /* --- Test 8: 0-RTT early data setter (opt-in, default OFF) --- */
    ctx = NULL;
    err = cwist_http3_init_context_ephemeral(&ctx);
    assert(err.error.err_i16 == 0);
    assert(ctx->early_data_enabled == 0);
    assert(ctx->early_data_guard == 0);

    cwist_http3_set_early_data(ctx, true);
    assert(ctx->early_data_enabled == 1);
    /* Guard defaults ON when early data is enabled */
    assert(ctx->early_data_guard == 1);

    cwist_http3_set_early_data_guard(ctx, 0);
    assert(ctx->early_data_guard == 0);
    cwist_http3_set_early_data_guard(ctx, 1);
    assert(ctx->early_data_guard == 1);

    cwist_http3_set_early_data(ctx, false);
    assert(ctx->early_data_enabled == 0);
    cwist_http3_destroy_context(ctx);
    printf("[PASS] HTTP/3 0-RTT early data setter and replay guard defaults.\n");

    /* Same behavior on the certificate-based init path */
    ctx = NULL;
    err = cwist_http3_init_context(&ctx, TEST_CERT, TEST_KEY);
    assert(err.error.err_i16 == 0);
    assert(ctx->early_data_enabled == 0);
    cwist_http3_set_early_data(ctx, true);
    assert(ctx->early_data_enabled == 1 && ctx->early_data_guard == 1);
    cwist_http3_destroy_context(ctx);
    printf("[PASS] HTTP/3 0-RTT setter works on certificate-based context.\n");

    /* --- Test 9: replay guard method classification --- */
    assert(cwist_http3_method_is_idempotent("GET") == 1);
    assert(cwist_http3_method_is_idempotent("HEAD") == 1);
    assert(cwist_http3_method_is_idempotent("OPTIONS") == 1);
    assert(cwist_http3_method_is_idempotent("PUT") == 1);
    assert(cwist_http3_method_is_idempotent("DELETE") == 1);
    assert(cwist_http3_method_is_idempotent("TRACE") == 1);
    assert(cwist_http3_method_is_idempotent("POST") == 0);
    assert(cwist_http3_method_is_idempotent("PATCH") == 0);
    assert(cwist_http3_method_is_idempotent("CONNECT") == 0);
    assert(cwist_http3_method_is_idempotent(NULL) == 0);
    assert(cwist_http3_method_is_idempotent("garbage") == 0);
    printf("[PASS] HTTP/3 replay guard method classification.\n");

    /* --- Test 10: RFC 9114 Section 4.3.1 pseudo-header validation over the
     * wire: a well-formed request is dispatched, an unknown pseudo-header is
     * rejected with 400 and never reaches the handler. --- */
    ctx = NULL;
    err = cwist_http3_init_context_ephemeral(&ctx);
    assert(err.error.err_i16 == 0);

    udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
    assert(udp_fd >= 0);
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(0);
    assert(bind(udp_fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
    socklen_t addr_len = sizeof(addr);
    assert(getsockname(udp_fd, (struct sockaddr *)&addr, &addr_len) == 0);

    server_thread_args_t args10 = { .udp_fd = udp_fd, .ctx = ctx };
    rc = pthread_create(&tid, NULL, http3_server_thread, &args10);
    assert(rc == 0);
    usleep(50000);

    cwist_http3_client *client = cwist_http3_client_create();
    assert(client != NULL);
    assert(cwist_http3_client_set_server(client, "127.0.0.1",
                                         ntohs(addr.sin_port)) == 0);
    /* The ephemeral server cert is self-signed: disable verification. */
    assert(cwist_http3_client_set_ca_bundle(client, NULL) == 0);
    cwist_http3_client_set_timeout_ms(client, 10000);

    /* Well-formed request: handler runs, 200 comes back. */
    g_handler_called = 0;
    cwist_http_response *res = NULL;
    err = cwist_http3_client_request(client, "/", CWIST_HTTP_GET,
                                     NULL, NULL, 0, &res);
    assert(err.error.err_i16 == 0);
    assert(res != NULL);
    assert(g_handler_called == 1);
    assert(res->status_code == 200);
    cwist_http_response_destroy(res);
    printf("[PASS] HTTP/3 well-formed request is dispatched (200).\n");

    /* Unknown pseudo-header: malformed, 400, handler must not run. */
    g_handler_called = 0;
    cwist_http_header_node *hdrs = NULL;
    cwist_http_header_add(&hdrs, ":bogus", "1");
    res = NULL;
    err = cwist_http3_client_request(client, "/", CWIST_HTTP_GET,
                                     hdrs, NULL, 0, &res);
    cwist_http_header_free_all(hdrs);
    assert(err.error.err_i16 == 0);
    assert(res != NULL);
    assert(res->status_code == CWIST_HTTP_BAD_REQUEST);
    assert(g_handler_called == 0);
    cwist_http_response_destroy(res);
    printf("[PASS] HTTP/3 unknown pseudo-header rejected with 400.\n");

    /* Plain CONNECT carrying :scheme/:path is malformed (RFC 9114 4.3.1):
     * the client always sends :scheme and :path, so this must be rejected. */
    g_handler_called = 0;
    res = NULL;
    err = cwist_http3_client_request(client, "/", CWIST_HTTP_CONNECT,
                                     NULL, NULL, 0, &res);
    assert(err.error.err_i16 == 0);
    assert(res != NULL);
    assert(res->status_code == CWIST_HTTP_BAD_REQUEST);
    assert(g_handler_called == 0);
    cwist_http_response_destroy(res);
    printf("[PASS] HTTP/3 CONNECT with :scheme/:path rejected with 400.\n");

    cwist_http3_client_destroy(client);
    ctx->running = 0;
    rc = pthread_join(tid, NULL);
    assert(rc == 0);
    cwist_http3_destroy_context(ctx);
    close(udp_fd);

    printf("All HTTP/3 infrastructure tests passed!\n");
    return 0;
}
