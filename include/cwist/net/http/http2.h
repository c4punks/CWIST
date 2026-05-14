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

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Decode an HPACK integer.
 */
int h2_decode_integer(const unsigned char *buf, size_t len, size_t *pos, uint8_t prefix_bits, uint32_t *value);

/**
 * @brief Decode an HPACK huffman-encoded string.
 */
char *h2_huffman_decode(const unsigned char *src, size_t src_len, size_t *out_len);

/**
 * @brief Decode an HPACK string (literal or huffman).
 */
char *h2_decode_string(const unsigned char *buf, size_t len, size_t *pos);

#ifdef __cplusplus
}
#endif

#endif
