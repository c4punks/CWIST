/** @file webrtc_internal.h
 * @brief Shared internals between the webrtc module translation units.
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

typedef enum {
    CWIST_CONN_STUN = 0, /* waiting for ICE nomination (or response, client) */
    CWIST_CONN_DTLS = 1, /* DTLS handshake in flight */
    CWIST_CONN_ESTABLISHED = 2, /* DTLS up, SCTP pending or up */
    CWIST_CONN_DEAD = 3
} cwist_conn_state;

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

/* ice.c */
#define CWIST_STUN_BINDING_REQUEST 0x0001
#define CWIST_STUN_BINDING_RESPONSE 0x0101

int cwist_ice_stun_is_message(const uint8_t *buf, size_t len);
bool cwist_ice_stun_is_binding_request(const uint8_t *buf, size_t len);
bool cwist_ice_stun_has_use_candidate(const uint8_t *buf, size_t len);
/* Extract the remote ufrag from the USERNAME attribute ("remoteufrag:localufrag"). */
int cwist_ice_stun_get_remote_ufrag(const uint8_t *buf, size_t len, char *out, size_t cap);
/* Validate MESSAGE-INTEGRITY of a request against pwd. 1 = valid, 0 = invalid. */
int cwist_ice_stun_validate_request(const uint8_t *buf, size_t len, const char *pwd);
/* Build a binding response (XOR-MAPPED-ADDRESS + MI + fingerprint). Returns length or -1. */
int cwist_ice_stun_build_response(uint8_t *out, size_t cap, const uint8_t *req, size_t req_len,
                                  const struct sockaddr_in *mapped, const char *pwd);
/* Build a controlling binding request with USE-CANDIDATE. Returns length or -1. */
int cwist_ice_stun_build_request(uint8_t *out, size_t cap, const uint8_t txid[12],
                                 const char *username);
/* Sign a built request (append MI + FINGERPRINT). Returns new length or -1. */
int cwist_ice_stun_sign_request(uint8_t *msg, size_t cap, size_t msg_len, const char *pwd);
/* Validate a response: cookie, txid match, MI with pwd, return mapped address. */
int cwist_ice_stun_parse_response(const uint8_t *buf, size_t len, const uint8_t txid[12],
                                  const char *pwd, struct sockaddr_in *mapped);
void cwist_ice_random_creds(char *ufrag, size_t ufrag_cap, char *pwd, size_t pwd_cap);

/* sdp.c */
typedef struct {
    char ice_ufrag[64];
    char ice_pwd[128];
    char fingerprint[128];
    char setup[16];
    char mid[16];
    int has_lite;
} cwist_sdp_info;

/* Parse the fields the ICE-lite agent needs. Missing fields stay zeroed. Returns 0 on success. */
int cwist_sdp_parse(const char *sdp, size_t len, cwist_sdp_info *info);
int cwist_sdp_write_answer(char *out, size_t cap, const char *fingerprint, const char *ufrag,
                           const char *pwd, const char *mid, const char *host, uint16_t port);
int cwist_sdp_write_offer(char *out, size_t cap, const char *fingerprint, const char *ufrag,
                          const char *pwd, const char *mid);

/* dtls.c */
int cwist_dtls_generate_cert(X509 **cert, EVP_PKEY **pkey);
int cwist_dtls_fingerprint(X509 *cert, char *out, size_t cap);
SSL_CTX *cwist_dtls_ctx_new(int is_server, X509 *cert, EVP_PKEY *pkey);

/* sctp.c */
/* Global usrsctp init (nothreads; caller pumps usrsctp_handle_timers). Idempotent. */
int cwist_sctp_global_init(void);
/* Create the per-conn SCTP socket. If active_role, also connect() to start the association. */
int cwist_sctp_conn_open(struct cwist_webrtc_conn *conn);
void cwist_sctp_conn_close(struct cwist_webrtc_conn *conn);
/* Feed a DTLS-decrypted SCTP packet into usrsctp. */
void cwist_sctp_conn_input(struct cwist_webrtc_conn *conn, const uint8_t *data, size_t len);
/* Accept the association (passive side) and drain queued inbound messages. */
void cwist_sctp_conn_drain(struct cwist_webrtc_conn *conn);
int dcep_send(struct cwist_webrtc_conn *conn, uint16_t channel, uint8_t type,
              const char *label);
/* Send a DataChannel message; sends DCEP OPEN first when the channel is new. */
int cwist_sctp_send(struct cwist_webrtc_conn *conn, uint16_t channel, const uint8_t *data,
                    size_t len, int is_string);
bool cwist_sctp_assoc_established(struct cwist_webrtc_conn *conn);

/* webrtc.c helpers used across the module */
int cwist_webrtc_conn_send_raw(struct cwist_webrtc_conn *conn, const uint8_t *data, size_t len);
void cwist_webrtc_conn_sctp_out(struct cwist_webrtc_conn *conn, const void *buffer, size_t len);
void cwist_webrtc_conn_on_channel_open(struct cwist_webrtc_conn *conn, uint16_t channel,
                                       const char *label);
void cwist_webrtc_conn_on_message(struct cwist_webrtc_conn *conn, uint16_t channel,
                                  const uint8_t *data, size_t len, int is_string);
struct cwist_webrtc_conn *cwist_webrtc_ctx_find_conn(cwist_webrtc_ctx *ctx,
                                                     const struct sockaddr_in *remote);
void cwist_webrtc_ctx_add_conn(cwist_webrtc_ctx *ctx, struct cwist_webrtc_conn *conn);
void cwist_webrtc_conn_start_dtls(struct cwist_webrtc_conn *conn);
void cwist_webrtc_conn_wake(cwist_webrtc_ctx *ctx);

/* Internal client-role dialer (used by loopback tests): starts ICE against a
 * remote ICE-lite endpoint described by peer_ufrag/peer_pwd from its answer. */
cwist_webrtc_conn *cwist_webrtc_connect(cwist_webrtc_ctx *ctx, const struct sockaddr_in *remote,
                                        const char *peer_ufrag, const char *peer_pwd);

#endif
