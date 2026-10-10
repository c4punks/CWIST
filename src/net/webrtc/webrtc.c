/** @file webrtc.c
 * @brief WebRTC DataChannel context: UDP I/O, ICE-lite, DTLS, SCTP driving.
 *
 * A ctx is one UDP socket served by one cwist reactor (see webrtc_internal.h
 * for the threading and lifetime rules).  Work happens only in reactor
 * callbacks, and every callback follows the same shape:
 *
 *   ctx_enter()  - mark this thread as servicing the ctx, cache the time
 *   ...          - handle datagrams / a timer / a posted request
 *   ctx_leave()  - service conns touched this round (drain SCTP, flush
 *                  queued sends, apply closes), then send every queued
 *                  datagram in one sendmmsg()
 *
 * Datagrams are read in batches with recvmmsg().  A DTLS record is decrypted
 * and fed to usrsctp right away, but draining SCTP and flushing sends waits
 * until the batch is done, so a burst of packets for one conn costs one drain.
 * Nothing runs on a fixed tick: each conn has one timer (STUN retransmit,
 * DTLS retransmit, liveness or park expiry, whichever applies) and the ctx
 * drives usrsctp's clock only while it holds associations, every 10 ms
 * after recent traffic and every 250 ms otherwise.
 */
#include "webrtc_internal.h"

#include <cwist/core/mem/alloc.h>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <openssl/rand.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/** @name Timing */
/**@{*/
#define STUN_RTO_INITIAL_MS 300     /**< First Binding retransmit (client role). */
#define STUN_RTO_MAX_MS 3000        /**< Retransmit backoff cap. */
#define STUN_MAX_ATTEMPTS 7         /**< Binding requests before giving up. */
#define PARK_TIMEOUT_MS 30000       /**< Answered offer with no nomination. */
#define HANDSHAKE_TIMEOUT_MS 30000  /**< ICE + DTLS must finish within this. */
#define LIVENESS_TIMEOUT_MS 30000   /**< Silence before an established conn is dropped. */
#define SCTP_TICK_ACTIVE_MS 10      /**< usrsctp clock step after recent traffic. */
#define SCTP_TICK_IDLE_MS 250       /**< usrsctp clock step when quiet. */
#define SCTP_ACTIVE_WINDOW_MS 1000  /**< "Recent" for the tick choice. */
/**@}*/

/** Most datagrams read in one readiness callback before yielding. */
#define RX_BUDGET 256
/** Socket buffer requested for the UDP socket (the kernel may clamp it). */
#define UDP_SOCKBUF (4 << 20)

/** The ctx this thread is servicing right now, if any. */
static _Thread_local cwist_webrtc_ctx *tl_ctx;

static void ctx_release(cwist_webrtc_ctx *ctx);
static void ctx_release_resources(cwist_webrtc_ctx *ctx);
static void conn_teardown(struct cwist_webrtc_conn *conn);
static void conn_rearm_timer(struct cwist_webrtc_conn *conn);
static void ctx_arm_sctp_timer(cwist_webrtc_ctx *ctx);
static void ctx_note_activity(cwist_webrtc_ctx *ctx);
static void ctx_teardown_owner(cwist_webrtc_ctx *ctx);
static void conn_timer_cb(void *arg);

/* ---- time ---- */

/** @brief CLOCK_MONOTONIC in nanoseconds. */
static uint64_t mono_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* ---- references ---- */

/** @brief Take a ctx reference. */
static void ctx_retain(cwist_webrtc_ctx *ctx) {
    atomic_fetch_add_explicit(&ctx->refs, 1, memory_order_relaxed);
}

void cwist_webrtc_conn_retain(cwist_webrtc_conn *conn) {
    if (conn) atomic_fetch_add_explicit(&conn->refs, 1, memory_order_relaxed);
}

void cwist_webrtc_conn_release(cwist_webrtc_conn *conn) {
    if (!conn || atomic_fetch_sub_explicit(&conn->refs, 1, memory_order_acq_rel) != 1) return;
    /* Last reference: teardown already released everything but the memory,
     * unless the conn never got registered (post dropped at ctx teardown). */
    cwist_webrtc_ctx *ctx = conn->ctx;
    if (conn->ssl) SSL_free(conn->ssl);
    cwist_free(conn->ch_bits);
    cwist_free(conn->rx_partial);
    cwist_free(conn);
    ctx_release(ctx);
}

/* ---- owner-thread bracketing ---- */

bool cwist_webrtc_on_owner(const cwist_webrtc_ctx *ctx) {
    return tl_ctx == ctx;
}

/**
 * @brief Enter a reactor callback for @p ctx.  The outermost entry holds a
 *        ctx reference, so a handler that frees the ctx cannot pull memory
 *        out from under the callback still running.
 * @return The previously serviced ctx (restore it with ctx_leave()).
 */
static cwist_webrtc_ctx *ctx_enter(cwist_webrtc_ctx *ctx) {
    cwist_webrtc_ctx *prev = tl_ctx;
    if (prev != ctx) {
        ctx_retain(ctx);
        tl_ctx = ctx;
        ctx->now_ns = mono_ns();
    }
    return prev;
}

/** @brief Send every queued datagram (sendmmsg on Linux). */
static void ctx_tx_flush(cwist_webrtc_ctx *ctx) {
    uint32_t n = ctx->tx_n;
    ctx->tx_n = 0;
    if (n == 0 || ctx->udp_fd < 0) return;
#ifdef __linux__
    struct mmsghdr msgs[CWIST_WEBRTC_TX_BATCH];
    struct iovec iov[CWIST_WEBRTC_TX_BATCH];
    for (uint32_t i = 0; i < n; i++) {
        iov[i].iov_base = ctx->tx_bufs + (size_t)i * CWIST_WEBRTC_DGRAM_MAX;
        iov[i].iov_len = ctx->tx_len[i];
        memset(&msgs[i].msg_hdr, 0, sizeof(msgs[i].msg_hdr));
        msgs[i].msg_hdr.msg_name = &ctx->tx_addr[i];
        msgs[i].msg_hdr.msg_namelen = sizeof(ctx->tx_addr[i]);
        msgs[i].msg_hdr.msg_iov = &iov[i];
        msgs[i].msg_hdr.msg_iovlen = 1;
    }
    uint32_t off = 0;
    while (off < n) {
        int sent = sendmmsg(ctx->udp_fd, msgs + off, n - off, MSG_DONTWAIT);
        if (sent > 0) {
            off += (uint32_t)sent;
            continue;
        }
        if (sent < 0 && errno == EINTR) continue;
        /* EAGAIN/ENOBUFS or a per-destination error: UDP is lossy anyway and
         * DTLS/SCTP retransmit, so skip this datagram and keep going. */
        off++;
    }
#else
    for (uint32_t i = 0; i < n; i++) {
        sendto(ctx->udp_fd, ctx->tx_bufs + (size_t)i * CWIST_WEBRTC_DGRAM_MAX, ctx->tx_len[i], 0,
               (const struct sockaddr *)&ctx->tx_addr[i], sizeof(ctx->tx_addr[i]));
    }
#endif
}

