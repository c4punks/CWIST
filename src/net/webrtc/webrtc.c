/** @file webrtc.c
 * @brief WebRTC DataChannel server: ctx, connection state machine, UDP loop.
 *
 * One background thread per ctx drives ICE-lite, the DTLS handshake
 * (BoringSSL over memory BIOs), and SCTP-over-DTLS via usrsctp. Connections
 * are keyed by the nominated remote UDP address.
 */
#include "webrtc_internal.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <openssl/rand.h>
#include <poll.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <usrsctp.h>

#define STUN_RETRANS_MS 300
#define STUN_MAX_ATTEMPTS 7
#define SCTP_TIMER_MS 10

typedef struct pending_send {
    struct pending_send *next;
    uint16_t channel;
    int is_string;
    size_t len;
    uint8_t data[];
} pending_send;

/* ---- cross-TU helper implementations used by sctp.c / ice.c ---- */

void cwist_webrtc_conn_on_message(cwist_webrtc_conn *conn, uint16_t channel,
                                  const uint8_t *data, size_t len, int is_string) {
    cwist_webrtc_ctx *ctx = conn->ctx;
    if (ctx->msg_cb)
        ctx->msg_cb(conn, channel, data, len, ctx->msg_user);
    (void)is_string;
}

void cwist_webrtc_conn_on_channel_open(cwist_webrtc_conn *conn, uint16_t channel,
                                       const char *label) {
    cwist_webrtc_ctx *ctx = conn->ctx;
    if (ctx->ch_cb)
        ctx->ch_cb(conn, channel, label, ctx->ch_user);
}

void cwist_webrtc_conn_sctp_out(struct cwist_webrtc_conn *conn, const void *buffer, size_t len) {
    if (!conn->ssl)
        return;
    SSL_write(conn->ssl, buffer, (int)len);
}

int cwist_webrtc_conn_send_raw(struct cwist_webrtc_conn *conn, const uint8_t *data, size_t len) {
    return sendto(conn->ctx->udp_fd, data, len, 0, (struct sockaddr *)&conn->remote,
                  conn->remote_len);
}

struct cwist_webrtc_conn *cwist_webrtc_ctx_find_conn(cwist_webrtc_ctx *ctx,
                                                     const struct sockaddr_in *remote) {
    for (struct cwist_webrtc_conn *c = ctx->conns; c; c = c->next) {
        if (c->remote.sin_addr.s_addr == remote->sin_addr.s_addr &&
            c->remote.sin_port == remote->sin_port)
            return c;
    }
    return NULL;
}

void cwist_webrtc_ctx_add_conn(cwist_webrtc_ctx *ctx, struct cwist_webrtc_conn *conn) {
    pthread_mutex_lock(&ctx->lock);
    conn->next = ctx->conns;
    ctx->conns = conn;
    pthread_mutex_unlock(&ctx->lock);
}

void cwist_webrtc_conn_wake(cwist_webrtc_ctx *ctx) {
    uint8_t b = 1;
    ssize_t rc = write(ctx->wake_pipe[1], &b, 1);
    (void)rc;
}

/* Best-effort local IPv4 for host candidates: first non-loopback address. */
static void cwist_webrtc_detect_host_ip(char *out, size_t cap) {
    snprintf(out, cap, "127.0.0.1");
    struct ifaddrs *ifas = NULL;
    if (getifaddrs(&ifas) < 0)
        return;
    for (struct ifaddrs *ifa = ifas; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET)
            continue;
        struct sockaddr_in *sin = (struct sockaddr_in *)ifa->ifa_addr;
        uint32_t a = ntohl(sin->sin_addr.s_addr);
        if (((a >> 24) & 0xFF) == 127)
            continue;
        snprintf(out, cap, "%u.%u.%u.%u", (a >> 24) & 0xFF, (a >> 16) & 0xFF, (a >> 8) & 0xFF,
                 a & 0xFF);
        break;
    }
    freeifaddrs(ifas);
}

/* ---- time helpers ---- */

static void now_ts(struct timespec *ts) {
    clock_gettime(CLOCK_MONOTONIC, ts);
}

