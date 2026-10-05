#define _POSIX_C_SOURCE 200809L
#include <cwist/net/http/sse.h>
#include <cwist/core/mem/alloc.h>
#include <cwist/core/sstring/sstring.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>

struct cwist_sse_stream { int fd; int closed; pthread_mutex_t mutex; };

/** @brief Wrap a plain integer result as a cwist error value.
 * @param value 0 on success, -1 on failure.
 * @return Error value on the CWIST_ERR_INT16 channel.
 */
static cwist_error_t sse_error(int value) {
    return (cwist_error_t){ .errtype = CWIST_ERR_INT16, .error.err_i16 = value };
}

/** @brief Check an sstring error result and dispose of it.
 *
 * sstring calls report on the INT8 channel and a failed allocation on the
 * JSON channel, so neither shows up in err_i16; check with
 * cwist_error_is_ok() and release the error payload.
 *
 * @param err Error value to check; ownership is always consumed.
 * @return true on success, false on failure.
 */
static bool sse_ok(cwist_error_t err) {
    bool ok = cwist_error_is_ok(&err);
    cwist_error_dispose(&err);
    return ok;
}

/** @brief Append raw bytes to an sstring, translating its error result.
 * @param out Destination string.
 * @param data Bytes to append.
 * @param len Number of bytes in @p data.
 * @return true on success, false on allocation or append failure.
 */
static bool sse_append(cwist_sstring *out, const char *data, size_t len) {
    return sse_ok(cwist_sstring_append_len(out, data, len));
}

/** @brief Append one SSE field line to a frame, splitting multi-line values.
 *
 * Writes "name:value\n" for each line of @p value, splitting on '\n' and
 * stripping a trailing '\r' from each line. A NULL value is written as an
 * empty field.
 *
 * @param out Destination string.
 * @param name Field name; an empty name produces a comment line (":value").
 * @param value Field value, may be NULL or contain newlines.
 * @return 0 on success, -1 if appending fails (out may be partially written).
 */
static int append_field(cwist_sstring *out, const char *name, const char *value) {
    const char *line = value ? value : "";
    do {
        const char *end = strchr(line, '\n');
        size_t len = end ? (size_t)(end - line) : strlen(line);
        if (len && line[len - 1] == '\r') len--;
        if (!sse_append(out, name, strlen(name)) || !sse_append(out, ":", 1) ||
            (len && !sse_append(out, line, len)) || !sse_append(out, "\n", 1))
            return -1;
        line = end ? end + 1 : NULL;
    } while (line);
    return 0;
}

/** @brief Build a complete SSE frame as an sstring.
 *
 * Formats either a comment line (when @p is_comment is set) or the full
 * event block with id/event/retry/data fields followed by the mandatory
 * blank line.
 *
 * @param event Event type field, optional.
 * @param id Event id field, optional.
 * @param retry_ms Reconnection delay in milliseconds, negative to omit.
 * @param data Field payload; for comments this is the comment text.
 * @param is_comment Non-zero to format a comment instead of an event.
 * @return Newly allocated frame on success, NULL on allocation or
 *         formatting failure (the frame is destroyed in that case).
 */
static cwist_sstring *format_event(const char *event, const char *id, int retry_ms,
                                   const char *data, int is_comment) {
    cwist_sstring *frame = cwist_sstring_create();
    if (!frame) return NULL;
    int failed = is_comment ? append_field(frame, "", data) : 0;
    if (!is_comment && id) failed |= append_field(frame, "id", id);
    if (!is_comment && event) failed |= append_field(frame, "event", event);
    if (!is_comment && retry_ms >= 0) {
        char retry[32];
        snprintf(retry, sizeof(retry), "%d", retry_ms);
        failed |= append_field(frame, "retry", retry);
    }
    if (!is_comment) failed |= append_field(frame, "data", data);
    if (failed || !sse_append(frame, "\n", 1)) {
        cwist_sstring_destroy(frame);
        return NULL;
    }
    return frame;
}

