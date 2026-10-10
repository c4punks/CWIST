/** @file sctp.c
 * @brief SCTP DataChannels over DTLS (RFC 8831) and DCEP (RFC 8832).
 *
 * usrsctp runs in AF_CONN raw mode tunneled over the DTLS connection
 * (RFC 8831), initialised without its own threads.  Its global output
 * callback hands packets to cwist_webrtc_conn_sctp_out(), which encrypts them
 * on the conn's owner thread.
 *
 * The address handle convention follows usrsctp's ekr_loop example: every
 * endpoint registers its connection pointer as its handle, binds with it,
 * and the offering side "connects" to its own handle.  Packets are injected
 * with usrsctp_conninput(conn) and replies come back through the same
 * handle, which keeps routing unambiguous when several associations share
 * one usrsctp instance.  The answering side listens and accepts.  Inbound
 * data is drained with usrsctp_recvv() by the event loop; no receive
 * callbacks are registered, so usrsctp never calls into us with data on an
 * arbitrary thread.
 *
 * Timers: usrsctp keeps one process-wide timer wheel.  cwist_sctp_tick()
 * advances it by the real time elapsed since the previous tick, whichever
 * ctx calls it, so several ctxs ticking concurrently cannot make SCTP time
 * run fast.  A timer that fires may emit a packet for a conn owned by another
 * reactor; the output path marshals it there.  Teardown takes the same lock
 * as the tick, so a timer pass never runs against a half-closed conn.
 */
#include "webrtc_internal.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <usrsctp.h>

#include <cwist/core/mem/alloc.h>

/** @name DCEP message types (RFC 8832) */
/**@{*/
#define DCEP_ACK 0x02  /**< DCEP ACK: acknowledges a received OPEN. */
#define DCEP_OPEN 0x03 /**< DCEP OPEN: announces a new DataChannel. */
/**@}*/

/** @name SCTP payload protocol identifiers (RFC 8831 section 8) */
/**@{*/
#define PPID_DCEP 50         /**< DCEP control message (OPEN/ACK). */
#define PPID_STRING 51       /**< UTF-8 string message. */
#define PPID_BINARY 53       /**< Binary message. */
#define PPID_STRING_EMPTY 56 /**< Empty string (sent as one 0x00 byte). */
#define PPID_BINARY_EMPTY 57 /**< Empty binary message (sent as one 0x00 byte). */
/**@}*/

/** WebRTC DataChannel well-known SCTP port (RFC 8831, both endpoints use it). */
#define CWIST_SCTP_PORT 5000

/** Streams negotiated in each direction (libwebrtc uses the same count). */
#define CWIST_SCTP_STREAMS 1024

/** SCTP packet size.  DTLS 1.2 with AES-GCM adds 37 bytes (13 header, 8
 *  explicit nonce, 16 tag), so the datagram stays under the 1200-byte DTLS
 *  link MTU with room to spare. */
#define CWIST_SCTP_MTU 1160

/** SCTP heartbeat interval (ms). */
#define CWIST_SCTP_HEARTBEAT_MS 10000

/** Socket buffer for each association: holds several max-size messages. */
#define CWIST_SCTP_SOCKBUF (1 << 20)

/** Serialises usrsctp_handle_timers() and the tick clock, and keeps conn
 *  teardown out of a timer pass (see the file comment). */
static pthread_mutex_t g_sctp_lock = PTHREAD_MUTEX_INITIALIZER;
/** Monotonic time (ms) up to which usrsctp's clock has been advanced. */
static uint64_t g_sctp_clock_ms;
/** Guards the one-time usrsctp initialisation. */
static pthread_once_t g_sctp_once = PTHREAD_ONCE_INIT;

/** @brief CLOCK_MONOTONIC in milliseconds. */
static uint64_t sctp_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}

/**
 * @brief usrsctp's global output callback.
 * @param addr The conn registered as the AF_CONN address.
 * @return 0 when handed off, -1 if there is no conn.
 */
static int sctp_global_output(void *addr, void *buffer, size_t length, uint8_t tos,
                              uint8_t set_df) {
    (void)tos;
    (void)set_df;
    struct cwist_webrtc_conn *conn = addr;
    if (!conn) return -1;
    cwist_webrtc_conn_sctp_out(conn, buffer, length);
    return 0;
}

