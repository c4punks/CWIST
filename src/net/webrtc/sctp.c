/** @file sctp.c
 * @brief SCTP DataChannels over DTLS (RFC 8831) and DCEP (RFC 8832).
 *
 * usrsctp runs in AF_CONN raw mode tunneled over the DTLS connection
 * (RFC 8831): the module pumps usrsctp_handle_timers() from the ctx event
 * loop and the global conn_output callback (registered with
 * usrsctp_init_nothreads) hands packets back to the owning connection.
 *
 * The address handle convention follows usrsctp's ekr_loop example: every
 * endpoint registers its connection pointer as its handle, binds with it,
 * and the offering side "connects" to its own handle. Packets are injected
 * with usrsctp_conninput(conn) and replies come back through the same
 * handle, which keeps routing unambiguous when several associations share
 * one usrsctp instance. The answering side listens and accepts; the
 * accepted socket carries no receive callback, so inbound data is drained
 * with usrsctp_recvmsg() from the event loop.
 */
#include "webrtc_internal.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <usrsctp.h>

#define DCEP_ACK 0x02
#define DCEP_OPEN 0x03

#define PPID_STRING 50
#define PPID_BINARY 51
#define PPID_DCEP 53

#define CWIST_SCTP_PORT 5000
#define CHANNEL_BITS(n) ((n) * 2)
#define CH_STATE_BYTES (65536 / 4) /* 2 bits per channel */

static int sctp_global_output(void *addr, void *buffer, size_t length, uint8_t tos,
                              uint8_t set_df) {
    (void)tos;
    (void)set_df;
    struct cwist_webrtc_conn *conn = addr;
    if (!conn || conn->state == CWIST_CONN_DEAD)
        return -1;
    cwist_webrtc_conn_sctp_out(conn, buffer, length);
    return 0;
}

int cwist_sctp_global_init(void) {
    static int initialized = 0;
    if (initialized)
        return 0;
    usrsctp_init_nothreads(0, &sctp_global_output, NULL);
    /* RFC 8831: SCTP runs directly over DTLS, never over UDP tunneling. */
    usrsctp_sysctl_set_sctp_udp_tunneling_port(0);
    initialized = 1;
    return 0;
}

static bool ch_get(struct cwist_webrtc_conn *conn, uint16_t ch, int bit) {
    if (!conn->ch_state)
        return false;
    size_t off = CHANNEL_BITS(ch) + (size_t)bit;
    return (conn->ch_state[off / 8] >> (off % 8)) & 1u;
}

static void ch_set(struct cwist_webrtc_conn *conn, uint16_t ch, int bit) {
    if (!conn->ch_state)
        return;
    size_t off = CHANNEL_BITS(ch) + (size_t)bit;
    conn->ch_state[off / 8] |= (uint8_t)(1u << (off % 8));
}

/* Effective data socket: the accepted socket on the answering side. */
static struct socket *cwist_sctp_data_sock(struct cwist_webrtc_conn *conn) {
    return conn->sctp_acc ? conn->sctp_acc : conn->sctp_sock;
}

static void dispatch_message(struct cwist_webrtc_conn *conn, uint16_t sid, uint32_t ppid,
                             const uint8_t *msg, size_t datalen) {
    if (ppid == PPID_DCEP && datalen >= 1) {
        if (msg[0] == DCEP_OPEN && datalen >= 12) {
            uint16_t label_len = (uint16_t)((uint16_t)msg[8] << 8 | msg[9]);
            char label[201];
            size_t n = label_len;
            if (n > sizeof(label) - 1)
                n = sizeof(label) - 1;
            if (12 + n > datalen)
                n = datalen > 12 ? datalen - 12 : 0;
            memcpy(label, msg + 12, n);
            label[n] = '\0';
            ch_set(conn, sid, 0);
            dcep_send(conn, sid, DCEP_ACK, NULL);
            cwist_webrtc_conn_on_channel_open(conn, sid, label);
        }
        /* ACK (0x02): channel ready both ways. */
    } else if (ppid == PPID_BINARY) {
        cwist_webrtc_conn_on_message(conn, sid, msg, datalen, 0);
    } else if (ppid == PPID_STRING) {
        cwist_webrtc_conn_on_message(conn, sid, msg, datalen, 1);
    }
}

