/**
 * @file ws_async_internal.h
 * @brief Private wiring between the app layer (C1M upgrade path) and the
 * reactor-driven WebSocket implementation in websocket_async.c.
 *
 * Not part of the public API; native-only (pulls in reactor.h / sockets).
 */

#ifndef __CWIST_WS_ASYNC_INTERNAL_H__
#define __CWIST_WS_ASYNC_INTERNAL_H__

#include <cwist/net/websocket/websocket_async.h>
#include <cwist/sys/io/reactor.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * @brief Take ownership of a connected, upgraded fd and drive it reactively.
 *
 * Registers a one-shot read slot with the reactor and returns.  From then on
 * the reactor drains the socket without blocking and delivers each complete
 * message through @p on_message.  The HTTP layer must not touch the fd after
 * a successful attach (it returns CWIST_ASYNC_DETACH).
 *
 * @param fd Connected, upgraded client socket (set non-blocking here).
 * @param reactor Owning reactor.
 * @param on_message Per-message callback.
 * @param user_data Opaque pointer forwarded to the callback.
 * @param initial Bytes already read past the upgrade request (conn->rbuf);
 *                fed through the frame parser before the first reactor wait.
 * @param initial_len Number of valid bytes at @p initial (0 when none).
 * @return true when the fd was handed to the reactor path, false on failure
 *         (fd is closed and all state freed before returning).
 */
bool cwist_websocket_async_attach(int fd, cwist_reactor_t *reactor,
                                  cwist_ws_on_message_t on_message, void *user_data,
                                  const uint8_t *initial, size_t initial_len);

/**
 * @brief Attach variant that reports the connection handle and teardown.
 *
 * Identical to cwist_websocket_async_attach() plus an optional on_close hook
 * invoked exactly once on the reactor thread at final teardown (before the
 * connection state is freed), and a return value exposing the handle to the
 * caller.
 *
 * @return Connection handle on success, NULL on failure (fd closed and all
 *         state freed before returning).
 */
cwist_websocket_async *cwist_websocket_async_attach_ex(int fd, cwist_reactor_t *reactor,
                                                       cwist_ws_on_message_t on_message,
                                                       cwist_ws_on_close_t on_close,
                                                       void *user_data, const uint8_t *initial,
                                                       size_t initial_len);

/** Close with a specific status code (e.g. graphql-ws 4400-4429 range) on the
 *  wire, mirroring cwist_websocket_async_close() teardown semantics. */
void cwist_websocket_async_close_code(cwist_websocket_async *ws, uint16_t code);

#endif