/** @brief Initialise usrsctp once: no threads, no UDP encapsulation. */
static void sctp_init_once(void) {
    usrsctp_init_nothreads(0, &sctp_global_output, NULL);
    /* RFC 8831: SCTP runs directly over DTLS, never over UDP tunneling. */
    usrsctp_sysctl_set_sctp_udp_tunneling_port(0);
    g_sctp_clock_ms = sctp_now_ms();
}

int cwist_sctp_global_init(void) {
    pthread_once(&g_sctp_once, sctp_init_once);
    return 0;
}

void cwist_sctp_tick(void) {
    pthread_mutex_lock(&g_sctp_lock);
    uint64_t now = sctp_now_ms();
    if (now > g_sctp_clock_ms) {
        uint64_t elapsed = now - g_sctp_clock_ms;
        g_sctp_clock_ms = now;
        usrsctp_handle_timers((uint32_t)(elapsed > UINT32_MAX ? UINT32_MAX : elapsed));
    }
    pthread_mutex_unlock(&g_sctp_lock);
}

/* ---- per-stream state: 2 bits per stream id ---- */

/** @brief Read flag @p bit (0: opened by peer, 1: OPEN sent) of stream @p ch. */
static bool ch_get(const struct cwist_webrtc_conn *conn, uint16_t ch, int bit) {
    size_t off = (size_t)ch * 2 + (size_t)bit;
    if (off / 8 >= conn->ch_bits_len) return false;
    return (conn->ch_bits[off / 8] >> (off % 8)) & 1u;
}

/**
 * @brief Set flag @p bit of stream @p ch, growing the bitmap on demand (most
 *        conns use a handful of low stream ids).
 * @return 0, or -1 on allocation failure.
 */
static int ch_set(struct cwist_webrtc_conn *conn, uint16_t ch, int bit) {
    size_t off = (size_t)ch * 2 + (size_t)bit;
    if (off / 8 >= conn->ch_bits_len) {
        uint32_t len = conn->ch_bits_len ? conn->ch_bits_len : 16;
        while (off / 8 >= len) len *= 2;
        uint8_t *bits = cwist_alloc(len);
        if (!bits) return -1;
        if (conn->ch_bits_len) memcpy(bits, conn->ch_bits, conn->ch_bits_len);
        cwist_free(conn->ch_bits);
        conn->ch_bits = bits;
        conn->ch_bits_len = len;
    }
    conn->ch_bits[off / 8] |= (uint8_t)(1u << (off % 8));
    return 0;
}

/** @brief The socket carrying the association (accepted socket on the server). */
static struct socket *sctp_data_sock(const struct cwist_webrtc_conn *conn) {
    return conn->sctp_acc ? conn->sctp_acc : conn->is_server_role ? NULL : conn->sctp_sock;
}

/**
 * @brief Send a DCEP control message (OPEN with @p label, or ACK).
 * @return 0 when sent, 1 when the send buffer is full, -1 on error.
 */
static int dcep_send(struct cwist_webrtc_conn *conn, uint16_t channel, uint8_t type,
                     const char *label) {
    uint8_t msg[256];
    size_t len = 0;
    msg[len++] = type;
    if (type == DCEP_OPEN) {
        size_t label_len = label ? strlen(label) : 0;
        if (label_len > 200) label_len = 200;
        msg[len++] = 0x00; /* DATA_CHANNEL_RELIABLE */
        msg[len++] = 0x00;
        msg[len++] = 0x00; /* priority */
        msg[len++] = 0x00;
        msg[len++] = 0x00;
        msg[len++] = 0x00; /* reliability parameter */
        msg[len++] = 0x00;
        msg[len++] = (uint8_t)(label_len >> 8);
        msg[len++] = (uint8_t)label_len;
        msg[len++] = 0x00;
        msg[len++] = 0x00; /* protocol length 0 */
        memcpy(msg + len, label, label_len);
        len += label_len;
    }

    struct sctp_sndinfo info;
    memset(&info, 0, sizeof(info));
    info.snd_sid = channel;
    info.snd_ppid = htonl(PPID_DCEP);
    ssize_t rc = usrsctp_sendv(sctp_data_sock(conn), msg, len, NULL, 0, &info, sizeof(info),
                               SCTP_SENDV_SNDINFO, 0);
    if (rc >= 0) return 0;
    return (errno == EWOULDBLOCK || errno == EAGAIN) ? 1 : -1;
}