/** @brief Queue one datagram for the end-of-round flush (owner thread). */
static void ctx_tx_queue(cwist_webrtc_ctx *ctx, const uint8_t *data, size_t len,
                         const struct sockaddr_in *to) {
    if (len > CWIST_WEBRTC_DGRAM_MAX || ctx->closed) return;
    if (ctx->tx_n == CWIST_WEBRTC_TX_BATCH) ctx_tx_flush(ctx);
    uint32_t i = ctx->tx_n++;
    memcpy(ctx->tx_bufs + (size_t)i * CWIST_WEBRTC_DGRAM_MAX, data, len);
    ctx->tx_len[i] = len;
    ctx->tx_addr[i] = *to;
}

/** @brief Put @p conn on the end-of-round service list (holds a reference). */
static void conn_mark_dirty(struct cwist_webrtc_conn *conn) {
    if (conn->dirty) return;
    conn->dirty = true;
    cwist_webrtc_conn_retain(conn);
    conn->dirty_next = conn->ctx->dirty;
    conn->ctx->dirty = conn;
}

/** @brief Hand queued sends to SCTP until it pushes back. */
static void conn_flush_pending(struct cwist_webrtc_conn *conn) {
    while (conn->pending_head && conn->sctp_ready) {
        cwist_webrtc_msg *m = conn->pending_head;
        int rc = cwist_sctp_send(conn, m->channel, m->data, m->len, m->is_string);
        if (rc == 1) break; /* send buffer full: retried after the next inbound packet */
        conn->pending_head = m->next;
        if (!conn->pending_head) conn->pending_tail = NULL;
        atomic_fetch_sub_explicit(&conn->buffered, m->len, memory_order_relaxed);
        cwist_free(m);
    }
}

/** @brief End-of-round work for one conn: SCTP drain, sends, close. */
static void conn_service(struct cwist_webrtc_conn *conn) {
    if (atomic_load(&conn->detached)) return;
    if (conn->state == CWIST_CONN_ESTABLISHED && !conn->close_requested) {
        if (cwist_sctp_conn_drain(conn) < 0) conn->close_requested = true;
        if (!conn->close_requested && !conn->sctp_ready && cwist_sctp_assoc_established(conn)) {
            conn->sctp_ready = true;
            conn->notified_open = true;
            atomic_fetch_add(&conn->ctx->ready_count, 1);
        }
        if (!conn->close_requested) conn_flush_pending(conn);
    }
    if (conn->close_requested) conn_teardown(conn);
}

/** @brief Leave a reactor callback: service dirty conns, flush datagrams. */
static void ctx_leave(cwist_webrtc_ctx *ctx, cwist_webrtc_ctx *prev) {
    if (prev != ctx) {
        /* Servicing can dirty more conns (a message handler sending on
         * another conn), so loop until the list stays empty. */
        while (ctx->dirty) {
            struct cwist_webrtc_conn *list = ctx->dirty;
            ctx->dirty = NULL;
            while (list) {
                struct cwist_webrtc_conn *next = list->dirty_next;
                list->dirty = false;
                conn_service(list);
                cwist_webrtc_conn_release(list);
                list = next;
            }
        }
        ctx_tx_flush(ctx);
        tl_ctx = prev;
        ctx_release(ctx);
    }
}

/* ---- connection table ---- */

/** @brief Bucket index for a remote address. */
static uint32_t conn_hash(const cwist_webrtc_ctx *ctx, const struct sockaddr_in *a) {
    uint32_t h = a->sin_addr.s_addr * 0x9E3779B1u ^ (uint32_t)a->sin_port * 0x85EBCA77u;
    h ^= h >> 15;
    return h & (ctx->nbuckets - 1);
}

/** @brief Look up the conn nominated on @p remote. */
static struct cwist_webrtc_conn *table_find(const cwist_webrtc_ctx *ctx,
                                            const struct sockaddr_in *remote) {
    for (struct cwist_webrtc_conn *c = ctx->buckets[conn_hash(ctx, remote)]; c; c = c->hnext) {
        if (c->remote.sin_addr.s_addr == remote->sin_addr.s_addr &&
            c->remote.sin_port == remote->sin_port)
            return c;
    }
    return NULL;
}

/** @brief Double the bucket array once the load factor passes 1. */
static void table_grow(cwist_webrtc_ctx *ctx) {
    uint32_t nb = ctx->nbuckets * 2;
    struct cwist_webrtc_conn **b = cwist_alloc(nb * sizeof(*b));
    if (!b) return; /* keep the old, longer chains */
    uint32_t old_n = ctx->nbuckets;
    struct cwist_webrtc_conn **old = ctx->buckets;
    ctx->buckets = b;
    ctx->nbuckets = nb;
    for (uint32_t i = 0; i < old_n; i++) {
        struct cwist_webrtc_conn *c = old[i];
        while (c) {
            struct cwist_webrtc_conn *next = c->hnext;
            uint32_t h = conn_hash(ctx, &c->remote);
            c->hnext = b[h];
            b[h] = c;
            c = next;
        }
    }
    cwist_free(old);
}

/** @brief Insert @p conn keyed by its remote address (takes the table's reference). */
static void table_insert(cwist_webrtc_ctx *ctx, struct cwist_webrtc_conn *conn) {
    if (ctx->nconns + 1 > ctx->nbuckets) table_grow(ctx);
    uint32_t h = conn_hash(ctx, &conn->remote);
    conn->hnext = ctx->buckets[h];
    ctx->buckets[h] = conn;
    ctx->nconns++;
    conn->registered = true;
    conn->parked = false;
}

/** @brief Unlink @p conn from the table or the parked list. */
static void table_remove(cwist_webrtc_ctx *ctx, struct cwist_webrtc_conn *conn) {
    struct cwist_webrtc_conn **pp =
        conn->parked ? &ctx->parked : &ctx->buckets[conn_hash(ctx, &conn->remote)];
    while (*pp && *pp != conn) pp = &(*pp)->hnext;
    if (*pp) {
        *pp = conn->hnext;
        if (!conn->parked) ctx->nconns--;
    }
    conn->hnext = NULL;
    conn->registered = false;
    conn->parked = false;
}

/* ---- conn lifecycle ---- */

/** @brief Allocate a conn (one reference, owned by the caller). */
static struct cwist_webrtc_conn *conn_new(cwist_webrtc_ctx *ctx, const struct sockaddr_in *remote,
                                          bool is_server_role) {
    struct cwist_webrtc_conn *conn = cwist_malloc(sizeof(*conn));
    if (!conn) return NULL;
    conn->ctx = ctx;
    ctx_retain(ctx);
    atomic_init(&conn->refs, 1);
    atomic_init(&conn->detached, false);
    atomic_init(&conn->close_posted, false);
    atomic_init(&conn->buffered, 0);
    conn->is_server_role = is_server_role;
    conn->state = CWIST_CONN_STUN;
    if (remote) conn->remote = *remote;
    conn->created_ns = mono_ns();
    conn->last_rx_ns = conn->created_ns;
    conn->stun_rto_ms = STUN_RTO_INITIAL_MS;
    cwist_reactor_timer_init(&conn->timer, conn_timer_cb, conn);
    return conn;
}

