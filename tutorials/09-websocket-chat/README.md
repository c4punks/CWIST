# Tutorial 09: Full-Duplex WebSocket Server

Handle bidirectional full-duplex WebSocket connections and frame messaging.

## Key Concepts
- Register a WebSocket endpoint with `cwist_app_ws(app, "/ws", handler)`. The framework performs the upgrade and calls `handler(cwist_websocket *ws)` once per connection; the connection is closed when the handler returns.
- Loop on `cwist_websocket_receive(ws)` until it returns `NULL` or a `CWIST_WS_FRAME_CLOSE` frame, and free each frame with `cwist_websocket_frame_destroy()`. PING frames are answered with PONG automatically.
- Send frames back with `cwist_websocket_send(ws, CWIST_WS_FRAME_TEXT, data, len)`.

## Build and Run

```bash
mkdir build && cd build
cmake ..
make
./tut09
```
