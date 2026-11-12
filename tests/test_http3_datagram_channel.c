/**
 * @file test_http3_datagram_channel.c
 * @brief Raw datagrams and WebTransport on one HTTP/3 client connection (#604).
 *
 * Scenario 1, server with WebTransport (HTTP Datagrams negotiated): one
 * client connection carries an ordinary GET, a WebTransport session with
 * session datagrams, and raw datagrams sent through the datagram channel,
 * interleaved. Checks that each kind reaches only its own receiver on the
 * server, that the server's raw echo reaches cwist_http3_client_recv_datagram()
 * intact, and that GETs before and after still work.
 *
 * Scenario 2, server without WebTransport (no HTTP Datagrams): the same
 * client mode falls back to plain RFC 9221 DATAGRAM frames.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <cwist/net/http/http3.h>
#include <cwist/net/http/http3_client.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define PORT_WT 19961
#define PORT_RAW 19962
#define ROUNDS 5

static int failures;
#define CHECK(cond, ...)                                                                           \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "FAIL: ");                                                             \
            fprintf(stderr, __VA_ARGS__);                                                          \
            fputc('\n', stderr);                                                                   \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

/* --- server side ---------------------------------------------------------- */

static _Atomic int g_raw_seen[ROUNDS];  /* raw-N on the conn callback */
static _Atomic int g_wt_seen[ROUNDS];   /* wt-N on the session callback */
static _Atomic int g_misrouted;         /* a payload on the wrong callback */

static int payload_index(const void *data, size_t len, const char *prefix) {
    size_t plen = strlen(prefix);
    if (len != plen + 1 || memcmp(data, prefix, plen) != 0) return -1;
    int i = ((const char *)data)[plen] - '0';
    return (i >= 0 && i < ROUNDS) ? i : -1;
}

/* WebTransport session datagrams (and nothing else, once a conn callback is set). */
static void on_wt_datagram(const void *data, size_t len, void *user) {
    (void)user;
    int i = payload_index(data, len, "wt-");
    if (i >= 0)
        atomic_store(&g_wt_seen[i], 1);
    else
        atomic_fetch_add(&g_misrouted, 1);
}

/* Raw datagrams, from DATAGRAM frames or the datagram channel: echo them. */
static void on_raw_datagram(void *conn, const void *data, size_t len, void *user) {
    (void)user;
    int i = payload_index(data, len, "raw-");
    if (i < 0) {
        atomic_fetch_add(&g_misrouted, 1);
        return;
    }
    atomic_store(&g_raw_seen[i], 1);
    char echo[16];
    int n = snprintf(echo, sizeof(echo), "echo-%d", i);
    cwist_http3_send_datagram(conn, echo, (size_t)n);
}

static void wt_handler(cwist_http_request *req, cwist_http_response *res, void *session) {
    (void)req;
    (void)session;
    res->status_code = CWIST_HTTP_OK;
}

static void get_handler(void *user, cwist_http_request *req, cwist_http_response *res) {
    (void)user;
    (void)req;
    res->status_code = CWIST_HTTP_OK;
    cwist_sstring_assign(res->body, "pong");
}

typedef struct {
    int fd;
    cwist_http3_context *ctx;
} server_t;

static void *server_thread(void *arg) {
    server_t *s = arg;
    cwist_http3_server_loop(s->fd, s->ctx, get_handler, NULL);
    return NULL;
}

static int start_server(server_t *s, pthread_t *tid, int port, bool with_wt) {
    if (cwist_http3_init_context_ephemeral(&s->ctx).error.err_i16 != 0) return -1;
    cwist_http3_set_datagram_enabled(s->ctx, 1);
    cwist_http3_set_datagram_callback(s->ctx, on_wt_datagram, NULL);
    cwist_http3_set_datagram_conn_callback(s->ctx, on_raw_datagram, NULL);
    if (with_wt) cwist_http3_set_webtransport_handler(s->ctx, wt_handler);
    s->fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port)};
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (s->fd < 0 || bind(s->fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) return -1;
    return pthread_create(tid, NULL, server_thread, s);
}

static void stop_server(server_t *s, pthread_t tid) {
    __atomic_store_n(&s->ctx->running, 0, __ATOMIC_RELEASE);
    pthread_join(tid, NULL);
    close(s->fd);
    cwist_http3_destroy_context(s->ctx);
}