/** @brief Route one complete inbound message: DCEP control or user data. */
static void dispatch_message(struct cwist_webrtc_conn *conn, uint16_t sid, uint32_t ppid,
                             const uint8_t *msg, size_t datalen) {
    if (ppid == PPID_DCEP && datalen >= 1) {
        if (msg[0] == DCEP_OPEN && datalen >= 12) {
            uint16_t label_len = (uint16_t)((uint16_t)msg[8] << 8 | msg[9]);
            char label[201];
            size_t n = label_len;
            if (n > sizeof(label) - 1) n = sizeof(label) - 1;
            if (12 + n > datalen) n = datalen - 12;
            memcpy(label, msg + 12, n);
            label[n] = '\0';
            ch_set(conn, sid, 0);
            dcep_send(conn, sid, DCEP_ACK, NULL);
            cwist_webrtc_conn_on_channel_open(conn, sid, label);
        }
        /* ACK (0x02): the channel we opened is confirmed; nothing to do. */
    } else if (ppid == PPID_BINARY) {
        cwist_webrtc_conn_on_message(conn, sid, msg, datalen, 0);
    } else if (ppid == PPID_STRING) {
        cwist_webrtc_conn_on_message(conn, sid, msg, datalen, 1);
    } else if (ppid == PPID_BINARY_EMPTY || ppid == PPID_STRING_EMPTY) {
        /* SCTP cannot carry a zero-length message: the 0x00 is padding. */
        cwist_webrtc_conn_on_message(conn, sid, msg, 0, ppid == PPID_STRING_EMPTY);
    }
}

/** @brief Socket options shared by the listening, connecting and accepted sockets. */
static void sctp_apply_sockopts(struct socket *sock) {
    int on = 1;
    usrsctp_set_non_blocking(sock, 1);
    usrsctp_setsockopt(sock, IPPROTO_SCTP, SCTP_RECVRCVINFO, &on, sizeof(on));
    usrsctp_setsockopt(sock, IPPROTO_SCTP, SCTP_NODELAY, &on, sizeof(on));

    /* close() aborts instead of lingering in SHUTDOWN: no timer can fire for
     * the association after its conn is gone. */
    struct linger lg = {.l_onoff = 1, .l_linger = 0};
    usrsctp_setsockopt(sock, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg));

    int buf = CWIST_SCTP_SOCKBUF;
    usrsctp_setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &buf, sizeof(buf));
    usrsctp_setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &buf, sizeof(buf));

    struct sctp_initmsg init;
    memset(&init, 0, sizeof(init));
    init.sinit_num_ostreams = CWIST_SCTP_STREAMS;
    init.sinit_max_instreams = CWIST_SCTP_STREAMS;
    usrsctp_setsockopt(sock, IPPROTO_SCTP, SCTP_INITMSG, &init, sizeof(init));

    /* Fixed path MTU: the DTLS link MTU is known and PMTU probes would only
     * get lost inside the tunnel. */
    struct sctp_paddrparams pp;
    memset(&pp, 0, sizeof(pp));
    pp.spp_assoc_id = SCTP_FUTURE_ASSOC;
    pp.spp_flags = SPP_PMTUD_DISABLE | SPP_HB_ENABLE;
    pp.spp_pathmtu = CWIST_SCTP_MTU;
    /* Heartbeats keep both directions talking well inside the 30 s liveness
     * window even when the peer sends no ICE consent checks. */
    pp.spp_hbinterval = CWIST_SCTP_HEARTBEAT_MS;
    usrsctp_setsockopt(sock, IPPROTO_SCTP, SCTP_PEER_ADDR_PARAMS, &pp, sizeof(pp));

    /* Association loss (abort, shutdown) arrives as a notification so the
     * conn is closed right away instead of waiting for the liveness timer. */
    struct sctp_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.se_assoc_id = SCTP_ALL_ASSOC;
    ev.se_on = 1;
    ev.se_type = SCTP_ASSOC_CHANGE;
    usrsctp_setsockopt(sock, IPPROTO_SCTP, SCTP_EVENT, &ev, sizeof(ev));
}

