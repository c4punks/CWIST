/** @file webrtc_internal.h
 * @brief Shared internals between the webrtc module translation units.
 *
 * Declares the private connection/context types plus the internal API split
 * across ice.c, sdp.c, dtls.c, sctp.c, and webrtc.c. Not part of the public
 * <cwist/net/webrtc.h> interface.
 */
#ifndef __CWIST_WEBRTC_INTERNAL_H__
#define __CWIST_WEBRTC_INTERNAL_H__

#include <cwist/net/webrtc.h>

#include <netinet/in.h>
#include <openssl/ssl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/socket.h>
#include <time.h>

struct socket; /* usrsctp */
struct pending_send;

/**
 * @brief High-level ICE/DTLS/SCTP connection state of a single peer.
 */
typedef enum {
    CWIST_CONN_STUN = 0, /* waiting for ICE nomination (or response, client) */
    CWIST_CONN_DTLS = 1, /* DTLS handshake in flight */
    CWIST_CONN_ESTABLISHED = 2, /* DTLS up, SCTP pending or up */
    CWIST_CONN_DEAD = 3
} cwist_conn_state;

/**
 * @brief Internal state for one WebRTC peer connection (single remote address).
 *
 * Owned by a cwist_webrtc_ctx; linked into ctx->conns. Guarded by ctx->lock
 * for list membership and callback-relevant fields.
 */
struct cwist_webrtc_conn {
    cwist_webrtc_ctx *ctx;
    int is_server_role; /* 1: passive DTLS role (answering), 0: active (offering) */
    cwist_conn_state state;
    bool sctp_ready;
    struct sockaddr_in remote;
    socklen_t remote_len;

    SSL *ssl;

    struct socket *sctp_sock;
    struct socket *sctp_acc; /* accepted socket on the answering side */
    uint8_t *ch_state; /* 2 bits per channel: 0=opened_by_peer, 1=open_sent */

    /* ICE */
    char peer_ufrag[64];
    char peer_pwd[128];
    uint8_t stun_txid[12];
    struct timespec stun_next;
    int stun_attempts;

    bool closed;
    struct pending_send *pending_head;
    struct pending_send *pending_tail;
    struct cwist_webrtc_conn *next;
};

/**
 * @brief Shared context: UDP socket, certificate, callbacks, and connection list.
 *
 * One context owns one UDP socket and serves all peer connections on it.
 * A background thread pumps ICE retransmits and DTLS/SCTP I/O until
 * thread_running goes false.
 */
struct cwist_webrtc_ctx {
    int udp_fd;
    uint16_t port;
    pthread_t thread;
    bool thread_running;
    int wake_pipe[2];
    bool stop;
    int ice_lite_server; /* set once an offer has been answered */

    X509 *cert;
    EVP_PKEY *pkey;
    SSL_CTX *server_ssl_ctx;
    SSL_CTX *client_ssl_ctx;
    char fingerprint[128];
    char ice_ufrag[16];
    char ice_pwd[64];
    char host_ip[64]; /* best-guess local IPv4 for SDP host candidates */

    cwist_webrtc_message_cb msg_cb;
    void *msg_user;
    cwist_webrtc_channel_cb ch_cb;
    void *ch_user;

    cwist_webrtc_conn *conns;
    pthread_mutex_t lock;
};

/** @{ @name STUN message types (ice.c) */
#define CWIST_STUN_BINDING_REQUEST 0x0001  /**< STUN Binding request. */
#define CWIST_STUN_BINDING_RESPONSE 0x0101 /**< STUN Binding success response. */
/** @} */

/**
 * @brief Check whether a datagram looks like a STUN message.
 * @param buf Datagram bytes.
 * @param len Datagram length.
 * @return 1 if @p len covers a STUN header and the magic cookie matches, else 0.
 */
int cwist_ice_stun_is_message(const uint8_t *buf, size_t len);
/**
 * @brief Check whether a datagram is a STUN Binding request.
 * @return true if it is a STUN message with type CWIST_STUN_BINDING_REQUEST.
 */
bool cwist_ice_stun_is_binding_request(const uint8_t *buf, size_t len);
/**
 * @brief Check for the USE-CANDIDATE attribute in a Binding request.
 * @return true if the request carries USE-CANDIDATE (nominated pair).
 */
bool cwist_ice_stun_has_use_candidate(const uint8_t *buf, size_t len);
/**
 * @brief Extract the remote ufrag from the USERNAME attribute ("remoteufrag:localufrag").
 * @param out Output buffer.
 * @param cap Capacity of @p out.
 * @return 0 on success, -1 if the attribute is missing/malformed.
 */
int cwist_ice_stun_get_remote_ufrag(const uint8_t *buf, size_t len, char *out, size_t cap);
/**
 * @brief Validate MESSAGE-INTEGRITY of a request against pwd.
 * @retval 1 valid
 * @retval 0 invalid
 */
