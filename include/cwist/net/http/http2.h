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