/**
 * @brief Close @p conn now (owner thread): SCTP abort, DTLS free, unlink,
 *        close handler.  Idempotent; memory goes with the last reference.
 */
static void conn_teardown(struct cwist_webrtc_conn *conn) {
    if (atomic_exchange(&conn->detached, true)) return;
    cwist_webrtc_ctx *ctx = conn->ctx;
    conn->state = CWIST_CONN_CLOSED;
    cwist_reactor_timer_cancel(ctx->reactor, &conn->timer);
    if (conn->sctp_ready) atomic_fetch_sub(&ctx->ready_count, 1);
    conn->sctp_ready = false;
    if (conn->sctp_sock || conn->sctp_acc) {
        cwist_sctp_conn_close(conn);
        if (--ctx->sctp_assocs == 0) cwist_reactor_timer_cancel(ctx->reactor, &ctx->sctp_timer);
    }
    if (conn->ssl) {
        SSL_free(conn->ssl);
        conn->ssl = NULL;
    }
    while (conn->pending_head) {
        cwist_webrtc_msg *m = conn->pending_head;
        conn->pending_head = m->next;
        atomic_fetch_sub_explicit(&conn->buffered, m->len, memory_order_relaxed);
        cwist_free(m);
    }
    conn->pending_tail = NULL;
    bool notify = conn->notified_open;
    conn->notified_open = false;
    if (notify && ctx->close_cb) ctx->close_cb(conn, ctx->close_user);
    if (conn->registered) {
        table_remove(ctx, conn);
        cwist_webrtc_conn_release(conn); /* the table's reference */
    }
}

/** @brief Create the DTLS session on the custom datagram BIO and start it. */
static void conn_start_dtls(struct cwist_webrtc_conn *conn) {
    cwist_webrtc_ctx *ctx = conn->ctx;
    SSL_CTX *ssl_ctx = conn->is_server_role ? ctx->server_ssl_ctx : ctx->client_ssl_ctx;
    conn->ssl = SSL_new(ssl_ctx);
    BIO *bio = conn->ssl ? cwist_dtls_bio_new(conn) : NULL;
    if (!bio) {
        conn->close_requested = true;
        conn_mark_dirty(conn);
        return;
    }
    SSL_set_bio(conn->ssl, bio, bio); /* one BIO for both directions */
    SSL_set_options(conn->ssl, SSL_OP_NO_QUERY_MTU);
    SSL_set_mtu(conn->ssl, cwist_dtls_link_mtu());
    if (conn->is_server_role)
        SSL_set_accept_state(conn->ssl);
    else
        SSL_set_connect_state(conn->ssl);
    conn->state = CWIST_CONN_DTLS;
    SSL_do_handshake(conn->ssl); /* client: emits ClientHello */
    conn_rearm_timer(conn);
}

/** @brief DTLS finished: open SCTP and start the usrsctp clock. */
static void conn_on_dtls_up(struct cwist_webrtc_conn *conn) {
    cwist_webrtc_ctx *ctx = conn->ctx;
    conn->state = CWIST_CONN_ESTABLISHED;
    if (cwist_sctp_conn_open(conn) < 0) {
        conn->close_requested = true;
        return;
    }
    if (ctx->sctp_assocs++ == 0) ctx_arm_sctp_timer(ctx);
    conn_rearm_timer(conn);
}

/** @brief Feed one DTLS datagram through the session (and SCTP above it). */
static void conn_dtls_input(struct cwist_webrtc_conn *conn, const uint8_t *buf, size_t len) {
    conn->dtls_in = buf;
    conn->dtls_in_len = len;
    if (conn->state == CWIST_CONN_DTLS) {
        int rc = SSL_do_handshake(conn->ssl);
        if (rc == 1) {
            conn_on_dtls_up(conn);
        } else if (SSL_get_error(conn->ssl, rc) == SSL_ERROR_SSL) {
            conn->close_requested = true;
        } else {
            conn_rearm_timer(conn); /* a flight arrived: new retransmit deadline */
        }
    }
    if (conn->state == CWIST_CONN_ESTABLISHED && !conn->close_requested) {
        for (;;) {
            int n = SSL_read(conn->ssl, conn->ctx->plain_buf, CWIST_WEBRTC_MAX_MESSAGE);
            if (n > 0) {
                cwist_sctp_conn_input(conn, conn->ctx->plain_buf, (size_t)n);
                continue;
            }
            int err = SSL_get_error(conn->ssl, n);
            if (err == SSL_ERROR_ZERO_RETURN || err == SSL_ERROR_SSL ||
                (err == SSL_ERROR_SYSCALL && n == 0))
                conn->close_requested = true;
            break;
        }
    }
    conn->dtls_in = NULL;
    conn->dtls_in_len = 0;
    conn_mark_dirty(conn);
}

/** @brief Send (or resend) the client-role Binding request. */
static void conn_send_stun_request(struct cwist_webrtc_conn *conn) {
    uint8_t msg[512];
    char username[128];
    snprintf(username, sizeof(username), "%s:%s", conn->peer_ufrag, conn->ctx->ice_ufrag);
    int len = cwist_ice_stun_build_request(msg, sizeof(msg), conn->stun_txid, username);
    if (len < 0) return;
    len = cwist_ice_stun_sign_request(msg, sizeof(msg), (size_t)len, conn->peer_pwd);
    if (len < 0) return;
    ctx_tx_queue(conn->ctx, msg, (size_t)len, &conn->remote);
    conn->stun_attempts++;
}

/* ---- per-conn timer ---- */

/** @brief Point the conn timer at the next deadline its state needs. */
static void conn_rearm_timer(struct cwist_webrtc_conn *conn) {
    cwist_webrtc_ctx *ctx = conn->ctx;
    uint64_t now = ctx->now_ns ? ctx->now_ns : mono_ns();
    uint64_t delay_us;
    switch (conn->state) {
        case CWIST_CONN_STUN:
            if (conn->parked) {
                uint64_t end = conn->created_ns + PARK_TIMEOUT_MS * 1000000ull;
                delay_us = end > now ? (end - now) / 1000 : 0;
            } else if (!conn->is_server_role) {
                delay_us = (uint64_t)conn->stun_rto_ms * 1000;
            } else {
                uint64_t end = conn->created_ns + HANDSHAKE_TIMEOUT_MS * 1000000ull;
                delay_us = end > now ? (end - now) / 1000 : 0;
            }
            break;
        case CWIST_CONN_DTLS: {
            struct timeval tv;
            uint64_t end = conn->created_ns + HANDSHAKE_TIMEOUT_MS * 1000000ull;
            uint64_t hs_us = end > now ? (end - now) / 1000 : 0;
            delay_us = hs_us;
            if (DTLSv1_get_timeout(conn->ssl, &tv) == 1) {
                uint64_t t = (uint64_t)tv.tv_sec * 1000000ull + (uint64_t)tv.tv_usec;
                if (t < delay_us) delay_us = t;
            }
            break;
        }
        case CWIST_CONN_ESTABLISHED: {
            uint64_t end = conn->last_rx_ns + LIVENESS_TIMEOUT_MS * 1000000ull;
            delay_us = end > now ? (end - now) / 1000 : 0;
            break;
        }
        default: return;
    }
    cwist_reactor_timer_arm(ctx->reactor, &conn->timer, delay_us);
}

