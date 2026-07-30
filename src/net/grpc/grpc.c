/**
 * @file grpc.c
 * @brief Unary gRPC over HTTP/2 support.
 */

#define _POSIX_C_SOURCE 200809L
#include <cwist/net/grpc/grpc.h>
#include <cwist/sys/app/app.h>
#include <cwist/core/mem/alloc.h>
#include <ctype.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <zlib.h>

typedef struct cwist_grpc_route {
    char *path;
    int streaming;
    int builtin;
    cwist_grpc_unary_handler_func handler;
    cwist_grpc_stream_handler_func stream_handler;
    void *user_ctx;
    struct cwist_grpc_route *next;
} cwist_grpc_route;

typedef struct cwist_grpc_health_state {
    char *service;
    int serving;
    struct cwist_grpc_health_state *next;
} cwist_grpc_health_state;

static int grpc_register_route(cwist_app *app, const char *service, const char *method,
                               int streaming, cwist_grpc_unary_handler_func unary_handler,
                               cwist_grpc_stream_handler_func stream_handler, void *user_ctx);

static int grpc_content_type_is_grpc(const char *content_type) {
    if (!content_type) return 0;
    return strncmp(content_type, "application/grpc", 16) == 0;
}

/** @brief Look up a registered gRPC route by request path.
 *
 * @param app App owning the route list; may be NULL.
 * @param path Request path to match; may be NULL.
 * @return Matching route, or NULL when no route matches. */
static cwist_grpc_route *grpc_find_route(cwist_app *app, const char *path) {
    if (!app || !path) return NULL;
    cwist_grpc_route *route = (cwist_grpc_route *)app->grpc_routes;
    while (route) {
        if (route->path && strcmp(route->path, path) == 0) return route;
        route = route->next;
    }
    return NULL;
}

/** @brief Current monotonic clock in milliseconds.
 *
 * @return Milliseconds elapsed since an unspecified epoch (CLOCK_MONOTONIC). */
static uint64_t grpc_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

/** @brief Case-insensitively fetch a request header value.
 *
 * @param req Request to search; may be NULL.
 * @param key Header name to match; may be NULL.
 * @return Header value, or NULL when absent or on NULL input. */
static const char *grpc_header_get(cwist_http_request *req, const char *key) {
    if (!req || !key) return NULL;
    for (cwist_http_header_node *h = req->headers; h; h = h->next) {
        if (h->key && h->key->data && h->value && h->value->data &&
            strcasecmp(h->key->data, key) == 0)
            return h->value->data;
    }
    return NULL;
}

/** @brief Parse a gRPC timeout value into milliseconds.
 *
 * Accepts the gRPC "TimeoutValue Unit" form with units H, M, S, m, u, n
 * (hours down to nanoseconds); sub-millisecond values round up so a positive
 * timeout never becomes zero.
 *
 * @param value Timeout string, e.g. "10S" or "500m"; may be NULL.
 * @param out_ms Receives the timeout in milliseconds.
 * @retval 0 Parsed successfully.
 * @retval -1 NULL input, malformed value, or unsupported unit. */
int cwist_grpc_parse_timeout(const char *value, uint64_t *out_ms) {
    if (!value || !out_ms) return -1;
    size_t len = strlen(value);
    if (len < 2 || len > 9) return -1; /* up to 8 digits + unit */
    char unit = value[len - 1];
    uint64_t to_ms_num = 1, to_ms_den = 1;
    switch (unit) {
        case 'H': to_ms_num = 3600000; break;
        case 'M': to_ms_num = 60000; break;
        case 'S': to_ms_num = 1000; break;
        case 'm': to_ms_num = 1; break;
        case 'u':
            to_ms_num = 1;
            to_ms_den = 1000;
            break;
        case 'n':
            to_ms_num = 1;
            to_ms_den = 1000000;
            break;
        default: return -1;
    }
    uint64_t v = 0;
    for (size_t i = 0; i + 1 < len; i++) {
        if (!isdigit((unsigned char)value[i])) return -1;
        v = v * 10 + (uint64_t)(value[i] - '0');
    }
    /* Round sub-millisecond units up so a positive timeout never becomes 0. */
    uint64_t ms = (v * to_ms_num + to_ms_den - 1) / to_ms_den;
    if (v > 0 && ms == 0) ms = 1;
    *out_ms = ms;
    return 0;
}

/** @brief Determine the grpc-encoding declared by the request.
 *
 * @param req Request to inspect.
 * @return 0 for identity/absent, 1 for gzip, -1 for unsupported encodings
 *         (which must be rejected with UNIMPLEMENTED). */
static int grpc_request_encoding(cwist_http_request *req) {
    const char *enc = grpc_header_get(req, "grpc-encoding");
    if (!enc || strcmp(enc, "identity") == 0) return 0;
    if (strcmp(enc, "gzip") == 0) return 1;
    return -1;
}

/** @brief Check whether the client advertised gzip in grpc-accept-encoding.
 *
 * Tolerates whitespace and multiple comma-separated tokens.
 *
 * @param req Request to inspect.
 * @retval 1 gzip is accepted by the client.
 * @retval 0 Header absent or gzip not listed. */
static int grpc_client_accepts_gzip(cwist_http_request *req) {
    const char *ae = grpc_header_get(req, "grpc-accept-encoding");
    if (!ae) return 0;
    while (*ae) {
        while (*ae == ' ' || *ae == '\t' || *ae == ',') ae++;
        if (!*ae) break;
        const char *start = ae;
        while (*ae && *ae != ',' && *ae != ' ' && *ae != '\t') ae++;
        size_t len = (size_t)(ae - start);
        if (len == 4 && strncasecmp(start, "gzip", 4) == 0) return 1;
    }
    return 0;
}

/** @brief Gzip-compress a buffer with zlib.
 *
 * @param in Input bytes; may be NULL when @p in_len is 0.
 * @param in_len Input length in bytes.
 * @param out Receives a newly allocated buffer (caller frees with cwist_free()).
 * @param out_len Receives the compressed length in bytes.
 * @retval 0 Compression succeeded.
 * @retval -1 zlib initialization or compression failure. */
static int grpc_gzip_deflate(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len) {
    z_stream zs;
    memset(&zs, 0, sizeof(zs));
    if (deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 16 + MAX_WBITS, 8,
                     Z_DEFAULT_STRATEGY) != Z_OK)
        return -1;
    size_t cap = deflateBound(&zs, (uLong)in_len) + 32;
    if (cap < 64) cap = 64;
    uint8_t *buf = (uint8_t *)cwist_alloc(cap);
    if (!buf) {
        deflateEnd(&zs);
        return -1;
    }
    zs.next_in = (Bytef *)in;
    zs.avail_in = (uInt)in_len;
    zs.next_out = buf;
    zs.avail_out = (uInt)cap;
    int zrc = deflate(&zs, Z_FINISH);
    if (zrc != Z_STREAM_END) {
        deflateEnd(&zs);
        cwist_free(buf);
        return -1;
    }
    *out_len = cap - zs.avail_out;
    deflateEnd(&zs);
    *out = buf;
    return 0;
}

/** @brief Decompress a gzip buffer with zlib.
 *
 * The output buffer grows geometrically as needed.
 *
 * @param in Compressed input bytes.
 * @param in_len Compressed length in bytes.
 * @param out Receives a newly allocated buffer (caller frees with cwist_free()).
 * @param out_len Receives the decompressed length in bytes.
 * @retval 0 Decompression succeeded.
 * @retval -1 zlib failure, corrupt input, or allocation failure. */
static int grpc_gzip_inflate(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len) {
    z_stream zs;
    memset(&zs, 0, sizeof(zs));
    if (inflateInit2(&zs, 16 + MAX_WBITS) != Z_OK) return -1;
    size_t cap = in_len * 3 + 1024;
    if (cap < 8192) cap = 8192;
    uint8_t *buf = (uint8_t *)cwist_alloc(cap);
    if (!buf) {
        inflateEnd(&zs);
        return -1;
    }
    zs.next_in = (Bytef *)in;
    zs.avail_in = (uInt)in_len;
    size_t used = 0;
    int zrc = Z_OK;
    while (zrc == Z_OK) {
        if (used == cap) {
            cap *= 2;
            uint8_t *nb = (uint8_t *)cwist_realloc(buf, cap);
            if (!nb) {
                cwist_free(buf);
                inflateEnd(&zs);
                return -1;
            }
            buf = nb;
        }
        zs.next_out = buf + used;
        zs.avail_out = (uInt)(cap - used);
        zrc = inflate(&zs, Z_NO_FLUSH);
        used = cap - zs.avail_out;
    }
    inflateEnd(&zs);
    if (zrc != Z_STREAM_END) {
        cwist_free(buf);
        return -1;
    }
    *out = buf;
    *out_len = used;
    return 0;
}

