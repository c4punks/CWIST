# Live message runtime (experimental)

`cwist_livemsg` runs whole-message application callbacks over either WebSocket
or WebRTC DataChannel. The transport, codec, framing, optional SDP signaling,
and non-blocking sends belong to the runtime; the application supplies
`process()` and `respond()`.

*Header:* `<cwist/livemsg.h>`

The API is experimental in v4.x. It does not change the v4.0 API guarantee.

## Minimal server

```c
#include <cwist/livemsg.h>

static int process(cwist_livemsg_peer *peer, const void *msg, size_t len,
                   void **result, void *user_data) {
    (void)peer; (void)len; (void)user_data;
    *result = (void *)msg;
    return CWIST_LIVEMSG_OK;
}

static int respond(cwist_livemsg_peer *peer, void *result,
                   cwist_livemsg_reply *reply, void *user_data) {
    (void)peer; (void)user_data;
    return cwist_livemsg_reply_set_json(reply, result);
}

static cwist_livemsg_config make_config(void) {
    cwist_livemsg_config config;
    cwist_livemsg_config_init(&config, CWIST_LIVEMSG_TRANSPORT_WS);
    config.port = 8080;
    config.codec = CWIST_LIVEMSG_CODEC_JSON;
    config.process = process;
    config.respond = respond;
    return config;
}

int main(void) {
    return cwist_livemsg_serve(make_config);
}
```

Build and run `example/livemsg-websocket/livemsg-websocket`, then connect a
WebSocket client to `ws://localhost:8080/`. JSON messages are parsed and echoed.
`example/livemsg-webrtc/livemsg-webrtc` uses the same callbacks, accepts SDP at
`POST http://localhost:8080/signal`, and uses UDP port 9000 for ICE/DTLS/SCTP.
The request and response bodies for signaling use `application/sdp`.

## Configuration

Call `cwist_livemsg_config_init()` before setting callbacks and ports. It sets
`ws_path` to `/` and, for WebRTC, `signaling_path` to `/signal`. Set
`signaling_path` to `NULL` to disable the built-in HTTP endpoint. The default
`port` and `udp_port` are zero; applications should select explicit ports,
except when an ephemeral WebRTC UDP port is intentional.

`transport` is a bitmask for forward compatibility. Version 1 accepts exactly
one of:

- `CWIST_LIVEMSG_TRANSPORT_WS`: non-blocking WebSocket on the C1M reactor.
- `CWIST_LIVEMSG_TRANSPORT_WEBRTC`: one peer per DataChannel, with ICE/DTLS/SCTP
  state owned by one process.

Selecting both or an unknown transport fails with `-ENOTSUP` through the serve
wrappers. `create()` returns `NULL` and sets `errno` instead. Both callbacks are
required. `RAW` and `JSON` are the built-in codecs. Protobuf and FlatBuffers
applications use `RAW` with their own serializers, including `cwist_pb_*` for
Protobuf. Rooms and simultaneous transports are outside v1.

`cwist_livemsg_run()` always uses one C1M worker process. It does not inherit
`CWIST_WORKERS`; forked workers cannot share WebRTC ICE/DTLS state.

## Message and result ownership

For each incoming message the runtime creates a 512 KiB-class arena, copies the
wire bytes into it, and keeps that arena through `process()` and `respond()`.
It destroys the arena and response buffer immediately after the callback
sequence. There is no application free callback.

- With `RAW`, `msg` points to the copied bytes and `len` is their exact length.
- With `JSON`, `msg` is a borrowed `cJSON *`; `len` remains the wire length.
- A result that needs independent storage must come from
  `cwist_livemsg_alloc(peer, size)` or point into the decoded input.
- The decoded JSON tree is deleted by the runtime after `respond()`.
- `cwist_livemsg_reply_set_json()` serializes a borrowed tree without deleting
  it. Applications that create another cJSON tree still own that tree.
- Input, results, arena allocations, and the reply handle must not escape the
  callback sequence.

Returning `CWIST_LIVEMSG_NO_REPLY` from either callback, or returning success
with an empty response buffer, sends no packet. Other nonzero callback results
also suppress the response and leave the session available for later messages.

## Replies, push, and backpressure

`cwist_livemsg_reply_write()` grows the opaque response buffer. JSON responses
are text messages; raw responses are binary messages.

`cwist_livemsg_send_raw()` and `cwist_livemsg_send_json()` also support
server-initiated push. Use `cwist_livemsg_peer_ctx()` with
`cwist_livemsg_foreach_peer()` to visit all peers for broadcast. A peer pointer
is valid only until its `on_disconnect()` returns; do not retain it. Calls that
touch peers or iterate the peer list must run on that transport's reactor
thread. WebRTC sends have the underlying thread-safe queue, but the v1 peer
lifetime contract is intentionally reactor-scoped.

Both transports enforce the same limits:

- Maximum incoming or outgoing message: 256 KiB (`-EMSGSIZE` when an API call
  exceeds it). WebSocket closes oversize input with status 1009 and invalid
  JSON with status 1007.
- Maximum queued outgoing bytes per WebSocket peer: 4 MiB. DataChannels on
  one WebRTC connection share that connection's 4 MiB queue. A full queue
  returns `-EAGAIN` immediately; the reactor never waits for a slow consumer.

Argument and state errors use `-EINVAL` or `-ENOTSUP`; allocation and transport
failures use `-ENOMEM` or `-EIO`.

## External signaling and embedding

The one-line serve functions are sufficient for built-in signaling. Applications
that already exchange SDP elsewhere can use the lifecycle API:

```c
cwist_livemsg_config config;
cwist_livemsg_config_init(&config, CWIST_LIVEMSG_TRANSPORT_WEBRTC);
config.signaling_path = NULL;
config.udp_port = 9000;
config.process = process;
config.respond = respond;

cwist_livemsg_ctx *ctx = cwist_livemsg_create(&config);
char *answer = NULL;
if (ctx && cwist_livemsg_webrtc_offer(ctx, offer_sdp, &answer) == 0) {
    /* Return answer through the application's signaling channel. */
    cwist_free(answer);
}
/* cwist_livemsg_run(ctx) may serve an optional HTTP port in this process. */
cwist_livemsg_destroy(ctx);
```

`answer_sdp_out` is allocated with `cwist_alloc()` and must be released with
`cwist_free()`. Creating the context starts its WebRTC reactor, so offers can be
processed before `cwist_livemsg_run()`. Destroy the context only after its run
loop has returned and external users have stopped submitting offers.

## Validation

`make test_livemsg` exercises real WebSocket servers with concurrent clients,
RAW and JSON echo, no-reply behavior, arena results, push, broadcast, message
limits, and send-queue saturation. `make test_livemsg_webrtc` drives external
SDP signaling and a real ICE/DTLS/SCTP loopback channel, including push, a
200000-byte echo, and disconnect cleanup. Both tests run under the project's
sanitizer configurations.
