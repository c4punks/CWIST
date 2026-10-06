/**
 * @file webrtc_internal.h
 * @brief Internal types and cross-file helpers for the WebRTC DataChannel
 *        module (ICE-lite + DTLS + SCTP over UDP).
 *
 * Threading model: every ctx is bound to one cwist reactor, and all mutable
 * ctx/conn state is touched only on that reactor's run thread (the "owner").
 * Other threads reach a ctx or conn through cwist_reactor_post() nodes:
 * cwist_webrtc_conn_send(), cwist_webrtc_conn_close(),
 * cwist_webrtc_handle_offer() and cwist_webrtc_ctx_free() all post when
 * called off the owner thread and act inline when called on it.
 *
 * Lifetimes are reference counted.  A conn holds a reference on its ctx; the
 * ctx's connection table holds a reference on each registered conn; every
 * in-flight post holds a reference on the object it targets.  Teardown
 * (closing sockets, freeing SSL/SCTP state) happens on the owner thread and is
 * separate from freeing the memory, which happens on the last release.
 *
 * Not part of the public API; only the module's .c files and its tests
 * include this header.
 */
#ifndef __CWIST_WEBRTC_INTERNAL_H__
#define __CWIST_WEBRTC_INTERNAL_H__

#include <cwist/net/webrtc.h>
#include <cwist/sys/io/reactor.h>

#include <netinet/in.h>
#include <openssl/ssl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/socket.h>
#include <sys/types.h>

struct socket; /* usrsctp */

/** Largest DataChannel message accepted either way; advertised in the SDP as
 *  a=max-message-size. */
#define CWIST_WEBRTC_MAX_MESSAGE 262144
/** Per-conn cap on bytes queued by cwist_webrtc_conn_send() and not yet taken
 *  by SCTP.  Sends beyond it fail instead of growing the queue. */
#define CWIST_WEBRTC_MAX_BUFFERED (4u << 20)
/** Largest UDP datagram handled; bigger ones are dropped. */
#define CWIST_WEBRTC_DGRAM_MAX 2048
/** Datagrams received per recvmmsg() call. */
#define CWIST_WEBRTC_RX_BATCH 32
/** Datagrams queued before a sendmmsg() flush. */
#define CWIST_WEBRTC_TX_BATCH 64

/** Connection lifecycle. */
typedef enum {
    CWIST_CONN_STUN = 0,        /**< Waiting for ICE nomination (or a STUN response, client). */
    CWIST_CONN_DTLS = 1,        /**< DTLS handshake in flight. */
    CWIST_CONN_ESTABLISHED = 2, /**< DTLS up; SCTP pending or up. */
    CWIST_CONN_CLOSED = 3       /**< Torn down. */
} cwist_conn_state;

/**
 * @brief A DataChannel message on its way into SCTP.
 *
 * Foreign-thread sends travel through the conn's inbox in the same node they
 * wait in on the pending queue, so a queued send costs one allocation.
 */
typedef struct cwist_webrtc_msg {
    struct cwist_webrtc_msg *next; /**< Inbox / pending-queue link. */
    struct cwist_webrtc_conn *conn; /**< Target conn (referenced while posted). */
    uint32_t len;                 /**< Payload length in bytes. */
    uint16_t channel;             /**< SCTP stream id. */
    uint8_t is_string;            /**< Non-zero for PPID 50 (string). */
    uint8_t data[];               /**< Payload. */
} cwist_webrtc_msg;

/** @brief One peer: ICE state, DTLS session, SCTP association. */
struct cwist_webrtc_conn {
    cwist_webrtc_ctx *ctx;   /**< Owning ctx (referenced). */
    atomic_int refs;         /**< Reference count; memory is freed at zero. */
    atomic_bool detached;    /**< Set at teardown; API calls fail fast. */
    atomic_bool close_posted; /**< A foreign close post is in flight. */
    atomic_size_t buffered;  /**< Bytes queued by sends, not yet taken by SCTP. */