/** @brief Prepare a response buffer for a Server-Sent Events reply.
 *
 * Adds the Content-Type, Cache-Control, and X-Accel-Buffering headers
 * required for SSE and enables keep-alive.
 *
 * @param res Response to initialize; must not be NULL.
 * @return 0 on success, -1 if @p res is NULL or a header could not be added.
 */
cwist_error_t cwist_sse_response_init(cwist_http_response *res) {
    if (!res ||
        !sse_ok(cwist_http_header_add(&res->headers, "Content-Type",
                                      "text/event-stream; charset=utf-8")) ||
        !sse_ok(cwist_http_header_add(&res->headers, "Cache-Control", "no-cache")) ||
        !sse_ok(cwist_http_header_add(&res->headers, "X-Accel-Buffering", "no")))
        return sse_error(-1);
    res->keep_alive = true;
    return sse_error(0);
}

/** @brief Append a formatted SSE event to a response buffer.
 *
 * @param res Response whose body receives the frame; must not be NULL.
 * @param event Event type field, optional.
 * @param id Event id field, optional.
 * @param retry_ms Reconnection delay in milliseconds; -1 to omit.
 * @param data Event payload data.
 * @return 0 on success, -1 if @p res is NULL, @p retry_ms is less than -1,
 *         or the frame could not be built or appended.
 */
cwist_error_t cwist_sse_response_event(cwist_http_response *res, const char *event, const char *id,
                                       int retry_ms, const char *data) {
    if (!res || retry_ms < -1) return sse_error(-1);
    cwist_sstring *frame = format_event(event, id, retry_ms, data, 0);
    if (!frame) return sse_error(-1);
    cwist_error_t result = cwist_sstring_append_len(res->body, frame->data, frame->size);
    cwist_sstring_destroy(frame);
    return result;
}

/** @brief Append an SSE comment line to a response buffer.
 * @param res Response whose body receives the comment; must not be NULL.
 * @param comment Comment text.
 * @return 0 on success, -1 if @p res is NULL or the comment frame could not
 *         be built or appended.
 */
cwist_error_t cwist_sse_response_comment(cwist_http_response *res, const char *comment) {
    if (!res) return sse_error(-1);
    cwist_sstring *frame = format_event(NULL, NULL, -1, comment, 1);
    if (!frame) return sse_error(-1);
    cwist_error_t result = cwist_sstring_append_len(res->body, frame->data, frame->size);
    cwist_sstring_destroy(frame);
    return result;
}

/** @brief Append an SSE event described by a cwist_sse_event_t to a response.
 * @param res Response whose body receives the frame.
 * @param event Event descriptor; must not be NULL.
 * @return 0 on success, -1 on failure (see cwist_sse_response_event()).
 */
cwist_error_t cwist_sse_response_write(cwist_http_response *res, const cwist_sse_event_t *event) {
    if (!event) return sse_error(-1);
    return cwist_sse_response_event(res, event->event, event->id, event->retry_ms, event->data);
}

/** @brief Send all bytes on a socket, retrying on EINTR.
 *
 * Suppresses SIGPIPE via MSG_NOSIGNAL. Not thread-safe by itself; callers
 * must serialize access to @p fd.
 *
 * @param fd Socket file descriptor.
 * @param data Bytes to send.
 * @param len Number of bytes in @p data.
 * @return 0 when all bytes were sent, -1 on send error or closed socket.
 */