/** @brief Decompress a compressed-flag message according to grpc-encoding.
 *
 * On success the message points at an owned buffer returned in @p owned
 * (caller frees).  Returns 0 on success, -1 when the encoding is
 * unsupported or the payload is corrupt. */
static int grpc_message_inflate(cwist_http_request *req, cwist_grpc_message *message,
                                uint8_t **owned) {
    *owned = NULL;
    if (!message->compressed) return 0;
    if (grpc_request_encoding(req) != 1) return -1;
    uint8_t *plain = NULL;
    size_t plain_len = 0;
    if (grpc_gzip_inflate(message->data, message->len, &plain, &plain_len) != 0) return -1;
    message->data = plain;
    message->len = plain_len;
    message->compressed = 0;
    *owned = plain;
    return 0;
}

/** @brief Map a base64 character to its 6-bit value.
 *
 * @param c Character to decode.
 * @return Value 0-63, or -1 when the character is not in the base64 alphabet. */
static int grpc_b64_val(int c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

/** @brief Decode a base64 string (with optional '=' padding) into bytes.
 *
 * Decoding stops at the first '=' padding character.
 *
 * @param in Base64 input text.
 * @param in_len Input length in bytes.
 * @param out Output buffer.
 * @param out_cap Capacity of @p out in bytes.
 * @param out_len Receives the decoded length in bytes.
 * @retval 0 Decoding succeeded.
 * @retval -1 Invalid character encountered.
 * @retval -2 Output buffer too small. */
static int grpc_base64_decode(const char *in, size_t in_len, uint8_t *out, size_t out_cap,
                              size_t *out_len) {
    uint32_t acc = 0;
    int bits = 0;
    size_t used = 0;
    for (size_t i = 0; i < in_len; i++) {
        if (in[i] == '=') break;
        int v = grpc_b64_val((unsigned char)in[i]);
        if (v < 0) return -1;
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (used >= out_cap) return -2;
            out[used++] = (uint8_t)(acc >> bits);
        }
    }
    *out_len = used;
    return 0;
}

/** @brief Fetch a metadata header value from a gRPC request.
 *
 * @param req Request to search; may be NULL.
 * @param key Header name to match; may be NULL.
 * @return Header value, or NULL when absent or on NULL input. */
const char *cwist_grpc_metadata_get(cwist_http_request *req, const char *key) {
    return grpc_header_get(req, key);
}

/** @brief Decode a binary ("-bin" suffixed) metadata header from base64.
 *
 * @param req Request to search; may be NULL.
 * @param key Metadata key; must end in "-bin".
 * @param out Output buffer for the decoded bytes.
 * @param out_cap Capacity of @p out in bytes.
 * @param out_len Receives the decoded length in bytes.
 * @retval 0 Decoding succeeded.
 * @retval -1 NULL input, key without "-bin" suffix, missing header, or
 *         decode failure (see grpc_base64_decode for -2 overflow). */
int cwist_grpc_metadata_get_binary(cwist_http_request *req, const char *key, uint8_t *out,
                                   size_t out_cap, size_t *out_len) {
    if (!req || !key || !out_len) return -1;
    size_t key_len = strlen(key);
    if (key_len < 4 || strcmp(key + key_len - 4, "-bin") != 0) return -1;
    const char *value = grpc_header_get(req, key);
    if (!value) return -1;
    return grpc_base64_decode(value, strlen(value), out, out_cap, out_len);
}

/** @brief Dispatch a unary gRPC request to its registered handler.
 *
 * Validates content-type, body, and grpc-encoding, decodes (and when needed
 * inflates) the request message, invokes the route handler, then opportunistically
 * gzip-compresses the response body when the client advertised
 * grpc-accept-encoding: gzip.  All protocol errors are written into @p res
 * as gRPC error responses.
 *
 * @param req Incoming unary request.
 * @param res Response object the handler fills in. */
static void grpc_dispatch_unary(cwist_http_request *req, cwist_http_response *res) {
    if (!req || !res || !req->app || !req->path || !req->path->data) return;

    cwist_grpc_route *route = grpc_find_route(req->app, req->path->data);
    if (!route || !route->handler) {
        cwist_grpc_set_error(res, CWIST_GRPC_UNIMPLEMENTED, "gRPC method not registered");
        return;
    }

    const char *ct = cwist_http_header_get(req->headers, "content-type");
    if (!grpc_content_type_is_grpc(ct)) {
        ct = cwist_http_header_get(req->headers, "Content-Type");
    }
    if (!grpc_content_type_is_grpc(ct)) {
        cwist_grpc_set_error(res, CWIST_GRPC_INVALID_ARGUMENT,
                             "content-type must be application/grpc");
        return;
    }

    if (!req->body || !req->body->data) {
        cwist_grpc_set_error(res, CWIST_GRPC_INVALID_ARGUMENT, "missing gRPC request body");
        return;
    }

    cwist_grpc_message message;
    if (cwist_grpc_decode_message(req->body->data, req->body->size, &message) != 0) {
        cwist_grpc_set_error(res, CWIST_GRPC_INVALID_ARGUMENT, "malformed gRPC message frame");
        return;
    }
    if (grpc_request_encoding(req) < 0) {
        cwist_grpc_set_error(res, CWIST_GRPC_UNIMPLEMENTED, "unsupported grpc-encoding");
        return;
    }
    uint8_t *inflated = NULL;
    if (grpc_message_inflate(req, &message, &inflated) != 0) {
        cwist_grpc_set_error(res, CWIST_GRPC_UNIMPLEMENTED, "unsupported compressed gRPC message");
        return;
    }

    route->handler(req, res, &message, route->user_ctx);
    cwist_free(inflated);

    if (grpc_client_accepts_gzip(req) && res->body && res->body->size >= 5) {
        cwist_grpc_message resp_msg;
        if (cwist_grpc_decode_message(res->body->data, res->body->size, &resp_msg) == 0 &&
            !resp_msg.compressed && resp_msg.len > 0) {
            uint8_t *cbuf = NULL;
            size_t clen = 0;
            if (grpc_gzip_deflate(resp_msg.data, resp_msg.len, &cbuf, &clen) == 0) {
                uint8_t *new_frame = NULL;
                size_t new_frame_len = 0;
                if (cwist_grpc_encode_message(cbuf, clen, 1, &new_frame, &new_frame_len) == 0) {
                    cwist_sstring_assign_len(res->body, (char *)new_frame, new_frame_len);
                    cwist_free(new_frame);
                    cwist_http_header_remove(&res->headers, "grpc-encoding");
                    cwist_http_header_add(&res->headers, "grpc-encoding", "gzip");
                }
                cwist_free(cbuf);
            }
        }
    }
}

static int grpc_validate_request(cwist_http_request *req, cwist_http_response *res) {
    const char *ct = cwist_http_header_get(req->headers, "content-type");
    if (!grpc_content_type_is_grpc(ct)) {
        ct = cwist_http_header_get(req->headers, "Content-Type");
    }
    if (!grpc_content_type_is_grpc(ct)) {
        cwist_grpc_set_error(res, CWIST_GRPC_INVALID_ARGUMENT, "content-type must be application/grpc");
        return -1;
    }
    if (!req->body || !req->body->data) {
        cwist_grpc_set_error(res, CWIST_GRPC_INVALID_ARGUMENT, "missing gRPC request body");
        return -1;
    }
    return 0;
}

