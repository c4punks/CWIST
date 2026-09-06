#ifndef __CWIST_HTTP2_H__
#define __CWIST_HTTP2_H__

#include <cwist/net/http/http.h>
#include <cwist/net/http/https.h>

typedef void (*cwist_http2_request_handler_func)(void *user_ctx,
                                                 cwist_http_request *req,
                                                 cwist_http_response *res);

cwist_error_t cwist_http2_serve_connection(cwist_https_connection *conn,
                                           void *user_ctx,
                                           cwist_http2_request_handler_func handler);

/**
 * @brief Push a resource to the client over HTTP/2 Server Push.
 *
 * Sends a PUSH_PROMISE frame on the original request stream, then delivers
 * the pushed response headers and body on a newly allocated server-initiated
 * stream.  The caller should ensure HTTP/2 Server Push is enabled on the
 * connection (via SETTINGS_ENABLE_PUSH).
 *
 * @param req            The current HTTP/2 request (must have stream_id and
 *                       private_data populated by the HTTP/2 layer).
 * @param path           The resource path to push (e.g., "/style.css").
 * @param content_type   Optional Content-Type header value (may be NULL).
 * @param data           Response body bytes (may be NULL).
 * @param data_len       Length of @p data.
 * @return 0 on success, -1 on failure.
 */
int cwist_http2_push_resource(cwist_http_request *req,
                              const char *path,
                              const char *content_type,
                              const unsigned char *data,
                              size_t data_len);

/**
 * Immediate outbound writes for hook-taken streams.  Safe to call from the
 * stream handler's own thread; frames bypass the batching delay and are
 * flushed to the socket before returning.
 *
 * cwist_http2_stream_send_headers emits a response HEADERS frame (status is
 * taken from @p status, the pairs carry the remaining fields).
 * cwist_http2_stream_send_data emits DATA frames (chunked to the peer's
 * frame/window limits).  cwist_http2_stream_send_trailers emits a trailer
 * HEADERS frame with END_STREAM.  All return 0 on success, -1 on failure.
 */
int cwist_http2_stream_send_headers(cwist_h2_stream *stream, int status,
                                    const cwist_http2_header *headers, size_t header_count,
                                    int end_stream);
int cwist_http2_stream_send_data(cwist_h2_stream *stream,
                                 const unsigned char *data, size_t len);
int cwist_http2_stream_send_trailers(cwist_h2_stream *stream,
                                     const cwist_http2_header *trailers, size_t trailer_count);

/* --- Async defer completion queue (internal; used by net/http/async.c) ---
 *
 * A handler on the HTTP/2 path may defer its response with
 * cwist_async_defer().  The completing worker thread never writes frames
 * itself (single-threaded HPACK/flow-control invariant); it enqueues the
 * finished exchange on the per-connection queue and pokes the wake fd, and
 * the connection thread drains the queue and emits HEADERS/DATA.  The queue
 * is refcounted: cwist_async_defer acquires a reference so a completion that
 * lands after connection teardown is discarded safely instead of touching
 * freed connection state. */
typedef struct cwist_h2_async_queue cwist_h2_async_queue;

cwist_h2_async_queue *cwist_h2_async_queue_acquire(cwist_h2_async_queue *q);
void cwist_h2_async_queue_release(cwist_h2_async_queue *q);
int cwist_h2_async_queue_enqueue(cwist_h2_async_queue *q, uint32_t stream_id,
                                 cwist_http_request *req,
                                 cwist_http_response *send,
                                 cwist_http_response *res,
                                 bool send_owned);

#ifdef __cplusplus
extern "C" {
#endif

int h2_decode_integer(const unsigned char *buf, size_t len, size_t *pos, uint8_t prefix_bits, uint32_t *value);
char *h2_huffman_decode(const unsigned char *src, size_t src_len, size_t *out_len);
char *h2_decode_string(const unsigned char *buf, size_t len, size_t *pos);

#ifdef __cplusplus
}
#endif

#endif