/** @brief Conn timer: retransmit, give up, or check liveness. */
static void conn_timer_cb(void *arg) {
    struct cwist_webrtc_conn *conn = arg;
    cwist_webrtc_ctx *ctx = conn->ctx;
    cwist_webrtc_ctx *prev = ctx_enter(ctx);
    uint64_t now = ctx->now_ns;
    uint64_t age_ms = (now - conn->created_ns) / 1000000ull;
    switch (conn->state) {
        case CWIST_CONN_STUN:
            if (conn->parked || conn->is_server_role) {
                if (age_ms >= (conn->parked ? PARK_TIMEOUT_MS : HANDSHAKE_TIMEOUT_MS))
                    conn->close_requested = true;
            } else if (conn->stun_attempts >= STUN_MAX_ATTEMPTS) {
                conn->close_requested = true;
            } else {
                conn_send_stun_request(conn);
                conn->stun_rto_ms = conn->stun_rto_ms * 2 > STUN_RTO_MAX_MS ? STUN_RTO_MAX_MS
                                                                            : conn->stun_rto_ms * 2;
            }
            break;
        case CWIST_CONN_DTLS:
            if (age_ms >= HANDSHAKE_TIMEOUT_MS) {
                conn->close_requested = true;
            } else if (DTLSv1_handle_timeout(conn->ssl) < 0) {
                conn->close_requested = true;
            }
            break;
        case CWIST_CONN_ESTABLISHED:
            if ((now - conn->last_rx_ns) / 1000000ull >= LIVENESS_TIMEOUT_MS)
                conn->close_requested = true;
            break;
        default: break;
    }
    if (conn->close_requested)
        conn_mark_dirty(conn);
    else
        conn_rearm_timer(conn);
    ctx_leave(ctx, prev);
}

/* ---- SCTP clock ---- */

/** @brief Arm the usrsctp tick: fast after recent traffic, slow when quiet. */
static void ctx_arm_sctp_timer(cwist_webrtc_ctx *ctx) {
    uint64_t now = ctx->now_ns ? ctx->now_ns : mono_ns();
    ctx->sctp_fast = now - ctx->last_activity_ns < SCTP_ACTIVE_WINDOW_MS * 1000000ull;
    uint64_t step_ms = ctx->sctp_fast ? SCTP_TICK_ACTIVE_MS : SCTP_TICK_IDLE_MS;
    cwist_reactor_timer_arm(ctx->reactor, &ctx->sctp_timer, step_ms * 1000);
}

/** @brief Record traffic; switch a slow SCTP tick back to the fast rate. */
static void ctx_note_activity(cwist_webrtc_ctx *ctx) {
    ctx->last_activity_ns = ctx->now_ns;
    if (ctx->sctp_assocs > 0 && !ctx->sctp_fast) ctx_arm_sctp_timer(ctx);
}

/** @brief Advance usrsctp's clock (fires SCTP retransmit/SACK timers). */
static void ctx_sctp_timer_cb(void *arg) {
    cwist_webrtc_ctx *ctx = arg;
    cwist_webrtc_ctx *prev = ctx_enter(ctx);
    cwist_sctp_tick();
    if (ctx->sctp_assocs > 0) ctx_arm_sctp_timer(ctx);
    ctx_leave(ctx, prev);
}

/* ---- callbacks used by sctp.c / dtls.c ---- */

void cwist_webrtc_conn_on_message(struct cwist_webrtc_conn *conn, uint16_t channel,
                                  const uint8_t *data, size_t len, int is_string) {
    cwist_webrtc_ctx *ctx = conn->ctx;
    if (ctx->msg_cb)
        ctx->msg_cb(conn, channel, data, len,
                    is_string ? CWIST_WEBRTC_DATA_STRING : CWIST_WEBRTC_DATA_BINARY, ctx->msg_user);
}

void cwist_webrtc_conn_on_channel_open(struct cwist_webrtc_conn *conn, uint16_t channel,
                                       const char *label) {
    cwist_webrtc_ctx *ctx = conn->ctx;
    if (ctx->ch_cb) ctx->ch_cb(conn, channel, label, ctx->ch_user);
}

void cwist_webrtc_conn_dtls_out(struct cwist_webrtc_conn *conn, const uint8_t *data, size_t len) {
    ctx_tx_queue(conn->ctx, data, len, &conn->remote);
}

/** @brief An SCTP packet produced off the owner thread, on its way there. */
typedef struct {
    cwist_reactor_post_t post; /**< Post node. */
    struct cwist_webrtc_conn *conn; /**< Referenced while posted. */
    size_t len;                /**< Packet length. */
    uint8_t data[];            /**< Packet. */
} sctp_out_post;

/** @brief Owner side of a marshalled SCTP packet: encrypt and queue it. */
static void sctp_out_post_cb(void *arg) {
    sctp_out_post *p = arg;
    struct cwist_webrtc_conn *conn = p->conn;
    cwist_webrtc_ctx *prev = ctx_enter(conn->ctx);
    if (conn->ssl) SSL_write(conn->ssl, p->data, (int)p->len);
    ctx_leave(conn->ctx, prev);
    cwist_webrtc_conn_release(conn);
    cwist_free(p);
}

void cwist_webrtc_conn_sctp_out(struct cwist_webrtc_conn *conn, const void *buffer, size_t len) {
    if (cwist_webrtc_on_owner(conn->ctx)) {
        /* No detached check: teardown closes SCTP (emitting its ABORT)
         * before it frees the DTLS session. */
        if (conn->ssl) SSL_write(conn->ssl, buffer, (int)len);
        return;
    }
    /* A usrsctp timer pass on another thread produced this packet. */
    sctp_out_post *p = cwist_alloc(sizeof(*p) + len);
    if (!p) return; /* SCTP retransmits */
    p->conn = conn;
    p->len = len;
    memcpy(p->data, buffer, len);
    p->post.cb = sctp_out_post_cb;
    p->post.ctx = p;
    cwist_webrtc_conn_retain(conn);
    cwist_reactor_post(conn->ctx->reactor, &p->post);
}

/* ---- inbound datagrams ---- */