static void grpc_dispatch_stream(cwist_http_request *req, cwist_http_response *res) {
    if (!req || !res || !req->app || !req->path || !req->path->data) return;

    cwist_grpc_route *route = grpc_find_route(req->app, req->path->data);
    if (!route || !route->stream_handler) {
        cwist_grpc_set_error(res, CWIST_GRPC_UNIMPLEMENTED, "gRPC stream method not registered");
        return;
    }
    if (grpc_validate_request(req, res) != 0) return;

    size_t offset = 0;
    size_t cap = 4;
    size_t count = 0;
    cwist_grpc_message *messages = (cwist_grpc_message *)cwist_alloc(cap * sizeof(*messages));
    if (!messages) {
        cwist_grpc_set_error(res, CWIST_GRPC_RESOURCE_EXHAUSTED, "failed to allocate gRPC stream messages");
        return;
    }

    while (offset < req->body->size) {
        if (count == cap) {
            cap *= 2;
            cwist_grpc_message *next = (cwist_grpc_message *)cwist_realloc(messages, cap * sizeof(*messages));
            if (!next) {
                cwist_free(messages);
                cwist_grpc_set_error(res, CWIST_GRPC_RESOURCE_EXHAUSTED, "failed to grow gRPC stream messages");
                return;
            }
            messages = next;
        }
        if (cwist_grpc_decode_next_message(req->body->data, req->body->size,
                                           &offset, &messages[count]) != 0) {
            cwist_free(messages);
            cwist_grpc_set_error(res, CWIST_GRPC_INVALID_ARGUMENT, "malformed gRPC stream frame");
            return;
        }
        if (messages[count].compressed != 0) {
            cwist_free(messages);
            cwist_grpc_set_error(res, CWIST_GRPC_UNIMPLEMENTED, "compressed gRPC messages are not supported");
            return;
        }
        count++;
    }

    res->status_code = CWIST_HTTP_OK;
    cwist_http_header_add(&res->headers, "content-type", "application/grpc");
    cwist_http_header_add(&res->headers, "grpc-accept-encoding", "identity");
    if (res->body) cwist_sstring_assign(res->body, "");

    cwist_grpc_stream stream = {
        .req = req,
        .res = res,
        .messages = messages,
        .message_count = count,
        .status = CWIST_GRPC_OK,
        .status_message = NULL,
        .closed = 0,
    };
    route->stream_handler(&stream, route->user_ctx);
    cwist_grpc_stream_close(&stream, stream.status, stream.status_message);
    cwist_free(messages);
}

static int grpc_validate_request(cwist_http_request *req, cwist_http_response *res) {
    const char *ct = cwist_http_header_get(req->headers, "content-type");
    if (!grpc_content_type_is_grpc(ct)) {
        ct = cwist_http_header_get(req->headers, "Content-Type");
    }
    if (!grpc_content_type_is_grpc(ct)) {
        cwist_grpc_set_error(res, CWIST_GRPC_INVALID_ARGUMENT, "content-type must be application/grpc");
        return -1;
    }
    if (!req->body || !req->body->data) {
        cwist_grpc_set_error(res, CWIST_GRPC_INVALID_ARGUMENT, "missing gRPC request body");
        return -1;
    }
    return 0;
}

static void grpc_dispatch_stream(cwist_http_request *req, cwist_http_response *res) {
    if (!req || !res || !req->app || !req->path || !req->path->data) return;

    cwist_grpc_route *route = grpc_find_route(req->app, req->path->data);
    if (!route || !route->stream_handler) {
        cwist_grpc_set_error(res, CWIST_GRPC_UNIMPLEMENTED, "gRPC stream method not registered");
        return;
    }
    if (grpc_validate_request(req, res) != 0) return;

    size_t offset = 0;
    size_t cap = 4;
    size_t count = 0;
    cwist_grpc_message *messages = (cwist_grpc_message *)cwist_alloc(cap * sizeof(*messages));
    if (!messages) {
        cwist_grpc_set_error(res, CWIST_GRPC_RESOURCE_EXHAUSTED, "failed to allocate gRPC stream messages");
        return;
    }

    while (offset < req->body->size) {
        if (count == cap) {
            cap *= 2;
            cwist_grpc_message *next = (cwist_grpc_message *)cwist_realloc(messages, cap * sizeof(*messages));
            if (!next) {
                cwist_free(messages);
                cwist_grpc_set_error(res, CWIST_GRPC_RESOURCE_EXHAUSTED, "failed to grow gRPC stream messages");
                return;
            }
            messages = next;
        }
        if (cwist_grpc_decode_next_message(req->body->data, req->body->size,
                                           &offset, &messages[count]) != 0) {
            cwist_free(messages);
            cwist_grpc_set_error(res, CWIST_GRPC_INVALID_ARGUMENT, "malformed gRPC stream frame");
            return;
        }
        if (messages[count].compressed != 0) {
            cwist_free(messages);
            cwist_grpc_set_error(res, CWIST_GRPC_UNIMPLEMENTED, "compressed gRPC messages are not supported");
            return;
        }
        count++;
    }

    res->status_code = CWIST_HTTP_OK;
    cwist_http_header_add(&res->headers, "content-type", "application/grpc");
    cwist_http_header_add(&res->headers, "grpc-accept-encoding", "identity");
    if (res->body) cwist_sstring_assign(res->body, "");

    cwist_grpc_stream stream = {
        .req = req,
        .res = res,
        .messages = messages,
        .message_count = count,
        .status = CWIST_GRPC_OK,
        .status_message = NULL,
        .closed = 0,
    };
    route->stream_handler(&stream, route->user_ctx);
    cwist_grpc_stream_close(&stream, stream.status, stream.status_message);
    cwist_free(messages);
}

int cwist_grpc_decode_message(const void *frame,
                              size_t frame_len,
                              cwist_grpc_message *out) {
    if (!frame || !out || frame_len < 5) return -1;
    const uint8_t *p = (const uint8_t *)frame;
    uint32_t len =
        ((uint32_t)p[1] << 24) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 8) | (uint32_t)p[4];
    if ((size_t)len > frame_len - 5) return -1;
    if ((size_t)len != frame_len - 5) return -1;

    out->compressed = p[0];
    out->data = p + 5;
    out->len = (size_t)len;
    return 0;
}

int cwist_grpc_decode_next_message(const void *frames,
                                   size_t frames_len,
                                   size_t *offset,
                                   cwist_grpc_message *out) {
    if (!frames || !offset || !out || *offset > frames_len) return -1;
    size_t pos = *offset;
    if (frames_len - pos < 5) return -1;

    const uint8_t *p = (const uint8_t *)frames + pos;
    uint32_t len = ((uint32_t)p[1] << 24) |
                   ((uint32_t)p[2] << 16) |
                   ((uint32_t)p[3] << 8) |
                   (uint32_t)p[4];
    if ((size_t)len > frames_len - pos - 5) return -1;

    out->compressed = p[0];
    out->data = p + 5;
    out->len = (size_t)len;
    *offset = pos + 5 + (size_t)len;
    return 0;
}

int cwist_grpc_encode_message(const void *payload,
                              size_t payload_len,
                              uint8_t compressed,
                              uint8_t **out,
                              size_t *out_len) {
    if (!out || !out_len || (payload_len > 0 && !payload)) return -1;
    if (payload_len > UINT32_MAX) return -1;

    uint8_t *buf = (uint8_t *)cwist_alloc(payload_len + 5);
    if (!buf) return -1;
    buf[0] = compressed ? 1 : 0;
    buf[1] = (uint8_t)((payload_len >> 24) & 0xff);
    buf[2] = (uint8_t)((payload_len >> 16) & 0xff);
    buf[3] = (uint8_t)((payload_len >> 8) & 0xff);
    buf[4] = (uint8_t)(payload_len & 0xff);
    if (payload_len > 0) memcpy(buf + 5, payload, payload_len);

    *out = buf;
    *out_len = payload_len + 5;
    return 0;
}

void cwist_grpc_decoder_init(cwist_grpc_decoder *decoder, size_t max_message_size) {
    if (!decoder) return;
    memset(decoder, 0, sizeof(*decoder));
    decoder->max_message_size = max_message_size ? max_message_size : (16u * 1024u * 1024u);
}

void cwist_grpc_decoder_destroy(cwist_grpc_decoder *decoder) {
    if (!decoder) return;
    cwist_free(decoder->payload);
    memset(decoder, 0, sizeof(*decoder));
}

int cwist_grpc_decoder_feed(cwist_grpc_decoder *decoder, const void *data, size_t len,
                            cwist_grpc_message_callback callback, void *ctx) {
    if (!decoder || (!data && len) || !callback) return -1;
    const uint8_t *input = data;
    while (len) {
        if (decoder->header_len < sizeof(decoder->header)) {
            size_t take = sizeof(decoder->header) - decoder->header_len;
            if (take > len) take = len;
            memcpy(decoder->header + decoder->header_len, input, take);
            decoder->header_len += take;
            input += take;
            len -= take;
            if (decoder->header_len < sizeof(decoder->header)) continue;
            decoder->compressed = decoder->header[0];
            decoder->payload_len = ((size_t)decoder->header[1] << 24) |
                                   ((size_t)decoder->header[2] << 16) |
                                   ((size_t)decoder->header[3] << 8) |
                                   (size_t)decoder->header[4];
            if (decoder->compressed > 1 || decoder->payload_len > decoder->max_message_size)
                return -1;
            if (decoder->payload_len) {
                decoder->payload = cwist_alloc(decoder->payload_len);
                if (!decoder->payload) return -1;
            }
        }
        size_t take = decoder->payload_len - decoder->payload_used;
        if (take > len) take = len;
        if (take) memcpy(decoder->payload + decoder->payload_used, input, take);
        decoder->payload_used += take;
        input += take;
        len -= take;
        if (decoder->payload_used != decoder->payload_len) continue;
        cwist_grpc_message message = { decoder->compressed, decoder->payload, decoder->payload_len };
        if (callback(ctx, &message) != 0) return -1;
        cwist_free(decoder->payload);
        decoder->payload = NULL;
        decoder->payload_len = decoder->payload_used = decoder->header_len = 0;
    }
    return 0;
}