int cwist_ice_stun_validate_request(const uint8_t *buf, size_t len, const char *pwd);
/**
 * @brief Build a binding response (XOR-MAPPED-ADDRESS + MI + fingerprint).
 * @return Message length, or -1 on failure/overflow.
 */
int cwist_ice_stun_build_response(uint8_t *out, size_t cap, const uint8_t *req, size_t req_len,
                                  const struct sockaddr_in *mapped, const char *pwd);
/**
 * @brief Build a controlling binding request with USE-CANDIDATE.
 * @param txid 12-byte transaction ID to embed (must match the stored one).
 * @param username "localufrag:remoteufrag".
 * @return Message length, or -1 on failure.
 */
int cwist_ice_stun_build_request(uint8_t *out, size_t cap, const uint8_t txid[12],
                                 const char *username);
/**
 * @brief Sign a built request (append MI + FINGERPRINT).
 * @return New message length, or -1 on failure.
 */
int cwist_ice_stun_sign_request(uint8_t *msg, size_t cap, size_t msg_len, const char *pwd);
/**
 * @brief Validate a response: cookie, txid match, MI with pwd, return mapped address.
 * @param mapped Filled with the XOR-MAPPED-ADDRESS on success.
 * @return 1 on success, 0 on validation failure.
 */
int cwist_ice_stun_parse_response(const uint8_t *buf, size_t len, const uint8_t txid[12],
                                  const char *pwd, struct sockaddr_in *mapped);
/**
 * @brief Generate random ICE ufrag/pwd credentials.
 * @param ufrag Output buffer for the ufrag.
 * @param ufrag_cap Capacity of @p ufrag.
 * @param pwd Output buffer for the password.
 * @param pwd_cap Capacity of @p pwd.
 */
void cwist_ice_random_creds(char *ufrag, size_t ufrag_cap, char *pwd, size_t pwd_cap);

/** @{ @name SDP parsing and serialization (sdp.c) */

/**
 * @brief Parsed subset of a remote SDP description needed by the ICE-lite agent.
 *
 * Fields the parser does not find stay zeroed (memset at entry).
 */
typedef struct {
    char ice_ufrag[64];     /**< Remote ICE ufrag. */
    char ice_pwd[128];      /**< Remote ICE password. */
    char fingerprint[128];  /**< Remote DTLS certificate fingerprint. */
    char setup[16];         /**< a=setup value (e.g. "active", "passive"). */
    char mid[16];           /**< a=mid value. */
    int has_lite;           /**< Non-zero if a=ice-lite was present. */
} cwist_sdp_info;

/**
 * @brief Parse the fields the ICE-lite agent needs from an SDP description.
 * @param info Filled on return; missing fields stay zeroed.
 * @return 0 on success, -1 if ice-ufrag or ice-pwd is missing or empty.
 */
int cwist_sdp_parse(const char *sdp, size_t len, cwist_sdp_info *info);
/**
 * @brief Serialize an SDP answer for the ICE-lite answering endpoint.
 *
 * The answer takes the passive DTLS role and advertises a host candidate for
 * @p host:@p port.
 * @return 0 on success, -1 if the output was truncated or encoding failed.
 */
int cwist_sdp_write_answer(char *out, size_t cap, const char *fingerprint, const char *ufrag,
                           const char *pwd, const char *mid, const char *host, uint16_t port);
/**
 * @brief Serialize an SDP offer for the full-agent offering endpoint.
 *
 * The offer takes the active DTLS role; candidates are signalled separately.
 * @return 0 on success, -1 if the output was truncated or encoding failed.
 */
int cwist_sdp_write_offer(char *out, size_t cap, const char *fingerprint, const char *ufrag,
                          const char *pwd, const char *mid);
/** @} */

/** @{ @name DTLS certificate helpers (dtls.c) */

/**
 * @brief Generate a self-signed EC (P-256) certificate/key pair for DTLS.
 * @param cert Receives the certificate (NULL on failure).
 * @param pkey Receives the private key (NULL on failure).
 * @return 0 on success, -1 on failure.
 */
int cwist_dtls_generate_cert(X509 **cert, EVP_PKEY **pkey);
/**
 * @brief Compute the SHA-256 fingerprint of a certificate in "AA:BB:..." form.
 * @param out Output buffer.
 * @param cap Capacity of @p out.
 * @return 0 on success, -1 on failure.
 */
int cwist_dtls_fingerprint(X509 *cert, char *out, size_t cap);
/**
 * @brief Create a DTLS 1.2 SSL_CTX for one endpoint role.
 * @param is_server Non-zero for the passive (server) context; certificate and
 *                  key are only loaded in this mode.
 * @param cert Certificate used by the server context.
 * @param pkey Private key matching @p cert.
 * @return New SSL_CTX, or NULL on failure. Caller frees with SSL_CTX_free.
 * @note Peer fingerprint verification is not enforced; callers must match
 *       the SDP fingerprint out of band.
 */