int cwist_sctp_conn_open(struct cwist_webrtc_conn *conn) {
    cwist_sctp_global_init();
    conn->sctp_sock = usrsctp_socket(AF_CONN, SOCK_STREAM, IPPROTO_SCTP, NULL, NULL, 0, NULL);
    if (!conn->sctp_sock) return -1;
    sctp_apply_sockopts(conn->sctp_sock);
    usrsctp_register_address(conn);

    struct sockaddr_conn local;
    memset(&local, 0, sizeof(local));
    local.sconn_family = AF_CONN;
    local.sconn_port = htons(CWIST_SCTP_PORT);
    local.sconn_addr = conn;
    if (usrsctp_bind(conn->sctp_sock, (struct sockaddr *)&local, sizeof(local)) < 0) goto fail;

    if (conn->is_server_role) {
        /* Passive side must accept the incoming SCTP association. */
        if (usrsctp_listen(conn->sctp_sock, 1) < 0) goto fail;
    } else {
        /* Active side initiates the SCTP association (sends the INIT). */
        struct sockaddr_conn remote = local;
        if (usrsctp_connect(conn->sctp_sock, (struct sockaddr *)&remote, sizeof(remote)) < 0 &&
            errno != EINPROGRESS)
            goto fail;
    }
    return 0;

fail:
    cwist_sctp_conn_close(conn);
    return -1;
}

void cwist_sctp_conn_close(struct cwist_webrtc_conn *conn) {
    if (!conn->sctp_sock && !conn->sctp_acc) return;
    pthread_mutex_lock(&g_sctp_lock);
    if (conn->sctp_acc) {
        usrsctp_close(conn->sctp_acc);
        conn->sctp_acc = NULL;
    }
    if (conn->sctp_sock) {
        usrsctp_close(conn->sctp_sock);
        conn->sctp_sock = NULL;
    }
    usrsctp_deregister_address(conn);
    pthread_mutex_unlock(&g_sctp_lock);
    cwist_free(conn->ch_bits);
    conn->ch_bits = NULL;
    conn->ch_bits_len = 0;
    cwist_free(conn->rx_partial);
    conn->rx_partial = NULL;
    conn->rx_partial_len = conn->rx_partial_cap = 0;
}

void cwist_sctp_conn_input(struct cwist_webrtc_conn *conn, const uint8_t *data, size_t len) {
    if (conn->sctp_sock || conn->sctp_acc) usrsctp_conninput(conn, data, len, 0);
}

/**
 * @brief Append a piece of a partially delivered message.
 * @return 0, or -1 if the message would exceed the advertised maximum.
 */
static int rx_partial_append(struct cwist_webrtc_conn *conn, const uint8_t *data, size_t len) {
    size_t need = conn->rx_partial_len + len;
    if (need > CWIST_WEBRTC_MAX_MESSAGE) return -1;
    if (need > conn->rx_partial_cap) {
        size_t cap = conn->rx_partial_cap ? conn->rx_partial_cap : 65536;
        while (cap < need) cap *= 2;
        uint8_t *buf = cwist_alloc(cap);
        if (!buf) return -1;
        if (conn->rx_partial_len) memcpy(buf, conn->rx_partial, conn->rx_partial_len);
        cwist_free(conn->rx_partial);
        conn->rx_partial = buf;
        conn->rx_partial_cap = cap;
    }
    memcpy(conn->rx_partial + conn->rx_partial_len, data, len);
    conn->rx_partial_len = need;
    return 0;
}