void cwist_grpc_set_response(cwist_http_response *res,
                             cwist_grpc_status_t status,
                             const char *message,
                             const void *payload,
                             size_t payload_len) {
    if (!res) return;

    res->status_code = CWIST_HTTP_OK;
    cwist_http_header_add(&res->headers, "content-type", "application/grpc");
    cwist_http_header_add(&res->headers, "grpc-accept-encoding", "gzip, identity");

    char status_buf[16];
    snprintf(status_buf, sizeof(status_buf), "%d", (int)status);
    cwist_http_header_add(&res->headers, "grpc-status", status_buf);
    if (message) {
        cwist_http_header_add(&res->headers, "grpc-message", message);
    }

    uint8_t *frame = NULL;
    size_t frame_len = 0;
    if (cwist_grpc_encode_message(payload, payload_len, 0, &frame, &frame_len) == 0) {
        cwist_sstring_assign_len(res->body, (char *)frame, frame_len);
        cwist_free(frame);
    } else {
        res->status_code = CWIST_HTTP_INTERNAL_ERROR;
        cwist_sstring_assign(res->body, "");
        cwist_http_header_add(&res->headers, "grpc-status", "13");
        cwist_http_header_add(&res->headers, "grpc-message", "failed to encode gRPC response");
    }
}

/** @brief Build a buffered gRPC error response with an empty message.
 *
 * Convenience wrapper over cwist_grpc_set_response() with no payload.
 *
 * @param res Response object to fill.
 * @param status gRPC status code.
 * @param message Status message for the grpc-message header; may be NULL. */
void cwist_grpc_set_error(cwist_http_response *res, cwist_grpc_status_t status,
                          const char *message) {
    cwist_grpc_set_response(res, status, message, NULL, 0);
}

int cwist_grpc_stream_send(cwist_grpc_stream *stream,
                           const void *payload,
                           size_t payload_len) {
    if (!stream || !stream->res || !stream->res->body) return -1;
    uint8_t *frame = NULL;
    size_t frame_len = 0;
    if (cwist_grpc_encode_message(payload, payload_len, 0, &frame, &frame_len) != 0) {
        stream->status = CWIST_GRPC_INTERNAL;
        stream->status_message = "failed to encode gRPC stream message";
        return -1;
    }
    if (stream->write_frame && stream->write_frame(stream->write_frame_ctx, frame, frame_len, 0) != 0) {
        cwist_free(frame);
        stream->status = CWIST_GRPC_UNAVAILABLE;
        stream->status_message = "failed to write gRPC DATA frame";
        return -1;
    }
    cwist_error_t err = cwist_sstring_append_len(stream->res->body, (char *)frame, frame_len);
    cwist_free(frame);
    if (err.error.err_i16 != 0) {
        stream->status = CWIST_GRPC_INTERNAL;
        stream->status_message = "failed to append gRPC stream message";
        return -1;
    }
    return 0;
}

void cwist_grpc_stream_set_writer(cwist_grpc_stream *stream,
                                  int (*write_frame)(void *, const uint8_t *, size_t, int),
                                  void *ctx) {
    if (!stream) return;
    stream->write_frame = write_frame;
    stream->write_frame_ctx = ctx;
}

void cwist_grpc_stream_close(cwist_grpc_stream *stream,
                             cwist_grpc_status_t status,
                             const char *message) {
    if (!stream || !stream->res) return;
    if (stream->closed) return;
    stream->status = status;
    stream->status_message = message;
    stream->closed = 1;

    char status_buf[16];
    snprintf(status_buf, sizeof(status_buf), "%d", (int)status);
    cwist_http_header_add(&stream->res->headers, "grpc-status", status_buf);
    if (message) {
        cwist_http_header_add(&stream->res->headers, "grpc-message", message);
    }
}

static int grpc_health_status(const cwist_grpc_health_state *state, const char *service) {
    for (const cwist_grpc_health_state *it = state; it; it = it->next)
        if (it->service && strcmp(it->service, service ? service : "") == 0)
            return it->serving ? 1 : 2;
    return service && *service ? 3 : 1; /* SERVICE_UNKNOWN, or overall SERVING */
}

static void grpc_health_reply(cwist_http_response *res, cwist_grpc_health_state *state,
                              const cwist_grpc_message *message) {
    const char *service = "";
    char *owned = NULL;
    cwist_pb_reader reader;
    cwist_pb_reader_init(&reader, message->data, message->len);
    cwist_pb_field field;
    while (cwist_pb_read_field(&reader, &field) > 0)
        if (field.number == 1 && field.wire_type == CWIST_PB_LEN) {
            owned = cwist_alloc(field.len + 1);
            if (!owned) { cwist_grpc_set_error(res, CWIST_GRPC_RESOURCE_EXHAUSTED, "health allocation failed"); return; }
            memcpy(owned, field.bytes, field.len); owned[field.len] = '\0'; service = owned; break;
        }
    cwist_pb_writer writer;
    cwist_pb_writer_init(&writer);
    if (cwist_pb_write_uint64_field(&writer, 1, (uint64_t)grpc_health_status(state, service)) != 0)
        cwist_grpc_set_error(res, CWIST_GRPC_INTERNAL, "health encoding failed");
    else
        cwist_grpc_set_response(res, CWIST_GRPC_OK, NULL, writer.data, writer.len);
    cwist_pb_writer_free(&writer);
    cwist_free(owned);
}

static void grpc_health_check(cwist_http_request *req, cwist_http_response *res,
                              const cwist_grpc_message *message, void *ctx) {
    (void)req;
    grpc_health_reply(res, ctx, message);
}

static void grpc_health_watch(cwist_grpc_stream *stream, void *ctx) {
    cwist_grpc_message empty = { 0, NULL, 0 };
    const cwist_grpc_message *message = stream->message_count ? &stream->messages[0] : &empty;
    cwist_http_response *res = stream->res;
    grpc_health_reply(res, ctx, message);
}

int cwist_app_grpc_health(cwist_app *app) {
    if (!app) return -1;
    cwist_grpc_health_state *state = cwist_alloc(sizeof(*state));
    if (!state) return -1;
    memset(state, 0, sizeof(*state));
    if (grpc_register_route(app, "grpc.health.v1.Health", "Check", 0,
                            grpc_health_check, NULL, state) != 0 ||
        grpc_register_route(app, "grpc.health.v1.Health", "Watch", 1,
                            NULL, grpc_health_watch, state) != 0) {
        cwist_free(state);
        return -1;
    }
    cwist_grpc_route *route = grpc_find_route(app, "/grpc.health.v1.Health/Check");
    if (route) route->builtin = 1;
    route = grpc_find_route(app, "/grpc.health.v1.Health/Watch");
    if (route) route->builtin = 2;
    return 0;
}

int cwist_app_grpc_health_set_status(cwist_app *app, const char *service, int serving) {
    cwist_grpc_route *route = grpc_find_route(app, "/grpc.health.v1.Health/Check");
    if (!route || !route->user_ctx || !service) return -1;
    cwist_grpc_health_state *state = route->user_ctx;
    for (cwist_grpc_health_state *it = state; it; it = it->next)
        if (it->service && strcmp(it->service, service) == 0) { it->serving = !!serving; return 0; }
    cwist_grpc_health_state *item = cwist_alloc(sizeof(*item));
    if (!item) return -1;
    item->service = cwist_alloc(strlen(service) + 1);
    if (!item->service) { cwist_free(item); return -1; }
    strcpy(item->service, service); item->serving = !!serving; item->next = state->next; state->next = item;
    return 0;
}