/* receive_cb for the offering (connected) socket. */
static int on_incoming(struct socket *sock, union sctp_sockstore addr, void *data, size_t datalen,
                       struct sctp_rcvinfo rcv, int flags, void *ulp_info) {
    (void)sock;
    (void)addr;
    (void)flags;
    struct cwist_webrtc_conn *conn = ulp_info;
    if (!conn || !data)
        return 1;
    dispatch_message(conn, rcv.rcv_sid, ntohl(rcv.rcv_ppid), (const uint8_t *)data, datalen);
    free(data);
    return 1;
}

int dcep_send(struct cwist_webrtc_conn *conn, uint16_t channel, uint8_t type, const char *label) {
    uint8_t msg[256];
    size_t len = 0;
    msg[len++] = type;
    if (type == DCEP_OPEN) {
        size_t label_len = label ? strlen(label) : 0;
        if (label_len > 200)
            label_len = 200;
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
    } else {
        memset(msg + len, 0, 11); /* ACK is a 12-byte header */
        len += 11;
    }

    struct sctp_sndinfo info;
    memset(&info, 0, sizeof(info));
    info.snd_sid = channel;
    info.snd_ppid = htonl(PPID_DCEP);
    int rc = usrsctp_sendv(cwist_sctp_data_sock(conn), msg, len, NULL, 0, &info, sizeof(info),
                           SCTP_SENDV_SNDINFO, 0);
    return rc >= 0 ? 0 : -1;
}

int cwist_sctp_conn_open(struct cwist_webrtc_conn *conn) {
    conn->sctp_sock = usrsctp_socket(AF_CONN, SOCK_STREAM, IPPROTO_SCTP,
                                     conn->is_server_role ? NULL : &on_incoming, NULL, 0,
                                     conn->is_server_role ? NULL : conn);
    if (!conn->sctp_sock) {
        return -1;
    }
    usrsctp_set_non_blocking(conn->sctp_sock, 1);

    int on = 1;
    usrsctp_setsockopt(conn->sctp_sock, IPPROTO_SCTP, SCTP_RECVRCVINFO, &on, sizeof(on));
    usrsctp_setsockopt(conn->sctp_sock, IPPROTO_SCTP, SCTP_NODELAY, &on, sizeof(on));

    conn->ch_state = calloc(1, CH_STATE_BYTES);
    if (!conn->ch_state) {
        usrsctp_close(conn->sctp_sock);
        conn->sctp_sock = NULL;
        return -1;
    }

    usrsctp_register_address(conn);

    struct sockaddr_conn local;
    memset(&local, 0, sizeof(local));
    local.sconn_family = AF_CONN;
    local.sconn_port = htons(CWIST_SCTP_PORT);
    local.sconn_addr = conn;
    if (usrsctp_bind(conn->sctp_sock, (struct sockaddr *)&local, sizeof(local)) < 0) {
        usrsctp_deregister_address(conn);
        free(conn->ch_state);
        conn->ch_state = NULL;
        usrsctp_close(conn->sctp_sock);
        conn->sctp_sock = NULL;
        return -1;
    }

    if (conn->is_server_role) {
        /* Passive side must accept the incoming SCTP association. */
        if (usrsctp_listen(conn->sctp_sock, 1) < 0) {
            usrsctp_deregister_address(conn);
            free(conn->ch_state);
            conn->ch_state = NULL;
            usrsctp_close(conn->sctp_sock);
            conn->sctp_sock = NULL;
            return -1;
        }
    } else {
        /* Active side initiates the SCTP association (sends the INIT).
         * The remote handle is our own: the answering endpoint's replies
         * are routed back through the handle recorded from its conninput(),
         * i.e. its own pointer (see ekr_loop in the usrsctp tree). */
        struct sockaddr_conn remote;
        memset(&remote, 0, sizeof(remote));
        remote.sconn_family = AF_CONN;
        remote.sconn_port = htons(CWIST_SCTP_PORT);
        remote.sconn_addr = conn;
        if (usrsctp_connect(conn->sctp_sock, (struct sockaddr *)&remote, sizeof(remote)) < 0 &&
            errno != EINPROGRESS) {
            usrsctp_deregister_address(conn);
            free(conn->ch_state);
            conn->ch_state = NULL;
            usrsctp_close(conn->sctp_sock);
            conn->sctp_sock = NULL;
            return -1;
        }
    }
    return 0;
}

