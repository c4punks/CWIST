# WebSocket API

The WebSocket module allows upgrading standard HTTP connections to persistent WebSocket connections.

## Functions

### `cwist_websocket_upgrade`
Upgrades an incoming HTTP request to a WebSocket connection.
```c
cwist_websocket *cwist_websocket_upgrade(cwist_http_request *req, int client_fd);
```
- Returns a `cwist_websocket` context on success, or `NULL` on failure.
- Automatically handles the handshake and sends the `101 Switching Protocols` response.
- **Ownership:** The returned pointer is owned by the caller and must be destroyed with `cwist_websocket_destroy`.

### `cwist_websocket_receive`
Receives a single WebSocket frame.
```c
cwist_ws_frame *cwist_websocket_receive(cwist_websocket *ws);
```
- Blocks until a frame is available.
- Returns `NULL` if the connection is closed or an error occurs.
- **Ownership:** The returned frame must be freed with `cwist_websocket_frame_destroy`.

### `cwist_websocket_send`
Sends data as a WebSocket frame.
```c
int cwist_websocket_send(cwist_websocket *ws, cwist_ws_opcode_t opcode, const uint8_t *data, size_t len);
```

## Opcode Types
- `CWIST_WS_FRAME_TEXT`: Text data (UTF-8)
- `CWIST_WS_FRAME_BINARY`: Binary data
- `CWIST_WS_FRAME_CLOSE`: Connection close
- `CWIST_WS_FRAME_PING` / `CWIST_WS_FRAME_PONG`: Heartbeat

## Integration with `cwist_app`
Use `cwist_app_ws` to register a WebSocket route.

```c
void my_ws_handler(cwist_websocket *ws) {
    // Handling logic
}

cwist_app_ws(app, "/chat", my_ws_handler);
```

## Async (C1M) WebSocket API

*Header:* `<cwist/net/websocket/websocket_async.h>`

In C1M mode (`CWIST_C1M_MODE=1`) the reactor watches sockets and delivers
each complete message through a callback instead of blocking in
`cwist_websocket_receive`. Use `cwist_app_ws_async` to register a route on
this path.

### `cwist_app_ws_async`
```c
void cwist_app_ws_async(cwist_app *app, const char *path,
                        cwist_ws_on_message_t on_message, void *user_data);
```
Registers a reactor-driven WebSocket route. For each complete message the
reactor invokes `on_message(ws, frame, user_data)` on the reactor thread.
`user_data` is an opaque pointer passed through unchanged; use it for
per-handler state (broadcast list, room context, etc.).

### `cwist_ws_on_message_t`
```c
typedef void (*cwist_ws_on_message_t)(cwist_websocket_async *ws,
                                      cwist_ws_frame *frame,
                                      void *user_data);
```
Callback invoked once per complete message. The callback **must** release
`frame` with `cwist_websocket_frame_destroy()`. It is safe to call
`cwist_websocket_async_send()` and `cwist_websocket_async_close()` from
inside this callback.

### `cwist_websocket_async_send`
```c
int cwist_websocket_async_send(cwist_websocket_async *ws,
                               cwist_ws_opcode_t opcode,
                               const uint8_t *data, size_t len);
```
Sends one FIN-terminated frame without blocking. On `EAGAIN` or a short
write the remainder is parked on a one-shot reactor write slot and drained
when the socket becomes writable. Returns `0` on success, `-1` on failure.

### `cwist_websocket_async_close`
```c
void cwist_websocket_async_close(cwist_websocket_async *ws);
```
Initiates the close handshake without blocking. Queues a CLOSE frame and
schedules connection cleanup. Safe to call from inside the `on_message`
callback; `NULL` is ignored.

## GraphQL subscriptions (graphql-ws) — EXPERIMENTAL

> v3.7 Phase 4 experimental feature. The API and wire behavior may change
> without notice; the feature is opt-in only and changes no existing behavior
> when unused.

`cwist/graphql_ws.h` implements the `graphql-transport-ws` subprotocol on top
of the non-blocking WebSocket transport:

- `connection_init` -> `connection_ack`; `subscribe` before init closes the
  socket with code `4429`.
- `subscribe {id, payload:{query,variables,operationName}}` -> one
  `next {id, payload:{data:{...}}}` per published event, `error {id, payload:[...]}`
  when the operation fails to start, `complete {id}` when it ends.
- `ping`/`pong` JSON keepalive after init. Malformed traffic closes with
  `4400`/`4401`; duplicate operation ids close with `4409`.

Subscription fields are registered on an endpoint with
`cwist_graphql_ws_add_subscription()`; the resolver returns an event source
(`cwist_graphql_event_source_create()` + `cwist_graphql_event_source_add_topic()`).
Application code fans events out through the in-process, thread-safe topic
broker `cwist_graphql_publish(topic, payload)` — delivery to the wire happens
on the owning reactor thread, so publish may be called from any thread.

```c
cwist_graphql_ws_t *gws = cwist_graphql_ws_create(schema);
cwist_graphql_ws_add_subscription(gws, "counter", counter_subscribe, NULL);
cwist_graphql_ws_attach(gws, upgraded_fd, reactor, NULL, 0);
/* elsewhere, any thread: */
cwist_graphql_publish("counter", payload); /* -> next {data:{counter: payload}} */
```

Client `complete {id}` and socket close both tear the subscription down
without leaking. See `tests/test_graphql_subscriptions.c` for an end-to-end
example over a socketpair-driven reactor.
