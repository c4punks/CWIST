#include <cwist/sys/metrics/metrics.h>
#include <cwist/net/http/https.h>
#include <assert.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <signal.h>

static const char *TEST_CERT = "example/othello-web/server.crt";
static const char *TEST_KEY = "example/othello-web/server.key";

typedef struct metrics_server_ctx {
    int fd;
    cwist_https_context *ctx;
    cwist_error_t result;
} metrics_server_ctx;

static void *metrics_server_thread(void *arg) {
    metrics_server_ctx *server = arg;
    cwist_https_connection *conn = NULL;
    server->result = cwist_https_accept(server->ctx, server->fd, &conn);
    if (server->result.errtype == CWIST_ERR_INT16 && server->result.error.err_i16 == 0) {
        cwist_https_close_connection(conn);
    } else {
        close(server->fd);
    }
    return NULL;
}

/* Perform a client handshake on @p fd. When @p resume is non-NULL it is
 * offered to the server and replaced with the session negotiated here, so
 * the caller can chain a resumed follow-up handshake. */
static void run_tls_client(int fd, SSL_SESSION **resume, bool force_tls12) {
    SSL_CTX *client_ctx = SSL_CTX_new(TLS_client_method());
    assert(client_ctx != NULL);
    SSL_CTX_set_verify(client_ctx, SSL_VERIFY_NONE, NULL);
    if (force_tls12)
        assert(SSL_CTX_set_max_proto_version(client_ctx, TLS1_2_VERSION) == 1);
    SSL *client = SSL_new(client_ctx);
    assert(client != NULL);
    assert(SSL_set_fd(client, fd) == 1);
    if (*resume) {
        assert(SSL_set_session(client, *resume) == 1);
        SSL_SESSION_free(*resume);
        *resume = NULL;
    }
    assert(SSL_connect(client) == 1);
    SSL_SESSION *s = SSL_get1_session(client);
    assert(s != NULL);
    *resume = s;
    SSL_shutdown(client);
    SSL_free(client);
    SSL_CTX_free(client_ctx);
    close(fd);
}

static void one_handshake(cwist_https_context *ctx, SSL_SESSION **resume, bool force_tls12) {
    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    metrics_server_ctx server = {.fd = sv[0], .ctx = ctx, .result = {0}};
    pthread_t tid;
    assert(pthread_create(&tid, NULL, metrics_server_thread, &server) == 0);
    run_tls_client(sv[1], resume, force_tls12);
    pthread_join(tid, NULL);
    assert(server.result.errtype == CWIST_ERR_INT16);
    assert(server.result.error.err_i16 == 0);
}

static void test_tls_handshake_counters_full_and_resumed(void) {
    printf("Testing TLS handshake metrics (full + resumed)...\n");
    cwist_https_context *ctx = NULL;
    cwist_error_t err = cwist_https_init_context(&ctx, TEST_CERT, TEST_KEY);
    assert(err.errtype == CWIST_ERR_INT16 && err.error.err_i16 == 0);

    long hs0 = cwist_https_tls_handshakes_total();
    long resumed0 = cwist_https_tls_handshakes_resumed_total();
    long tls12_0 = cwist_https_tls_handshakes_tls12_total();
    long active0 = cwist_https_tls_connections_active();

    /* TLS 1.2 pinned on the client so session resumption completes
     * in-handshake (TLS 1.3 tickets arrive post-handshake). */
    SSL_SESSION *sess = NULL;
    one_handshake(ctx, &sess, true); /* full handshake, keeps the session */
    assert(sess != NULL);
    one_handshake(ctx, &sess, true); /* resumed handshake */
    SSL_SESSION_free(sess);
    sess = NULL;

    assert(cwist_https_tls_handshakes_total() == hs0 + 2);
    assert(cwist_https_tls_handshakes_resumed_total() == resumed0 + 1);
    assert(cwist_https_tls_handshakes_tls12_total() == tls12_0 + 2);
    assert(cwist_https_tls_connections_active() == active0);

    cwist_https_destroy_context(ctx);
    printf("Passed TLS handshake metrics (full + resumed).\n");
}

static void test_tls_handshake_counters_tls13_and_prometheus(void) {
    printf("Testing TLS 1.3 counters and /metrics exposition...\n");
    cwist_https_context *ctx = NULL;
    cwist_error_t err = cwist_https_init_context(&ctx, TEST_CERT, TEST_KEY);
    assert(err.errtype == CWIST_ERR_INT16 && err.error.err_i16 == 0);

    long hs0 = cwist_https_tls_handshakes_total();
    long tls13_0 = cwist_https_tls_handshakes_tls13_total();
    long ciphers0 = cwist_https_tls_ciphers_aes128_gcm_total() +
                    cwist_https_tls_ciphers_aes256_gcm_total() +
                    cwist_https_tls_ciphers_chacha20_total() +
                    cwist_https_tls_ciphers_other_total();

    SSL_SESSION *sess = NULL;
    one_handshake(ctx, &sess, false);
    SSL_SESSION_free(sess);

    assert(cwist_https_tls_handshakes_total() == hs0 + 1);
    assert(cwist_https_tls_handshakes_tls13_total() == tls13_0 + 1);
    long ciphers1 = cwist_https_tls_ciphers_aes128_gcm_total() +
                    cwist_https_tls_ciphers_aes256_gcm_total() +
                    cwist_https_tls_ciphers_chacha20_total() +
                    cwist_https_tls_ciphers_other_total();
    assert(ciphers1 == ciphers0 + 1);

    /* The registry must mirror the HTTPS-layer counters into the rendered
     * Prometheus exposition. */
    cwist_metrics_registry_t *reg = cwist_metrics_registry();
    assert(reg != NULL);
    uintmax_t total_raw = cwist_metric_load(reg, CWIST_METRIC_TLS_HANDSHAKES_TOTAL);
    assert(total_raw == (uintmax_t)cwist_https_tls_handshakes_total() * 1000);
    uintmax_t active_raw = cwist_metric_load(reg, CWIST_METRIC_TLS_CONNECTIONS_ACTIVE);
    assert(active_raw == (uintmax_t)cwist_https_tls_connections_active() * 1000);

    char *text = cwist_metrics_render_prometheus(reg);
    assert(text != NULL);
    assert(strstr(text, "cwist_tls_handshakes_total") != NULL);
    assert(strstr(text, "cwist_tls_handshakes_resumed_total") != NULL);
    assert(strstr(text, "cwist_tls_connections_active") != NULL);
    assert(strstr(text, "cwist_tls_handshakes_tls12_total") != NULL);
    assert(strstr(text, "cwist_tls_handshakes_tls13_total") != NULL);
    free(text);

    cwist_https_destroy_context(ctx);
    printf("Passed TLS 1.3 counters and /metrics exposition.\n");
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    test_tls_handshake_counters_full_and_resumed();
    test_tls_handshake_counters_tls13_and_prometheus();
    printf("All TLS metrics tests passed.\n");
    return 0;
}