static int send_all(int fd, const char *data, size_t len) {
    while (len) {
        ssize_t n = send(fd, data, len, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        data += n; len -= (size_t)n;
    }
    return 0;
}

/** @brief Upgrade a request connection to a live SSE stream.
 *
 * Sends the SSE HTTP response headers and marks the request as upgraded,
 * transferring ownership of the connection to the returned stream (release
 * with cwist_sse_stream_close()).
 *
 * @param req Request to upgrade; must not be NULL and must not already be
 *            upgraded.
 * @return New stream on success, NULL if the request is invalid or the
 *         headers could not be sent or the stream could not be allocated.
 */
cwist_sse_stream_t *cwist_sse_stream_open(cwist_http_request *req) {
    static const char headers[] = "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream; charset=utf-8\r\nCache-Control: no-cache\r\nX-Accel-Buffering: no\r\nConnection: keep-alive\r\n\r\n";
    if (!req || req->client_fd < 0 || req->upgraded || send_all(req->client_fd, headers, sizeof(headers) - 1)) return NULL;
    cwist_sse_stream_t *stream = cwist_alloc(sizeof(*stream));
    if (!stream) return NULL;
    stream->fd = req->client_fd; stream->closed = 0;
    pthread_mutex_init(&stream->mutex, NULL);
    req->upgraded = true;
    return stream;
}

/** @brief Send a formatted SSE event on a live stream.
 *
 * Serializes access to the stream with its mutex; a failed send marks the
 * stream as closed.
 *
 * @param stream Stream to write to; must not be NULL.
 * @param event Event type field, optional.
 * @param id Event id field, optional.
 * @param retry_ms Reconnection delay in milliseconds; -1 to omit.
 * @param data Event payload data.
 * @return 0 on success, -1 if the stream or frame is invalid, the stream is
 *         closed, or the send fails.
 */
cwist_error_t cwist_sse_stream_send(cwist_sse_stream_t *stream, const char *event, const char *id,
                                    int retry_ms, const char *data) {
    if (!stream || retry_ms < -1) return sse_error(-1);
    cwist_sstring *frame = format_event(event, id, retry_ms, data, 0);
    if (!frame) return sse_error(-1);
    pthread_mutex_lock(&stream->mutex);
    int rc = stream->closed ? -1 : send_all(stream->fd, frame->data, frame->size);
    if (rc) stream->closed = 1;
    pthread_mutex_unlock(&stream->mutex);
    cwist_sstring_destroy(frame);
    return sse_error(rc);
}

/** @brief Send an SSE comment line on a live stream.
 *
 * Serializes access to the stream with its mutex; a failed send marks the
 * stream as closed.
 *
 * @param stream Stream to write to; must not be NULL.
 * @param comment Comment text.
 * @return 0 on success, -1 if the stream or frame is invalid, the stream is
 *         closed, or the send fails.
 */
cwist_error_t cwist_sse_stream_comment(cwist_sse_stream_t *stream, const char *comment) {
    if (!stream) return sse_error(-1);
    cwist_sstring *frame = format_event(NULL, NULL, -1, comment, 1);
    if (!frame) return sse_error(-1);
    pthread_mutex_lock(&stream->mutex);
    int rc = stream->closed ? -1 : send_all(stream->fd, frame->data, frame->size);
    if (rc) stream->closed = 1;
    pthread_mutex_unlock(&stream->mutex);
    cwist_sstring_destroy(frame);
    return sse_error(rc);
}

/** @brief Send an SSE event described by a cwist_sse_event_t on a stream.
 * @param stream Stream to write to.
 * @param event Event descriptor; must not be NULL.
 * @return 0 on success, -1 on failure (see cwist_sse_stream_send()).
 */
cwist_error_t cwist_sse_stream_write(cwist_sse_stream_t *stream, const cwist_sse_event_t *event) {
    if (!event) return sse_error(-1);
    return cwist_sse_stream_send(stream, event->event, event->id, event->retry_ms, event->data);
}

/** @brief Close a live SSE stream and release its resources.
 *
 * Destroys the stream mutex and frees the stream; the underlying socket
 * remains owned by the upgraded request.
 *
 * @param stream Stream to close; NULL is silently ignored.
 */
void cwist_sse_stream_close(cwist_sse_stream_t *stream) {
    if (!stream) return;
    pthread_mutex_destroy(&stream->mutex);
    cwist_free(stream);
}
