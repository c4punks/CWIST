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

#include <cwist/core/mem/alloc.h>

/** @name DCEP message types (RFC 8832) */
/**@{*/
#define DCEP_ACK 0x02  /**< DCEP ACK: acknowledges a received OPEN. */
#define DCEP_OPEN 0x03 /**< DCEP OPEN: announces a new DataChannel. */
/**@}*/

/** @name SCTP payload protocol identifiers (RFC 8831) */
/**@{*/
#define PPID_STRING 50 /**< DataChannel message in UTF-8 string form. */
#define PPID_BINARY 51 /**< DataChannel message in binary form. */
#define PPID_DCEP 53   /**< DCEP control message (OPEN/ACK). */
/**@}*/

/** WebRTC DataChannel well-known SCTP port (RFC 8831, both endpoints use it). */
#define CWIST_SCTP_PORT 5000

/** @name Per-connection channel state bitmap
 *
 * conn->ch_state tracks two flags per DataChannel id in a packed bit array:
 * bit 0 = channel announced by the peer (DCEP OPEN received), bit 1 = we
 * sent DCEP OPEN for the channel. Any bit set means the channel is usable.
 */
/**@{*/
#define CHANNEL_BITS(n) ((n) * 2)                 /**< Bit offset of channel (n)'s flags. */
#define CH_STATE_BYTES (65536 / 4) /* 2 bits per channel */
/**@}*/

/**
 * @brief Global usrsctp conn_output callback (AF_CONN raw mode).
 *
 * Hands a usrsctp-generated SCTP packet to the owning connection for
 * transmission over its DTLS link. Registered once via
 * usrsctp_init_nothreads() in cwist_sctp_global_init(). The @p addr handle is
 * the cwist_webrtc_conn pointer registered with usrsctp_register_address().
 *
 * @param addr     Connection handle (cwist_webrtc_conn *); may be NULL.
 * @param buffer   SCTP packet to transmit; ownership stays with usrsctp.
 * @param length   Length of @p buffer in bytes.
 * @param tos      Ignored (AF_CONN).
 * @param set_df   Ignored (AF_CONN).
 * @return 0 on success, -1 if the connection is gone (packet is dropped).
 * @warning Called from usrsctp context; must not block or free @p buffer.
 */
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

/**
 * @brief Initialize the process-wide usrsctp instance. Idempotent.
 *
 * Registers the global conn_output callback (sctp_global_output) and disables
 * UDP tunneling as required by RFC 8831. usrsctp must be pumped with
 * usrsctp_handle_timers() from the caller's event loop afterwards.
 *
 * @return Always 0; later calls are no-ops.
 */
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

/**
 * @brief Read a per-channel flag bit from conn->ch_state.
 *
 * @param conn  Connection whose channel bitmap is read.
 * @param ch    DataChannel id (0..65535).
 * @param bit   Flag index within the channel's two bits (0 or 1).
 * @return The flag value, or false if no bitmap is allocated.
 */
static bool ch_get(struct cwist_webrtc_conn *conn, uint16_t ch, int bit) {
    if (!conn->ch_state)
        return false;
    size_t off = CHANNEL_BITS(ch) + (size_t)bit;
    return (conn->ch_state[off / 8] >> (off % 8)) & 1u;
}

/**
 * @brief Set a per-channel flag bit in conn->ch_state.
 *
 * @param conn  Connection whose channel bitmap is updated.
 * @param ch    DataChannel id (0..65535).
 * @param bit   Flag index within the channel's two bits (0 or 1).
 */
static void ch_set(struct cwist_webrtc_conn *conn, uint16_t ch, int bit) {
    if (!conn->ch_state)
        return;
    size_t off = CHANNEL_BITS(ch) + (size_t)bit;
    conn->ch_state[off / 8] |= (uint8_t)(1u << (off % 8));
}

/**
 * @brief Resolve the socket that carries DataChannel traffic.
 *
 * @param conn  Connection to query.
 * @return The accepted socket on the answering side, otherwise the listening
 *         (offering) socket; may be NULL before cwist_sctp_conn_open().
 */
static struct socket *cwist_sctp_data_sock(struct cwist_webrtc_conn *conn) {
    return conn->sctp_acc ? conn->sctp_acc : conn->sctp_sock;
}

/**
 * @brief Route one received SCTP message to the right handler.
 *
 * DCEP OPEN messages are parsed for their label (truncated to 200 bytes),
 * the channel is marked as peer-opened, a DCEP ACK is sent back, and
 * cwist_webrtc_conn_on_channel_open() is fired. A received DCEP ACK only
 * confirms the channel is ready both ways and is otherwise ignored. Binary
 * and string messages are passed to cwist_webrtc_conn_on_message().
 *
 * @param conn    Owning connection; used for callbacks and channel flags.
 * @param sid     SCTP stream id == DataChannel id.
 * @param ppid    Payload protocol identifier in host byte order (PPID_*).
 * @param msg     Message payload; not NUL-terminated.
 * @param datalen Length of @p msg in bytes.
 */
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

/**
 * @brief usrsctp receive callback for the offering (connected) socket.
 *
 * Fires inside usrsctp whenever data arrives on the active side's socket;
 * hands the message to dispatch_message() and frees the usrsctp-allocated
 * buffer. The answering side instead drains with usrsctp_recvv() in
 * cwist_sctp_conn_drain(), so this callback is not installed there.
 *
 * @param sock     Ignored.
 * @param addr     Ignored.
 * @param data     usrsctp-allocated message buffer; freed here.
 * @param datalen  Length of @p data.
 * @param rcv      Receive info carrying the stream id and PPID.
 * @param flags    Ignored.
 * @param ulp_info The cwist_webrtc_conn registered at socket creation.
 * @return Always 1, telling usrsctp the data was consumed.
 */