int cwist_sctp_conn_drain(struct cwist_webrtc_conn *conn) {
    if (!conn->sctp_sock && !conn->sctp_acc) return 0;
    if (conn->is_server_role && !conn->sctp_acc) {
        conn->sctp_acc = usrsctp_accept(conn->sctp_sock, NULL, NULL);
        if (!conn->sctp_acc) return 0;
        sctp_apply_sockopts(conn->sctp_acc);
        /* One association per conn: the listener has done its job. */
        pthread_mutex_lock(&g_sctp_lock);
        usrsctp_close(conn->sctp_sock);
        conn->sctp_sock = NULL;
        pthread_mutex_unlock(&g_sctp_lock);
    }
    struct socket *sock = sctp_data_sock(conn);
    if (!sock) return 0;
    uint8_t *buf = conn->ctx->sctp_buf;
    for (;;) {
        struct sctp_rcvinfo rcv;
        socklen_t infolen = sizeof(rcv);
        unsigned int infotype = 0;
        int msg_flags = 0;
        struct sockaddr_conn from;
        socklen_t fromlen = sizeof(from);
        ssize_t n = usrsctp_recvv(sock, buf, CWIST_WEBRTC_MAX_MESSAGE, (struct sockaddr *)&from,
                                  &fromlen, &rcv, &infolen, &infotype, &msg_flags);
        if (n < 0) return (errno == EWOULDBLOCK || errno == EAGAIN) ? 0 : -1;
        if (n == 0) return -1; /* peer shut the association down */
        if (msg_flags & MSG_NOTIFICATION) {
            const union sctp_notification *note = (const union sctp_notification *)buf;
            if ((size_t)n >= sizeof(note->sn_header) &&
                note->sn_header.sn_type == SCTP_ASSOC_CHANGE &&
                (size_t)n >= sizeof(note->sn_assoc_change)) {
                uint16_t state = note->sn_assoc_change.sac_state;
                if (state == SCTP_COMM_LOST || state == SCTP_SHUTDOWN_COMP ||
                    state == SCTP_CANT_STR_ASSOC)
                    return -1;
            }
            continue;
        }
        if (infotype != SCTP_RECVV_RCVINFO || infolen < (socklen_t)sizeof(rcv)) continue;
        if (!(msg_flags & MSG_EOR)) {
            /* Partial delivery: keep the piece until the end of the message. */
            if (rx_partial_append(conn, buf, (size_t)n) < 0) return -1;
            continue;
        }
        if (conn->rx_partial_len) {
            if (rx_partial_append(conn, buf, (size_t)n) < 0) return -1;
            size_t total = conn->rx_partial_len;
            conn->rx_partial_len = 0;
            dispatch_message(conn, rcv.rcv_sid, ntohl(rcv.rcv_ppid), conn->rx_partial, total);
        } else {
            dispatch_message(conn, rcv.rcv_sid, ntohl(rcv.rcv_ppid), buf, (size_t)n);
        }
        if (conn->close_requested || atomic_load(&conn->detached)) return 0;
    }
}

bool cwist_sctp_assoc_established(struct cwist_webrtc_conn *conn) {
    if (conn->is_server_role) return conn->sctp_acc != NULL;
    if (!conn->sctp_sock) return false;
    struct sctp_status status;
    socklen_t slen = sizeof(status);
    memset(&status, 0, sizeof(status));
    if (usrsctp_getsockopt(conn->sctp_sock, IPPROTO_SCTP, SCTP_STATUS, &status, &slen) < 0)
        return false;
    return status.sstat_state == SCTP_ESTABLISHED;
}

int cwist_sctp_send(struct cwist_webrtc_conn *conn, uint16_t channel, const uint8_t *data,
                    size_t len, int is_string) {
    struct socket *sock = sctp_data_sock(conn);
    if (!sock || !conn->sctp_ready || channel >= CWIST_SCTP_STREAMS) return -1;
    if (!ch_get(conn, channel, 0) && !ch_get(conn, channel, 1)) {
        /* Brand-new channel opened by us: announce it with DCEP OPEN. */
        char label[32];
        snprintf(label, sizeof(label), "dc%u", channel);
        int rc = dcep_send(conn, channel, DCEP_OPEN, label);
        if (rc != 0) return rc;
        if (ch_set(conn, channel, 1) < 0) return -1;
    }
    struct sctp_sndinfo info;
    memset(&info, 0, sizeof(info));
    info.snd_sid = channel;
    static const uint8_t empty_pad = 0;
    if (len == 0) {
        /* RFC 8831 section 6.6: an empty message is one byte with an
         * "empty" PPID. */
        info.snd_ppid = htonl(is_string ? PPID_STRING_EMPTY : PPID_BINARY_EMPTY);
        data = &empty_pad;
        len = 1;
    } else {
        info.snd_ppid = htonl(is_string ? PPID_STRING : PPID_BINARY);
    }
    ssize_t rc =
        usrsctp_sendv(sock, data, len, NULL, 0, &info, sizeof(info), SCTP_SENDV_SNDINFO, 0);
    if (rc >= 0) return 0;
    return (errno == EWOULDBLOCK || errno == EAGAIN) ? 1 : -1;
}
