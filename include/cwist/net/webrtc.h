/** @file webrtc.h
 * @brief WebRTC DataChannel server (SDP + ICE-lite + DTLS + SCTP) interface.
 *
 * DataChannel-only WebRTC per RFC 8831/8832: the server acts as an ICE-lite
 * endpoint, completes a DTLS handshake (BoringSSL, plain DTLS, no SRTP) over
 * the nominated UDP path, and runs SCTP with DataChannel establishment over
 * the DTLS connection via usrsctp.
 *
 * Threading: a ctx runs on one cwist reactor.  cwist_webrtc_ctx_new() creates
 * a private reactor and thread; cwist_webrtc_ctx_new_on() attaches to a
 * reactor the caller already runs.  Every callback is invoked on that
 * reactor's run thread.  cwist_webrtc_conn_send(), cwist_webrtc_conn_close(),
 * cwist_webrtc_handle_offer() and cwist_webrtc_ctx_free() may be called from
 * any thread.
 *
 * Processes: a ctx belongs to the process that created it.  A forked child
 * (e.g. a cwist_app_listen() worker) must create its own ctx; on an inherited
 * one, cwist_webrtc_handle_offer() returns -1 and cwist_webrtc_ctx_free() does
 * nothing.
 *
 * Connection lifetime: a cwist_webrtc_conn pointer passed to a callback is
 * valid for the duration of that callback.  To use it later or from another
 * thread, take a reference with cwist_webrtc_conn_retain() and drop it with
 * cwist_webrtc_conn_release().  After the close handler runs, sends on a
 * retained conn fail with -1.
 */
#ifndef __CWIST_NET_WEBRTC_H__
#define __CWIST_NET_WEBRTC_H__

#include <stddef.h>
#include <stdint.h>

#include <cwist/sys/io/reactor.h>

typedef struct cwist_webrtc_ctx cwist_webrtc_ctx;
typedef struct cwist_webrtc_conn cwist_webrtc_conn;

/** DataChannel message type (RFC 8831 section 8 payload protocol ids). */
typedef enum {
    CWIST_WEBRTC_DATA_BINARY = 0,  /**< PPID 53 (57 when empty); ArrayBuffer/Blob in a browser. */
    CWIST_WEBRTC_DATA_STRING = 1   /**< PPID 51 (56 when empty); UTF-8 string in a browser. */
} cwist_webrtc_data_type;

/**
 * @brief DataChannel message callback, invoked on the ctx's reactor thread.
 * @param channel_id SCTP stream id (DataChannel id).
 * @param type Whether the peer sent a string or binary message; echo it back
 *             with the same type to preserve it.
 */
typedef void (*cwist_webrtc_message_cb)(cwist_webrtc_conn *conn, uint16_t channel_id,
                                        const uint8_t *data, size_t len,
                                        cwist_webrtc_data_type type, void *user);

/**
 * @brief Optional DataChannel open notification (remote peer opened a channel).
 */
typedef void (*cwist_webrtc_channel_cb)(cwist_webrtc_conn *conn, uint16_t channel_id,
                                        const char *label, void *user);

/**
 * @brief Optional connection-closed notification.  Runs once per connection
 *        whose SCTP association came up; after it returns, the conn pointer is
 *        only valid through references the application still holds.
 */
typedef void (*cwist_webrtc_close_cb)(cwist_webrtc_conn *conn, void *user);

/**
 * @brief Create a WebRTC context bound to a UDP port (0 picks an ephemeral
 *        port), running on its own reactor thread.
 * @return NULL on failure.
 */
cwist_webrtc_ctx *cwist_webrtc_ctx_new(uint16_t port);

/**
 * @brief Create a WebRTC context on a reactor the caller runs.
 *
 * The ctx registers its UDP socket and timers with @p reactor and does all of
 * its work inside that reactor's callbacks; no thread is created.  The
 * reactor must keep running until cwist_webrtc_ctx_free() returns.
 * @return NULL on failure.
 */