    bool is_server_role;     /**< true: answering (DTLS passive); false: offering. */
    cwist_conn_state state;  /**< Lifecycle state. */
    bool registered;         /**< In the ctx table or parked list. */
    bool parked;             /**< Waiting for its first STUN check (remote unknown). */
    bool sctp_ready;         /**< SCTP association up; sends go straight to SCTP. */
    bool close_requested;    /**< Close at the end of the current service pass. */
    bool dirty;              /**< On the ctx dirty list. */
    bool notified_open;      /**< The close handler is owed a call. */

    struct sockaddr_in remote;              /**< Nominated peer address. */
    struct cwist_webrtc_conn *hnext;        /**< Hash-bucket / parked-list link. */
    struct cwist_webrtc_conn *dirty_next;   /**< Dirty-list link. */

    /* ICE */
    char peer_ufrag[64];     /**< Remote ICE ufrag. */
    char peer_pwd[128];      /**< Remote ICE password (client role signs with it). */
    uint8_t stun_txid[12];   /**< Outstanding Binding request (client role). */
    int stun_attempts;       /**< Binding requests sent so far (client role). */
    uint32_t stun_rto_ms;    /**< Current retransmit timeout (client role). */

    /* DTLS */
    SSL *ssl;                /**< DTLS session (custom datagram BIO). */
    const uint8_t *dtls_in;  /**< Datagram being fed to the BIO, or NULL. */
    size_t dtls_in_len;      /**< Length of dtls_in. */

    /* SCTP */
    struct socket *sctp_sock; /**< Listening (server) or connected (client) socket. */
    struct socket *sctp_acc;  /**< Accepted association socket (server role). */
    uint8_t *ch_bits;        /**< 2 bits per stream: bit0 opened by peer, bit1 OPEN sent. */
    uint32_t ch_bits_len;    /**< Size of ch_bits in bytes. */
    uint8_t *rx_partial;     /**< Reassembly buffer for a message delivered in pieces. */
    size_t rx_partial_len;   /**< Bytes held in rx_partial. */
    size_t rx_partial_cap;   /**< Capacity of rx_partial. */
    cwist_webrtc_msg *pending_head; /**< Sends waiting for SCTP. */
    cwist_webrtc_msg *pending_tail; /**< Tail of the pending queue. */
    _Atomic(cwist_webrtc_msg *) inbox; /**< Foreign-thread sends (LIFO stack). */

    /* Timers and posts */
    cwist_reactor_timer_t timer;   /**< STUN retransmit / DTLS / liveness / park expiry. */
    uint64_t created_ns;           /**< Creation time (handshake deadline). */
    uint64_t last_rx_ns;           /**< Last datagram from the peer (liveness). */
    cwist_reactor_post_t reg_post;   /**< Registration post (offer/connect). */
    cwist_reactor_post_t close_post; /**< Foreign-thread close post. */
    cwist_reactor_post_t kick_post;  /**< Drains the inbox on the owner thread. */
};

/** @brief A UDP endpoint bound to one reactor, serving many conns. */
struct cwist_webrtc_ctx {
    cwist_reactor_t *reactor; /**< Reactor whose run thread owns this ctx. */
    bool owns_reactor;        /**< Created by cwist_webrtc_ctx_new(); run on our thread. */
    pid_t owner_pid;          /**< Process that created the ctx (fork detection). */
    pthread_t thread;         /**< Run thread when owns_reactor. */
    bool thread_running;      /**< thread was started. */
    atomic_int refs;          /**< Reference count (user + conns + posts + armed fd slot). */

    int udp_fd;               /**< Non-blocking UDP socket. */
    uint16_t port;            /**< Bound UDP port. */
    bool udp_armed;           /**< A read slot is pending on the reactor. */
    bool closed;              /**< Teardown ran (owner thread). */
    atomic_int ice_lite_server; /**< An offer was answered: accept Binding requests. */