SSL_CTX *cwist_dtls_ctx_new(int is_server, X509 *cert, EVP_PKEY *pkey);
/** @} */

/** @{ @name SCTP / DataChannel layer (sctp.c) */

/**
 * @brief Global usrsctp init (nothreads; caller pumps usrsctp_handle_timers).
 * @return 0 on success. Idempotent.
 */
int cwist_sctp_global_init(void);
/**
 * @brief Create the per-conn SCTP socket.
 * @param conn Connection whose sctp_sock/sctp_acc are set up.
 * @return 0 on success, -1 on failure.
 * @note If conn->is_server_role is false (active role), also connect() to
 *       start the association.
 */
int cwist_sctp_conn_open(struct cwist_webrtc_conn *conn);
/**
 * @brief Tear down the connection's SCTP socket(s) and free related state.
 */
void cwist_sctp_conn_close(struct cwist_webrtc_conn *conn);
/**
 * @brief Feed a DTLS-decrypted SCTP packet into usrsctp.
 */
void cwist_sctp_conn_input(struct cwist_webrtc_conn *conn, const uint8_t *data, size_t len);
/**
 * @brief Accept the association (passive side) and drain queued inbound messages.
 */
void cwist_sctp_conn_drain(struct cwist_webrtc_conn *conn);
/**
 * @brief Send a DCEP control message on a channel.
 * @param type DCEP message type (e.g. OPEN, ACK).
 * @param label Channel label; only used for OPEN, truncated to 200 bytes.
 * @return 0 on success, -1 on failure.
 */
int dcep_send(struct cwist_webrtc_conn *conn, uint16_t channel, uint8_t type,
              const char *label);
/**
 * @brief Send a DataChannel message; sends DCEP OPEN first when the channel is new.
 * @param is_string Non-zero for string messages, zero for binary.
 * @return 0 on success, -1 on failure.
 */
int cwist_sctp_send(struct cwist_webrtc_conn *conn, uint16_t channel, const uint8_t *data,
                    size_t len, int is_string);
/**
 * @brief Check whether the SCTP association is established.
 * @return true once the association handshake completed.
 */
bool cwist_sctp_assoc_established(struct cwist_webrtc_conn *conn);
/** @} */

/** @{ @name Cross-module helpers implemented in webrtc.c */

/**
 * @brief Send a DTLS-encrypted SCTP packet to the peer over the ctx UDP socket.
 * @return Bytes sent, or -1 on error.
 */
int cwist_webrtc_conn_send_raw(struct cwist_webrtc_conn *conn, const uint8_t *data, size_t len);
/**
 * @brief usrsctp send callback entry point: encrypt via DTLS and transmit.
 * @param buffer SCTP packet produced by usrsctp.
 */
void cwist_webrtc_conn_sctp_out(struct cwist_webrtc_conn *conn, const void *buffer, size_t len);
/**
 * @brief Dispatch a channel-open notification to the user ch_cb.
 */
void cwist_webrtc_conn_on_channel_open(struct cwist_webrtc_conn *conn, uint16_t channel,
                                       const char *label);
/**
 * @brief Dispatch an inbound DataChannel message to the user msg_cb.
 */
void cwist_webrtc_conn_on_message(struct cwist_webrtc_conn *conn, uint16_t channel,
                                  const uint8_t *data, size_t len, int is_string);
/**
 * @brief Find a connection by remote address in the ctx list.
 * @return Matching connection, or NULL if none.
 */
struct cwist_webrtc_conn *cwist_webrtc_ctx_find_conn(cwist_webrtc_ctx *ctx,
                                                     const struct sockaddr_in *remote);
/**
 * @brief Link a connection into the ctx connection list.
 * @note Takes ctx->lock internally.
 */
void cwist_webrtc_ctx_add_conn(cwist_webrtc_ctx *ctx, struct cwist_webrtc_conn *conn);
/**
 * @brief Kick off the DTLS handshake for a connection that nominated its pair.
 * @note Performs SSL_do_handshake work on the current state.
 */
void cwist_webrtc_conn_start_dtls(struct cwist_webrtc_conn *conn);
/**
 * @brief Write a byte to the ctx wake pipe to interrupt the pump thread's poll.
 */
void cwist_webrtc_conn_wake(cwist_webrtc_ctx *ctx);

/**
 * @brief Internal client-role dialer (used by loopback tests).
 *
 * Starts ICE against a remote ICE-lite endpoint described by peer_ufrag/peer_pwd
 * from its answer.
 * @return New connection, or NULL on failure.
 */
cwist_webrtc_conn *cwist_webrtc_connect(cwist_webrtc_ctx *ctx, const struct sockaddr_in *remote,
                                        const char *peer_ufrag, const char *peer_pwd);
/** @} */

#endif