/** @brief Handle a STUN message: Binding request (ICE-lite) or response (client). */
static void handle_stun(cwist_webrtc_ctx *ctx, const uint8_t *buf, size_t len,
                        const struct sockaddr_in *remote) {
    if (cwist_ice_stun_is_binding_request(buf, len)) {
        if (!atomic_load(&ctx->ice_lite_server))
            return; /* no offer answered yet: not an ICE-lite endpoint */
        if (!cwist_ice_stun_validate_request(buf, len, ctx->ice_pwd))
            return; /* bad MESSAGE-INTEGRITY: drop (RFC 8445 would send a 400) */
        uint8_t resp[256];
        int rlen =
            cwist_ice_stun_build_response(resp, sizeof(resp), buf, len, remote, ctx->ice_pwd);
        if (rlen < 0) return;
        ctx_tx_queue(ctx, resp, (size_t)rlen, remote);

        bool nominate = cwist_ice_stun_has_use_candidate(buf, len);
        struct cwist_webrtc_conn *conn = table_find(ctx, remote);
        if (conn) {
            conn->last_rx_ns = ctx->now_ns; /* consent check keeps it alive */
        } else if (nominate) {
            /* Adopt the conn parked by cwist_webrtc_handle_offer() when the
             * USERNAME's remote ufrag matches; otherwise the request is still
             * authenticated with our password, so start a fresh conn. */
            char rfrag[64] = "";
            cwist_ice_stun_get_remote_ufrag(buf, len, rfrag, sizeof(rfrag));
            for (struct cwist_webrtc_conn *c = ctx->parked; c; c = c->hnext) {
                if (rfrag[0] && strcmp(c->peer_ufrag, rfrag) == 0) {
                    conn = c;
                    break;
                }
            }
            if (conn) {
                table_remove(ctx, conn); /* the parked list's reference moves */
            } else {
                conn = conn_new(ctx, NULL, true);
                if (!conn) return;
            }
            conn->remote = *remote;
            conn->last_rx_ns = ctx->now_ns;
            table_insert(ctx, conn);
        }
        if (conn && nominate && conn->state == CWIST_CONN_STUN) conn_start_dtls(conn);
        return;
    }
    /* STUN response: only client-role conns expect these. */
    struct cwist_webrtc_conn *conn = table_find(ctx, remote);
    if (!conn || conn->is_server_role || conn->state != CWIST_CONN_STUN) return;
    struct sockaddr_in mapped;
    if (cwist_ice_stun_parse_response(buf, len, conn->stun_txid, conn->peer_pwd, &mapped)) {
        conn->last_rx_ns = ctx->now_ns;
        conn_start_dtls(conn);
    }
}

/** @brief Demultiplex one datagram (RFC 7983): STUN or DTLS. */
static void handle_packet(cwist_webrtc_ctx *ctx, const uint8_t *buf, size_t len,
                          const struct sockaddr_in *remote) {
    if (len == 0) return;
    if (buf[0] <= 3) {
        if (cwist_ice_stun_is_message(buf, len)) handle_stun(ctx, buf, len, remote);
        return;
    }
    if (buf[0] < 20 || buf[0] > 63) return; /* not DTLS (RTP/RTCP or garbage) */
    struct cwist_webrtc_conn *conn = table_find(ctx, remote);
    if (!conn || !conn->ssl || atomic_load(&conn->detached)) return;
    conn->last_rx_ns = ctx->now_ns;
    conn_dtls_input(conn, buf, len);
}

static void udp_readable(int fd, void *payload);

/** @brief Arm the UDP read slot. */
static bool ctx_arm_udp_slot(cwist_webrtc_ctx *ctx) {
    if (!ctx->owns_reactor) ctx_retain(ctx);
    ctx->udp_armed = cwist_reactor_add(ctx->reactor, ctx->udp_fd, udp_readable, &ctx, sizeof(ctx));
    if (!ctx->udp_armed && !ctx->owns_reactor) ctx_release(ctx);
    return ctx->udp_armed;
}

/** @brief Close the UDP socket and wake a waiting cwist_webrtc_ctx_free(). */
static void ctx_close_fd(cwist_webrtc_ctx *ctx) {
    if (ctx->udp_fd >= 0) {
        close(ctx->udp_fd);
        ctx->udp_fd = -1;
    }
    pthread_mutex_lock(&ctx->free_mu);
    ctx->fd_closed = true;
    pthread_cond_broadcast(&ctx->free_cv);
    pthread_mutex_unlock(&ctx->free_mu);
}

/** @brief Read slot fired: drain the socket in recvmmsg batches. */
static void udp_readable(int fd, void *payload) {
    cwist_webrtc_ctx *ctx = *(cwist_webrtc_ctx **)payload;
    ctx->udp_armed = false;
    if (ctx->closed) {
        /* Woken by teardown: the slot is consumed, so the fd can go. */
        ctx_close_fd(ctx);
        if (!ctx->owns_reactor) ctx_release(ctx);
        return;
    }
    cwist_webrtc_ctx *prev = ctx_enter(ctx);
    int handled = 0;
    while (handled < RX_BUDGET) {
#ifdef __linux__
        struct mmsghdr msgs[CWIST_WEBRTC_RX_BATCH];
        struct iovec iov[CWIST_WEBRTC_RX_BATCH];
        struct sockaddr_in from[CWIST_WEBRTC_RX_BATCH];
        for (int i = 0; i < CWIST_WEBRTC_RX_BATCH; i++) {
            iov[i].iov_base = ctx->rx_bufs + (size_t)i * CWIST_WEBRTC_DGRAM_MAX;
            iov[i].iov_len = CWIST_WEBRTC_DGRAM_MAX;
            memset(&msgs[i].msg_hdr, 0, sizeof(msgs[i].msg_hdr));
            msgs[i].msg_hdr.msg_name = &from[i];
            msgs[i].msg_hdr.msg_namelen = sizeof(from[i]);
            msgs[i].msg_hdr.msg_iov = &iov[i];
            msgs[i].msg_hdr.msg_iovlen = 1;
        }
        int n = recvmmsg(fd, msgs, CWIST_WEBRTC_RX_BATCH, MSG_DONTWAIT, NULL);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        for (int i = 0; i < n; i++) {
            if (msgs[i].msg_hdr.msg_flags & MSG_TRUNC) continue;
            if (msgs[i].msg_hdr.msg_namelen < sizeof(struct sockaddr_in) ||
                from[i].sin_family != AF_INET)
                continue;
            handle_packet(ctx, iov[i].iov_base, msgs[i].msg_len, &from[i]);
        }
        handled += n;
        if (n < CWIST_WEBRTC_RX_BATCH) break;
#else
        struct sockaddr_in from;
        socklen_t flen = sizeof(from);
        ssize_t n = recvfrom(fd, ctx->rx_bufs, CWIST_WEBRTC_DGRAM_MAX, MSG_DONTWAIT,
                             (struct sockaddr *)&from, &flen);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        if (flen >= sizeof(struct sockaddr_in) && from.sin_family == AF_INET)
            handle_packet(ctx, ctx->rx_bufs, (size_t)n, &from);
        handled++;
#endif
    }
    if (handled > 0) ctx_note_activity(ctx);
    ctx_leave(ctx, prev);
    if (!ctx->closed) {
        /* The slot that fired held a ref; the new slot takes its own. */
        ctx_arm_udp_slot(ctx);
    }
    if (!ctx->owns_reactor) ctx_release(ctx);
}

/* ---- host address ---- */