static int on_incoming(struct socket *sock, union sctp_sockstore addr, void *data, size_t datalen,
                       struct sctp_rcvinfo rcv, int flags, void *ulp_info) {
    (void)sock;
    (void)addr;
    (void)flags;
    struct cwist_webrtc_conn *conn = ulp_info;
    if (!conn || !data)
        return 1;
    dispatch_message(conn, rcv.rcv_sid, ntohl(rcv.rcv_ppid), (const uint8_t *)data, datalen);
    cwist_free(data);
    return 1;
}

/**
 * @brief Send a DCEP control message (OPEN or ACK) on a DataChannel.
 *
 * Builds the message in a stack buffer (max 12-byte header + 200-byte label)
 * and sends it with PPID_DCEP on stream @p channel via the effective data
 * socket. OPEN carries a RELIABLE channel type and the given label;
 * ACK is a bare 12-byte header with no label.
 *
 * @param conn    Owning connection.
 * @param channel DataChannel id == SCTP stream id to send on.
 * @param type    DCEP message type (DCEP_OPEN or DCEP_ACK).
 * @param label   Channel label for OPEN; ignored (may be NULL) for ACK.
 * @return 0 if the message was handed to usrsctp, -1 on send failure.
 */
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

/**
 * @brief Create the per-connection SCTP socket and start (or listen for) the
 * association.
 *
 * Creates a non-blocking AF_CONN socket with SCTP_RECVRCVINFO and SCTP_NODELAY
 * enabled, allocates the channel state bitmap, registers the connection as
 * the AF_CONN address handle, and binds to CWIST_SCTP_PORT. The offering
 * side additionally connect()s to its own handle to send the INIT (EINPROGRESS
 * is treated as in-flight); the answering side listen()s for the association.
 * On any failure all partially created state is torn down.
 *
 * @param conn  Connection to set up; sctp_sock/ch_state are filled in.
 * @return 0 on success, -1 if the socket, bitmap, bind, or listen/connect
 *         step fails.
 * @warning The DTLS link must already be up; the active side's connect only
 *          starts the SCTP handshake, whose packets travel via the
 *          conn_output callback into cwist_webrtc_conn_sctp_out().
 */
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

    conn->ch_state = cwist_malloc(CH_STATE_BYTES);
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
        cwist_free(conn->ch_state);
        conn->ch_state = NULL;
        usrsctp_close(conn->sctp_sock);
        conn->sctp_sock = NULL;
        return -1;
    }

    if (conn->is_server_role) {
        /* Passive side must accept the incoming SCTP association. */
        if (usrsctp_listen(conn->sctp_sock, 1) < 0) {
            usrsctp_deregister_address(conn);
            cwist_free(conn->ch_state);
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
            cwist_free(conn->ch_state);
            conn->ch_state = NULL;
            usrsctp_close(conn->sctp_sock);
            conn->sctp_sock = NULL;
            return -1;
        }
    }
    return 0;
}

/**
 * @brief Tear down all per-connection SCTP state.
 *
 * Closes the accepted socket (if any) and the main socket, then deregisters
 * the AF_CONN handle and frees the channel state bitmap. Safe to call on a
 * connection that was never opened or already closed.
 *
 * @param conn  Connection whose SCTP state is destroyed.
 */
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
        cwist_free(conn->ch_state);
        conn->ch_state = NULL;
    }
}

/**
 * @brief Feed a DTLS-decrypted SCTP packet into usrsctp.
 *
 * Injects the packet into the connection's association; any immediate
 * replies are routed back through sctp_global_output(). A no-op if the
 * socket is not open yet.
 *
 * @param conn  Owning connection (registered AF_CONN handle).
 * @param data  SCTP packet bytes as received over DTLS.
 * @param len   Length of @p data.
 */
void cwist_sctp_conn_input(struct cwist_webrtc_conn *conn, const uint8_t *data, size_t len) {
    if (conn->sctp_sock)
        usrsctp_conninput(conn, data, len, 0);
}

/**
 * @brief Accept the association (answering side) and drain inbound messages.
 *
 * On the passive side, accepts the pending association once and enables
 * non-blocking mode plus SCTP_RECVRCVINFO on the accepted socket. Then reads
 * all queued messages from the effective data socket into a static 1 MiB
 * buffer, skipping notifications and datagrams without complete rcvinfo, and
 * dispatches each message. Runs from the event loop; never blocks.
 *
 * @param conn  Connection to service.
 * @note The receive buffer is a shared static, so this must not be called
 *       concurrently from multiple threads.
 */
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

/**
 * @brief Check whether the SCTP association is fully established.
 *
 * On the answering side the association is up once the accepted socket
 * exists; on the offering side the SCTP_STATUS socket option must report
 * SCTP_ESTABLISHED.
 *
 * @param conn  Connection to query.
 * @return true if DataChannel traffic can flow, false otherwise.
 */
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

/**
 * @brief Send a DataChannel message, opening the channel first if needed.
 *
 * Fails unless the association's data socket exists and conn->sctp_ready is
 * set. If neither channel flag is set (channel never announced in either
 * direction), a DCEP OPEN with an auto-generated "dc<N>" label is sent first
 * and the open_sent bit is recorded so the OPEN is sent only once. The
 * payload goes out with PPID_STRING or PPID_BINARY on the channel's stream.
 *
 * @param conn      Owning connection.
 * @param channel   DataChannel id == SCTP stream id.
 * @param data      Message payload; may be NULL when @p len is 0.
 * @param len       Payload length in bytes.
 * @param is_string Non-zero to send as a UTF-8 string message.
 * @return 0 if the message was handed to usrsctp, -1 if the link is not
 *         ready or sending failed.
 */
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