    X509 *cert;               /**< Ephemeral DTLS certificate. */
    EVP_PKEY *pkey;           /**< Its private key. */
    SSL_CTX *server_ssl_ctx;  /**< DTLS server (passive) context. */
    SSL_CTX *client_ssl_ctx;  /**< DTLS client (active) context. */
    char fingerprint[128];    /**< SHA-256 fingerprint of cert, SDP form. */
    char ice_ufrag[16];       /**< Local ICE ufrag. */
    char ice_pwd[64];         /**< Local ICE password. */
    char host_ip[64];         /**< Best-guess local IPv4 for the SDP host candidate. */

    cwist_webrtc_message_cb msg_cb; /**< Message handler. */
    void *msg_user;                 /**< Its user pointer. */
    cwist_webrtc_channel_cb ch_cb;  /**< Channel-open handler. */
    void *ch_user;                  /**< Its user pointer. */
    cwist_webrtc_close_cb close_cb; /**< Connection-closed handler. */
    void *close_user;               /**< Its user pointer. */

    /* Connection table, keyed by remote address. */
    cwist_webrtc_conn **buckets; /**< Hash buckets (power of two). */
    uint32_t nbuckets;           /**< Bucket count. */
    uint32_t nconns;             /**< Conns in the table. */
    cwist_webrtc_conn *parked;   /**< Answered offers not yet nominated. */
    cwist_webrtc_conn *dirty;    /**< Conns to service at the end of this round. */
    atomic_int ready_count;      /**< Conns with SCTP up. */

    /* SCTP clock */
    cwist_reactor_timer_t sctp_timer; /**< Drives usrsctp timers while associations exist. */
    int sctp_assocs;                  /**< Conns holding an SCTP socket. */
    uint64_t last_activity_ns;        /**< Last datagram or send; picks the tick rate. */
    bool sctp_fast;                   /**< The tick is armed at the fast rate. */

    /* I/O */
    uint8_t *rx_bufs;   /**< CWIST_WEBRTC_RX_BATCH datagram buffers. */
    uint8_t *plain_buf; /**< Decrypted DTLS record buffer. */
    uint8_t *sctp_buf;  /**< usrsctp_recvv() buffer (one full message). */
    uint8_t *tx_bufs;   /**< CWIST_WEBRTC_TX_BATCH datagram buffers. */
    size_t tx_len[CWIST_WEBRTC_TX_BATCH];             /**< Queued datagram lengths. */
    struct sockaddr_in tx_addr[CWIST_WEBRTC_TX_BATCH]; /**< Queued destinations. */
    uint32_t tx_n;      /**< Datagrams queued. */
    uint64_t now_ns;    /**< Monotonic time cached at the start of a round. */

    /* Teardown handshake for ctxs on a caller-run reactor. */
    cwist_reactor_post_t free_post; /**< Teardown post. */
    pthread_mutex_t free_mu;        /**< Guards fd_closed. */
    pthread_cond_t free_cv;         /**< Signals fd_closed. */
    bool fd_closed;                 /**< UDP socket closed by the owner. */
};

/** @name STUN message types (ice.c) */
/**@{*/
#define CWIST_STUN_BINDING_REQUEST 0x0001  /**< STUN Binding request. */
#define CWIST_STUN_BINDING_RESPONSE 0x0101 /**< STUN Binding success response. */
/**@}*/

/** @name ICE-lite STUN (ice.c) */
/**@{*/
/** @brief Non-zero if @p buf looks like a STUN message (magic cookie, length). */
int cwist_ice_stun_is_message(const uint8_t *buf, size_t len);
/** @brief true if @p buf is a STUN Binding request. */
bool cwist_ice_stun_is_binding_request(const uint8_t *buf, size_t len);
/** @brief true if the request carries USE-CANDIDATE (nomination). */
bool cwist_ice_stun_has_use_candidate(const uint8_t *buf, size_t len);
/**
 * @brief Extract the sender's ufrag from USERNAME ("recipient:sender",
 *        RFC 8445 section 7.2.2) -- the peer's SDP ice-ufrag.
 * @return 0 on success, -1 if USERNAME is missing or has no ':'.
 */