/* --- client side ---------------------------------------------------------- */

static bool get_pong(cwist_http3_client *c) {
    cwist_http_response *res = NULL;
    cwist_error_t e = cwist_http3_client_request(c, "/ping", CWIST_HTTP_GET, NULL, NULL, 0, &res);
    bool ok = e.error.err_i16 == 0 && res && res->status_code == CWIST_HTTP_OK && res->body &&
              res->body->data && strcmp(res->body->data, "pong") == 0;
    if (res) cwist_http_response_destroy(res);
    return ok;
}

static void reset_seen(void) {
    for (int i = 0; i < ROUNDS; i++) {
        atomic_store(&g_raw_seen[i], 0);
        atomic_store(&g_wt_seen[i], 0);
    }
    atomic_store(&g_misrouted, 0);
}

/* Datagrams are unreliable: resend the missing ones until all arrive. */
static void exchange(cwist_http3_client *c, cwist_webtransport_client_session *session,
                     int echoes[ROUNDS]) {
    for (int attempt = 0; attempt < 30; attempt++) {
        bool done = true;
        for (int i = 0; i < ROUNDS; i++) {
            char buf[8];
            if (session && !atomic_load(&g_wt_seen[i])) {
                snprintf(buf, sizeof(buf), "wt-%d", i);
                cwist_webtransport_client_send_datagram(session, buf, strlen(buf));
                done = false;
            }
            if (!echoes[i]) {
                /* The raw DATAGRAM path holds one datagram at a time and
                 * refuses the next until it is written; the loop resends. */
                snprintf(buf, sizeof(buf), "raw-%d", i);
                (void)cwist_http3_client_send_datagram(c, buf, strlen(buf));
                done = false;
            }
        }
        if (done) return;
        for (int t = 0; t < 5; t++) {
            cwist_webtransport_client_poll(c, 20);
            char in[64];
            ssize_t n;
            while ((n = cwist_http3_client_recv_datagram(c, in, sizeof(in))) > 0) {
                int i = payload_index(in, (size_t)n, "echo-");
                if (i >= 0)
                    echoes[i] = 1;
                else
                    atomic_fetch_add(&g_misrouted, 1);
            }
        }
    }
}

static void scenario(const char *name, int port, bool with_wt) {
    printf("%s\n", name);
    reset_seen();
    server_t srv;
    pthread_t tid;
    if (start_server(&srv, &tid, port, with_wt) != 0) {
        CHECK(0, "server start");
        return;
    }
    usleep(100000);

    cwist_http3_client *c = cwist_http3_client_create();
    cwist_http3_client_set_insecure(c, 1);
    cwist_http3_client_set_server(c, "127.0.0.1", (uint16_t)port);
    cwist_http3_client_set_timeout_ms(c, 3000);
    cwist_http3_client_set_conn_timeout_ms(c, 3000);
    CHECK(cwist_http3_client_enable_datagram_channel(c, 1) == 0, "enable channel mode");

    CHECK(get_pong(c), "GET before datagrams");

    cwist_webtransport_client_session *session = NULL;
    if (with_wt) {
        cwist_error_t e = cwist_http3_client_webtransport_connect(c, "/wt", NULL, &session);
        CHECK(e.error.err_i16 == 0 && session, "WebTransport session opens");
    }

    int echoes[ROUNDS] = {0};
    exchange(c, session, echoes);
    for (int i = 0; i < ROUNDS; i++) {
        CHECK(atomic_load(&g_raw_seen[i]), "raw-%d reached the raw callback", i);
        CHECK(echoes[i], "echo-%d reached recv_datagram", i);
        if (with_wt) CHECK(atomic_load(&g_wt_seen[i]), "wt-%d reached the session callback", i);
    }
    CHECK(atomic_load(&g_misrouted) == 0, "%d datagram(s) reached the wrong receiver",
          atomic_load(&g_misrouted));

    CHECK(get_pong(c), "GET after datagrams");

    if (session) cwist_webtransport_client_close(session, 0, NULL);
    cwist_http3_client_destroy(c);
    stop_server(&srv, tid);
}

int main(void) {
    scenario("raw + WebTransport over HTTP Datagrams (datagram channel)", PORT_WT, true);
    scenario("raw fallback when the server does not enable HTTP Datagrams", PORT_RAW, false);
    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("All HTTP/3 datagram channel tests passed!\n");
    return 0;
}