/** @brief Best-guess non-loopback IPv4 address for the SDP host candidate. */
static void detect_host_ip(char *out, size_t cap) {
    snprintf(out, cap, "127.0.0.1");
    struct ifaddrs *ifas = NULL;
    if (getifaddrs(&ifas) < 0) return;
    for (struct ifaddrs *ifa = ifas; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) continue;
        struct sockaddr_in *sin = (struct sockaddr_in *)ifa->ifa_addr;
        uint32_t a = ntohl(sin->sin_addr.s_addr);
        if (((a >> 24) & 0xFF) == 127) continue;
        snprintf(out, cap, "%u.%u.%u.%u", (a >> 24) & 0xFF, (a >> 16) & 0xFF, (a >> 8) & 0xFF,
                 a & 0xFF);
        break;
    }
    freeifaddrs(ifas);
}

/* ---- ctx lifecycle ---- */

/** @brief Free the ctx memory once the last reference is gone. */
static void ctx_release(cwist_webrtc_ctx *ctx) {
    if (atomic_fetch_sub_explicit(&ctx->refs, 1, memory_order_acq_rel) != 1) return;
    ctx_release_resources(ctx);
    pthread_mutex_destroy(&ctx->free_mu);
    pthread_cond_destroy(&ctx->free_cv);
    cwist_free(ctx);
}

/** @brief Release everything but the memory: sockets, TLS state, buffers. */
static void ctx_release_resources(cwist_webrtc_ctx *ctx) {
    if (ctx->server_ssl_ctx) SSL_CTX_free(ctx->server_ssl_ctx);
    if (ctx->client_ssl_ctx) SSL_CTX_free(ctx->client_ssl_ctx);
    if (ctx->cert) X509_free(ctx->cert);
    if (ctx->pkey) EVP_PKEY_free(ctx->pkey);
    ctx->server_ssl_ctx = ctx->client_ssl_ctx = NULL;
    ctx->cert = NULL;
    ctx->pkey = NULL;
    cwist_free(ctx->buckets);
    cwist_free(ctx->rx_bufs);
    cwist_free(ctx->plain_buf);
    cwist_free(ctx->sctp_buf);
    cwist_free(ctx->tx_bufs);
    ctx->buckets = NULL;
    ctx->rx_bufs = ctx->plain_buf = ctx->sctp_buf = ctx->tx_bufs = NULL;
}

/**
 * @brief Close every conn and stop the ctx's timers (owner thread, or any
 *        thread once the reactor no longer runs).  Idempotent.
 */
static void ctx_teardown_owner(cwist_webrtc_ctx *ctx) {
    if (ctx->closed) return;
    cwist_webrtc_ctx *prev = ctx_enter(ctx);
    while (ctx->parked) conn_teardown(ctx->parked);
    for (uint32_t i = 0; i < ctx->nbuckets; i++) {
        while (ctx->buckets[i]) conn_teardown(ctx->buckets[i]);
    }
    cwist_reactor_timer_cancel(ctx->reactor, &ctx->sctp_timer);
    ctx_tx_flush(ctx); /* SCTP ABORTs from the closes */
    ctx->closed = true;
    ctx_leave(ctx, prev); /* dirty conns are detached: only their refs drop */
}