int cwist_ice_stun_get_remote_ufrag(const uint8_t *buf, size_t len, char *out, size_t cap);
/**
 * @brief Check MESSAGE-INTEGRITY (HMAC-SHA1 keyed with @p pwd).
 * @return 1 if valid, 0 otherwise.
 */
int cwist_ice_stun_validate_request(const uint8_t *buf, size_t len, const char *pwd);
/**
 * @brief Build a signed Binding success response with XOR-MAPPED-ADDRESS.
 * @return Response length, or -1 if it does not fit in @p cap.
 */
int cwist_ice_stun_build_response(uint8_t *out, size_t cap, const uint8_t *req, size_t req_len,
                                  const struct sockaddr_in *mapped, const char *pwd);
/**
 * @brief Build an unsigned Binding request (USERNAME, USE-CANDIDATE, ...).
 * @return Message length, or -1 if it does not fit in @p cap.
 */
int cwist_ice_stun_build_request(uint8_t *out, size_t cap, const uint8_t txid[12],
                                 const char *username);
/**
 * @brief Append MESSAGE-INTEGRITY and FINGERPRINT to a request.
 * @return New message length, or -1 if it does not fit in @p cap.
 */
int cwist_ice_stun_sign_request(uint8_t *msg, size_t cap, size_t msg_len, const char *pwd);
/**
 * @brief Validate a response: cookie, txid match, MI with pwd, return mapped address.
 * @param mapped Filled with the XOR-MAPPED-ADDRESS on success.
 * @return 1 on success, 0 on validation failure.
 */
int cwist_ice_stun_parse_response(const uint8_t *buf, size_t len, const uint8_t txid[12],
                                  const char *pwd, struct sockaddr_in *mapped);
/** @brief Generate random ICE credentials (ice-chars only). */
void cwist_ice_random_creds(char *ufrag, size_t ufrag_cap, char *pwd, size_t pwd_cap);
/**@}*/

/** @brief Fields the ICE-lite agent needs from an SDP description. */
typedef struct {
    char ice_ufrag[64];     /**< Remote ICE ufrag. */
    char ice_pwd[128];      /**< Remote ICE password. */
    char fingerprint[128];  /**< Remote DTLS certificate fingerprint. */
    char setup[16];         /**< a=setup value (e.g. "active", "passive"). */
    char mid[16];           /**< a=mid value. */
    int has_lite;           /**< Non-zero if a=ice-lite was present. */
} cwist_sdp_info;

/** @name SDP (sdp.c) */
/**@{*/
/**
 * @brief Parse the fields the ICE-lite agent needs from an SDP description.
 * @param info Filled on return; missing fields stay zeroed.
 * @return 0 on success, -1 if ice-ufrag or ice-pwd is missing or empty.
 */
int cwist_sdp_parse(const char *sdp, size_t len, cwist_sdp_info *info);
/**
 * @brief Write an ICE-lite, DTLS-passive DataChannel answer.
 * @return 0 on success, -1 if it does not fit in @p cap.
 */
int cwist_sdp_write_answer(char *out, size_t cap, const char *fingerprint, const char *ufrag,
                           const char *pwd, const char *mid, const char *host, uint16_t port);
/**
 * @brief Write a DTLS-active DataChannel offer (loopback tests).
 * @return 0 on success, -1 if it does not fit in @p cap.
 */
int cwist_sdp_write_offer(char *out, size_t cap, const char *fingerprint, const char *ufrag,
                          const char *pwd, const char *mid);
/**@}*/