static long ms_until(const struct timespec *ts) {
    struct timespec now;
    now_ts(&now);
    long ms = (ts->tv_sec - now.tv_sec) * 1000 + (ts->tv_nsec - now.tv_nsec) / 1000000;
    return ms;
}

static void in_ms(long ms, struct timespec *ts) {
    now_ts(ts);
    ts->tv_sec += ms / 1000;
    ts->tv_nsec += (ms % 1000) * 1000000;
    if (ts->tv_nsec >= 1000000000) {
        ts->tv_sec += 1;
        ts->tv_nsec -= 1000000000;
    }
}

/* ---- conn lifecycle ---- */

static void conn_free(struct cwist_webrtc_conn *conn) {
    cwist_sctp_conn_close(conn);
    if (conn->ssl)
        SSL_free(conn->ssl);
    while (conn->pending_head) {
        pending_send *p = conn->pending_head;
        conn->pending_head = p->next;
        cwist_free(p);
    }
    cwist_free(conn);
}

static struct cwist_webrtc_conn *conn_new(cwist_webrtc_ctx *ctx,
                                          const struct sockaddr_in *remote, int is_server_role) {
    struct cwist_webrtc_conn *conn = cwist_malloc(1, sizeof(*conn));
    if (!conn)
        return NULL;
    conn->ctx = ctx;
    conn->is_server_role = is_server_role;
    conn->state = CWIST_CONN_STUN;
    conn->remote = *remote;
    conn->remote_len = sizeof(*remote);
    return conn;
}

static void conn_arm_stun_timer(struct cwist_webrtc_conn *conn) {
    in_ms(STUN_RETRANS_MS, &conn->stun_next);
}

static void conn_send_stun_request(struct cwist_webrtc_conn *conn) {
    uint8_t msg[512];
    char username[128];
    snprintf(username, sizeof(username), "%s:%s", conn->peer_ufrag, conn->ctx->ice_ufrag);
    int len = cwist_ice_stun_build_request(msg, sizeof(msg), conn->stun_txid, username);
    if (len < 0)
        return;
    len = cwist_ice_stun_sign_request(msg, sizeof(msg), (size_t)len, conn->peer_pwd);
    if (len < 0)
        return;
    cwist_webrtc_conn_send_raw(conn, msg, (size_t)len);
    conn_arm_stun_timer(conn);
    conn->stun_attempts++;
}

static void conn_start_dtls(struct cwist_webrtc_conn *conn) {
    cwist_webrtc_ctx *ctx = conn->ctx;
    SSL_CTX *ssl_ctx = conn->is_server_role ? ctx->server_ssl_ctx : ctx->client_ssl_ctx;
    conn->ssl = SSL_new(ssl_ctx);
    if (!conn->ssl) {
        conn->state = CWIST_CONN_DEAD;
        return;
    }
    BIO *rbio = BIO_new(BIO_s_mem());
    BIO *wbio = BIO_new(BIO_s_mem());
    BIO_set_mem_eof_return(rbio, -1);
    SSL_set_bio(conn->ssl, rbio, wbio);
    SSL_set_options(conn->ssl, SSL_OP_NO_QUERY_MTU);
    SSL_set_mtu(conn->ssl, 1200);
    if (conn->is_server_role)
        SSL_set_accept_state(conn->ssl);
    else
        SSL_set_connect_state(conn->ssl);
    conn->state = CWIST_CONN_DTLS;
    SSL_do_handshake(conn->ssl);
}

static void conn_flush_dtls_out(struct cwist_webrtc_conn *conn) {
    uint8_t buf[2048];
    for (;;) {
        int n = BIO_read(SSL_get_wbio(conn->ssl), buf, sizeof(buf));
        if (n <= 0)
            break;
        cwist_webrtc_conn_send_raw(conn, buf, (size_t)n);
        if (n < (int)sizeof(buf))
            break;
    }
}

static void conn_mark_dead(struct cwist_webrtc_conn *conn) {
    conn->state = CWIST_CONN_DEAD;
}