static int grpc_reflection_append_service(cwist_pb_writer *response, const char *service) {
    cwist_pb_writer item, list;
    cwist_pb_writer_init(&item); cwist_pb_writer_init(&list);
    int rc = cwist_pb_write_string_field(&item, 1, service) ||
             cwist_pb_write_bytes_field(&list, 1, item.data, item.len) ||
             cwist_pb_write_bytes_field(response, 6, list.data, list.len);
    cwist_pb_writer_free(&item); cwist_pb_writer_free(&list);
    return rc ? -1 : 0;
}

static void grpc_reflection_info(cwist_grpc_stream *stream, void *ctx) {
    (void)ctx;
    cwist_pb_writer response;
    cwist_pb_writer_init(&response);
    cwist_grpc_route *route = (cwist_grpc_route *)stream->req->app->grpc_routes;
    for (; route; route = route->next) {
        if (!route->path || route->path[0] != '/') continue;
        const char *slash = strrchr(route->path + 1, '/');
        if (!slash) continue;
        size_t len = (size_t)(slash - route->path - 1);
        char *service = cwist_alloc(len + 1);
        if (!service) { stream->status = CWIST_GRPC_RESOURCE_EXHAUSTED; break; }
        memcpy(service, route->path + 1, len); service[len] = '\0';
        if (grpc_reflection_append_service(&response, service) != 0) stream->status = CWIST_GRPC_INTERNAL;
        cwist_free(service);
        if (stream->status != CWIST_GRPC_OK) break;
    }
    if (stream->status == CWIST_GRPC_OK) cwist_grpc_stream_send(stream, response.data, response.len);
    cwist_pb_writer_free(&response);
}

int cwist_app_grpc_reflection(cwist_app *app) {
    if (grpc_register_route(app, "grpc.reflection.v1alpha.ServerReflection", "ServerReflectionInfo",
                            1, NULL, grpc_reflection_info, NULL) != 0) return -1;
    cwist_grpc_route *route = grpc_find_route(app,
        "/grpc.reflection.v1alpha.ServerReflection/ServerReflectionInfo");
    if (route) route->builtin = 3;
    return 0;
}

static int grpc_register_route(cwist_app *app,
                               const char *service,
                               const char *method,
                               int streaming,
                               cwist_grpc_unary_handler_func unary_handler,
                               cwist_grpc_stream_handler_func stream_handler,
                               void *user_ctx) {
    if (!app || !service || !method) return -1;
    if ((!streaming && !unary_handler) || (streaming && !stream_handler)) return -1;
    size_t service_len = strlen(service);
    size_t method_len = strlen(method);
    if (service_len == 0 || method_len == 0) return -1;

    size_t path_len = service_len + method_len + 3;
    char *path = (char *)cwist_alloc(path_len);
    if (!path) return -1;
    snprintf(path, path_len, "/%s/%s", service, method);

    cwist_grpc_route *existing = grpc_find_route(app, path);
    if (existing) {
        existing->streaming = streaming;
        existing->handler = unary_handler;
        existing->stream_handler = stream_handler;
        existing->user_ctx = user_ctx;
        cwist_free(path);
        return 0;
    }

    cwist_grpc_route *route = (cwist_grpc_route *)cwist_alloc(sizeof(*route));
    if (!route) {
        cwist_free(path);
        return -1;
    }
    route->path = path;
    route->streaming = streaming;
    route->handler = unary_handler;
    route->stream_handler = stream_handler;
    route->user_ctx = user_ctx;
    route->next = (cwist_grpc_route *)app->grpc_routes;
    app->grpc_routes = route;

    cwist_app_post(app, path, streaming ? grpc_dispatch_stream : grpc_dispatch_unary);
    return 0;
}

int cwist_app_grpc_unary(cwist_app *app,
                         const char *service,
                         const char *method,
                         cwist_grpc_unary_handler_func handler,
                         void *user_ctx) {
    return grpc_register_route(app, service, method, 0, handler, NULL, user_ctx);
}

int cwist_app_grpc_stream(cwist_app *app,
                          const char *service,
                          const char *method,
                          cwist_grpc_stream_handler_func handler,
                          void *user_ctx) {
    return grpc_register_route(app, service, method, 1, NULL, handler, user_ctx);
}

void cwist_grpc_routes_destroy(cwist_app *app) {
    if (!app) return;
    cwist_grpc_route *route = (cwist_grpc_route *)app->grpc_routes;
    while (route) {
        cwist_grpc_route *next = route->next;
        if (route->builtin == 1 && route->user_ctx) {
            cwist_grpc_health_state *state = route->user_ctx;
            while (state) {
                cwist_grpc_health_state *state_next = state->next;
                cwist_free(state->service);
                cwist_free(state);
                state = state_next;
            }
        }
        cwist_free(route->path);
        cwist_free(route);
        route = next;
    }
    app->grpc_routes = NULL;
}

/** @brief Copy all gRPC routes (and built-in service state) to another app.
 *
 * Re-registers every route from @p src on @p dst.  The built-in health
 * service is re-created on @p dst with its recorded statuses, and the
 * reflection service is re-registered.  Routes already present on @p dst
 * with the same path are replaced by grpc_register_route().
 *
 * @retval 0 All routes cloned.
 * @retval -1 NULL argument, allocation failure, malformed route path, or a
 *         registration failure part-way through (dst may hold partial state). */
