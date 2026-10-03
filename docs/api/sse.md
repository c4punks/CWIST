# Server-Sent Events API

*Header:* `<cwist/net/http/sse.h>`

Helpers for sending `text/event-stream` data. There are two ways to use them:

* **Response mode** (`cwist_sse_response_*`): frames are appended to the
  response body of an ordinary handler. The response is sent when the handler
  returns, so this suits a fixed batch of events.
* **Stream mode** (`cwist_sse_stream_*`): the headers are written to the client
  socket immediately, and each event is sent as soon as it is written. This
  suits events produced over time.

Most functions return a `cwist_error_t`. A call that is rejected for a bad
argument (a `NULL` pointer, or `retry_ms` below `-1`) or fails to build the
frame returns `err_i16 == -1`. The tests check success with
`err.error.err_i16 == 0`.

Every event is written as lines of the form `field:value` followed by a blank
line. The fields appear in the order `id`, `event`, `retry`, `data`. A `NULL`
`event` or `id` is omitted. A `data` string that contains newlines is split
into one `data:` line per line, and a `NULL` `data` produces an empty `data:`
line. There is no space after the colon, so the event
`("update", "42", 1500, "first\nsecond")` is sent as:

```
id:42
event:update
retry:1500
data:first
data:second

```

## Types

### `cwist_sse_event_t`
```c
typedef struct cwist_sse_event {
    const char *event;
    const char *id;
    int retry_ms;
    const char *data;
} cwist_sse_event_t;
```
A structured event for `cwist_sse_response_write` and `cwist_sse_stream_write`.
`NULL` fields are omitted. `retry_ms` is sent when it is `0` or greater and
omitted when it is `-1`.

### `cwist_sse_stream_t`
An opaque handle for an open stream, created by `cwist_sse_stream_open`.

## Response mode

### `cwist_sse_response_init`
```c
cwist_error_t cwist_sse_response_init(cwist_http_response *res);
```
Adds the SSE headers to `res` and sets `res->keep_alive` to `true`. The headers
are:

| Header | Value |
|--------|-------|
| `Content-Type` | `text/event-stream; charset=utf-8` |
| `Cache-Control` | `no-cache` |
| `X-Accel-Buffering` | `no` |

Returns `err_i16 == -1` if `res` is `NULL` or a header cannot be added.

### `cwist_sse_response_event`
```c
cwist_error_t cwist_sse_response_event(cwist_http_response *res, const char *event,
                                       const char *id, int retry_ms, const char *data);
```
Appends one event to the response body. Pass `NULL` for `event` or `id`, and
`-1` for `retry_ms`, to leave that field out. A `retry_ms` below `-1` is
rejected with `err_i16 == -1`.

### `cwist_sse_response_write`
```c
cwist_error_t cwist_sse_response_write(cwist_http_response *res,
                                       const cwist_sse_event_t *event);
```
Same as `cwist_sse_response_event`, taking the fields from a
`cwist_sse_event_t`. Returns `err_i16 == -1` if `event` is `NULL`.

### `cwist_sse_response_comment`
```c
cwist_error_t cwist_sse_response_comment(cwist_http_response *res, const char *comment);
```
Appends a comment line (`:text`) followed by a blank line. Clients ignore
comments, so they are used as heartbeats to keep a connection open.

## Stream mode

### `cwist_sse_stream_open`
```c
cwist_sse_stream_t *cwist_sse_stream_open(cwist_http_request *req);
```
Writes the `200 OK` status line and the SSE headers to `req->client_fd` and
sets `req->upgraded`. When the handler returns, the server sees `req->upgraded`,
writes no response of its own, and does not keep the connection alive, so every
event must be sent before the handler returns. Returns `NULL` if `req` is `NULL`, `req->client_fd` is negative,
`req->upgraded` is already set, or the write fails.

### `cwist_sse_stream_send`
```c
cwist_error_t cwist_sse_stream_send(cwist_sse_stream_t *stream, const char *event,
                                    const char *id, int retry_ms, const char *data);
```
Sends one event on the stream with the same field rules as
`cwist_sse_response_event`. Writes are serialized with a mutex, so one stream
can be used from several threads. If a write fails, the stream is marked closed
and every later send on it returns `err_i16 == -1`.

### `cwist_sse_stream_write`
```c
cwist_error_t cwist_sse_stream_write(cwist_sse_stream_t *stream,
                                     const cwist_sse_event_t *event);
```
Same as `cwist_sse_stream_send`, taking the fields from a `cwist_sse_event_t`.

### `cwist_sse_stream_comment`
```c
cwist_error_t cwist_sse_stream_comment(cwist_sse_stream_t *stream, const char *comment);
```
Sends a comment line on the stream, for example as a heartbeat.

### `cwist_sse_stream_close`
```c
void cwist_sse_stream_close(cwist_sse_stream_t *stream);
```
Frees the stream handle. It does not close the client socket. Passing `NULL` is
allowed.

## Convenience macros

| Macro | Expands to |
|-------|-----------|
| `CWIST_SSE_EVENT(payload)` | a `cwist_sse_event_t` with only `data` set |
| `CWIST_SSE_NAMED(name, payload)` | a `cwist_sse_event_t` with `event` and `data` set |
| `CWIST_SSE_JSON(payload)` | `CWIST_SSE_NAMED("message", payload)` |
| `CWIST_SSE_RESPONSE_SEND(res, event)` | `cwist_sse_response_write(res, &event)` |
| `CWIST_SSE_STREAM_SEND(stream, event)` | `cwist_sse_stream_write(stream, &event)` |

The two `SEND` macros take the event as an lvalue because they take its
address.

## Example

A handler that sends a fixed batch of events (this is the pattern used by
[tutorial 08](../../tutorials/08-sse-realtime/README.md)):

```c
static void handle_events(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    cwist_sse_response_init(res);
    cwist_sse_response_event(res, "message", "1", -1, "Welcome to Live SSE Stream");
    cwist_sse_response_event(res, "update", "2", -1, "Tick 1");
}
```

A handler that streams events directly to the socket. Events are sent while
the handler is still running:

```c
static void handle_stream(cwist_http_request *req, cwist_http_response *res) {
    (void)res;
    cwist_sse_stream_t *stream = cwist_sse_stream_open(req);
    if (!stream) return;

    cwist_sse_event_t ev = CWIST_SSE_NAMED("tick", "1");
    CWIST_SSE_STREAM_SEND(stream, ev);
    cwist_sse_stream_comment(stream, "heartbeat");

    cwist_sse_stream_close(stream);
}
```

See `tests/test_sse.c` for the exact output of each call.