/* Drive handshake/data for a conn whose rbio has input. */
static void conn_service(struct cwist_webrtc_conn *conn) {
    if (conn->state == CWIST_CONN_DTLS) {
        int rc = SSL_do_handshake(conn->ssl);
        if (rc == 1) {
            if (cwist_sctp_conn_open(conn) < 0) {
                conn_mark_dead(conn);
                return;
            }
            conn->state = CWIST_CONN_ESTABLISHED;
        } else {
            int err = SSL_get_error(conn->ssl, rc);
            if (err == SSL_ERROR_SSL)
                conn_mark_dead(conn);
        }
    }
    if (conn->state == CWIST_CONN_ESTABLISHED) {
        uint8_t buf[65536];
        for (;;) {
            int n = SSL_read(conn->ssl, buf, sizeof(buf));
            if (n > 0) {
                cwist_sctp_conn_input(conn, buf, (size_t)n);
                continue;
            }
            int err = SSL_get_error(conn->ssl, n);
            if (err == SSL_ERROR_ZERO_RETURN || (err == SSL_ERROR_SYSCALL && n == 0)) {
                conn_mark_dead(conn);
            }
            break;
        }
        if (conn->state == CWIST_CONN_ESTABLISHED && !conn->sctp_ready &&
            cwist_sctp_assoc_established(conn)) {
            conn->sctp_ready = true;
        }
        cwist_sctp_conn_drain(conn);
        if (conn->sctp_ready) {
            while (conn->pending_head) {
                pending_send *p = conn->pending_head;
                if (cwist_sctp_send(conn, p->channel, p->data, p->len, p->is_string) < 0)
                    break;
                conn->pending_head = p->next;
                if (!conn->pending_head)
                    conn->pending_tail = NULL;
                cwist_free(p);
            }
        }
    }
    if (conn->ssl)
        conn_flush_dtls_out(conn);
}

/* ---- packet handling ---- */

static void handle_stun(cwist_webrtc_ctx *ctx, const uint8_t *buf, size_t len,
                        const struct sockaddr_in *remote) {
    if (cwist_ice_stun_is_binding_request(buf, len)) {
        if (!ctx->ice_lite_server)
            return; /* this ctx has not answered any offer: not an ICE-lite endpoint */
        struct cwist_webrtc_conn *conn = cwist_webrtc_ctx_find_conn(ctx, remote);
        if (!cwist_ice_stun_validate_request(buf, len, ctx->ice_pwd))
            return; /* bad MESSAGE-INTEGRITY: drop (RFC 8445 would send a 400) */
        uint8_t resp[256];
        int rlen = cwist_ice_stun_build_response(resp, sizeof(resp), buf, len, remote,
                                                 ctx->ice_pwd);
        if (rlen < 0)
            return;
        sendto(ctx->udp_fd, resp, (size_t)rlen, 0, (const struct sockaddr *)remote,
               sizeof(*remote));
        if (!conn && cwist_ice_stun_has_use_candidate(buf, len)) {
            /* Adopt a connection parked by cwist_webrtc_handle_offer() when the
             * request USERNAME carries its ufrag, else start a fresh one. */
            char rfrag[64] = "";
            cwist_ice_stun_get_remote_ufrag(buf, len, rfrag, sizeof(rfrag));
            for (struct cwist_webrtc_conn *c = ctx->conns; c; c = c->next) {
                if (c->is_server_role && c->state == CWIST_CONN_STUN && c->remote.sin_port == 0 &&
                    (rfrag[0] == '\0' || strcmp(c->peer_ufrag, rfrag) == 0)) {
                    conn = c;
                    break;
                }
            }
            int fresh = 0;
            if (!conn) {
                conn = conn_new(ctx, remote, 1);
                fresh = 1;
            }
            if (conn) {
                conn->remote = *remote;
                conn->remote_len = sizeof(*remote);
                if (fresh)
                    cwist_webrtc_ctx_add_conn(ctx, conn);
            }
        }
        if (conn && conn->state == CWIST_CONN_STUN &&
            cwist_ice_stun_has_use_candidate(buf, len)) {
            conn_start_dtls(conn);
            conn_service(conn);
        }
        return;
    }
    if (!cwist_ice_stun_is_message(buf, len))
        return;
    /* STUN response: only our client-role connections expect these. */
    struct cwist_webrtc_conn *conn = cwist_webrtc_ctx_find_conn(ctx, remote);
    if (!conn || conn->is_server_role || conn->state != CWIST_CONN_STUN)
        return;
    struct sockaddr_in mapped;
    if (cwist_ice_stun_parse_response(buf, len, conn->stun_txid, conn->peer_pwd, &mapped)) {
        conn_start_dtls(conn);
        conn_service(conn);
    }
}