cwist_webrtc_ctx *cwist_webrtc_ctx_new_on(cwist_reactor_t *reactor, uint16_t port);

/**
 * @brief UDP port the context is bound to.
 */
uint16_t cwist_webrtc_ctx_port(const cwist_webrtc_ctx *ctx);

/**
 * @brief Set the callback for incoming DataChannel messages.  Set handlers
 *        before answering the first offer.
 */
void cwist_webrtc_ctx_set_message_handler(cwist_webrtc_ctx *ctx, cwist_webrtc_message_cb cb,
                                          void *user);

/**
 * @brief Set the callback for incoming DataChannel open (DCEP) events.
 */
void cwist_webrtc_ctx_set_channel_handler(cwist_webrtc_ctx *ctx, cwist_webrtc_channel_cb cb,
                                          void *user);

/**
 * @brief Set the callback for closed connections.
 */
void cwist_webrtc_ctx_set_close_handler(cwist_webrtc_ctx *ctx, cwist_webrtc_close_cb cb,
                                        void *user);

/**
 * @brief Answer a browser SDP offer (ICE-lite, setup:passive).
 * @param offer The remote SDP offer.
 * @param answer_out Buffer receiving the generated SDP answer.
 * @param out_len Size of answer_out.
 * @return 0 on success, -1 on error.
 *
 * Safe from any thread.  The connection completes asynchronously (ICE
 * nomination, DTLS, SCTP) on the ctx's reactor thread.  An answered offer
 * whose peer never shows up is dropped after 30 seconds.
 */
int cwist_webrtc_handle_offer(cwist_webrtc_ctx *ctx, const char *offer, char *answer_out,
                              size_t out_len);

/**
 * @brief Number of connections whose SCTP association is up.
 */
int cwist_webrtc_ctx_connection_count(const cwist_webrtc_ctx *ctx);

/**
 * @brief Send a message on a DataChannel.
 *
 * Safe from any thread.  The message is queued if SCTP is not ready or its
 * send buffer is full; queued bytes are reported by
 * cwist_webrtc_conn_buffered_amount().
 * @return 0 when sent or queued; -1 if the conn is closed, @p len exceeds
 *         262144 bytes (the advertised max-message-size), or the queue already
 *         holds 4 MiB.
 */
int cwist_webrtc_conn_send(cwist_webrtc_conn *conn, uint16_t channel_id, const uint8_t *data,
                           size_t len, cwist_webrtc_data_type type);

/**
 * @brief Bytes accepted by cwist_webrtc_conn_send() and not yet handed to
 *        SCTP (like RTCDataChannel.bufferedAmount, summed over channels).
 */
size_t cwist_webrtc_conn_buffered_amount(const cwist_webrtc_conn *conn);

/**
 * @brief Local certificate fingerprint (SHA-256, colon-separated hex).
 */
const char *cwist_webrtc_ctx_fingerprint(const cwist_webrtc_ctx *ctx);

/**
 * @brief Take a reference on @p conn so it stays valid outside a callback.
 */
void cwist_webrtc_conn_retain(cwist_webrtc_conn *conn);

/**
 * @brief Drop a reference taken with cwist_webrtc_conn_retain().
 */
void cwist_webrtc_conn_release(cwist_webrtc_conn *conn);

/**
 * @brief Close a connection (SCTP abort, DTLS teardown).  Safe from any
 *        thread; the close handler runs on the reactor thread.
 */
void cwist_webrtc_conn_close(cwist_webrtc_conn *conn);

/**
 * @brief Close every connection and free the context.
 *
 * Safe from any thread.  For a ctx made with cwist_webrtc_ctx_new_on() and
 * called off the reactor thread, this waits for the reactor to finish the
 * teardown, so the reactor must still be running.
 */
void cwist_webrtc_ctx_free(cwist_webrtc_ctx *ctx);

#endif