void cwist_sctp_conn_close(struct cwist_webrtc_conn *conn) {
    if (conn->sctp_acc) {
        usrsctp_close(conn->sctp_acc);
        conn->sctp_acc = NULL;
    }
    if (conn->sctp_sock) {
        usrsctp_close(conn->sctp_sock);
        conn->sctp_sock = NULL;
    }
    if (conn->ch_state) {
        usrsctp_deregister_address(conn);
        free(conn->ch_state);
        conn->ch_state = NULL;
    }
}

void cwist_sctp_conn_input(struct cwist_webrtc_conn *conn, const uint8_t *data, size_t len) {
    if (conn->sctp_sock)
        usrsctp_conninput(conn, data, len, 0);
}

/* Drain inbound data; accept the association on the passive side. */
void cwist_sctp_conn_drain(struct cwist_webrtc_conn *conn) {
    if (!conn->sctp_sock)
        return;
    if (conn->is_server_role && !conn->sctp_acc) {
        conn->sctp_acc = usrsctp_accept(conn->sctp_sock, NULL, NULL);
        if (conn->sctp_acc) {
            int on = 1;
            usrsctp_set_non_blocking(conn->sctp_acc, 1);
            usrsctp_setsockopt(conn->sctp_acc, IPPROTO_SCTP, SCTP_RECVRCVINFO, &on,
                               sizeof(on));
        }
    }
    struct socket *sock = cwist_sctp_data_sock(conn);
    if (!sock)
        return;
    static uint8_t buf[1 << 20];
    for (;;) {
        struct sctp_rcvinfo rcv;
        socklen_t infolen = sizeof(rcv);
        unsigned int infotype = SCTP_RECVV_RCVINFO;
        int msg_flags = 0;
        struct sockaddr_conn from;
        socklen_t fromlen = sizeof(from);
        ssize_t n = usrsctp_recvv(sock, buf, sizeof(buf), (struct sockaddr *)&from, &fromlen,
                                  &rcv, &infolen, &infotype, &msg_flags);
        (void)infotype;
        if (n <= 0)
            break;
        if (msg_flags & MSG_NOTIFICATION)
            continue;
        if ((int)infolen < (int)sizeof(rcv))
            continue;
        dispatch_message(conn, rcv.rcv_sid, ntohl(rcv.rcv_ppid), buf, (size_t)n);
    }
}

bool cwist_sctp_assoc_established(struct cwist_webrtc_conn *conn) {
    if (!conn->sctp_sock)
        return false;
    if (conn->is_server_role)
        return conn->sctp_acc != NULL;
    struct sctp_status status;
    socklen_t slen = sizeof(status);
    memset(&status, 0, sizeof(status));
    if (usrsctp_getsockopt(conn->sctp_sock, IPPROTO_SCTP, SCTP_STATUS, &status, &slen) < 0)
        return false;
    return status.sstat_state == SCTP_ESTABLISHED;
}

int cwist_sctp_send(struct cwist_webrtc_conn *conn, uint16_t channel, const uint8_t *data,
                    size_t len, int is_string) {
    if (!cwist_sctp_data_sock(conn) || !conn->sctp_ready)
        return -1;
    if (!ch_get(conn, channel, 0) && !ch_get(conn, channel, 1)) {
        /* Brand-new channel opened by us: announce it with DCEP OPEN. */
        char label[32];
        snprintf(label, sizeof(label), "dc%u", channel);
        if (dcep_send(conn, channel, DCEP_OPEN, label) < 0)
            return -1;
        ch_set(conn, channel, 1);
    }
    struct sctp_sndinfo info;
    memset(&info, 0, sizeof(info));
    info.snd_sid = channel;
    info.snd_ppid = htonl(is_string ? PPID_STRING : PPID_BINARY);
    int rc = usrsctp_sendv(cwist_sctp_data_sock(conn), data, len, NULL, 0, &info, sizeof(info),
                           SCTP_SENDV_SNDINFO, 0);
    return rc >= 0 ? 0 : -1;
}