static void handle_packet(cwist_webrtc_ctx *ctx, const uint8_t *buf, size_t len,
                          const struct sockaddr_in *remote) {
    if (cwist_ice_stun_is_message(buf, len)) {
        handle_stun(ctx, buf, len, remote);
        return;
    }
    /* DTLS record (content type 20-64). */
    struct cwist_webrtc_conn *conn = cwist_webrtc_ctx_find_conn(ctx, remote);
    if (!conn || !conn->ssl || conn->state == CWIST_CONN_STUN)
        return;
    if (len > INT32_MAX)
        return;
    BIO_write(SSL_get_rbio(conn->ssl), buf, (int)len);
    conn_service(conn);
}

/* ---- event loop ---- */

static void loop_run_timers(cwist_webrtc_ctx *ctx) {
    struct timespec now;
    now_ts(&now);
    struct cwist_webrtc_conn *dead = NULL;

    pthread_mutex_lock(&ctx->lock);
    struct cwist_webrtc_conn **pp = &ctx->conns;
    while (*pp) {
        struct cwist_webrtc_conn *conn = *pp;
        int reap = conn->state == CWIST_CONN_DEAD || conn->closed;
        if (reap) {
            *pp = conn->next;
            conn->next = dead;
            dead = conn;
            continue;
        }
        pp = &conn->next;
    }
    pthread_mutex_unlock(&ctx->lock);

    while (dead) {
        struct cwist_webrtc_conn *next = dead->next;
        conn_free(dead);
        dead = next;
    }

    for (struct cwist_webrtc_conn *conn = ctx->conns; conn; conn = conn->next) {
        if (conn->state == CWIST_CONN_STUN && !conn->is_server_role &&
            conn->stun_attempts > 0 && ms_until(&conn->stun_next) <= 0) {
            if (conn->stun_attempts >= STUN_MAX_ATTEMPTS) {
                conn_mark_dead(conn);
                continue;
            }
            conn_send_stun_request(conn);
        }
        if (conn->state == CWIST_CONN_DTLS) {
            struct timeval tv;
            if (DTLSv1_get_timeout(conn->ssl, &tv) == 1 && tv.tv_sec == 0 && tv.tv_usec == 0) {
                DTLSv1_handle_timeout(conn->ssl);
                conn_service(conn);
            }
        }
        if (conn->state == CWIST_CONN_ESTABLISHED) {
            conn_service(conn);
        }
    }
}

