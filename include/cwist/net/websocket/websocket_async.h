/**
 * @file websocket_async.h
 * @brief Reactor-driven, callback-shaped non-blocking WebSocket API (C1M mode).
 *
 * Classic mode (CWIST_C1M_MODE=0) keeps the blocking cwist_websocket_receive()
 * API in websocket.h.  This header exposes the C1M-only path where the
 * reactor watches the socket and delivers each complete message through
 * cwist_ws_on_message_t, mirroring how HTTP handlers receive requests.
 */

#ifndef __CWIST_WEBSOCKET_ASYNC_H__
#define __CWIST_WEBSOCKET_ASYNC_H__

#include <cwist/net/websocket/websocket.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cwist_websocket_async cwist_websocket_async;

/**
 * @brief Callback invoked for each complete WebSocket message on the C1M path.
 * @param ws Connection the message arrived on. Safe to pass to
 *           cwist_websocket_async_send() and cwist_websocket_async_close().
 * @param frame Owning frame pointer (same shape as cwist_ws_frame). The
 *              callback MUST release it with cwist_websocket_frame_destroy().
 * @param user_data Opaque pointer registered with cwist_app_ws_async().
 */
typedef void (*cwist_ws_on_message_t)(cwist_websocket_async *ws, cwist_ws_frame *frame,
                                      void *user_data);

/**
 * @brief Serialize and send one FIN-terminated frame without blocking.
 *
 * The header and payload are serialized into a single buffer and sent with
 * MSG_DONTWAIT|MSG_NOSIGNAL.  On EAGAIN or a short write the unsent remainder
 * is parked on a one-shot reactor write slot (same model as the HTTP
 * parked-response writer) and drained when the socket becomes writable again.
 *
 * @param ws Active async WebSocket connection.
 * @param opcode WebSocket opcode describing the payload semantics.
 * @param data Optional payload buffer. May be NULL when @p len is zero.
 * @param len Number of payload bytes to transmit.
 * @return 0 on success, -1 on failure.
 */
int cwist_websocket_async_send(cwist_websocket_async *ws, cwist_ws_opcode_t opcode,
                               const uint8_t *data, size_t len);

/**
 * @brief Initiate the close handshake without blocking.
 *
 * Queues a CLOSE frame and schedules connection cleanup.  Must be callable
 * from inside the on_message callback (i.e. from the reactor thread); the
 * connection state is released once the current reactor turn unwinds.
 *
 * @param ws Connection to close. NULL is ignored.
 */
void cwist_websocket_async_close(cwist_websocket_async *ws);

#ifdef __cplusplus
}
#endif

#endif