/** @name DTLS (dtls.c) */
/**@{*/
/** @brief Generate an ephemeral self-signed P-256 certificate. @return 0 or -1. */
int cwist_dtls_generate_cert(X509 **cert, EVP_PKEY **pkey);
/** @brief SHA-256 fingerprint of @p cert in SDP form ("AB:CD:..."). @return 0 or -1. */
int cwist_dtls_fingerprint(X509 *cert, char *out, size_t cap);
/** @brief DTLS 1.2 SSL_CTX for the server (with cert) or client role. */
SSL_CTX *cwist_dtls_ctx_new(int is_server, X509 *cert, EVP_PKEY *pkey);
/**
 * @brief Datagram BIO for @p conn: reads return conn->dtls_in once, writes go
 *        to cwist_webrtc_conn_dtls_out() one datagram per call.
 */
BIO *cwist_dtls_bio_new(struct cwist_webrtc_conn *conn);
/** @brief DTLS link MTU (bytes per datagram) used for handshake fragmentation. */
unsigned int cwist_dtls_link_mtu(void);
/**@}*/

/** @name SCTP (sctp.c) */
/**@{*/
/** @brief One-time usrsctp initialisation (no threads, AF_CONN only). @return 0. */
int cwist_sctp_global_init(void);
/** @brief Advance usrsctp's process-wide timer clock to now. Any thread. */
void cwist_sctp_tick(void);
/** @brief Create, bind and listen/connect the conn's SCTP socket. @return 0 or -1. */
int cwist_sctp_conn_open(struct cwist_webrtc_conn *conn);
/** @brief Abort the association and release SCTP state. Idempotent. */
void cwist_sctp_conn_close(struct cwist_webrtc_conn *conn);
/** @brief Feed one decrypted SCTP packet to usrsctp. */
void cwist_sctp_conn_input(struct cwist_webrtc_conn *conn, const uint8_t *data, size_t len);
/**
 * @brief Accept (server role), then deliver every complete inbound message.
 * @return 0 normally, -1 if the association is gone and the conn should close.
 */
int cwist_sctp_conn_drain(struct cwist_webrtc_conn *conn);
/** @brief true once the SCTP association is up. */
bool cwist_sctp_assoc_established(struct cwist_webrtc_conn *conn);
/**
 * @brief Send one message, announcing the channel with DCEP OPEN first if we
 *        are opening it.
 * @return 0 when SCTP took it, 1 when the send buffer is full (retry after the
 *         next inbound packet), -1 on a hard error (message dropped).
 */
int cwist_sctp_send(struct cwist_webrtc_conn *conn, uint16_t channel, const uint8_t *data,
                    size_t len, int is_string);
/**@}*/

/** @name Cross-file helpers (webrtc.c) */
/**@{*/
/** @brief true when the calling thread is servicing @p ctx right now. */
bool cwist_webrtc_on_owner(const cwist_webrtc_ctx *ctx);
/** @brief Queue one datagram to the conn's peer (owner thread). */
void cwist_webrtc_conn_dtls_out(struct cwist_webrtc_conn *conn, const uint8_t *data, size_t len);
/** @brief usrsctp output for @p conn; marshalled to the owner thread when needed. */
void cwist_webrtc_conn_sctp_out(struct cwist_webrtc_conn *conn, const void *buffer, size_t len);
/** @brief Deliver a complete inbound message to the message handler. */
void cwist_webrtc_conn_on_message(struct cwist_webrtc_conn *conn, uint16_t channel,
                                  const uint8_t *data, size_t len, int is_string);
/** @brief Report a channel the peer opened to the channel handler. */
void cwist_webrtc_conn_on_channel_open(struct cwist_webrtc_conn *conn, uint16_t channel,
                                       const char *label);
/**
 * @brief Offering-side dialer used by the loopback tests: registers a client
 *        conn to @p remote and starts ICE.
 * @return A conn reference owned by the caller (cwist_webrtc_conn_release()),
 *         or NULL.
 */
cwist_webrtc_conn *cwist_webrtc_connect(cwist_webrtc_ctx *ctx, const struct sockaddr_in *remote,
                                        const char *peer_ufrag, const char *peer_pwd);
/**@}*/

#endif