/** @brief Allocate the ctx, bind the socket, create the DTLS identity. */
static cwist_webrtc_ctx *ctx_create(cwist_reactor_t *reactor, bool owns_reactor, uint16_t port) {
    if (!reactor || cwist_sctp_global_init() < 0) return NULL;
    cwist_webrtc_ctx *ctx = cwist_malloc(sizeof(*ctx));
    if (!ctx) return NULL;
    ctx->reactor = reactor;
    ctx->owns_reactor = owns_reactor;
    ctx->owner_pid = getpid();
    atomic_init(&ctx->refs, 1);
    atomic_init(&ctx->ice_lite_server, 0);
    atomic_init(&ctx->ready_count, 0);
    ctx->udp_fd = -1;
    pthread_mutex_init(&ctx->free_mu, NULL);
    pthread_cond_init(&ctx->free_cv, NULL);
    cwist_reactor_timer_init(&ctx->sctp_timer, ctx_sctp_timer_cb, ctx);

    ctx->nbuckets = 64;
    ctx->buckets = cwist_alloc(ctx->nbuckets * sizeof(*ctx->buckets));
    ctx->rx_bufs = cwist_alloc((size_t)CWIST_WEBRTC_RX_BATCH * CWIST_WEBRTC_DGRAM_MAX);
    ctx->tx_bufs = cwist_alloc((size_t)CWIST_WEBRTC_TX_BATCH * CWIST_WEBRTC_DGRAM_MAX);
    ctx->plain_buf = cwist_alloc(CWIST_WEBRTC_MAX_MESSAGE);
    ctx->sctp_buf = cwist_alloc(CWIST_WEBRTC_MAX_MESSAGE);
    if (!ctx->buckets || !ctx->rx_bufs || !ctx->tx_bufs || !ctx->plain_buf || !ctx->sctp_buf)
        goto fail;

    ctx->udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (ctx->udp_fd < 0) goto fail;
    int flags = fcntl(ctx->udp_fd, F_GETFL, 0);
    fcntl(ctx->udp_fd, F_SETFL, flags | O_NONBLOCK);
    int buf = UDP_SOCKBUF;
    setsockopt(ctx->udp_fd, SOL_SOCKET, SO_RCVBUF, &buf, sizeof(buf));
    setsockopt(ctx->udp_fd, SOL_SOCKET, SO_SNDBUF, &buf, sizeof(buf));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (bind(ctx->udp_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) goto fail;
    socklen_t alen = sizeof(addr);
    if (getsockname(ctx->udp_fd, (struct sockaddr *)&addr, &alen) < 0) goto fail;
    ctx->port = ntohs(addr.sin_port);

    if (cwist_dtls_generate_cert(&ctx->cert, &ctx->pkey) < 0) goto fail;
    if (cwist_dtls_fingerprint(ctx->cert, ctx->fingerprint, sizeof(ctx->fingerprint)) < 0)
        goto fail;
    ctx->server_ssl_ctx = cwist_dtls_ctx_new(1, ctx->cert, ctx->pkey);
    ctx->client_ssl_ctx = cwist_dtls_ctx_new(0, NULL, NULL);
    if (!ctx->server_ssl_ctx || !ctx->client_ssl_ctx) goto fail;

    cwist_ice_random_creds(ctx->ice_ufrag, sizeof(ctx->ice_ufrag), ctx->ice_pwd,
                           sizeof(ctx->ice_pwd));
    detect_host_ip(ctx->host_ip, sizeof(ctx->host_ip));
    return ctx;

fail:
    if (ctx->udp_fd >= 0) close(ctx->udp_fd);
    ctx_release(ctx);
    return NULL;
}

/** @brief Run thread of a ctx created by cwist_webrtc_ctx_new(). */
static void *ctx_thread_main(void *arg) {
    cwist_webrtc_ctx *ctx = arg;
    cwist_reactor_run(ctx->reactor);
    return NULL;
}

cwist_webrtc_ctx *cwist_webrtc_ctx_new(uint16_t port) {
    cwist_reactor_t *reactor = cwist_reactor_create();
    if (!reactor) return NULL;
    cwist_webrtc_ctx *ctx = ctx_create(reactor, true, port);
    if (!ctx) {
        cwist_reactor_destroy(reactor);
        return NULL;
    }
    if (!ctx_arm_udp_slot(ctx) || pthread_create(&ctx->thread, NULL, &ctx_thread_main, ctx) != 0) {
        cwist_webrtc_ctx_free(ctx);
        return NULL;
    }
    ctx->thread_running = true;
    return ctx;
}

cwist_webrtc_ctx *cwist_webrtc_ctx_new_on(cwist_reactor_t *reactor, uint16_t port) {
    cwist_webrtc_ctx *ctx = ctx_create(reactor, false, port);
    if (!ctx) return NULL;
    if (!ctx_arm_udp_slot(ctx)) {
        close(ctx->udp_fd);
        ctx->udp_fd = -1;
        ctx_release(ctx);
        return NULL;
    }
    return ctx;
}

/**
 * @brief Teardown on a caller-run reactor (owner thread).  If the read slot
 *        is pending, a datagram to ourselves wakes it and its callback closes
 *        the fd; otherwise the fd is closed here.
 */
static void ctx_teardown_on_reactor(cwist_webrtc_ctx *ctx) {
    ctx_teardown_owner(ctx);
    if (ctx->udp_armed && ctx->udp_fd >= 0) {
        struct sockaddr_in self;
        memset(&self, 0, sizeof(self));
        self.sin_family = AF_INET;
        self.sin_port = htons(ctx->port);
        self.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        uint8_t b = 0;
        if (sendto(ctx->udp_fd, &b, 1, 0, (struct sockaddr *)&self, sizeof(self)) == 1) return;
        /* Could not wake it: cancel the slot and close here. */
        cwist_reactor_del(ctx->reactor, ctx->udp_fd);
        ctx->udp_armed = false;
        ctx_release(ctx); /* the slot's reference */
    }
    ctx_close_fd(ctx);
}

/** @brief Posted teardown for a caller-run reactor. */
static void ctx_free_post_cb(void *arg) {
    cwist_webrtc_ctx *ctx = arg;
    ctx_teardown_on_reactor(ctx);
    ctx_release(ctx); /* the post's reference */
}

/** @brief Posted teardown for an owned reactor: close conns, stop the loop. */
static void ctx_stop_post_cb(void *arg) {
    cwist_webrtc_ctx *ctx = arg;
    ctx_teardown_owner(ctx);
    cwist_reactor_stop(ctx->reactor);
}

void cwist_webrtc_ctx_free(cwist_webrtc_ctx *ctx) {
    if (!ctx) return;
    if (getpid() != ctx->owner_pid) {
        /* A forked child's copy: its reactor thread exists only in the
         * parent, and the sockets and memory are the parent's to release.
         * Leave the copy alone; it goes away with this process. */
        return;
    }
    if (ctx->owns_reactor) {
        if (ctx->thread_running) {
            ctx->free_post.cb = ctx_stop_post_cb;
            ctx->free_post.ctx = ctx;
            cwist_reactor_post(ctx->reactor, &ctx->free_post);
            pthread_join(ctx->thread, NULL);
        }
        /* The loop is gone (or never ran): finish here.  Destroying the
         * reactor drains leftover posts, which only drop references. */
        ctx_teardown_owner(ctx);
        if (ctx->udp_fd >= 0) {
            close(ctx->udp_fd);
            ctx->udp_fd = -1;
        }
        cwist_reactor_destroy(ctx->reactor);
        ctx->reactor = NULL;
        ctx_release(ctx);
        return;
    }
    if (tl_ctx == ctx) {
        /* Called from one of our own callbacks: tear down inline; the read
         * slot's callback finishes the fd close after we return. */
        ctx_teardown_on_reactor(ctx);
    } else {
        ctx_retain(ctx);
        ctx->free_post.cb = ctx_free_post_cb;
        ctx->free_post.ctx = ctx;
        cwist_reactor_post(ctx->reactor, &ctx->free_post);
        pthread_mutex_lock(&ctx->free_mu);
        while (!ctx->fd_closed) pthread_cond_wait(&ctx->free_cv, &ctx->free_mu);
        pthread_mutex_unlock(&ctx->free_mu);
    }
    ctx_release(ctx);
}

/* ---- public accessors ---- */

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

void cwist_webrtc_ctx_set_close_handler(cwist_webrtc_ctx *ctx, cwist_webrtc_close_cb cb,
                                        void *user) {
    ctx->close_cb = cb;
    ctx->close_user = user;
}

int cwist_webrtc_ctx_connection_count(const cwist_webrtc_ctx *ctx) {
    return atomic_load(&ctx->ready_count);
}

size_t cwist_webrtc_conn_buffered_amount(const cwist_webrtc_conn *conn) {
    return atomic_load_explicit(&conn->buffered, memory_order_relaxed);
}

/* ---- offer / connect (any thread) ---- */

/** @brief Owner side of handle_offer: park the conn until its first nomination. */
static void park_post_cb(void *arg) {
    struct cwist_webrtc_conn *conn = arg;
    cwist_webrtc_ctx *ctx = conn->ctx;
    if (ctx->closed) {
        cwist_webrtc_conn_release(conn);
        return;
    }
    cwist_webrtc_ctx *prev = ctx_enter(ctx);
    conn->hnext = ctx->parked;
    ctx->parked = conn;
    conn->registered = true;
    conn->parked = true; /* the post's reference becomes the list's */
    conn_rearm_timer(conn);
    ctx_leave(ctx, prev);
}

int cwist_webrtc_handle_offer(cwist_webrtc_ctx *ctx, const char *offer, char *answer_out,
                              size_t out_len) {
    cwist_sdp_info info;
    if (!ctx || !offer || cwist_sdp_parse(offer, strlen(offer), &info) < 0) return -1;
    if (getpid() != ctx->owner_pid)
        return -1; /* inherited across fork(): this process has no loop for it */
    const char *mid = info.mid[0] ? info.mid : "0";
    if (cwist_sdp_write_answer(answer_out, out_len, ctx->fingerprint, ctx->ice_ufrag, ctx->ice_pwd,
                               mid, ctx->host_ip, ctx->port) < 0)
        return -1;
    struct cwist_webrtc_conn *conn = conn_new(ctx, NULL, true);
    if (!conn) return -1;
    snprintf(conn->peer_ufrag, sizeof(conn->peer_ufrag), "%s", info.ice_ufrag);
    snprintf(conn->peer_pwd, sizeof(conn->peer_pwd), "%s", info.ice_pwd);
    atomic_store(&ctx->ice_lite_server, 1);
    conn->reg_post.cb = park_post_cb;
    conn->reg_post.ctx = conn;
    cwist_reactor_post(ctx->reactor, &conn->reg_post);
    return 0;
}

/** @brief Owner side of connect: register and send the first Binding request. */
static void connect_post_cb(void *arg) {
    struct cwist_webrtc_conn *conn = arg;
    cwist_webrtc_ctx *ctx = conn->ctx;
    if (ctx->closed) {
        cwist_webrtc_conn_release(conn);
        return;
    }
    cwist_webrtc_ctx *prev = ctx_enter(ctx);
    table_insert(ctx, conn); /* the post's reference becomes the table's */
    conn_send_stun_request(conn);
    conn_rearm_timer(conn);
    ctx_leave(ctx, prev);
}

cwist_webrtc_conn *cwist_webrtc_connect(cwist_webrtc_ctx *ctx, const struct sockaddr_in *remote,
                                        const char *peer_ufrag, const char *peer_pwd) {
    struct cwist_webrtc_conn *conn = conn_new(ctx, remote, false);
    if (!conn) return NULL;
    snprintf(conn->peer_ufrag, sizeof(conn->peer_ufrag), "%s", peer_ufrag);
    snprintf(conn->peer_pwd, sizeof(conn->peer_pwd), "%s", peer_pwd);
    RAND_bytes(conn->stun_txid, sizeof(conn->stun_txid));
    cwist_webrtc_conn_retain(conn); /* returned to the caller */
    conn->reg_post.cb = connect_post_cb;
    conn->reg_post.ctx = conn;
    cwist_reactor_post(ctx->reactor, &conn->reg_post);
    return conn;
}

/* ---- send / close (any thread) ---- */

/** @brief Append a message to the pending queue (owner thread). */
static void conn_enqueue(struct cwist_webrtc_conn *conn, cwist_webrtc_msg *m) {
    m->next = NULL;
    if (conn->pending_tail)
        conn->pending_tail->next = m;
    else
        conn->pending_head = m;
    conn->pending_tail = m;
}

/**
 * @brief Owner side of foreign-thread sends: move the whole inbox onto the
 *        pending queue in arrival order, then flush once at ctx_leave().
 *
 * Producers push onto conn->inbox and post the kick only when the inbox was
 * empty, so a burst of sends from another thread costs one wakeup, one
 * service pass and one sendmmsg instead of one each.
 */
static void kick_post_cb(void *arg) {
    struct cwist_webrtc_conn *conn = arg;
    /* acq_rel: producers that see the emptied inbox re-post kick_post, which
     * must happen after the reactor's reads of it for this run. */
    cwist_webrtc_msg *list = atomic_exchange_explicit(&conn->inbox, NULL, memory_order_acq_rel);
    cwist_webrtc_msg *rev = NULL;
    while (list) {
        cwist_webrtc_msg *next = list->next;
        list->next = rev;
        rev = list;
        list = next;
    }
    if (atomic_load(&conn->detached)) {
        while (rev) {
            cwist_webrtc_msg *next = rev->next;
            atomic_fetch_sub_explicit(&conn->buffered, rev->len, memory_order_relaxed);
            cwist_free(rev);
            rev = next;
        }
    } else if (rev) {
        cwist_webrtc_ctx *prev = ctx_enter(conn->ctx);
        ctx_note_activity(conn->ctx);
        while (rev) {
            cwist_webrtc_msg *next = rev->next;
            conn_enqueue(conn, rev);
            rev = next;
        }
        conn_mark_dirty(conn);
        ctx_leave(conn->ctx, prev);
    }
    cwist_webrtc_conn_release(conn); /* the kick's reference */
}

/**
 * @brief Reserve @p len bytes of the conn's send queue.
 * @return true if it fits under CWIST_WEBRTC_MAX_BUFFERED.
 */
static bool conn_reserve(struct cwist_webrtc_conn *conn, size_t len) {
    size_t before = atomic_fetch_add_explicit(&conn->buffered, len, memory_order_relaxed);
    if (before + len > CWIST_WEBRTC_MAX_BUFFERED && before > 0) {
        atomic_fetch_sub_explicit(&conn->buffered, len, memory_order_relaxed);
        return false;
    }
    return true;
}

int cwist_webrtc_conn_send(cwist_webrtc_conn *conn, uint16_t channel_id, const uint8_t *data,
                           size_t len, cwist_webrtc_data_type type) {
    if (!conn || atomic_load(&conn->detached) || len > CWIST_WEBRTC_MAX_MESSAGE ||
        (len > 0 && !data))
        return -1;
    int is_string = type == CWIST_WEBRTC_DATA_STRING;
    if (cwist_webrtc_on_owner(conn->ctx)) {
        ctx_note_activity(conn->ctx);
        if (conn->sctp_ready && !conn->pending_head) {
            int rc = cwist_sctp_send(conn, channel_id, data, len, is_string);
            if (rc == 0) return 0;
            if (rc < 0) return -1;
        }
        if (!conn_reserve(conn, len)) return -1;
        cwist_webrtc_msg *m = cwist_alloc(sizeof(*m) + len);
        if (!m) {
            atomic_fetch_sub_explicit(&conn->buffered, len, memory_order_relaxed);
            return -1;
        }
        m->conn = conn;
        m->len = (uint32_t)len;
        m->channel = channel_id;
        m->is_string = (uint8_t)is_string;
        if (len) memcpy(m->data, data, len);
        conn_enqueue(conn, m);
        conn_mark_dirty(conn);
        return 0;
    }
    if (!conn_reserve(conn, len)) return -1;
    cwist_webrtc_msg *m = cwist_alloc(sizeof(*m) + len);
    if (!m) {
        atomic_fetch_sub_explicit(&conn->buffered, len, memory_order_relaxed);
        return -1;
    }
    m->conn = conn;
    m->len = (uint32_t)len;
    m->channel = channel_id;
    m->is_string = (uint8_t)is_string;
    if (len) memcpy(m->data, data, len);
    cwist_webrtc_msg *head = atomic_load_explicit(&conn->inbox, memory_order_acquire);
    do {
        m->next = head;
    } while (!atomic_compare_exchange_weak_explicit(&conn->inbox, &head, m, memory_order_acq_rel,
                                                    memory_order_acquire));
    if (head == NULL) {
        /* First message of a batch: wake the owner.  The kick node is reused;
         * the reactor reads its link before running it, so re-posting from
         * here while the previous kick runs is safe. */
        cwist_webrtc_conn_retain(conn);
        conn->kick_post.cb = kick_post_cb;
        conn->kick_post.ctx = conn;
        cwist_reactor_post(conn->ctx->reactor, &conn->kick_post);
    }
    return 0;
}

/** @brief Owner side of a foreign-thread close. */
static void close_post_cb(void *arg) {
    struct cwist_webrtc_conn *conn = arg;
    if (!atomic_load(&conn->detached)) {
        cwist_webrtc_ctx *prev = ctx_enter(conn->ctx);
        conn->close_requested = true;
        conn_mark_dirty(conn);
        ctx_leave(conn->ctx, prev);
    }
    atomic_store(&conn->close_posted, false);
    cwist_webrtc_conn_release(conn);
}

void cwist_webrtc_conn_close(cwist_webrtc_conn *conn) {
    if (!conn || atomic_load(&conn->detached)) return;
    if (cwist_webrtc_on_owner(conn->ctx)) {
        /* Deferred to the end of the round: the caller may be inside a
         * message callback that is still reading this conn's SCTP socket. */
        conn->close_requested = true;
        conn_mark_dirty(conn);
        return;
    }
    if (atomic_exchange(&conn->close_posted, true)) return;
    cwist_webrtc_conn_retain(conn);
    conn->close_post.cb = close_post_cb;
    conn->close_post.ctx = conn;
    cwist_reactor_post(conn->ctx->reactor, &conn->close_post);
}