static void *ctx_thread_main(void *arg) {
    cwist_webrtc_ctx *ctx = arg;
    uint8_t buf[65536];
    struct timespec last_tick;
    now_ts(&last_tick);

    while (!ctx->stop) {
        struct pollfd fds[2] = {
            { .fd = ctx->udp_fd, .events = POLLIN },
            { .fd = ctx->wake_pipe[0], .events = POLLIN },
        };
        int prc = poll(fds, 2, SCTP_TIMER_MS);
        if (prc < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (fds[1].revents & POLLIN) {
            uint8_t drain[64];
            while (read(ctx->wake_pipe[0], drain, sizeof(drain)) > 0)
                ;
        }
        if (fds[0].revents & POLLIN) {
            for (;;) {
                struct sockaddr_in remote;
                socklen_t rlen = sizeof(remote);
                ssize_t n = recvfrom(ctx->udp_fd, buf, sizeof(buf), MSG_DONTWAIT,
                                     (struct sockaddr *)&remote, &rlen);
                if (n <= 0)
                    break;
                handle_packet(ctx, buf, (size_t)n, &remote);
            }
        }

        struct timespec now;
        now_ts(&now);
        long elapsed_ms = (now.tv_sec - last_tick.tv_sec) * 1000 +
                          (now.tv_nsec - last_tick.tv_nsec) / 1000000;
        if (elapsed_ms > SCTP_TIMER_MS)
            elapsed_ms = SCTP_TIMER_MS;
        if (elapsed_ms < 0)
            elapsed_ms = 0;
        if (elapsed_ms > 0) {
            usrsctp_handle_timers((uint32_t)elapsed_ms);
            last_tick = now;
        }
        loop_run_timers(ctx);
    }
    return NULL;
}

/* ---- public API ---- */

cwist_webrtc_ctx *cwist_webrtc_ctx_new(uint16_t port) {
    if (cwist_sctp_global_init() < 0)
        return NULL;

    cwist_webrtc_ctx *ctx = cwist_malloc(1, sizeof(*ctx));
    if (!ctx)
        return NULL;
    ctx->udp_fd = -1;
    ctx->wake_pipe[0] = -1;
    ctx->wake_pipe[1] = -1;
    pthread_mutex_init(&ctx->lock, NULL);

    ctx->udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (ctx->udp_fd < 0)
        goto fail;
    int flags = fcntl(ctx->udp_fd, F_GETFL, 0);
    fcntl(ctx->udp_fd, F_SETFL, flags | O_NONBLOCK);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (bind(ctx->udp_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
        goto fail;
    socklen_t alen = sizeof(addr);
    if (getsockname(ctx->udp_fd, (struct sockaddr *)&addr, &alen) < 0)
        goto fail;
    ctx->port = ntohs(addr.sin_port);

    if (pipe(ctx->wake_pipe) < 0)
        goto fail;
    for (int i = 0; i < 2; i++) {
        int fl = fcntl(ctx->wake_pipe[i], F_GETFL, 0);
        fcntl(ctx->wake_pipe[i], F_SETFL, fl | O_NONBLOCK);
    }

    if (cwist_dtls_generate_cert(&ctx->cert, &ctx->pkey) < 0)
        goto fail;
    if (cwist_dtls_fingerprint(ctx->cert, ctx->fingerprint, sizeof(ctx->fingerprint)) < 0)
        goto fail;
    ctx->server_ssl_ctx = cwist_dtls_ctx_new(1, ctx->cert, ctx->pkey);
    ctx->client_ssl_ctx = cwist_dtls_ctx_new(0, NULL, NULL);
    if (!ctx->server_ssl_ctx || !ctx->client_ssl_ctx)
        goto fail;

    cwist_ice_random_creds(ctx->ice_ufrag, sizeof(ctx->ice_ufrag), ctx->ice_pwd,
                           sizeof(ctx->ice_pwd));
    cwist_webrtc_detect_host_ip(ctx->host_ip, sizeof(ctx->host_ip));

    ctx->thread_running = pthread_create(&ctx->thread, NULL, &ctx_thread_main, ctx) == 0;
    if (!ctx->thread_running)
        goto fail;
    return ctx;

fail:
    cwist_webrtc_ctx_free(ctx);
    return NULL;
}

uint16_t cwist_webrtc_ctx_port(const cwist_webrtc_ctx *ctx) {
    return ctx->port;
}

const char *cwist_webrtc_ctx_fingerprint(const cwist_webrtc_ctx *ctx) {
    return ctx->fingerprint;
}

void cwist_webrtc_ctx_set_message_handler(cwist_webrtc_ctx *ctx, cwist_webrtc_message_cb cb,
                                          void *user) {
    ctx->msg_cb = cb;
    ctx->msg_user = user;
}

void cwist_webrtc_ctx_set_channel_handler(cwist_webrtc_ctx *ctx, cwist_webrtc_channel_cb cb,
                                          void *user) {
    ctx->ch_cb = cb;
    ctx->ch_user = user;
}

int cwist_webrtc_handle_offer(cwist_webrtc_ctx *ctx, const char *offer, char *answer_out,
                              size_t out_len) {
    cwist_sdp_info info;
    if (cwist_sdp_parse(offer, strlen(offer), &info) < 0)
        return -1;
    struct sockaddr_in wildcard;
    memset(&wildcard, 0, sizeof(wildcard));
    wildcard.sin_family = AF_INET;
    wildcard.sin_addr.s_addr = htonl(INADDR_ANY);
    wildcard.sin_port = 0;
    struct cwist_webrtc_conn *conn = conn_new(ctx, &wildcard, 1);
    if (!conn)
        return -1;
    snprintf(conn->peer_ufrag, sizeof(conn->peer_ufrag), "%s", info.ice_ufrag);
    snprintf(conn->peer_pwd, sizeof(conn->peer_pwd), "%s", info.ice_pwd);
    cwist_webrtc_ctx_add_conn(ctx, conn);
    ctx->ice_lite_server = 1;

    const char *mid = info.mid[0] ? info.mid : "0";
    return cwist_sdp_write_answer(answer_out, out_len, ctx->fingerprint, ctx->ice_ufrag,
                                  ctx->ice_pwd, mid, ctx->host_ip, ctx->port);
}

int cwist_webrtc_ctx_connection_count(const cwist_webrtc_ctx *ctx) {
    int n = 0;
    for (struct cwist_webrtc_conn *c = ctx->conns; c; c = c->next) {
        if (c->sctp_ready)
            n++;
    }
    return n;
}

int cwist_webrtc_conn_send(cwist_webrtc_conn *conn, uint16_t channel_id, const uint8_t *data,
                           size_t len, cwist_webrtc_data_type type) {
    if (len > 1 << 20)
        return -1;
    pending_send *p = cwist_alloc(sizeof(*p) + len);
    if (!p)
        return -1;
    p->next = NULL;
    p->channel = channel_id;
    p->is_string = type == CWIST_WEBRTC_DATA_STRING;
    p->len = len;
    memcpy(p->data, data, len);

    pthread_mutex_lock(&conn->ctx->lock);
    if (conn->pending_tail)
        conn->pending_tail->next = p;
    else
        conn->pending_head = p;
    conn->pending_tail = p;
    pthread_mutex_unlock(&conn->ctx->lock);
    cwist_webrtc_conn_wake(conn->ctx);
    return 0;
}

void cwist_webrtc_conn_close(cwist_webrtc_conn *conn) {
    conn->closed = true;
    cwist_webrtc_conn_wake(conn->ctx);
}

void cwist_webrtc_ctx_free(cwist_webrtc_ctx *ctx) {
    if (!ctx)
        return;
    ctx->stop = true;
    cwist_webrtc_conn_wake(ctx);
    if (ctx->thread_running)
        pthread_join(ctx->thread, NULL);

    struct cwist_webrtc_conn *conn = ctx->conns;
    while (conn) {
        struct cwist_webrtc_conn *next = conn->next;
        conn_free(conn);
        conn = next;
    }
    if (ctx->udp_fd >= 0)
        close(ctx->udp_fd);
    if (ctx->wake_pipe[0] >= 0)
        close(ctx->wake_pipe[0]);
    if (ctx->wake_pipe[1] >= 0)
        close(ctx->wake_pipe[1]);
    if (ctx->server_ssl_ctx)
        SSL_CTX_free(ctx->server_ssl_ctx);
    if (ctx->client_ssl_ctx)
        SSL_CTX_free(ctx->client_ssl_ctx);
    if (ctx->cert)
        X509_free(ctx->cert);
    if (ctx->pkey)
        EVP_PKEY_free(ctx->pkey);
    pthread_mutex_destroy(&ctx->lock);
    cwist_free(ctx);
}

/* ---- internal client-role dialer (loopback tests) ---- */

cwist_webrtc_conn *cwist_webrtc_connect(cwist_webrtc_ctx *ctx, const struct sockaddr_in *remote,
                                        const char *peer_ufrag, const char *peer_pwd) {
    struct cwist_webrtc_conn *conn = conn_new(ctx, remote, 0);
    if (!conn)
        return NULL;
    snprintf(conn->peer_ufrag, sizeof(conn->peer_ufrag), "%s", peer_ufrag);
    snprintf(conn->peer_pwd, sizeof(conn->peer_pwd), "%s", peer_pwd);
    RAND_bytes(conn->stun_txid, sizeof(conn->stun_txid));
    cwist_webrtc_ctx_add_conn(ctx, conn);
    conn_send_stun_request(conn);
    cwist_webrtc_conn_wake(ctx);
    return conn;
}
