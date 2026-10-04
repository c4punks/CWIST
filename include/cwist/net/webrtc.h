/** @file webrtc.h
 * @brief WebRTC DataChannel server (SDP + ICE-lite + DTLS + SCTP) interface.
 *
 * DataChannel-only WebRTC per RFC 8831/8832: the server acts as an ICE-lite
 * endpoint, completes a DTLS handshake (BoringSSL, plain DTLS, no SRTP) over
 * the nominated UDP path, and runs SCTP with DataChannel establishment over
 * the DTLS connection via usrsctp.
 */
#ifndef __CWIST_NET_WEBRTC_H__
#define __CWIST_NET_WEBRTC_H__

#include <stddef.h>
#include <stdint.h>

typedef struct cwist_webrtc_ctx cwist_webrtc_ctx;
typedef struct cwist_webrtc_conn cwist_webrtc_conn;

typedef enum {
    CWIST_WEBRTC_DATA_BINARY = 0,  /**< PPID 51 */
    CWIST_WEBRTC_DATA_STRING = 1   /**< PPID 50 */
} cwist_webrtc_data_type;

/**
 * @brief DataChannel message callback, invoked from the ctx event-loop thread.
 * @param channel_id SCTP stream id (DataChannel id).
 */
typedef void (*cwist_webrtc_message_cb)(cwist_webrtc_conn *conn, uint16_t channel_id,
                                        const uint8_t *data, size_t len, void *user);

/**
 * @brief Optional DataChannel open notification (remote peer opened a channel).
 */
typedef void (*cwist_webrtc_channel_cb)(cwist_webrtc_conn *conn, uint16_t channel_id,
                                        const char *label, void *user);

/**
 * @brief Create a WebRTC context bound to a UDP port (0 picks an ephemeral port).
 * @return NULL on failure.
 */
cwist_webrtc_ctx *cwist_webrtc_ctx_new(uint16_t port);

/**
 * @brief UDP port the context is bound to.
 */
uint16_t cwist_webrtc_ctx_port(const cwist_webrtc_ctx *ctx);

/**
 * @brief Set the callback for incoming DataChannel messages.
 */
void cwist_webrtc_ctx_set_message_handler(cwist_webrtc_ctx *ctx, cwist_webrtc_message_cb cb,
                                          void *user);

/**
 * @brief Set the callback for incoming DataChannel open (DCEP) events.
 */
void cwist_webrtc_ctx_set_channel_handler(cwist_webrtc_ctx *ctx, cwist_webrtc_channel_cb cb,
                                          void *user);

/**
 * @brief Answer a browser SDP offer (ICE-lite, setup:passive).
 * @param offer The remote SDP offer.
 * @param answer_out Buffer receiving the generated SDP answer.
 * @param out_len Size of answer_out.
 * @return 0 on success, -1 on error.
 *
 * The connection completes asynchronously (ICE nomination, DTLS, SCTP);
 * cwist_webrtc_ctx_poll() must be driven, or the ctx background thread
 * handles it when created with cwist_webrtc_ctx_new().
 */
int cwist_webrtc_handle_offer(cwist_webrtc_ctx *ctx, const char *offer, char *answer_out,
                              size_t out_len);

/**
 * @brief Number of fully established (SCTP/DTLS up) connections.
 */
int cwist_webrtc_ctx_connection_count(const cwist_webrtc_ctx *ctx);

/**
 * @brief Send a message on a DataChannel.
 * @return 0 on success, -1 on error.
 */
int cwist_webrtc_conn_send(cwist_webrtc_conn *conn, uint16_t channel_id, const uint8_t *data,
                           size_t len, cwist_webrtc_data_type type);

/**
 * @brief Local certificate fingerprint (SHA-256, colon-separated hex).
 */
const char *cwist_webrtc_ctx_fingerprint(const cwist_webrtc_ctx *ctx);

void cwist_webrtc_conn_close(cwist_webrtc_conn *conn);
void cwist_webrtc_ctx_free(cwist_webrtc_ctx *ctx);

#endif