int cwist_grpc_routes_clone(cwist_app *dst, const cwist_app *src) {
    if (!dst || !src) return -1;
    const cwist_grpc_route *route = (const cwist_grpc_route *)src->grpc_routes;
    while (route) {
        if (route->builtin == 1) {
            if (cwist_app_grpc_health(dst) != 0) return -1;
            const cwist_grpc_health_state *state = route->user_ctx;
            for (const cwist_grpc_health_state *it = state; it; it = it->next)
                if (it->service && cwist_app_grpc_health_set_status(dst, it->service, it->serving) != 0)
                    return -1;
            route = route->next;
            continue;
        }
        if (route->builtin == 2) { route = route->next; continue; }
        if (route->builtin == 3) {
            if (cwist_app_grpc_reflection(dst) != 0) return -1;
            route = route->next;
            continue;
        }
        const char *path = route->path;
        if (!path || path[0] != '/') return -1;
        const char *method_sep = strrchr(path + 1, '/');
        if (!method_sep || method_sep == path + 1 || method_sep[1] == '\0') return -1;

        size_t service_len = (size_t)(method_sep - (path + 1));
        char *service CWIST_DEFER_FREE = (char *)cwist_alloc(service_len + 1);
        if (!service) return -1;
        memcpy(service, path + 1, service_len);
        service[service_len] = '\0';

        int rc = route->streaming
            ? cwist_app_grpc_stream(dst, service, method_sep + 1,
                                    route->stream_handler, route->user_ctx)
            : cwist_app_grpc_unary(dst, service, method_sep + 1,
                                   route->handler, route->user_ctx);
        cwist_free(service);
        if (rc != 0) return -1;
        route = route->next;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Incremental HTTP/2 streaming session engine                         */
/* ------------------------------------------------------------------ */

typedef struct cwist_grpc_h2_conn_ctx {
    cwist_app *app;
    pthread_mutex_t mu;             /* sessions list + refs */
    int refs;                       /* connection + one per session */
    cwist_grpc_session *sessions;
} cwist_grpc_h2_conn_ctx;

/** @brief Drop one reference on a connection context, freeing it at zero.
 *
 * Thread-safe; destroys the mutex and frees the context on the last release.
 *
 * @param ctx Context to release; must be non-NULL. */
static void grpc_conn_ctx_release(cwist_grpc_h2_conn_ctx *ctx) {
    int last;
    pthread_mutex_lock(&ctx->mu);
    last = (--ctx->refs == 0);
    pthread_mutex_unlock(&ctx->mu);
    if (last) {
        pthread_mutex_destroy(&ctx->mu);
        cwist_free(ctx);
    }
}

/** @brief Drop one reference on a streaming session, freeing it at zero.
 *
 * Removes the session from its connection's session list, releases the
 * connection context, and frees the decoder, queued messages, receive buffer,
 * embedded request, and locks.  Thread-safe via the session's atomic refcount.
 *
 * @param session Session to release; must be non-NULL. */
static void grpc_session_release(cwist_grpc_session *session) {
    if (atomic_fetch_sub(&session->refs, 1) != 1) return;
    cwist_grpc_h2_conn_ctx *ctx = session->conn_ctx;
    pthread_mutex_lock(&ctx->mu);
    cwist_grpc_session **pp = &ctx->sessions;
    while (*pp && *pp != session) pp = &(*pp)->next;
    if (*pp) *pp = session->next;
    pthread_mutex_unlock(&ctx->mu);
    grpc_conn_ctx_release(ctx);

    cwist_grpc_decoder_destroy(&session->decoder);
    while (session->qhead) {
        grpc_qnode *next = session->qhead->next;
        cwist_free(session->qhead->data);
        cwist_free(session->qhead);
        session->qhead = next;
    }
    cwist_free(session->recv_buf);
    if (session->stream.req) cwist_http_request_destroy(session->stream.req);
    pthread_mutex_destroy(&session->mu);
    pthread_cond_destroy(&session->cond);
    pthread_mutex_destroy(&session->wmu);
    cwist_free(session);
}

/** @brief Queue a decoded inbound message for the handler thread.
 *
 * Takes session->mu.  When the session is already cancelled or at EOF the
 * data is freed instead of queued.
 *
 * @param session Session whose inbound queue receives the message.
 * @param data Owned message buffer (freed here on failure).
 * @param len Message length in bytes.
 * @retval 0 Queued; the session now owns @p data.
 * @retval -1 Session ended; @p data and the queue node were freed. */
static int grpc_session_push(cwist_grpc_session *session, uint8_t *data, size_t len) {
    grpc_qnode *node = (grpc_qnode *)cwist_alloc(sizeof(*node));
    if (!node) return -1;
    node->data = data;
    node->len = len;
    node->next = NULL;
    pthread_mutex_lock(&session->mu);
    if (session->cancelled || session->eof) {
        pthread_mutex_unlock(&session->mu);
        cwist_free(data);
        cwist_free(node);
        return -1;
    }
    if (session->qtail)
        session->qtail->next = node;
    else
        session->qhead = node;
    session->qtail = node;
    pthread_cond_signal(&session->cond);
    pthread_mutex_unlock(&session->mu);
    return 0;
}

/** @brief Decoder callback: take ownership of a decoded message and queue it.
 *
 * Inflates compressed messages when the session negotiated gzip.  The queued
 * buffer is always freshly allocated and owned by the session queue.
 *
 * @param ctx cwist_grpc_session.
 * @param message Decoded message (payload borrowed from the decoder).
 * @retval 0 Message queued.
 * @retval -1 Unsupported encoding, corrupt payload, allocation failure, or
 *         the session ended; the caller (decoder feed) then fails the call. */
static int grpc_session_decoded(void *ctx, const cwist_grpc_message *message) {
    cwist_grpc_session *session = ctx;
    uint8_t *copy;
    size_t len;
    if (message->compressed) {
        if (session->encoding != 1) return -1; /* rejected by on_data */
        if (grpc_gzip_inflate(message->data, message->len, &copy, &len) != 0) return -1;
    } else {
        copy = (uint8_t *)cwist_alloc(message->len ? message->len : 1);
        if (!copy) return -1;
        if (message->len) memcpy(copy, message->data, message->len);
        len = message->len;
    }
    if (grpc_session_push(session, copy, len) != 0) return -1;
    return 0;
}

/** @brief Fail the call: cancel the handler and emit error trailers.
 *
 * Marks the session and its public stream cancelled, wakes any blocked
 * receiver, and sends trailers carrying @p status/@p message.  Safe to call
 * from the transport thread; takes session->mu and session->wmu.
 *
 * @param session Session to fail.
 * @param status gRPC status for the trailers and stream state.
 * @param message Status message; may be NULL. */
static void grpc_session_fail(cwist_grpc_session *session, cwist_grpc_status_t status,
                              const char *message) {
    pthread_mutex_lock(&session->mu);
    session->cancelled = 1;
    session->stream.cancelled = 1;
    session->stream.status = status;
    session->stream.status_message = message;
    pthread_cond_broadcast(&session->cond);
    pthread_mutex_unlock(&session->mu);
    grpc_session_send_trailers(session, status, message);
}

/** @brief Send the delayed Response-Headers if they have not gone out yet.
 *
 * Emits the content-type/grpc-accept-encoding (and grpc-encoding when the
 * response is gzipped) HEADERS block with the 200 status.
 *
 * Caller holds session->wmu.
 *
 * @retval 0 Headers were already sent, or went out now.
 * @retval -1 Transport detached (session->h2s NULL) or the write failed. */
static int grpc_session_send_headers_locked(cwist_grpc_session *session) {
    if (session->headers_sent) return 0;
    if (!session->h2s) return -1;
    cwist_http2_header hdrs[3];
    size_t count = 0;
    hdrs[count++] = (cwist_http2_header){"content-type", "application/grpc"};
    hdrs[count++] = (cwist_http2_header){"grpc-accept-encoding", "gzip, identity"};
    if (session->resp_encoding == 1) hdrs[count++] = (cwist_http2_header){"grpc-encoding", "gzip"};
    if (cwist_http2_stream_send_headers(session->h2s, 200, hdrs, count, 0) != 0) return -1;
    session->headers_sent = 1;
    return 0;
}

/** @brief Emit the terminal trailers (or a Trailers-Only HEADERS block).
 *
 * Idempotent per session.  When the response headers never went out and the
 * status is not OK, the whole response collapses into a single Trailers-Only
 * HEADERS frame (gRFC A6) so conforming clients may retry.  Otherwise the
 * delayed Response-Headers are flushed first, followed by a trailers block
 * with grpc-status/grpc-message (and grpc-retry-pushback-ms when set).
 *
 * @param session Session to terminate.
 * @param status Final gRPC status.
 * @param message Final status message; may be NULL. */
static void grpc_session_send_trailers(cwist_grpc_session *session, cwist_grpc_status_t status,
                                       const char *message) {
    char status_buf[16];
    snprintf(status_buf, sizeof(status_buf), "%d", (int)status);
    char pushback_buf[20];
    if (session->stream.retry_pushback_set)
        snprintf(pushback_buf, sizeof(pushback_buf), "%d", (int)session->stream.retry_pushback_ms);
    pthread_mutex_lock(&session->wmu);
    if (session->h2s && !session->trailers_sent) {
        if (!session->headers_sent && status != CWIST_GRPC_OK) {
            /* Trailers-Only (PROTOCOL-HTTP2, gRFC A6): the whole response is
             * one HEADERS frame with END_STREAM.  Response-Headers never went
             * out, so the RPC stays retryable for conforming clients. */
            cwist_http2_header hdrs[6];
            size_t count = 0;
            hdrs[count++] = (cwist_http2_header){"content-type", "application/grpc"};
            hdrs[count++] = (cwist_http2_header){"grpc-accept-encoding", "gzip, identity"};
            hdrs[count++] = (cwist_http2_header){"grpc-status", status_buf};
            if (message) hdrs[count++] = (cwist_http2_header){"grpc-message", message};
            if (session->stream.retry_pushback_set)
                hdrs[count++] = (cwist_http2_header){"grpc-retry-pushback-ms", pushback_buf};
            if (cwist_http2_stream_send_headers(session->h2s, 200, hdrs, count, 1) == 0)
                session->headers_sent = 1;
            session->trailers_sent = 1;
        } else if (grpc_session_send_headers_locked(session) == 0) {
            cwist_http2_header pairs[3];
            size_t count = 0;
            pairs[count].name = "grpc-status";
            pairs[count].value = status_buf;
            count++;
            if (message) {
                pairs[count].name = "grpc-message";
                pairs[count].value = message;
                count++;
            }
            if (session->stream.retry_pushback_set) {
                pairs[count].name = "grpc-retry-pushback-ms";
                pairs[count].value = pushback_buf;
                count++;
            }
            (void)cwist_http2_stream_send_trailers(session->h2s, pairs, count);
            session->trailers_sent = 1;
        } else {
            session->trailers_sent = 1; /* transport broken; do not retry */
        }
    }
    pthread_mutex_unlock(&session->wmu);
}

/** @brief write_frame hook: emit a framed message as an HTTP/2 DATA frame.
 *
 * Flushes the delayed Response-Headers first, then sends @p frame.  @p
 * end_stream is ignored; stream termination happens via the trailers path.
 *
 * @param ctx cwist_grpc_session.
 * @param frame Framed gRPC message bytes.
 * @param frame_len Frame length in bytes.
 * @param end_stream Unused.
 * @retval 0 DATA frame sent.
 * @retval -1 Transport detached or trailers already sent. */
static int grpc_session_write_frame(void *ctx, const uint8_t *frame, size_t frame_len,
                                    int end_stream) {
    (void)end_stream;
    cwist_grpc_session *session = ctx;
    pthread_mutex_lock(&session->wmu);
    int rc = -1;
    if (session->h2s && !session->trailers_sent && grpc_session_send_headers_locked(session) == 0)
        rc = cwist_http2_stream_send_data(session->h2s, frame, frame_len);
    pthread_mutex_unlock(&session->wmu);
    return rc;
}

/** @brief Handler thread main: run the streaming handler, then finalize.
 *
 * Invokes the route's stream handler; if the handler returned without closing
 * the stream, closes it with the stream's current status.  Releases the
 * session reference on exit.
 *
 * @param arg cwist_grpc_session.
 * @return Always NULL. */
static void *grpc_session_thread(void *arg) {
    cwist_grpc_session *session = arg;
    session->handler(&session->stream, session->user_ctx);
    if (!session->stream.closed) {
        cwist_grpc_stream_close(&session->stream, session->stream.status,
                                session->stream.status_message);
    }
    grpc_session_release(session);
    return NULL;
}

/** @brief Receive the next inbound message on a stream.
 *
 * Buffered path: pops the next pre-decoded message, enforcing the deadline
 * first.  Transport path: blocks on the session queue until a message
 * arrives, the call is cancelled, EOF is reached, or the deadline expires.
 *
 * @param stream Stream to read from; may be NULL.
 * @param out Receives the message (borrowed pointer valid until the next
 *        recv on this stream).
 * @retval 1 A message was delivered.
 * @retval 0 End of stream (no more messages).
 * @retval -1 NULL argument, cancellation, or deadline exceeded (stream
 *         status set to DEADLINE_EXCEEDED in the latter case). */
int cwist_grpc_stream_recv(cwist_grpc_stream *stream, cwist_grpc_message *out) {
    if (!stream || !out) return -1;

    if (!stream->session) {
        /* Buffered path: pop the pre-decoded message array. */
        if (stream->deadline_ms && grpc_now_ms() >= stream->deadline_ms) {
            stream->cancelled = 1;
            return -1;
        }
        if (stream->recv_pos >= stream->message_count) return 0;
        *out = stream->messages[stream->recv_pos++];
        return 1;
    }

    cwist_grpc_session *session = stream->session;
    pthread_mutex_lock(&session->mu);
    for (;;) {
        if (session->qhead) {
            grpc_qnode *node = session->qhead;
            session->qhead = node->next;
            if (!session->qhead) session->qtail = NULL;
            pthread_mutex_unlock(&session->mu);
            cwist_free(session->recv_buf);
            session->recv_buf = node->data;
            out->compressed = 0;
            out->data = node->data;
            out->len = node->len;
            cwist_free(node);
            return 1;
        }
        if (session->cancelled) {
            pthread_mutex_unlock(&session->mu);
            return -1;
        }
        if (session->eof) {
            pthread_mutex_unlock(&session->mu);
            return 0;
        }
        if (stream->deadline_ms) {
            uint64_t now = grpc_now_ms();
            if (now >= stream->deadline_ms) {
                session->cancelled = 1;
                stream->cancelled = 1;
                /* Report the real outcome so a handler that simply closes
                 * with stream->status still emits DEADLINE_EXCEEDED. */
                stream->status = CWIST_GRPC_DEADLINE_EXCEEDED;
                stream->status_message = "deadline exceeded";
                pthread_mutex_unlock(&session->mu);
                return -1;
            }
            uint64_t left = stream->deadline_ms - now;
            struct timespec ts;
            struct timespec rt;
            clock_gettime(CLOCK_REALTIME, &rt);
            ts.tv_sec = rt.tv_sec + (time_t)(left / 1000);
            ts.tv_nsec = rt.tv_nsec + (long)((left % 1000) * 1000000);
            if (ts.tv_nsec >= 1000000000L) {
                ts.tv_sec++;
                ts.tv_nsec -= 1000000000L;
            }
            pthread_cond_timedwait(&session->cond, &session->mu, &ts);
        } else {
            pthread_cond_wait(&session->cond, &session->mu);
        }
    }
}

/** @brief Check whether a stream has been cancelled.
 *
 * Reads the session state under the session lock when backed by a transport.
 *
 * @param stream Stream to check; may be NULL (treated as not cancelled).
 * @retval 1 Cancelled.
 * @retval 0 Still active. */
int cwist_grpc_stream_cancelled(cwist_grpc_stream *stream) {
    if (!stream) return 0;
    if (stream->cancelled) return 1;
    if (stream->session) {
        cwist_grpc_session *session = stream->session;
        pthread_mutex_lock(&session->mu);
        int cancelled = session->cancelled;
        pthread_mutex_unlock(&session->mu);
        return cancelled;
    }
    return 0;
}

/** @brief Milliseconds remaining until the stream deadline.
 *
 * @param stream Stream to inspect; may be NULL (treated as no deadline).
 * @return Milliseconds left, 0 when the deadline has passed, or UINT64_MAX
 *         when no deadline was set. */
uint64_t cwist_grpc_stream_deadline_remaining_ms(cwist_grpc_stream *stream) {
    if (!stream || !stream->deadline_ms) return UINT64_MAX;
    uint64_t now = grpc_now_ms();
    return now >= stream->deadline_ms ? 0 : stream->deadline_ms - now;
}

/* --- HTTP/2 stream hooks --- */

/** @brief on_conn_open hook: allocate the per-connection gRPC context.
 *
 * @param user_ctx cwist_app served by this connection.
 * @return Newly allocated context with a single reference, or NULL on
 *         allocation failure (connection then proceeds without gRPC streams). */
static void *grpc_h2_on_conn_open(void *user_ctx) {
    cwist_grpc_h2_conn_ctx *ctx = (cwist_grpc_h2_conn_ctx *)cwist_alloc(sizeof(*ctx));
    if (!ctx) return NULL;
    ctx->app = (cwist_app *)user_ctx;
    ctx->refs = 1;
    ctx->sessions = NULL;
    pthread_mutex_init(&ctx->mu, NULL);
    return ctx;
}

/** @brief on_conn_close hook: drop the connection's context reference.
 *
 * @param conn_ctx Context returned by grpc_h2_on_conn_open; may be NULL. */
static void grpc_h2_on_conn_close(void *conn_ctx) {
    if (conn_ctx) grpc_conn_ctx_release(conn_ctx);
}

/** @brief on_headers hook: start a streaming session for a gRPC call.
 *
 * Only applies to registered streaming routes with a live health-Watch-style
 * builtin filter (builtin routes other than health Watch, e.g. unary Check
 * and reflection, stay on the buffered dispatch path).  Validates the
 * content type and grpc-encoding, then allocates a session, wires the public
 * stream (write hook, deadline from grpc-timeout, response encoding), links
 * it into the connection context, and spawns the detached handler thread.
 *
 * @param conn_ctx cwist_grpc_h2_conn_ctx.
 * @param req Fully received request headers.
 * @param stream HTTP/2 stream to attach to.
 * @return Session pointer used later as stream context, or NULL to leave the
 *         request to buffered dispatch or reject it. */
static void *grpc_h2_on_headers(void *conn_ctx, cwist_http_request *req, cwist_h2_stream *stream) {
    cwist_grpc_h2_conn_ctx *ctx = conn_ctx;
    if (!ctx || !ctx->app || !req || !req->path || !req->path->data) return NULL;

    cwist_grpc_route *route = grpc_find_route(ctx->app, req->path->data);
    /* Unary-only builtins (health Check) and reflection stay on the buffered
     * dispatch path; the health Watch builtin streams live. */
    if (!route || !route->stream_handler || (route->builtin && route->builtin != 2)) return NULL;

    const char *ct = grpc_header_get(req, "content-type");
    if (!grpc_content_type_is_grpc(ct)) return NULL;

    int encoding = grpc_request_encoding(req);
    if (encoding < 0) return NULL; /* buffered dispatch replies UNIMPLEMENTED */

    cwist_grpc_session *session = (cwist_grpc_session *)cwist_alloc(sizeof(*session));
    if (!session) return NULL;
    memset(session, 0, sizeof(*session));
    pthread_mutex_init(&session->mu, NULL);
    pthread_cond_init(&session->cond, NULL);
    pthread_mutex_init(&session->wmu, NULL);
    session->h2s = stream;
    session->conn_ctx = ctx;
    session->encoding = encoding;
    cwist_grpc_decoder_init(&session->decoder, 0);
    atomic_store(&session->refs, 2); /* transport + handler thread */

    session->stream.req = req;
    session->stream.res = NULL;
    session->stream.messages = NULL;
    session->stream.message_count = 0;
    session->stream.status = CWIST_GRPC_OK;
    session->stream.status_message = NULL;
    session->stream.closed = 0;
    session->stream.write_frame = grpc_session_write_frame;
    session->stream.write_frame_ctx = session;
    session->stream.session = session;
    session->stream.cancelled = 0;
    session->stream.recv_pos = 0;
    const char *timeout = grpc_header_get(req, "grpc-timeout");
    if (timeout) {
        uint64_t timeout_ms = 0;
        if (cwist_grpc_parse_timeout(timeout, &timeout_ms) == 0)
            session->stream.deadline_ms = grpc_now_ms() + timeout_ms;
    }
    session->handler = route->stream_handler;
    session->user_ctx = route->user_ctx;
    session->resp_encoding = grpc_client_accepts_gzip(req) ? 1 : 0;

    /* Response-Headers are deliberately not sent yet (gRFC A6): they go out
     * with the first DATA frame, or the call ends in Trailers-Only form so
     * conforming clients may retry the RPC. */

    pthread_mutex_lock(&ctx->mu);
    ctx->refs++;
    session->next = ctx->sessions;
    ctx->sessions = session;
    pthread_mutex_unlock(&ctx->mu);

    pthread_t tid;
    if (pthread_create(&tid, NULL, grpc_session_thread, session) != 0) {
        pthread_mutex_lock(&ctx->mu);
        ctx->refs--;
        cwist_grpc_session **pp = &ctx->sessions;
        while (*pp && *pp != session) pp = &(*pp)->next;
        if (*pp) *pp = session->next;
        pthread_mutex_unlock(&ctx->mu);
        pthread_mutex_destroy(&session->mu);
        pthread_cond_destroy(&session->cond);
        pthread_mutex_destroy(&session->wmu);
        cwist_grpc_decoder_destroy(&session->decoder);
        cwist_free(session);
        return NULL;
    }
    pthread_detach(tid);
    return session;
}

/** @brief on_data hook: feed DATA frames into the session's decoder.
 *
 * Decoded messages are queued for the handler thread.  A decode failure
 * fails the call with UNIMPLEMENTED (compressed message without
 * grpc-encoding: gzip) or INVALID_ARGUMENT (malformed frame) and error
 * trailers.  On end_stream the session is marked EOF, waking blocked
 * receivers.
 *
 * @param conn_ctx Unused.
 * @param stream_ctx Session from grpc_h2_on_headers.
 * @param data DATA frame payload; may be NULL when @p len is 0.
 * @param len Payload length.
 * @param end_stream Non-zero when the client half-closed.
 * @retval 0 Data accepted (or the session already failed; trailers carry it).
 * @retval 1 @p stream_ctx is NULL. */
static int grpc_h2_on_data(void *conn_ctx, void *stream_ctx, const unsigned char *data, size_t len,
                           int end_stream) {
    (void)conn_ctx;
    cwist_grpc_session *session = stream_ctx;
    if (!session) return 1;

    if (data && len > 0) {
        if (cwist_grpc_decoder_feed(&session->decoder, data, len, grpc_session_decoded, session) !=
            0) {
            if (session->encoding != 1) {
                /* Compressed flag set without grpc-encoding: gzip. */
                grpc_session_fail(session, CWIST_GRPC_UNIMPLEMENTED,
                                  "compressed gRPC message without grpc-encoding: gzip");
            } else {
                grpc_session_fail(session, CWIST_GRPC_INVALID_ARGUMENT,
                                  "malformed gRPC message frame");
            }
            return 0; /* trailers carry the error; the poll sweep reaps us */
        }
    }
    if (end_stream) {
        pthread_mutex_lock(&session->mu);
        session->eof = 1;
        pthread_cond_broadcast(&session->cond);
        pthread_mutex_unlock(&session->mu);
    }
    return 0;
}

/** @brief on_cancel hook: mark the session cancelled and wake the handler.
 *
 * No trailers are sent here; the poll sweep and handler observe the
 * cancellation and terminate the call.
 *
 * @param conn_ctx Unused.
 * @param stream_ctx Session to cancel; may be NULL. */
static void grpc_h2_on_cancel(void *conn_ctx, void *stream_ctx) {
    (void)conn_ctx;
    cwist_grpc_session *session = stream_ctx;
    if (!session) return;
    pthread_mutex_lock(&session->mu);
    session->cancelled = 1;
    session->stream.cancelled = 1;
    pthread_cond_broadcast(&session->cond);
    pthread_mutex_unlock(&session->mu);
}

/** @brief on_poll hook: enforce deadlines and report stream liveness.
 *
 * When the grpc-timeout deadline has passed and trailers have not been sent,
 * cancels the session and emits DEADLINE_EXCEEDED trailers.
 *
 * @param conn_ctx Unused.
 * @param stream_ctx Session to poll; NULL counts as finished.
 * @retval 0 Session still active.
 * @retval 1 Terminal trailers were sent (or no session); the transport may
 *         reap the stream. */
static int grpc_h2_on_poll(void *conn_ctx, void *stream_ctx) {
    (void)conn_ctx;
    cwist_grpc_session *session = stream_ctx;
    if (!session) return 1;

    /* Deadline enforcement: cancel the handler and emit DEADLINE_EXCEEDED
     * trailers so the client learns the outcome. */
    if (session->stream.deadline_ms && !session->trailers_sent &&
        grpc_now_ms() >= session->stream.deadline_ms) {
        pthread_mutex_lock(&session->mu);
        session->cancelled = 1;
        session->stream.cancelled = 1;
        pthread_cond_broadcast(&session->cond);
        pthread_mutex_unlock(&session->mu);
        grpc_session_send_trailers(session, CWIST_GRPC_DEADLINE_EXCEEDED, "deadline exceeded");
    }
    return session->trailers_sent;
}

/** @brief next_deadline_ms hook: earliest active session deadline.
 *
 * Lets the HTTP/2 poll loop sleep until the next deadline instead of
 * polling continuously.
 *
 * @param conn_ctx cwist_grpc_h2_conn_ctx.
 * @return Nearest session deadline in monotonic milliseconds, or 0 when no
 *         session has a pending deadline. */
static uint64_t grpc_h2_next_deadline_ms(void *conn_ctx) {
    cwist_grpc_h2_conn_ctx *ctx = conn_ctx;
    if (!ctx) return 0;
    uint64_t nearest = 0;
    pthread_mutex_lock(&ctx->mu);
    for (cwist_grpc_session *s = ctx->sessions; s; s = s->next) {
        if (s->trailers_sent || !s->stream.deadline_ms) continue;
        if (!nearest || s->stream.deadline_ms < nearest) nearest = s->stream.deadline_ms;
    }
    pthread_mutex_unlock(&ctx->mu);
    return nearest;
}

/** @brief on_close hook: detach the session from its transport.
 *
 * Clears session->h2s so handler-thread writes fail fast, cancels the
 * session and wakes blocked receivers, and releases the transport's session
 * reference (the handler thread holds the other one).
 *
 * @param conn_ctx Unused.
 * @param stream_ctx Session being closed; may be NULL. */
static void grpc_h2_on_close(void *conn_ctx, void *stream_ctx) {
    (void)conn_ctx;
    cwist_grpc_session *session = stream_ctx;
    if (!session) return;
    /* Transport is gone: detach so handler-thread sends fail fast. */
    pthread_mutex_lock(&session->wmu);
    session->h2s = NULL;
    pthread_mutex_unlock(&session->wmu);
    pthread_mutex_lock(&session->mu);
    session->cancelled = 1;
    session->stream.cancelled = 1;
    pthread_cond_broadcast(&session->cond);
    pthread_mutex_unlock(&session->mu);
    grpc_session_release(session);
}

static const cwist_http2_stream_hooks grpc_h2_hooks = {
    .on_conn_open = grpc_h2_on_conn_open,
    .on_conn_close = grpc_h2_on_conn_close,
    .on_headers = grpc_h2_on_headers,
    .on_data = grpc_h2_on_data,
    .on_cancel = grpc_h2_on_cancel,
    .on_poll = grpc_h2_on_poll,
    .next_deadline_ms = grpc_h2_next_deadline_ms,
    .on_close = grpc_h2_on_close,
};

/** @brief Access the gRPC HTTP/2 stream hooks table.
 *
 * @return Static hooks structure wiring the incremental gRPC streaming
 *         engine into the HTTP/2 transport. */
const cwist_http2_stream_hooks *cwist_grpc_http2_hooks(void) {
    return &grpc_h2_hooks;
}
