/**
 * @file http3.c
 * @brief lsquic/BoringSSL-based HTTP/3 server for CWIST.
 *
 * Implements a full HTTP/3 server using LiteSpeed's lsquic library
 * linked statically against BoringSSL.  Handles QUIC transport,
 * QPACK, and HTTP/3 framing per RFC 9114.
 */

#if defined(__APPLE__)
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE
#endif
#elif !defined(__FreeBSD__) && !defined(__NetBSD__) && !defined(__OpenBSD__) && \
    !defined(__DragonFly__)
#define _POSIX_C_SOURCE 200809L
#endif
#include <cwist/net/http/http3.h>
#include <cwist/net/http/http2.h>
#include <cwist/core/mem/alloc.h>
#include <cwist/core/seq/seq.h>
#include <cwist/sys/err/cwist_err.h>
#include <cwist/sys/app/shutdown.h>
#include "tls_chain.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/stat.h>
#include <poll.h>
#include <pthread.h>
#ifdef __linux__
#include <sys/epoll.h>
#endif
#include <ctype.h>
#include <strings.h>
#include <inttypes.h>

#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/evp.h>
#include <openssl/rsa.h>
#include <ctype.h>

#if CWIST_HAVE_OPENSSL_QUIC

static int cwist_http3_alpn_select_cb(SSL *ssl,
                                      const unsigned char **out,
                                      unsigned char *outlen,
                                      const unsigned char *in,
                                      unsigned int inlen,
                                      void *arg) {
    (void)ssl;
    (void)arg;
    static const unsigned char h3_alpn[] = "\x02h3";
    if (SSL_select_next_proto((unsigned char **)out,
                              outlen,
                              h3_alpn, sizeof(h3_alpn) - 1,
                              in, inlen) == OPENSSL_NPN_NEGOTIATED) {
        return SSL_TLSEXT_ERR_OK;
    }
    return SSL_TLSEXT_ERR_NOACK;
}

static const struct {
    const char *name;
    const char *value;
} qpack_static_table[] = {
    {":authority", ""}, {":path", "/"}, {"age", "0"},
    {"content-disposition", ""}, {"content-length", "0"}, {"cookie", ""},
    {"date", ""}, {"etag", ""}, {"if-modified-since", ""},
    {"if-none-match", ""}, {"last-modified", ""}, {"link", ""},
    {"location", ""}, {"referer", ""}, {"set-cookie", ""},
    {":method", "CONNECT"}, {":method", "DELETE"}, {":method", "GET"},
    {":method", "HEAD"}, {":method", "OPTIONS"}, {":method", "POST"},
    {":method", "PUT"}, {":scheme", "http"}, {":scheme", "https"},
    {":status", "103"}, {":status", "200"}, {":status", "304"},
    {":status", "404"}, {":status", "503"}, {"accept", "*/*"},
    {"accept", "application/dns-message"}, {"accept-encoding", "gzip, deflate, br"},
    {"accept-ranges", "bytes"}, {"access-control-allow-headers", "cache-control"},
    {"access-control-allow-headers", "content-type"}, {"access-control-allow-origin", "*"},
    {"cache-control", "max-age=0"}, {"cache-control", "max-age=2592000"},
    {"cache-control", "max-age=604800"}, {"cache-control", "no-cache"},
    {"cache-control", "no-store"}, {"cache-control", "public, max-age=31536000"},
    {"content-encoding", "br"}, {"content-encoding", "gzip"},
    {"content-type", "application/dns-message"}, {"content-type", "application/javascript"},
    {"content-type", "application/json"}, {"content-type", "application/x-www-form-urlencoded"},
    {"content-type", "image/gif"}, {"content-type", "image/jpeg"},
    {"content-type", "image/png"}, {"content-type", "text/css"},
    {"content-type", "text/html; charset=utf-8"}, {"content-type", "text/plain"},
    {"content-type", "text/plain;charset=utf-8"}, {"range", "bytes=0-"},
    {"strict-transport-security", "max-age=31536000"},
    {"strict-transport-security", "max-age=31536000; includesubdomains"},
    {"strict-transport-security", "max-age=31536000; includesubdomains; preload"},
    {"vary", "accept-encoding"}, {"vary", "origin"},
    {"x-content-type-options", "nosniff"}, {"x-xss-protection", "1; mode=block"},
    {":status", "100"}, {":status", "204"}, {":status", "206"},
    {":status", "302"}, {":status", "400"}, {":status", "403"},
    {":status", "421"}, {":status", "425"}, {":status", "500"},
    {"accept-language", ""}, {"access-control-allow-credentials", "FALSE"},
    {"access-control-allow-credentials", "TRUE"}, {"access-control-allow-headers", "*"},
    {"access-control-allow-methods", "get"}, {"access-control-allow-methods", "get, post, options"},
    {"access-control-allow-methods", "options"}, {"access-control-expose-headers", "content-length"},
    {"access-control-request-headers", "content-type"}, {"access-control-request-method", "get"},
    {"access-control-request-method", "post"}, {"alt-svc", "clear"},
    {"authorization", ""}, {"content-security-policy", "script-src 'none'; object-src 'none'; base-uri 'none'"},
    {"early-data", "1"}, {"expect-ct", ""}, {"forwarded", ""},
    {"if-range", ""}, {"origin", ""}, {"purpose", "prefetch"},
    {"server", ""}, {"timing-allow-origin", "*"}, {"upgrade-insecure-requests", "1"},
    {"user-agent", ""}, {"x-forwarded-for", ""}, {"x-frame-options", "deny"},
    {"x-frame-options", "sameorigin"}
};

#define QPACK_STATIC_TABLE_COUNT (sizeof(qpack_static_table)/sizeof(qpack_static_table[0]))

static size_t qpack_encode_integer(unsigned char *dst, size_t dst_cap, uint32_t value, uint8_t prefix_bits) {
    uint8_t mask = (uint8_t)((1u << prefix_bits) - 1u);
    unsigned char first = dst[0] & ~mask;
    if (value < mask) {
        dst[0] = first | (uint8_t)value;
        return 1;
    }
    dst[0] = first | mask;
    value -= mask;
    size_t i = 1;
    while (value >= 128) {
        if (i >= dst_cap) return 0;
        dst[i++] = (unsigned char)((value & 0x7F) | 0x80);
        value >>= 7;
    }
    if (i >= dst_cap) return 0;
    dst[i++] = (unsigned char)value;
    return i;
}

static size_t qpack_encode_string(unsigned char *dst, size_t dst_cap, const char *str) {
    size_t len = strlen(str);
    dst[0] = 0x00; /* literal, no huffman */
    size_t n = qpack_encode_integer(dst, dst_cap, (uint32_t)len, 7);
    if (n == 0 || n + len > dst_cap) return 0;
    memcpy(dst + n, str, len);
    return n + len;
}

static int qpack_static_table_find_name(const char *name) {
    for (size_t i = 0; i < QPACK_STATIC_TABLE_COUNT; ++i) {
        if (qpack_static_table[i].name && strcasecmp(qpack_static_table[i].name, name) == 0)
            return (int)i;
    }
    return -1;
}

static int qpack_static_status_index(int status_code) {
    switch (status_code) {
        case 100: return 63;
        case 103: return 24;
        case 200: return 25;
        case 204: return 64;
        case 206: return 65;
        case 302: return 66;
        case 304: return 26;
        case 400: return 67;
        case 403: return 68;
        case 404: return 27;
        case 421: return 69;
        case 425: return 70;
        case 500: return 71;
        case 503: return 28;
        default: return -1;
    }
}

static size_t qpack_encode_response_headers(cwist_http_response *res,
                                             unsigned char *dst, size_t dst_cap) {
    size_t pos = 0;
    /* Encoded Field Section Prefix: Required Insert Count = 0, Base = 0 */
    if (pos + 2 > dst_cap) return 0;
    dst[pos++] = 0x00;
    dst[pos++] = 0x00;

    /* :status */
    int status_idx = qpack_static_status_index(res->status_code);
    if (status_idx >= 0 && status_idx < 64) {
        if (pos + 1 > dst_cap) return 0;
        dst[pos++] = (unsigned char)(0xC0 | status_idx); /* Indexed Field Line, static */
    } else {
        char status_str[16];
        int status_len = snprintf(status_str, sizeof(status_str), "%d", res->status_code);
        (void)status_len;
        if (pos + 1 > dst_cap) return 0;
        dst[pos] = 0x20; /* Literal Field Line with Literal Name, H=0 */
        size_t n = qpack_encode_integer(dst + pos, dst_cap - pos, 7, 5); /* ":status" len */
        if (n == 0) return 0;
        pos += n;
        if (pos + 7 > dst_cap) return 0;
        memcpy(dst + pos, ":status", 7);
        pos += 7;
        n = qpack_encode_string(dst + pos, dst_cap - pos, status_str);
        if (n == 0) return 0;
        pos += n;
    }

    /* Auto content-length */
    size_t body_len = 0;
    if (res->use_file_stream) body_len = res->file_stream_len;
    else if (res->is_ptr_body) body_len = res->ptr_body_len;
    else if (res->body) body_len = res->body->size;

    if (!headers_have_content_length(res->headers)) {
        char cl_str[32];
        int cl_len = snprintf(cl_str, sizeof(cl_str), "%zu", body_len);
        (void)cl_len;
        int name_idx = qpack_static_table_find_name("content-length");
        if (name_idx >= 0 && name_idx < 16) {
            if (pos + 1 > dst_cap) return 0;
            dst[pos] = 0x50; /* Literal with Name Reference, static, indexed name */
            size_t n = qpack_encode_integer(dst + pos, dst_cap - pos, (uint32_t)name_idx, 4);
            if (n == 0) return 0;
            pos += n;
        } else {
            if (pos + 1 > dst_cap) return 0;
            dst[pos] = 0x20; /* Literal with Literal Name */
            size_t n = qpack_encode_integer(dst + pos, dst_cap - pos, 14, 5);
            if (n == 0) return 0;
            pos += n;
            if (pos + 14 > dst_cap) return 0;
            memcpy(dst + pos, "content-length", 14);
            pos += 14;
        }
        size_t n = qpack_encode_string(dst + pos, dst_cap - pos, cl_str);
        if (n == 0) return 0;
        pos += n;
    }

    /* User headers */
    cwist_http_header_node *curr = res->headers;
    while (curr) {
        if (!curr->key || !curr->key->data || !curr->value || !curr->value->data) {
            curr = curr->next;
            continue;
        }
        if (strcasecmp(curr->key->data, "connection") == 0 ||
            strcasecmp(curr->key->data, "keep-alive") == 0 ||
            strcasecmp(curr->key->data, "transfer-encoding") == 0 ||
            strcasecmp(curr->key->data, "upgrade") == 0) {
            curr = curr->next;
            continue;
        }

        char lower_name[256];
        size_t name_len = strlen(curr->key->data);
        if (name_len >= sizeof(lower_name)) name_len = sizeof(lower_name) - 1;
        for (size_t i = 0; i < name_len; ++i) {
            lower_name[i] = (char)tolower((unsigned char)curr->key->data[i]);
        }
        lower_name[name_len] = '\0';

        int name_idx = qpack_static_table_find_name(lower_name);
        if (name_idx >= 0 && name_idx < 16) {
            if (pos + 1 > dst_cap) return 0;
            dst[pos] = 0x50; /* Literal with Name Reference, static */
            size_t n = qpack_encode_integer(dst + pos, dst_cap - pos, (uint32_t)name_idx, 4);
            if (n == 0) return 0;
            pos += n;
        } else {
            if (pos + 1 > dst_cap) return 0;
            dst[pos] = 0x20; /* Literal with Literal Name */
            size_t n = qpack_encode_integer(dst + pos, dst_cap - pos, (uint32_t)name_len, 5);
            if (n == 0) return 0;
            pos += n;
            if (pos + name_len > dst_cap) return 0;
            memcpy(dst + pos, lower_name, name_len);
            pos += name_len;
        }
        size_t n = qpack_encode_string(dst + pos, dst_cap - pos, curr->value->data);
        if (n == 0) return 0;
        pos += n;

        curr = curr->next;
    }

    return pos;
}

/**
 * @brief Take a process-wide reference on the lsquic global state.
 * Initializes lsquic on the first reference; later calls only bump the
 * reference count.  Thread-safe via @ref g_h3_global_mtx.
 */
static void h3_global_init(void) {
    pthread_mutex_lock(&g_h3_global_mtx);
    if (g_h3_global_ref == 0) {
        lsquic_global_init(LSQUIC_GLOBAL_SERVER);
    }
    g_h3_global_ref++;
    pthread_mutex_unlock(&g_h3_global_mtx);
}

/**
 * @brief Release a reference taken by h3_global_init().
 * Runs lsquic_global_cleanup() when the last reference is dropped.
 * Thread-safe; safe to call with no matching init (no-op).
 */
static void h3_global_cleanup(void) {
    pthread_mutex_lock(&g_h3_global_mtx);
    if (g_h3_global_ref > 0) {
        g_h3_global_ref--;
        if (g_h3_global_ref == 0) {
            lsquic_global_cleanup();
        }
    }
    pthread_mutex_unlock(&g_h3_global_mtx);
}

/* ------------------------------------------------------------------ */
/* Internal stream context                                            */
/* ------------------------------------------------------------------ */

/* RFC 9114 Section 4.3.1 request pseudo-header tracking (bitmask) */
#define H3_PSEUDO_METHOD 0x01u
#define H3_PSEUDO_SCHEME 0x02u
#define H3_PSEUDO_PATH 0x04u
#define H3_PSEUDO_AUTHORITY 0x08u
#define H3_PSEUDO_PROTOCOL 0x10u

typedef struct h3_stream_ctx {
    lsquic_stream_t *stream;
    cwist_http_request *req;
    cwist_http_response *res;
    char *body;
    size_t body_len;
    size_t body_cap;
    int headers_done;
    int response_ready;
    int write_state; /* 0=headers, 1=body, 2=done */
    size_t body_sent;
#ifdef CWIST_WEBTRANSPORT
    int is_webtransport;
    int wt_taken;
#endif
} h3_stream_ctx_t;

/* ------------------------------------------------------------------ */
/* Per-connection context                                             */
/* ------------------------------------------------------------------ */

/* One queued outgoing datagram (see h3_conn_ctx_t::dgram_head below). */
typedef struct h3_dgram_node {
    struct h3_dgram_node *next;
    char *data;
    size_t len;
} h3_dgram_node_t;

/* lsquic_conn_set_ctx()'s payload for every connection this stream_if
 * services. Previously every connection shared the single, unlocked
 * cwist_http3_context pointer directly, which put datagram state
 * (g_h3_dgram) in a process-wide global: one connection's pending
 * datagram could be freed/overwritten by a concurrent send on a
 * different connection (double-free/UAF), with no lock protecting any
 * of it. This wrapper keeps the shared, read-mostly server config
 * (h3_ctx) reachable exactly as before while giving each connection its
 * own mutex-guarded outgoing-datagram queue. */
typedef struct h3_conn_ctx {
    cwist_http3_context *h3_ctx; /* shared server config, not owned here */
    pthread_mutex_t dgram_lock;
    h3_dgram_node_t *dgram_head;
    h3_dgram_node_t *dgram_tail;
} h3_conn_ctx_t;

/* Every lsquic_conn_get_ctx(conn) callsite below reads this wrapper's
 * ->h3_ctx instead of casting the raw ctx pointer directly. */
/**
 * @brief Extract the shared server context from a connection's context wrapper.
 * @param conn Connection whose ctx was set up as h3_conn_ctx_t, or NULL.
 * @return The shared cwist_http3_context*, or NULL if @p conn or its ctx is
 *         NULL.  Not owned by the caller.
 */
static inline cwist_http3_context *h3_shared_ctx(lsquic_conn_t *conn) {
    h3_conn_ctx_t *cc = conn ? (h3_conn_ctx_t *)lsquic_conn_get_ctx(conn) : NULL;
    return cc ? cc->h3_ctx : NULL;
}

#ifdef CWIST_WEBTRANSPORT

typedef enum cwist_wt_handle_kind {
    CWIST_WT_HANDLE_SESSION = 1,
    CWIST_WT_HANDLE_STREAM = 2,
} cwist_wt_handle_kind_t;

typedef struct cwist_wt_handle {
    uint64_t magic;
    uint16_t version;
    uint16_t kind;
    uint32_t flags;
    void *ptr;
    struct cwist_wt_handle *parent;
    struct cwist_wt_handle *first_child;
    struct cwist_wt_handle *next_sibling;
} cwist_wt_handle_t;

#define CWIST_WT_HANDLE_MAGIC UINT64_C(0x4357495354575431)
#define CWIST_WT_HANDLE_VERSION 1

/**
 * @brief Allocate a new typed WebTransport handle wrapping @p ptr.
 * @param kind Handle kind tag (session or stream).
 * @param ptr Underlying lsquic object to wrap; must not be NULL.
 * @return The new handle, or NULL if @p ptr is NULL or allocation fails.
 */
static cwist_wt_handle_t *cwist_wt_handle_new(cwist_wt_handle_kind_t kind, void *ptr) {
    if (!ptr) return NULL;
    cwist_wt_handle_t *handle = calloc(1, sizeof(*handle));
    if (!handle) return NULL;
    handle->magic = CWIST_WT_HANDLE_MAGIC;
    handle->version = CWIST_WT_HANDLE_VERSION;
    handle->kind = (uint16_t)kind;
    handle->ptr = ptr;
    return handle;
}

/**
 * @brief Free a handle and, recursively, all of its child handles.
 * Detaches the handle from its parent's child list first.  Safe to call
 * with NULL.
 * @param Handle to free; ownership is consumed.
 */
static void cwist_wt_handle_free(cwist_wt_handle_t *handle) {
    if (!handle) return;
    while (handle->first_child) {
        cwist_wt_handle_t *next = handle->first_child;
        handle->first_child = next->next_sibling;
        next->magic = 0;
        next->ptr = NULL;
        next->parent = NULL;
        next->first_child = NULL;
        next->next_sibling = NULL;
        free(next);
    }
    if (handle->parent) {
        cwist_wt_handle_t **link = &handle->parent->first_child;
        while (*link) {
            if (*link == handle) {
                *link = handle->next_sibling;
                break;
            }
            link = &(*link)->next_sibling;
        }
    }
    handle->magic = 0;
    handle->ptr = NULL;
    handle->parent = NULL;
    handle->first_child = NULL;
    handle->next_sibling = NULL;
    free(handle);
}

/**
 * @brief Link @p child into @p parent's child list.
 * The child is inserted at the head of the list.  No-op if either is NULL.
 */
static void cwist_wt_handle_attach(cwist_wt_handle_t *parent, cwist_wt_handle_t *child) {
    if (!parent || !child) return;
    child->parent = parent;
    child->next_sibling = parent->first_child;
    parent->first_child = child;
}

/**
 * @brief Validate an opaque pointer and cast it to a typed handle.
 * Checks the magic, version, kind tag, and wrapped pointer.
 * @param handle Opaque pointer supplied by the application.
 * @param kind Expected handle kind.
 * @return The typed handle, or NULL if any validation check fails.
 */
static cwist_wt_handle_t *cwist_wt_handle_cast(void *handle, cwist_wt_handle_kind_t kind) {
    cwist_wt_handle_t *h = (cwist_wt_handle_t *)handle;
    if (!h || h->magic != CWIST_WT_HANDLE_MAGIC || h->version != CWIST_WT_HANDLE_VERSION ||
        h->kind != (uint16_t)kind || !h->ptr) {
        return NULL;
    }
    return h;
}

/**
 * @brief Resolve an opaque handle to its underlying lsquic stream.
 * @return The wrapped lsquic_stream_t*, or NULL if @p handle is not a valid
 *         stream-kind handle.  Not owned by the caller.
 */
static lsquic_stream_t *cwist_wt_handle_stream(void *handle) {
    cwist_wt_handle_t *h = cwist_wt_handle_cast(handle, CWIST_WT_HANDLE_STREAM);
    return h ? (lsquic_stream_t *)h->ptr : NULL;
}

/**
 * @brief Resolve an opaque handle to its underlying WebTransport session.
 * @return The wrapped lsquic_wt_session_t*, or NULL if @p handle is not a
 *         valid session-kind handle.  Not owned by the caller.
 */
static lsquic_wt_session_t *cwist_wt_handle_session(void *handle) {
    cwist_wt_handle_t *h = cwist_wt_handle_cast(handle, CWIST_WT_HANDLE_SESSION);
    return h ? (lsquic_wt_session_t *)h->ptr : NULL;
}

#endif /* CWIST_WEBTRANSPORT */

/* ------------------------------------------------------------------ */
/* Header-set interface for lsquic (QPACK decode)                     */
/* ------------------------------------------------------------------ */

/* Browsers routinely send more than 64 HTTP/3 headers once Client Hints,
 * security metadata and cookies are included.  Keep a firm per-stream cap,
 * but leave enough headroom that a late Cookie or :path is never discarded. */
#define H3_MAX_HEADERS 256
#define H3_DECODE_BUF_SIZE 131072
#define H3_MAX_RESPONSE_HEADERS 256
#define H3_RESPONSE_HEADER_BUF_SIZE 32768

typedef struct cwist_h3_hset {
    lsquic_stream_t *stream;
    struct cwist_h3_hset *next;  /* intrusive list of live hsets per ctx */
    struct cwist_h3_hset **prev; /* link to the pointer that points at us */
    cwist_http3_context *owner;  /* tracking context, NULL if untracked */
    struct lsxpack_header headers[H3_MAX_HEADERS];
    size_t count;
    char decode_buf[H3_DECODE_BUF_SIZE];
    size_t decode_off;
} cwist_h3_hset_t;

static void *cwist_h3_hsi_create(void *hsi_ctx, lsquic_stream_t *stream,
                                 int is_push_promise) {
    (void)is_push_promise;
    cwist_h3_hset_t *hset = calloc(1, sizeof(*hset));
    if (!hset) return NULL;
    hset->stream = stream;
    cwist_h3_hset_track((cwist_http3_context *)hsi_ctx, hset);
    return hset;
}

/**
 * @brief hsi_prepare_decode callback: reserve decode storage for a header.
 * @param hset_p Header set.
 * @param xhdr Previous header when lsquic retries with a larger value
 *             requirement, NULL for a fresh slot.
 * @param req_space Required value capacity in bytes.
 * @return The (possibly resized) header slot, or NULL if the request exceeds
 *         the slot count, per-string, or decode-buffer limits.
 *
 * When @p xhdr is non-NULL the previously decoded name must be preserved:
 * only the value capacity is grown.  Reinitializing the slot here loses
 * :path/Cookie.
 */
static struct lsxpack_header *cwist_h3_hsi_prepare(void *hset_p, struct lsxpack_header *xhdr,
                                                   size_t req_space) {
    cwist_h3_hset_t *hset = hset_p;
    if (!hset) return NULL;

    if (xhdr) {
        /* Advance by the exact decoded size lsquic reports
         * (name_len + val_len + dec_overhead).  The old pointer-offset
         * arithmetic computed the wrong length and corrupted headers
         * when multiple Cookie values arrived in separate QPACK entries. */
        size_t total = lsxpack_header_get_dec_size(xhdr);
        if (total > sizeof(hset->decode_buf) - hset->decode_off)
            total = sizeof(hset->decode_buf) - hset->decode_off;
        hset->decode_off += total;
        if (hset->count < H3_MAX_HEADERS)
            hset->count++;
    }

    if (hset->count >= H3_MAX_HEADERS) return NULL;
    if (req_space > sizeof(hset->decode_buf) - hset->decode_off) return NULL;
    lsxpack_header_prepare_decode(&hset->headers[hset->count], hset->decode_buf, hset->decode_off,
                                  sizeof(hset->decode_buf) - hset->decode_off);
    return &hset->headers[hset->count];
}

/**
 * @brief hsi_process_header callback: account for a completed header.
 * @param hset_p Header set.
 * @param xhdr Completed header; the QPACK decoder exposes the exact storage
 *             it used.  NULL marks the end of a header block.
 * @retval 0 Header accepted and accounted in the decode buffer.
 * @retval -1 Header would overflow the decode buffer; logged to stderr.
 */
static int cwist_h3_hsi_process_header(void *hset_p, struct lsxpack_header *xhdr) {
    cwist_h3_hset_t *hset = hset_p;
    /* A NULL header marks the end of a header block. */
    if (!hset || !xhdr) return 0;

    /* The QPACK decoder exposes the exact storage used by this completed
     * header. */
    size_t total = lsxpack_header_get_dec_size(xhdr);
    if (total > sizeof(hset->decode_buf) - hset->decode_off) {
        fprintf(stderr, "[HTTP/3] Rejecting oversized QPACK header (size=%zu, used=%zu)\n", total,
                hset->decode_off);
        return -1;
    }
    hset->decode_off += total;
    hset->count++;
    return 0;
}

/**
 * @brief hsi_discard_header_set callback: untrack and free a header set.
 * @param hset_p Header set previously returned by cwist_h3_hsi_create().
 */
static void cwist_h3_hsi_discard(void *hset_p) {
    cwist_h3_hset_untrack((cwist_h3_hset_t *)hset_p);
    free(hset_p);
}

static const struct lsquic_hset_if cwist_h3_hset_if = {
    .hsi_create_header_set = cwist_h3_hsi_create,
    .hsi_prepare_decode = cwist_h3_hsi_prepare,
    .hsi_process_header = cwist_h3_hsi_process_header,
    .hsi_discard_header_set = cwist_h3_hsi_discard,
};

#ifdef CWIST_WEBTRANSPORT
static const struct lsquic_webtransport_if cwist_h3_wt_if;
#endif

/* ------------------------------------------------------------------ */
/* SSL context callback                                               */
/* ------------------------------------------------------------------ */

/**
 * @brief ea_get_ssl_ctx callback: pick the server SSL_CTX for a connection.
 * @param peer_ctx The cwist_http3_context registered as stream_if ctx.
 * @return Its SSL_CTX, or NULL if @p peer_ctx is NULL.
 */
static SSL_CTX *cwist_h3_get_ssl_ctx(void *peer_ctx, const struct sockaddr *local) {
    (void)local;
    cwist_http3_context *h3_ctx = (cwist_http3_context *)peer_ctx;
    return h3_ctx ? h3_ctx->ssl_ctx : NULL;
}

/* ------------------------------------------------------------------ */
/* SSL context callback                                               */
/* ------------------------------------------------------------------ */

static SSL_CTX *cwist_h3_get_ssl_ctx(void *peer_ctx,
                                      const struct sockaddr *local) {
    (void)local;
    cwist_http3_context *h3_ctx = (cwist_http3_context *)peer_ctx;
    return h3_ctx ? h3_ctx->ssl_ctx : NULL;
}

/* ------------------------------------------------------------------ */
/* Packet-out callback                                                */
/* ------------------------------------------------------------------ */

static int cwist_h3_packets_out(void *ctx,
                                  const struct lsquic_out_spec *specs,
                                  unsigned n_specs) {
    int udp_fd = *(int *)ctx;
    unsigned i;
    for (i = 0; i < n_specs; ++i) {
        const struct lsquic_out_spec *spec = &specs[i];
        struct msghdr msg = {0};
        msg.msg_name = (void *)spec->dest_sa;
        msg.msg_namelen = (spec->dest_sa && spec->dest_sa->sa_family == AF_INET)
                          ? sizeof(struct sockaddr_in)
                          : sizeof(struct sockaddr_in6);
        msg.msg_iov = (struct iovec *)spec->iov;
        msg.msg_iovlen = spec->iovlen;
        ssize_t nw = sendmsg(udp_fd, &msg, MSG_DONTWAIT);
        if (nw < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            /* Non-fatal errors: log and continue if possible */
            if (errno == ECONNREFUSED || errno == ENETUNREACH ||
                errno == EHOSTUNREACH || errno == EMSGSIZE)
                continue;
            return -1;
        }
        break;
    }
    return (int)i;
}
#endif

/**
 * @brief ea_packets_out callback: emit lsquic's queued UDP packets.
 * @param ctx Points at the UDP socket fd (int).
 * @param specs Packets to send.
 * @param n_specs Number of entries in @p specs.
 * @return Number of specs consumed, or -1 if none could be sent and the
 *         first attempt hit a hard error.
 *
 * Linux: one-time GSO probe (CWIST_H3_NO_GSO=1 disables); runs of equal-size
 * datagrams to the same peer are coalesced into a single UDP_SEGMENT
 * sendmsg, with a permanent fallback to h3_sendmmsg_batch() if the kernel
 * rejects GSO.  EAGAIN/EWOULDBLOCK and transient ICMP-style errors
 * (ECONNREFUSED/ENETUNREACH/EHOSTUNREACH/EMSGSIZE) consume the run without
 * failing the callback.  Non-Linux: plain per-spec sendmsg loop.
 */
static int cwist_h3_packets_out(void *ctx, const struct lsquic_out_spec *specs, unsigned n_specs) {
    int udp_fd = *(int *)ctx;
#if defined(__linux__)
    /* One-time GSO probe: check env var on first call. */
    if (__builtin_expect(h3_gso_state < 0, 0)) {
        const char *no_gso = getenv("CWIST_H3_NO_GSO");
        h3_gso_state = (no_gso && no_gso[0] == '1') ? 1 : 0;
    }

    if (!h3_gso_state) {
        /* GSO fast path: coalesce runs of equal-size datagrams to the same
         * peer into one sendmsg with UDP_SEGMENT. */
        unsigned i = 0;
        while (i < n_specs) {
            size_t seg = h3_spec_len(&specs[i]);
            unsigned run = 1;
            unsigned niov = specs[i].iovlen;
            if (seg > 0 && seg <= 65535) {
                /* Total payload must fit in one UDP datagram (65535 bytes);
                 * 65536/seg could allow an exactly-64KiB super-packet whose
                 * sendmsg fails with EMSGSIZE and would disable GSO. */
                unsigned max_segs = (unsigned)(65535 / seg);
                if (max_segs > 48) max_segs = 48;
                while (i + run < n_specs && run < max_segs && specs[i + run].ecn == specs[i].ecn &&
                       h3_same_dest(&specs[i], &specs[i + run]) &&
                       h3_spec_len(&specs[i + run]) == seg && niov + specs[i + run].iovlen <= 128) {
                    niov += specs[i + run].iovlen;
                    run++;
                }
            }
            if (run >= 2) {
                struct iovec iov[128];
                unsigned n = 0;
                for (unsigned k = 0; k < run; k++)
                    for (unsigned m = 0; m < specs[i + k].iovlen; m++)
                        iov[n++] = specs[i + k].iov[m];

                char ctrl[512];
                struct msghdr msg = {0};
                msg.msg_name = (void *)specs[i].dest_sa;
                msg.msg_namelen = (specs[i].dest_sa && specs[i].dest_sa->sa_family == AF_INET)
                                      ? sizeof(struct sockaddr_in)
                                      : sizeof(struct sockaddr_in6);
                msg.msg_iov = iov;
                msg.msg_iovlen = n;
                h3_setup_cmsg(&msg, ctrl, sizeof(ctrl), &specs[i], (uint16_t)seg);

                ssize_t nw = sendmsg(udp_fd, &msg, MSG_DONTWAIT);
                if (nw >= 0) {
                    i += run;
                    continue;
                }
                if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                /* GSO rejected: permanently disable and fall back to sendmmsg. */
                h3_gso_state = 1;
                return h3_sendmmsg_batch(udp_fd, specs, i, n_specs);
            }
            /* Single packet: send directly. */
            int nw = h3_send_one(udp_fd, &specs[i]);
            if (nw < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) return (int)i;
                if (errno == ECONNREFUSED || errno == ENETUNREACH || errno == EHOSTUNREACH ||
                    errno == EMSGSIZE) {
                    i++;
                    continue;
                }
                return (i > 0) ? (int)i : -1;
            }
            i++;
        }
        return (int)i;
    }

    /* sendmmsg fallback path (GSO disabled). */
    return h3_sendmmsg_batch(udp_fd, specs, 0, n_specs);
#else
    unsigned i = 0;
    for (; i < n_specs; ++i) {
        int nw = h3_send_one(udp_fd, &specs[i]);
        if (nw < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return (int)i;
            if (errno == ECONNREFUSED || errno == ENETUNREACH || errno == EHOSTUNREACH ||
                errno == EMSGSIZE)
                continue;
            return (i > 0) ? (int)i : -1;
        }
    }
    return (int)i;
#endif
}

/* ------------------------------------------------------------------ */
/* ALPN selection callback (BoringSSL)                                */
/* ------------------------------------------------------------------ */

/**
 * @brief ALPN selection callback (BoringSSL): pick "h3" or "h3-29".
 * @param out/onout Receives a pointer into the client's protocol list.
 * @retval SSL_TLSEXT_ERR_OK A supported protocol was selected.
 * @retval SSL_TLSEXT_ERR_NOACK No supported protocol offered; the handshake
 *         proceeds without ALPN.
 */
static int cwist_h3_alpn_select_cb(SSL *ssl, const uint8_t **out, uint8_t *outlen,
                                   const uint8_t *in, unsigned inlen, void *arg) {
    (void)ssl;
    (void)arg;
    static const char *const protos[] = {"h3", "h3-29"};
    for (size_t p = 0; p < sizeof(protos) / sizeof(protos[0]); ++p) {
        const char *proto = protos[p];
        size_t plen = strlen(proto);
        const uint8_t *ptr = in;
        const uint8_t *end = in + inlen;
        while (ptr < end) {
            uint8_t len = *ptr++;
            if (ptr + len > end) break;
            if (len == plen && memcmp(ptr, proto, plen) == 0) {
                *out = ptr;
                *outlen = (uint8_t)plen;
                return SSL_TLSEXT_ERR_OK;
            }
            ptr += len;
        }
    }
    return SSL_TLSEXT_ERR_NOACK;
}

/* ------------------------------------------------------------------ */
/* Stream callbacks                                                   */
/* ------------------------------------------------------------------ */

/**
 * @brief lsquic logger sink: write one log buffer to stderr.
 * @return Number of bytes written.
 */
static int cwist_h3_log_stderr(void *ctx, const char *buf, size_t len) {
    (void)ctx;
    return (int)fwrite(buf, 1, len, stderr);
}

/**
 * @brief on_new_conn callback: allocate and install the per-connection ctx.
 * @param stream_if_ctx The shared cwist_http3_context.
 * @return New h3_conn_ctx_t (stored on the connection), or NULL on OOM.
 *
 * On OOM the connection proceeds with a NULL ctx: every consumer tolerates
 * that, but the connection gets no request dispatch or datagram support.
 * The wrapper carries the shared config plus this connection's own
 * mutex-guarded datagram queue.
 */
static lsquic_conn_ctx_t *cwist_h3_on_new_conn(void *stream_if_ctx, lsquic_conn_t *conn) {
    cwist_http3_context *h3_ctx = stream_if_ctx;
    h3_conn_ctx_t *cc = (h3_conn_ctx_t *)calloc(1, sizeof(*cc));
    if (!cc) {
        /* OOM at connection-accept time: proceed without a ctx rather than
         * risk type confusion by storing a bare cwist_http3_context* here
         * on some paths and a h3_conn_ctx_t* on others. Every consumer
         * below already tolerates lsquic_conn_get_ctx() returning NULL
         * (h3_shared_ctx(), the datagram callbacks); this connection just
         * won't get request dispatch or datagram support. */
        CWIST_LOG_ERROR("[HTTP/3] OOM allocating per-connection context");
        return NULL;
    }
    cc->h3_ctx = h3_ctx;
    pthread_mutex_init(&cc->dgram_lock, NULL);
    lsquic_conn_set_ctx(conn, (lsquic_conn_ctx_t *)cc);
    return (lsquic_conn_ctx_t *)cc;
}

/**
 * @brief on_conn_closed callback: log the close status and free the
 *        per-connection ctx, including any queued datagrams.
 *
 * Deliberately avoids lsquic_conn_get_info(): it lazily allocates the
 * bandwidth sampler even while the connection is being destroyed, leaking it
 * under LSAN.  Stats must be collected while the connection is alive.
 */
static void cwist_h3_on_conn_closed(lsquic_conn_t *conn) {
    if (!conn) return;
    struct lsquic_conn_info info;
    if (lsquic_conn_get_info(conn, &info) == 0) {
        fprintf(stderr,
                "[HTTP/3] Conn closed rtt=%u rttvar=%u "
                "pkts_sent=%" PRIu64 " pkts_lost=%" PRIu64 " "
                "pkts_retx=%" PRIu64 " cwnd=%u\n",
                info.lci_rtt, info.lci_rttvar,
                info.lci_pkts_sent, info.lci_pkts_lost,
                info.lci_pkts_retx, info.lci_cwnd);
    }
}

static lsquic_stream_ctx_t *cwist_h3_on_new_stream(void *stream_if_ctx, lsquic_stream_t *stream) {
    cwist_http3_context *h3_ctx = (cwist_http3_context *)stream_if_ctx;
    h3_stream_ctx_t *st = calloc(1, sizeof(*st));
    if (!st) return NULL;
    st->stream = stream;
    st->req = cwist_http_request_create();
    if (!st->req) {
        free(st);
        return NULL;
    }
    cwist_sstring_assign(st->req->version, "HTTP/3");
    st->req->private_data = stream;
#ifdef CWIST_WEBTRANSPORT
    st->is_webtransport = 0;
#endif

    /* Handle server-pushed streams */
    if (h3_ctx && h3_ctx->push_enabled && lsquic_stream_is_pushed(stream)) {
        /* Pushed streams have their request headers already included
         * in the PUSH_PROMISE.  We process them the same way. */
        lsquic_stream_wantread(stream, 1);
        return (lsquic_stream_ctx_t *)st;
    }

    /* RFC 9218 Section 6: a server MUST NOT send a PRIORITY_UPDATE frame for
     * a request stream.  lsquic_stream_set_priority() emits exactly that
     * frame when ext priorities are negotiated, and strict clients (Firefox/
     * neqo) close the connection with H3_FRAME_UNEXPECTED.  Keep the default
     * scheduling priority instead of touching the stream. */
    lsquic_stream_wantread(stream, 1);

    return (lsquic_stream_ctx_t *)st;
}

/**
 * @brief Split a :path value into req->path and req->query (parsed map).
 *
 * Allocates the query map in @p req 's arena when a query string is present.
 * No query string: the whole value becomes the path.
 */
static void h3_parse_path(cwist_http_request *req, const char *path) {
    const char *q = strchr(path, '?');
    if (q) {
        cwist_sstring_assign_len(req->path, (char *)path, (size_t)(q - path));
        cwist_sstring_assign(req->query, (char *)(q + 1));
        if (req->query_params) {
            cwist_query_map_clear(req->query_params);
        } else {
            req->query_params = cwist_query_map_create_in_arena(req->arena);
        }
        if (req->query_params && req->query->size > 0) {
            cwist_query_map_parse(req->query_params, req->query->data);
        }
    } else {
        cwist_sstring_assign(req->path, (char *)path);
    }
}

static void h3_apply_header(cwist_http_request *req,
                            const char *name, const char *value) {
    if (strcmp(name, ":method") == 0) {
        req->method = cwist_http_string_to_method(value);
    } else if (strcmp(name, ":path") == 0) {
        h3_parse_path(req, value);
    } else if (strcmp(name, ":authority") == 0 || strcmp(name, "host") == 0) {
        if (!cwist_http_header_get(req->headers, "host")) {
            cwist_http_header_add(&req->headers, "host", value);
        }
    } else if (strcmp(name, ":scheme") == 0) {
        /* RFC 9114: silently ignore pseudo-headers we don't need to expose */
    } else if (strcmp(name, "content-length") == 0) {
        char *endptr = NULL;
        unsigned long long cl = strtoull(value, &endptr, 10);
        if (endptr && *endptr == '\0') {
            req->content_length = (size_t)cl;
        }
        cwist_http_header_add(&req->headers, name, value);
    } else if (strcmp(name, "content-type") == 0) {
        cwist_http_header_add(&req->headers, name, value);
    } else if (strcmp(name, "cookie") == 0) {
        cwist_http_header_add(&req->headers, name, value);
    } else if (strcmp(name, "authorization") == 0) {
        cwist_http_header_add(&req->headers, name, value);
    } else if (strcmp(name, "accept") == 0) {
        cwist_http_header_add(&req->headers, name, value);
    } else if (strcmp(name, "user-agent") == 0) {
        cwist_http_header_add(&req->headers, name, value);
    } else if (strcmp(name, "accept-encoding") == 0) {
        cwist_http_header_add(&req->headers, name, value);
    } else if (strcmp(name, "accept-language") == 0) {
        cwist_http_header_add(&req->headers, name, value);
    } else if (strcmp(name, "referer") == 0 || strcmp(name, "referrer") == 0) {
        cwist_http_header_add(&req->headers, "referer", value);
    } else if (strcmp(name, "origin") == 0) {
        cwist_http_header_add(&req->headers, name, value);
    } else if (strcmp(name, "x-requested-with") == 0) {
        cwist_http_header_add(&req->headers, name, value);
    } else if (strcmp(name, "priority") == 0) {
        /* RFC 9218 Extensible Priorities: u=urgency, i=incremental.
         * Record the header only: calling lsquic_stream_set_priority() here
         * would emit a PRIORITY_UPDATE frame for a request stream, which a
         * server MUST NOT send (strict clients abort with
         * H3_FRAME_UNEXPECTED). */
        cwist_http_header_add(&req->headers, name, value);
    } else if (name[0] != ':') {
        /* Any other non-pseudo header */
        cwist_http_header_add(&req->headers, name, value);
    }
}

/**
 * @brief Append one wire chunk to the sequenced-body buffer and feed every
 *        complete TASFA chunk to the ARQ assembler.
 * @retval 0 Chunk buffered and/or consumed; possibly no complete chunk yet.
 * @retval -1 Length overflow, assembler-capacity breach, OOM, or a chunk
 *            that fails to parse/feed; the caller resets the stream.
 *
 * HTTP/3 delivers an ordered QUIC byte stream, but an application-level
 * sequenced body can be split at arbitrary read boundaries.  Only one wire
 * chunk is buffered at a time; complete chunks are handed to the common
 * TASFA-style ARQ assembler.
 */
static int h3_seq_append_and_feed(h3_stream_ctx_t *st, const unsigned char *data, size_t len) {
    if (len > SIZE_MAX - st->seq_len) return -1;
    size_t need = st->seq_len + len;
    if (need > st->seq_cap) {
        size_t cap = st->seq_cap ? st->seq_cap : 4096;
        while (cap < need) {
            if (cap > (CWIST_SEQ_HEADER_SIZE + UINT16_MAX) / 2) {
                cap = CWIST_SEQ_HEADER_SIZE + UINT16_MAX;
                break;
            }
            cap *= 2;
        }
        if (cap < need || cap > CWIST_SEQ_HEADER_SIZE + UINT16_MAX) return -1;
        unsigned char *tmp = realloc(st->seq_buf, cap);
        if (!tmp) return -1;
        st->seq_buf = tmp;
        st->seq_cap = cap;
    }
    memcpy(st->seq_buf + st->seq_len, data, len);
    st->seq_len += len;

    while (st->seq_len >= CWIST_SEQ_HEADER_SIZE) {
        size_t payload_len = ((size_t)st->seq_buf[4] << 8) | st->seq_buf[5];
        size_t chunk_len = CWIST_SEQ_HEADER_SIZE + payload_len;
        if (payload_len == 0 || st->seq_len < chunk_len) break;
        cwist_seq_chunk_t chunk;
        if (!cwist_seq_chunk_parse(st->seq_buf, chunk_len, &chunk)) return -1;
        if (!st->body_assembler) {
            st->body_assembler = cwist_seq_assembler_create_limited(CWIST_HTTP_MAX_BODY_SIZE);
            if (!st->body_assembler) return -1;
        }
        if (!cwist_seq_assembler_feed(st->body_assembler, &chunk)) return -1;
        memmove(st->seq_buf, st->seq_buf + chunk_len, st->seq_len - chunk_len);
        st->seq_len -= chunk_len;
    }
    return 0;
}

/**
 * @brief Reject a request whose header block violates RFC 9114 Section 4.3.1.
 *
 * lsquic exposes no stream-reset API in this baseline, so enforcement is a
 * 400 response followed by closing both stream directions (mirrors the
 * existing 413 path), and the request is never dispatched to the handler.
 */
static void h3_reject_malformed_request(lsquic_stream_t *stream, h3_stream_ctx_t *st) {
    st->malformed = 1;
    st->headers_done = 1;
    if (!st->res) st->res = cwist_http_response_create();
    if (st->res) {
        st->res->status_code = CWIST_HTTP_BAD_REQUEST;
        if (st->res->body) {
            cwist_sstring_assign(st->res->body, "{\"error\":\"malformed request\"}");
        }
        if (st->res->headers) {
            cwist_http_header_add(&st->res->headers, "Content-Type", "application/json");
        }
    }
    st->response_ready = 1;
    lsquic_stream_wantread(stream, 0);
    lsquic_stream_shutdown(stream, 0);
    lsquic_stream_wantwrite(stream, 1);
}

/**
 * @brief Decode the request header set and enforce RFC 9114 Section 4.3.1.
 * @retval true Header block complete and valid; st->req is populated and
 *         st->headers_done is set.  Also returns true (idempotently) when
 *         headers were already processed.
 * @retval false Headers not yet available, or the block is malformed and a
 *         400 response has been staged (st->malformed).
 *
 * Checks pseudo-header ordering/uniqueness, the CONNECT vs extended-CONNECT
 * completeness rules, mandatory :method/:scheme/:path for normal requests,
 * and the OPTIONS-only asterisk-form exception for an empty :path.
 */
static bool h3_process_stream_headers(lsquic_stream_t *stream, h3_stream_ctx_t *st) {
    if (st->headers_done) return true;
    void *hset = lsquic_stream_get_hset(stream);
    if (!hset) return false;
    cwist_h3_hset_t *hs = (cwist_h3_hset_t *)hset;
    /* lsxpack exposes counted slices, not C strings, so each header needs a
     * NUL-terminated scratch copy to hand to strcmp()/h3_apply_header().
     * These used to be a malloc(name_len+1)/malloc(value_len+1) pair freed
     * at the bottom of every loop iteration - up to two heap round trips
     * per header, tens of them on a real request. The bounds below
     * (name_len <= 1024, value_len <= H3_DECODE_BUF_SIZE - 1) are already
     * enforced before anything touches these buffers, so a single
     * reusable pair sized to those same bounds, declared once outside the
     * loop, replaces all of that: h3_apply_header() copies out of them
     * before the next iteration overwrites them (see below), so reuse is
     * safe. */
    char name_scratch[1024 + 1];
    char value_scratch[H3_DECODE_BUF_SIZE];
    for (size_t i = 0; i < hs->count; ++i) {
        const struct lsxpack_header *xhdr = &hs->headers[i];
        const char *raw_name = lsxpack_header_get_name(xhdr);
        const char *raw_value = lsxpack_header_get_value(xhdr);
        size_t name_len = xhdr->name_len;
        size_t value_len = xhdr->val_len;
        if (raw_name && raw_value && name_len > 0 && name_len <= 1024 &&
            value_len <= H3_DECODE_BUF_SIZE - 1) {
            char *name = name_scratch;
            char *value = value_scratch;
            memcpy(name, raw_name, name_len);
            name[name_len] = '\0';
            memcpy(value, raw_value, value_len);
            value[value_len] = '\0';
            if (name[0] == ':') {
                /* RFC 9114 Section 4.3.1: pseudo-headers precede regular
                 * headers, appear at most once, and only the defined set is
                 * valid in requests. */
                unsigned bit = 0;
                if (strcmp(name, ":method") == 0)
                    bit = H3_PSEUDO_METHOD;
                else if (strcmp(name, ":scheme") == 0)
                    bit = H3_PSEUDO_SCHEME;
                else if (strcmp(name, ":path") == 0)
                    bit = H3_PSEUDO_PATH;
                else if (strcmp(name, ":authority") == 0)
                    bit = H3_PSEUDO_AUTHORITY;
                else if (strcmp(name, ":protocol") == 0)
                    bit = H3_PSEUDO_PROTOCOL;
                if (bit == 0 || st->seen_regular_header || (st->pseudo_seen & bit)) {
                    h3_reject_malformed_request(stream, st);
                    return false;
                }
                st->pseudo_seen |= bit;
                if (bit == H3_PSEUDO_METHOD && strcmp(value, "CONNECT") == 0) {
                    st->is_connect = 1;
                }
                if (bit == H3_PSEUDO_PATH && value_len == 0) {
                    st->saw_empty_path = 1;
                }
            } else {
                st->seen_regular_header = 1;
            }
            h3_apply_header(st->req, name, value);
#ifdef CWIST_WEBTRANSPORT
            if (strcmp(name, ":protocol") == 0 && strcmp(value, "webtransport") == 0) {
                if (st->req && st->req->method == CWIST_HTTP_CONNECT) {
                    st->is_webtransport = 1;
                }
            }
#endif
        }
    }
    /* RFC 9114 Section 4.3.1 completeness rules, evaluated once the whole
     * header block has been seen. */
    if (st->is_connect && !(st->pseudo_seen & H3_PSEUDO_PROTOCOL)) {
        /* Plain CONNECT: :scheme and :path MUST be omitted, :authority is
         * required. */
        if ((st->pseudo_seen & (H3_PSEUDO_SCHEME | H3_PSEUDO_PATH)) ||
            !(st->pseudo_seen & H3_PSEUDO_AUTHORITY)) {
            h3_reject_malformed_request(stream, st);
            return false;
        }
    } else if (st->is_connect) {
        /* Extended CONNECT (RFC 9220, e.g. WebTransport): :scheme, :path and
         * :authority are all required. */
        if (!(st->pseudo_seen & H3_PSEUDO_SCHEME) || !(st->pseudo_seen & H3_PSEUDO_PATH) ||
            !(st->pseudo_seen & H3_PSEUDO_AUTHORITY)) {
            h3_reject_malformed_request(stream, st);
            return false;
        }
    } else {
        /* Normal request: :method, :scheme and :path are mandatory,
         * :authority (or an equivalent Host header) is required for https,
         * and :protocol is only legal on extended CONNECT. */
        if (!(st->pseudo_seen & H3_PSEUDO_METHOD) || !(st->pseudo_seen & H3_PSEUDO_SCHEME) ||
            !(st->pseudo_seen & H3_PSEUDO_PATH) || (st->pseudo_seen & H3_PSEUDO_PROTOCOL) ||
            (!(st->pseudo_seen & H3_PSEUDO_AUTHORITY) &&
             !cwist_http_header_get(st->req->headers, "host"))) {
            h3_reject_malformed_request(stream, st);
            return false;
        }
        /* An empty :path is only legal for OPTIONS (asterisk-form). */
        if (st->saw_empty_path && (!st->req || st->req->method != CWIST_HTTP_OPTIONS)) {
            h3_reject_malformed_request(stream, st);
            return false;
        }
    }
    st->headers_done = 1;
    char *seq_header = cwist_http_header_get(st->req->headers, "x-cwist-sequenced-data");
    st->sequenced_data =
        seq_header && (strcmp(seq_header, "1") == 0 || strcasecmp(seq_header, "true") == 0);
    return true;
}

/**
 * @brief Whether a method is idempotent (RFC 9110) and thus acceptable for
 *        0-RTT replay.
 */
static bool h3_method_is_idempotent(cwist_http_method_t method) {
    switch (method) {
        case CWIST_HTTP_GET:
        case CWIST_HTTP_HEAD:
        case CWIST_HTTP_PUT:
        case CWIST_HTTP_DELETE:
        case CWIST_HTTP_OPTIONS: return true;
        default: return false;
    }
}

/**
 * @brief on_read callback: consume the request and dispatch it.
 *
 * Bodies are accumulated up to CWIST_HTTP_MAX_BODY_SIZE (413 beyond that);
 * sequenced bodies go through h3_seq_append_and_feed() and are only
 * dispatched when the assembler produces a complete payload.  On FIN the
 * request handler runs (with the 0-RTT replay guard for non-idempotent
 * methods while the handshake is in progress), then the response is armed
 * for writing.  Malformed requests get an inline 400 without dispatch.
 * Fatal read/allocation errors reset the stream.
 */
static void cwist_h3_on_read(lsquic_stream_t *stream, lsquic_stream_ctx_t *st_h) {
    h3_stream_ctx_t *st = (h3_stream_ctx_t *)st_h;
    if (!st) return;

    unsigned char buf[8192];
    ssize_t nread;

    h3_process_stream_headers(stream, st);
    if (st->malformed) return;

    while ((nread = lsquic_stream_read(stream, buf, sizeof(buf))) > 0) {
        if (st->sequenced_data) {
            if (h3_seq_append_and_feed(st, buf, (size_t)nread) != 0) {
                lsquic_stream_close(stream);
                return;
            }
            continue;
        }
        size_t need = st->body_len + (size_t)nread;
        if (need > CWIST_HTTP_MAX_BODY_SIZE) {
            /* Body too large: answer 413 with a small JSON body instead of a
             * bare stream close, so the client sees an explicit status
             * (RFC 9110 Section 15.5.14). */
            if (!st->res) st->res = cwist_http_response_create();
            if (st->res) {
                st->res->status_code = 413; /* Payload Too Large */
                if (st->res->body) {
                    cwist_sstring_assign(st->res->body, "{\"error\":\"payload too large\"}");
                }
                if (st->res->headers) {
                    cwist_http_header_add(&st->res->headers, "Content-Type", "application/json");
                }
                st->response_ready = 1;
                lsquic_stream_wantread(stream, 0);
                lsquic_stream_shutdown(stream, 0);
                lsquic_stream_wantwrite(stream, 1);
            } else {
                lsquic_stream_close(stream);
            }
            return;
        }
        if (need > st->body_cap) {
            size_t new_cap = st->body_cap ? st->body_cap * 2 : 4096;
            while (new_cap < need) new_cap *= 2;
            char *tmp = realloc(st->body, new_cap);
            if (!tmp) {
                lsquic_stream_close(stream);
                return;
            }
            st->body = tmp;
            st->body_cap = new_cap;
        }
        memcpy(st->body + st->body_len, buf, (size_t)nread);
        st->body_len += (size_t)nread;
    }

    if (nread == 0) {
        /* End of stream (FIN received) */
        h3_process_stream_headers(stream, st);

        if (st->malformed) return;

        if (!st->headers_done) {
            /* Malformed request: no headers before FIN */
            st->res = cwist_http_response_create();
            if (st->res) {
                st->res->status_code = CWIST_HTTP_BAD_REQUEST;
                if (st->res->body) {
                    cwist_sstring_assign(st->res->body, "{\"error\":\"malformed request\"}");
                }
                if (st->res->headers) {
                    cwist_http_header_add(&st->res->headers, "Content-Type", "application/json");
                }
            }
            st->response_ready = 1;
            lsquic_stream_wantread(stream, 0);
            lsquic_stream_shutdown(stream, 0);
            lsquic_stream_wantwrite(stream, 1);
            return;
        }

        if (st->sequenced_data) {
            const uint8_t *assembled = NULL;
            size_t assembled_len = 0;
            if (st->seq_len != 0 || !st->body_assembler ||
                !cwist_seq_assembler_get_data(st->body_assembler, &assembled, &assembled_len)) {
                /* Do not dispatch a partial request. */
                st->res = cwist_http_response_create();
                if (st->res) {
                    st->res->status_code = CWIST_HTTP_BAD_REQUEST;
                    cwist_http_header_add(&st->res->headers, "x-cwist-retry", "1");
                    if (st->res->body) {
                        cwist_sstring_assign(st->res->body,
                                             "{\"error\":\"incomplete sequenced body, retry\"}");
                    }
                    cwist_http_header_add(&st->res->headers, "Content-Type", "application/json");
                }
                st->response_ready = 1;
                lsquic_stream_wantread(stream, 0);
                lsquic_stream_shutdown(stream, 0);
                lsquic_stream_wantwrite(stream, 1);
                return;
            }
            if (assembled_len == 0) {
                /* realloc(p, 0) may legally return NULL; do not mistake an
                 * empty assembled body for an allocation failure (which used
                 * to reset the stream instead of dispatching the request). */
                free(st->body);
                st->body = NULL;
                st->body_len = 0;
                st->body_cap = 0;
            } else {
                st->body = realloc(st->body, assembled_len);
                if (!st->body) {
                    lsquic_stream_close(stream);
                    return;
                }
                memcpy(st->body, assembled, assembled_len);
                st->body_len = assembled_len;
                st->body_cap = assembled_len;
            }
        }

        if (st->body_len > 0 && st->req && st->req->body) {
            cwist_sstring_assign_len(st->req->body, st->body, st->body_len);
        }

        st->res = cwist_http_response_create();
        if (st->res && st->req) {
            cwist_http3_context *h3_ctx = h3_shared_ctx(lsquic_stream_conn(stream));
#ifdef CWIST_WEBTRANSPORT
            if (st->is_webtransport && h3_ctx && h3_ctx->wt_handler) {
                char *host = cwist_http_header_get(st->req->headers, "host");
                char *origin = cwist_http_header_get(st->req->headers, "origin");
                struct lsquic_wt_connect_info info = {
                    .wtci_authority = host,
                    .wtci_path = st->req->path ? st->req->path->data : NULL,
                    .wtci_origin = origin,
                    .wtci_protocol = NULL,
                    .wtci_draft = lsquic_wt_peer_draft(lsquic_stream_conn(stream)),
                };
                struct lsquic_wt_accept_params params = {
                    .wtap_status = LSQUIC_WTAP_STATUS_DEFAULT,
                    .wtap_wt_if = &cwist_h3_wt_if,
                    .wtap_wt_if_ctx = st,
                    .wtap_connect_info = &info,
                    .wtap_datagram_send_mode = LSQUIC_HTTP_DG_SEND_DEFAULT,
                };
                if (lsquic_wt_accept(stream, &params) == 0) {
                    st->wt_taken = 1;
                    lsquic_stream_wantread(stream, 0);
                    lsquic_stream_shutdown(stream, 0);
                    lsquic_stream_wantwrite(stream, 0);
                    return;
                }
                /* Accept failed: falling through with the default 200/empty
                 * response used to send the client a silent 0-byte body. */
                CWIST_LOG_WARN("[HTTP/3] WebTransport accept failed; answering 500");
                st->res->status_code = CWIST_HTTP_INTERNAL_ERROR;
                if (st->res->body && st->res->body->size == 0) {
                    cwist_sstring_assign(st->res->body,
                                         "{\"error\":\"webtransport accept failed\"}");
                }
                if (st->res->headers) {
                    cwist_http_header_add(&st->res->headers, "Content-Type", "application/json");
                }
            } else
#endif
                if (h3_ctx && h3_ctx->handler) {
                /* 0-RTT replay guard: requests delivered while the QUIC
                 * handshake is still in progress arrived as early data.
                 * lsquic (this baseline) exposes no
                 * lsquic_conn_is_early_data_accepted()-style query, so the
                 * guard uses the public lsquic_conn_status() instead. */
                bool is_early_data = h3_ctx->early_data_enabled && h3_ctx->early_data_guard &&
                                     lsquic_conn_status(lsquic_stream_conn(stream), NULL, 0) ==
                                         LSCONN_ST_HSK_IN_PROGRESS;
                if (is_early_data && st->req && !h3_method_is_idempotent(st->req->method)) {
                    /* RFC 8470: refuse replayable non-idempotent early data
                     * so the client retries after the handshake.  Attach a
                     * body: without one the response goes out as HEADERS+FIN
                     * with no content-length, which some clients surface as
                     * an empty (0-byte) reply instead of status 425. */
                    st->res->status_code = 425; /* Too Early */
                    if (st->res->body) {
                        cwist_sstring_assign(st->res->body,
                                             "{\"error\":\"too early, retry after handshake\"}");
                    }
                    if (st->res->headers) {
                        cwist_http_header_add(&st->res->headers, "Content-Type",
                                              "application/json");
                    }
                } else {
                    h3_ctx->handler(h3_ctx->user_ctx, st->req, st->res);
                }
            }
        }
        st->response_ready = 1;
        lsquic_stream_wantread(stream, 0);
        lsquic_stream_shutdown(stream, 0);
        lsquic_stream_wantwrite(stream, 1);
    } else if (nread < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        lsquic_stream_close(stream);
    }
}

/**
 * @brief Whether a content-length value is a non-empty run of digits
 *        (RFC 9110 Section 8.6).  Anything else from a handler (whitespace,
 *        junk, negative, empty) is rejected rather than forwarded into the
 *        response headers.
 */
static bool h3_content_length_is_valid(const char *cl) {
    if (!cl || cl[0] < '0' || cl[0] > '9') return false;
    for (const char *p = cl + 1; *p; ++p) {
        if (*p < '0' || *p > '9') return false;
    }
    return true;
}

/**
 * @brief Whether a status code forbids a response body (RFC 9114 Section
 *        4.1.2: 1xx/204/304); content-length on them would make the message
 *        malformed at connection level.
 */
static bool h3_status_forbids_body(int status_code) {
    return (status_code >= 100 && status_code < 200) || status_code == 204 || status_code == 304;
}

/**
 * @brief on_write callback: serialize the response (HEADERS, then body).
 *
 * Three write states: 0 builds and sends the HEADERS block (status coercion
 * to 500 for invalid values, content-length computed from the actual body,
 * RFC 9114-forbidden connection headers dropped, empty-body responses
 * finished with an explicit shutdown(1) because the sendmsg eos flag is
 * ignored for IETF QUIC); 1 streams the body from memory, pointer, or file
 * (file streaming is chunked a few pread()s per tick so one large response
 * cannot starve the other connections); 2 is done and re-arms reading.
 * Unusable body sources (NULL pointer body / bad fd with non-zero length)
 * reset the stream rather than announce bytes that can never be delivered.
 */
static void cwist_h3_on_write(lsquic_stream_t *stream, lsquic_stream_ctx_t *st_h) {
    h3_stream_ctx_t *st = (h3_stream_ctx_t *)st_h;
    if (!st || !st->response_ready) return;
    if (!st->res) {
        /* Response allocation failed upstream; reset instead of dereferencing
         * NULL (which used to take down every stream on the connection). */
        lsquic_stream_close(stream);
        return;
    }

    if (st->write_state == 0) {
        struct lsxpack_header headers_arr[H3_MAX_RESPONSE_HEADERS];
        char hbuf[H3_RESPONSE_HEADER_BUF_SIZE];
        size_t hbuf_off = 0;
        size_t hdr_count = 0;

        bool bodyless = h3_status_forbids_body(st->res->status_code);
        bool is_head = st->req && st->req->method == CWIST_HTTP_HEAD;

        /* Handler attached a body source that cannot produce bytes
         * (NULL pointer body / invalid file fd with a non-zero length).
         * Sending HEADERS would announce (or imply) a body we cannot
         * deliver; close the stream so the client sees an explicit error
         * instead of a silent empty 200. */
        if (!bodyless && !is_head &&
            ((st->res->is_ptr_body && !st->res->ptr_body && st->res->ptr_body_len > 0) ||
             (st->res->use_file_stream && st->res->file_stream_fd < 0 &&
              st->res->file_stream_len > 0))) {
            CWIST_LOG_WARN("[HTTP/3] unusable response body source; resetting stream");
            lsquic_stream_close(stream);
            return;
        }

        /* :status */
        if (st->res->status_code < 200 || st->res->status_code > 999) {
            /* RFC 9110 Section 15: a final response needs a status in
             * 200-999 (RFC 9114 Section 4.1.2 requires three digits).
             * A handler that leaves the status unset, picks 1xx (an
             * interim response, illegal as the complete answer), or an
             * out-of-range value gets coerced to 500 instead of putting
             * a malformed :status on the wire. */
            CWIST_LOG_WARN("[HTTP/3] invalid status %d; coercing to 500", st->res->status_code);
            st->res->status_code = CWIST_HTTP_INTERNAL_ERROR;
            bodyless = false;
            if (!is_head && st->res->body && st->res->body->size == 0 && !st->res->is_ptr_body &&
                !st->res->use_file_stream) {
                cwist_sstring_assign(st->res->body, "{\"error\":\"internal server error\"}");
            }
        }
        int status_code = st->res->status_code;
        char status_str[16];
        snprintf(status_str, sizeof(status_str), "%d", status_code);
        size_t slen = strlen(status_str);
        if (hdr_count < H3_MAX_RESPONSE_HEADERS && slen > 0 &&
            hbuf_off + 7 + 2 + slen <= sizeof(hbuf)) {
            memcpy(hbuf + hbuf_off, ":status", 7);
            memcpy(hbuf + hbuf_off + 9, status_str, slen);
            lsxpack_header_set_offset2(&headers_arr[hdr_count], hbuf + hbuf_off, 0, 7, 9, slen);
            hbuf_off += 9 + slen;
            hdr_count++;
        }

        /* content-length */
        size_t body_len = 0;
        if (st->res->use_file_stream) body_len = st->res->file_stream_len;
        else if (st->res->is_ptr_body) body_len = st->res->ptr_body_len;
        else if (st->res->body) body_len = st->res->body->size;

        const char *user_cl =
            st->res->headers ? cwist_http_header_get(st->res->headers, "content-length") : NULL;

        if (!bodyless && (body_len > 0 || is_head) && hdr_count < H3_MAX_RESPONSE_HEADERS) {
            /* For HEAD, content-length describes the would-be GET body: keep
             * the handler-provided value when present (e.g. static file size
             * with an empty body), otherwise compute it like a GET.  For all
             * other methods the computed body length is authoritative: it is
             * exactly what the write path below will send, so trusting a
             * divergent handler value would announce bytes never delivered. */
            char cl_str[32];
            const char *cl_val = NULL;
            if (is_head && user_cl) {
                if (h3_content_length_is_valid(user_cl)) {
                    cl_val = user_cl;
                } else {
                    /* Malformed handler content-length must not reach the
                     * wire; fall back to the computed body length. */
                    CWIST_LOG_WARN("[HTTP/3] dropping invalid HEAD content-length '%s'", user_cl);
                }
            }
            if (!cl_val) {
                snprintf(cl_str, sizeof(cl_str), "%zu", body_len);
                cl_val = cl_str;
            }
            if (cl_val && cl_val[0] != '\0') {
                size_t cl_name_len = 14;
                size_t cl_val_len = strlen(cl_val);
                size_t total = cl_name_len + 2 + cl_val_len;
                if (cl_val_len > 0 && hbuf_off + total <= sizeof(hbuf)) {
                    memcpy(hbuf + hbuf_off, "content-length", cl_name_len);
                    memcpy(hbuf + hbuf_off + cl_name_len + 2, cl_val, cl_val_len);
                    lsxpack_header_set_offset2(&headers_arr[hdr_count], hbuf + hbuf_off, 0,
                                               cl_name_len, cl_name_len + 2, cl_val_len);
                    hbuf_off += total;
                    hdr_count++;
                }
            }
        }

        /* content-type (if present) */
        if (st->res->headers) {
            char *ct = cwist_http_header_get(st->res->headers, "content-type");
            if (ct && ct[0] != '\0' && hdr_count < H3_MAX_RESPONSE_HEADERS) {
                size_t klen = 12;
                size_t vlen = strlen(ct);
                if (vlen > 0 && hbuf_off + klen + 2 + vlen <= sizeof(hbuf)) {
                    memcpy(hbuf + hbuf_off, "content-type", klen);
                    memcpy(hbuf + hbuf_off + klen + 2, ct, vlen);
                    lsxpack_header_set_offset2(&headers_arr[hdr_count], hbuf + hbuf_off, 0, klen,
                                               klen + 2, vlen);
                    hbuf_off += klen + 2 + vlen;
                    hdr_count++;
                }
            }
        }

        /* user headers (skip content-length/content-type already handled) */
        cwist_http_header_node *node = st->res->headers;
        while (node && hdr_count < H3_MAX_RESPONSE_HEADERS) {
            if (node->key && node->key->data && node->key->size > 0 && node->value &&
                node->value->data) {
                char h3_name[256];
                if (cwist_http3_normalize_response_header_name(node->key->data, h3_name,
                                                               sizeof(h3_name)) != 0 ||
                    h3_name[0] == '\0' || strlen(node->value->data) != node->value->size ||
                    !cwist_http3_response_header_value_is_safe(node->value->data)) {
                    node = node->next;
                    continue;
                }

                if (strcmp(h3_name, "content-length") == 0 ||
                    strcmp(h3_name, "content-type") == 0) {
                    node = node->next;
                    continue;
                }
                /* RFC 9114 section 4.2: connection-specific fields are malformed in
                 * HTTP/3; te is only allowed with the value "trailers". */
                if (strcmp(h3_name, "connection") == 0 || strcmp(h3_name, "keep-alive") == 0 ||
                    strcmp(h3_name, "proxy-connection") == 0 ||
                    strcmp(h3_name, "transfer-encoding") == 0 || strcmp(h3_name, "upgrade") == 0 ||
                    (strcmp(h3_name, "te") == 0 &&
                     strcasecmp(node->value->data, "trailers") != 0)) {
                    node = node->next;
                    continue;
                }

                size_t klen = strlen(h3_name);
                size_t vlen = node->value->size;
                if (klen > 0 && hbuf_off + klen + 2 + vlen <= sizeof(hbuf)) {
                    memcpy(hbuf + hbuf_off, h3_name, klen);
                    memcpy(hbuf + hbuf_off + klen + 2, node->value->data, vlen);
                    lsxpack_header_set_offset2(&headers_arr[hdr_count], hbuf + hbuf_off, 0, klen,
                                               klen + 2, vlen);
                    hbuf_off += klen + 2 + vlen;
                    hdr_count++;
                }
            }
            node = node->next;
        }

        if (status_code == CWIST_HTTP_OK && !bodyless && !is_head && body_len == 0) {
            /* Legal per RFC 9110, but a 200 with no body and no
             * content-length usually means a handler forgot to assign one;
             * make it diagnosable in the logs. */
            CWIST_LOG_WARN("[HTTP/3] handler produced 200 with empty body");
        }

        size_t initial_body_len = 0;
        if (st->res->use_file_stream && st->res->file_stream_fd >= 0) {
            initial_body_len = st->res->file_stream_len;
        } else if (st->res->is_ptr_body && st->res->ptr_body) {
            initial_body_len = st->res->ptr_body_len;
        } else if (st->res->body) {
            initial_body_len = st->res->body->size;
        }
        int eos = (bodyless || is_head || initial_body_len == 0) ? 1 : 0;

        lsquic_http_headers_t headers = {
            .count = (unsigned)hdr_count,
            .headers = headers_arr,
        };
        if (lsquic_stream_send_headers(stream, &headers, eos) != 0) {
            lsquic_stream_close(stream);
            return;
        }
        if (eos) {
            /* NB: the eos argument of lsquic_stream_send_headers is ignored
             * for IETF QUIC, so an empty body must be finished with an
             * explicit shutdown(1) here.  Relying on eos leaves every
             * empty-body response (204s, 304s, HEAD, error statuses) without
             * FIN, which browsers surface as a protocol error — timing- and
             * RTT-dependent because of header-block stashing under low cwnd. */
            lsquic_stream_shutdown(stream, 1);
            st->write_state = 2;
            lsquic_stream_wantread(stream, 1);
            return;
        }
        st->write_state = 1;
        lsquic_stream_wantwrite(stream, 1);
        return;
    }

    if (st->write_state == 1 && st->res) {
        bool bodyless = h3_status_forbids_body(st->res->status_code);
        bool is_head = st->req && st->req->method == CWIST_HTTP_HEAD;
        size_t body_len = 0;
        const char *body_data = NULL;

        if (st->res->use_file_stream) {
            /* Fill the congestion window on each callback: loop chunk writes
             * until the stream says EAGAIN instead of one 64 KiB chunk per
             * tick - but cap how many synchronous pread()s a single callback
             * does. This thread also drives lsquic_engine_process_conns()
             * and packet I/O for every other connection; with an unbounded
             * loop, one big response on a wide-open congestion window turns
             * into dozens of blocking pread()s back to back (worse under
             * disk contention/page faults than the SSD-idle case), starving
             * every other connection's ACKs/packets for that whole stretch.
             * Re-arming wantwrite and returning after H3_FILE_CHUNKS_PER_TICK
             * chunks gives the event loop a chance to service other
             * connections between bursts; lsquic calls this back again on
             * the next tick to keep going from where body_sent left off. */
#define H3_FILE_CHUNKS_PER_TICK 4
            int chunks_this_tick = 0;
            while (st->res->file_stream_fd >= 0 && st->body_sent < st->res->file_stream_len) {
                if (chunks_this_tick >= H3_FILE_CHUNKS_PER_TICK) {
                    lsquic_stream_wantwrite(stream, 1);
                    return;
                }
                static __thread char file_buf[65536];
                off_t offset = st->res->file_stream_offset + (off_t)st->body_sent;
                size_t to_read = st->res->file_stream_len - st->body_sent;
                if (to_read > sizeof(file_buf)) to_read = sizeof(file_buf);
                ssize_t nr = pread(st->res->file_stream_fd, file_buf, to_read, offset);
                if (nr > 0) {
                    chunks_this_tick++;
                    ssize_t nw = lsquic_stream_write(stream, file_buf, (size_t)nr);
                    if (nw < 0) {
                        lsquic_stream_close(stream);
                        return;
                    }
                    st->body_sent += (size_t)nw;
                } else if (nr == 0) {
                    /* EOF: mark everything as sent */
                    st->body_sent = st->res->file_stream_len;
                } else if (nr < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        /* Retry next tick */
                    } else {
                        lsquic_stream_close(stream);
                        return;
                    }
                    st->send_xor ^= h3_xor_bytes((const unsigned char *)file_buf, (size_t)nw);
                    st->body_sent += (size_t)nw;
                    if (nw < nr) {
                        /* Stream buffer full for now; come back when writable. */
                        lsquic_stream_wantwrite(stream, 1);
                        return;
                    }
                    continue;
                } else if (nr == 0) {
                    /* Short EOF (file truncated after fstat): the announced
                     * content-length can no longer be honoured, and finishing
                     * anyway would be a malformed-message error at the
                     * client.  Reset the stream instead. */
                    lsquic_stream_close(stream);
                    return;
                } else {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        /* Retry next tick */
                        lsquic_stream_wantwrite(stream, 1);
                        return;
                    }
                    lsquic_stream_close(stream);
                    return;
                }
            }
#undef H3_FILE_CHUNKS_PER_TICK
            if (st->res->file_stream_fd >= 0) body_len = st->res->file_stream_len;
        } else if (st->res->is_ptr_body) {
            if (!st->res->ptr_body && st->res->ptr_body_len > 0) {
                /* Announced a body we cannot produce; spinning here with
                 * wantwrite armed would hang the stream forever. */
                lsquic_stream_close(stream);
                return;
            }
            body_len = st->res->ptr_body_len;
            body_data = (const char *)st->res->ptr_body;
        } else if (st->res->body) {
            body_len = st->res->body->size;
            body_data = st->res->body->data;
        }

        /* Skip payload writing entirely if body is empty, forbidden, or already fully sent */
        if (bodyless || is_head || body_len == 0 || st->body_sent >= body_len) {
            st->write_state = 2;
            lsquic_stream_shutdown(stream, 1);
            lsquic_stream_wantread(stream, 1);
            return;
        }

        while (body_data && body_len > 0 && st->body_sent < body_len) {
            ssize_t n =
                lsquic_stream_write(stream, body_data + st->body_sent, body_len - st->body_sent);
            if (n > 0) {
                st->send_xor ^=
                    h3_xor_bytes((const unsigned char *)(body_data + st->body_sent), (size_t)n);
                st->body_sent += (size_t)n;
                if (st->body_sent < body_len) {
                    lsquic_stream_wantwrite(stream, 1);
                }
                continue;
            }
            if (n < 0) {
                lsquic_stream_close(stream);
                return;
            }
            st->body_sent += (size_t)n;
        }

        if (st->body_sent >= body_len) {
            st->write_state = 2;
            lsquic_stream_shutdown(stream, 1);
            lsquic_stream_wantread(stream, 1);
        } else {
            lsquic_stream_wantwrite(stream, 1);
        }
    }
}

/**
 * @brief on_close callback: destroy the stream ctx and everything it owns
 *        (request, response, body buffer, sequenced-data assembler).
 */
static void cwist_h3_on_close(lsquic_stream_t *stream, lsquic_stream_ctx_t *st_h) {
    h3_stream_ctx_t *st = (h3_stream_ctx_t *)st_h;
    if (st) {
        cwist_http_request_destroy(st->req);
        cwist_http_response_destroy(st->res);
        free(st->body);
        cwist_seq_assembler_destroy(st->body_assembler);
        free(st->seq_buf);
        free(st);
    }
    (void)stream;
}

/**
 * @brief on_dg_write callback: hand the oldest queued datagram to lsquic.
 * @return Bytes copied into @p buf, or 0 when the connection's queue is
 *         empty or the connection has no wrapper ctx.
 *
 * Pulls from this connection's own mutex-guarded queue (see h3_conn_ctx_t).
 * Previously this read/freed a single process-wide g_h3_dgram struct with no
 * lock: two connections sending datagrams at the same time could free() the
 * same buffer twice or hand one connection's payload to another.  When the
 * queue drains, want_datagram_write is disarmed so lsquic stops calling back.
 */
static ssize_t cwist_h3_on_dg_write(lsquic_conn_t *conn, void *buf, size_t len) {
    h3_conn_ctx_t *cc = (h3_conn_ctx_t *)lsquic_conn_get_ctx(conn);
    if (!cc) return 0;

    pthread_mutex_lock(&cc->dgram_lock);
    h3_dgram_node_t *n = cc->dgram_head;
    if (!n) {
        pthread_mutex_unlock(&cc->dgram_lock);
        return 0;
    }
    size_t to_copy = n->len < len ? n->len : len;
    memcpy(buf, n->data, to_copy);
    cc->dgram_head = n->next;
    if (!cc->dgram_head) cc->dgram_tail = NULL;
    bool more_queued = cc->dgram_head != NULL;
    pthread_mutex_unlock(&cc->dgram_lock);

    free(n->data);
    free(n);
    /* Keep want_datagram_write armed while the queue is non-empty so
     * lsquic calls back for the next entry. */
    if (!more_queued) lsquic_conn_want_datagram_write(conn, 0);
    return (ssize_t)to_copy;
}

/**
 * @brief on_datagram callback: deliver an incoming H3 datagram to the
 *        application's registered datagram callback, if any.
 */
static void cwist_h3_on_datagram(lsquic_conn_t *conn, const void *buf, size_t len) {
    cwist_http3_context *ctx = h3_shared_ctx(conn);
    if (ctx && ctx->datagram_cb) {
        ctx->datagram_cb(buf, len, ctx->datagram_user_ctx);
    }
}

#ifdef CWIST_WEBTRANSPORT

/**
 * @brief wti_on_session_open callback: accept an extended-CONNECT request as
 *        a WebTransport session.
 * @return Session ctx (a new session-kind cwist_wt_handle_t), or NULL if the
 *         stream ctx is unusable or the handle cannot be allocated.
 *
 * Invokes the application's wt_handler with the request/response pair so it
 * can respond and take over the session.
 */
static lsquic_wt_session_ctx_t *
cwist_h3_wt_on_session_open(void *ctx, lsquic_wt_session_t *sess,
                            const struct lsquic_wt_connect_info *info) {
    (void)info;
    h3_stream_ctx_t *st = (h3_stream_ctx_t *)ctx;
    if (st && st->req && st->res) {
        lsquic_conn_t *conn = lsquic_wt_session_conn(sess);
        cwist_http3_context *h3_ctx = h3_shared_ctx(conn);
        cwist_wt_handle_t *session_handle = cwist_wt_handle_new(CWIST_WT_HANDLE_SESSION, sess);
        if (!session_handle) return NULL;
        if (h3_ctx && h3_ctx->wt_handler) {
            h3_ctx->wt_handler(st->req, st->res, session_handle);
        }
        return (lsquic_wt_session_ctx_t *)session_handle;
    }
    return NULL;
}

/**
 * @brief wti_on_session_rejected callback: the peer rejected the session.
 * Intentionally a no-op; rejection details are not surfaced to the app.
 */
static void cwist_h3_wt_on_session_rejected(void *ctx, const struct lsquic_wt_connect_info *info,
                                            unsigned status, const char *reason,
                                            size_t reason_len) {
    (void)ctx;
    (void)info;
    (void)status;
    (void)reason;
    (void)reason_len;
}

static void cwist_h3_wt_on_session_close(lsquic_wt_session_t *sess,
                                         lsquic_wt_session_ctx_t *sess_ctx, uint64_t code,
                                         const char *reason, size_t reason_len) {
    (void)sess;
    (void)code;
    (void)reason;
    (void)reason_len;
    cwist_wt_handle_free((cwist_wt_handle_t *)sess_ctx);
}

/**
 * @brief Open-callback helper shared by the uni- and bidi-stream callbacks:
 *        wrap the stream in a stream-kind handle and notify the app.
 * @return Stream ctx (a stream-kind handle), or NULL only if handle
 *         allocation fails while a new-stream handler is registered.
 *
 * When no new-stream handler is registered the stream is still wrapped so it
 * stays readable; the app can take it over later.
 */
static lsquic_stream_ctx_t *cwist_h3_wt_on_stream(lsquic_wt_session_t *sess,
                                                  lsquic_stream_t *stream) {
    cwist_http3_context *ctx = NULL;
    if (sess) {
        lsquic_conn_t *conn = lsquic_wt_session_conn(sess);
        if (conn) {
            ctx = h3_shared_ctx(conn);
        }
    }
    if (ctx && ctx->wt_new_stream_handler) {
        cwist_wt_handle_t *stream_handle = cwist_wt_handle_new(CWIST_WT_HANDLE_STREAM, stream);
        if (!stream_handle) return NULL;
        ctx->wt_new_stream_handler(stream_handle, ctx->wt_new_stream_ctx);
        lsquic_stream_wantread(stream, 1);
        return (lsquic_stream_ctx_t *)stream_handle;
    }
    lsquic_stream_wantread(stream, 1);
    return (lsquic_stream_ctx_t *)cwist_wt_handle_new(CWIST_WT_HANDLE_STREAM, stream);
}

/**
 * @brief wti_on_uni_stream callback: peer opened a unidirectional stream.
 * Delegates to cwist_h3_wt_on_stream().
 */
static lsquic_stream_ctx_t *cwist_h3_wt_on_uni_stream(lsquic_wt_session_t *sess,
                                                      lsquic_stream_t *stream) {
    return cwist_h3_wt_on_stream(sess, stream);
}

/**
 * @brief wti_on_bidi_stream callback: peer opened a bidirectional stream.
 * Delegates to cwist_h3_wt_on_stream().
 */
static lsquic_stream_ctx_t *cwist_h3_wt_on_bidi_stream(lsquic_wt_session_t *sess,
                                                       lsquic_stream_t *stream) {
    return cwist_h3_wt_on_stream(sess, stream);
}

/**
 * @brief wti_on_stream_read callback: first data on a WT stream; notifies
 *        the app's new-stream handler with the stream handle.
 */
static void cwist_h3_wt_on_stream_read(lsquic_stream_t *stream, lsquic_stream_ctx_t *st_h) {
    cwist_wt_handle_t *stream_handle = (cwist_wt_handle_t *)st_h;
    lsquic_wt_session_t *sess = lsquic_wt_session_from_stream(stream);
    if (!sess) return;
    lsquic_conn_t *conn = lsquic_wt_session_conn(sess);
    cwist_http3_context *ctx = h3_shared_ctx(conn);
    if (ctx && ctx->wt_new_stream_handler) {
        ctx->wt_new_stream_handler(stream_handle, ctx->wt_new_stream_ctx);
    }
}

/**
 * @brief wti_on_stream_write callback: nothing to write from the server
 *        side for WT streams; no-op.
 */
static void cwist_h3_wt_on_stream_write(lsquic_stream_t *stream, lsquic_stream_ctx_t *st_h) {
    (void)stream;
    (void)st_h;
}

/**
 * @brief wti_on_stream_close callback: free the stream handle.
 */
static void cwist_h3_wt_on_stream_close(lsquic_stream_t *stream, lsquic_stream_ctx_t *st_h) {
    (void)stream;
    cwist_wt_handle_free((cwist_wt_handle_t *)st_h);
}

static uint64_t cwist_h3_wt_on_stream_ss_code(lsquic_stream_t *stream, lsquic_stream_ctx_t *st_h) {
    (void)stream;
    (void)st_h;
    return 0;
}

/**
 * @brief WT datagram-read callback: deliver the payload to the app's
 *        registered datagram callback, if any.
 */
static void cwist_h3_wt_on_datagram_read(lsquic_wt_session_t *sess, const void *buf, size_t len) {
    lsquic_conn_t *conn = lsquic_wt_session_conn(sess);
    cwist_http3_context *ctx = h3_shared_ctx(conn);
    if (ctx && ctx->datagram_cb) {
        ctx->datagram_cb(buf, len, ctx->datagram_user_ctx);
    }
}

/**
 * @brief WT datagram-write callback: nothing queued from the server side;
 *        returns 0 (no datagram to send).
 */
static int cwist_h3_wt_on_datagram_write(lsquic_wt_session_t *sess, size_t max_datagram_size) {
    (void)sess;
    (void)max_datagram_size;
    return 0;
}

static const struct lsquic_webtransport_if cwist_h3_wt_if = {
    .wti_on_session_open = cwist_h3_wt_on_session_open,
    .wti_on_session_rejected = cwist_h3_wt_on_session_rejected,
    .wti_on_session_close = cwist_h3_wt_on_session_close,
    .wti_on_uni_stream = cwist_h3_wt_on_uni_stream,
    .wti_on_bidi_stream = cwist_h3_wt_on_bidi_stream,
    .wti_on_stream_read = cwist_h3_wt_on_stream_read,
    .wti_on_stream_write = cwist_h3_wt_on_stream_write,
    .wti_on_stream_close = cwist_h3_wt_on_stream_close,
    .wti_on_stream_ss_code = cwist_h3_wt_on_stream_ss_code,
    .wti_on_datagram_read = cwist_h3_wt_on_datagram_read,
    .wti_on_datagram_write = cwist_h3_wt_on_datagram_write,
};

#endif /* CWIST_WEBTRANSPORT */

static const struct lsquic_stream_if cwist_h3_stream_if = {
    .on_new_conn = cwist_h3_on_new_conn,
    .on_conn_closed = cwist_h3_on_conn_closed,
    .on_new_stream = cwist_h3_on_new_stream,
    .on_read = cwist_h3_on_read,
    .on_write = cwist_h3_on_write,
    .on_close = cwist_h3_on_close,
    .on_dg_write = cwist_h3_on_dg_write,
    .on_datagram = cwist_h3_on_datagram,
};

/* ------------------------------------------------------------------ */
/* Context management                                                 */
/* ------------------------------------------------------------------ */

/* --- Shared session ticket key -------------------------------------------
 * Session resumption (and therefore 0-RTT early data) requires the server to
 * issue resumable tickets.  Mirrors the prefork-safe pattern used by the
 * HTTPS stack (src/net/http/https.c): one random key generated at context
 * creation and installed via an explicit ticket-key callback, so tickets
 * issued by one worker resume on any other (SO_REUSEPORT scatters
 * connections across workers).  The key lives for the process lifetime.
 * ------------------------------------------------------------------------- */
typedef struct cwist_h3_ticket_key {
    unsigned char name[16]; /* key_name sent in the ticket */
    unsigned char aes_key[32]; /* AES-256-CBC encryption key  */
    unsigned char hmac_key[32]; /* HMAC-SHA-256 MAC key        */
} cwist_h3_ticket_key;

static int g_h3_ticket_key_ex_data_idx = -1;
static pthread_once_t g_h3_ticket_key_ex_data_once = PTHREAD_ONCE_INIT;

/**
 * @brief pthread_once body: reserve the SSL_CTX ex-data slot that carries
 *        the per-context ticket key.
 */
static void cwist_h3_ticket_key_ex_data_init(void) {
    g_h3_ticket_key_ex_data_idx = SSL_CTX_get_ex_new_index(0, NULL, NULL, NULL, NULL);
}

/**
 * @brief BoringSSL session-ticket key callback (shared ticket-key scheme).
 * @param encrypt Non-zero when producing a new ticket, zero when resuming.
 * @retval 1 Key applied; proceed with the ticket.
 * @retval 0 Decline the ticket (unknown key name, missing key, or crypto
 *         setup failure); a full handshake results.
 *
 * One random key is generated per cwist_http3_context and installed here, so
 * tickets issued by one SO_REUSEPORT worker resume on any other.
 */
static int cwist_h3_ticket_key_cb(SSL *ssl, uint8_t *key_name, uint8_t *iv, EVP_CIPHER_CTX *ectx,
                                  HMAC_CTX *hctx, int encrypt) {
    SSL_CTX *ssl_ctx = ssl ? SSL_get_SSL_CTX(ssl) : NULL;
    const cwist_h3_ticket_key *key = NULL;
    if (ssl_ctx && g_h3_ticket_key_ex_data_idx >= 0) {
        key =
            (const cwist_h3_ticket_key *)SSL_CTX_get_ex_data(ssl_ctx, g_h3_ticket_key_ex_data_idx);
    }
    if (!key) return 0;

    if (encrypt) {
        memcpy(key_name, key->name, sizeof(key->name));
        if (RAND_bytes(iv, 16) != 1) return 0;
        if (EVP_EncryptInit_ex(ectx, EVP_aes_256_cbc(), NULL, key->aes_key, iv) != 1) return 0;
        if (HMAC_Init_ex(hctx, key->hmac_key, sizeof(key->hmac_key), EVP_sha256(), NULL) != 1)
            return 0;
        return 1;
    }

    if (memcmp(key_name, key->name, sizeof(key->name)) != 0) {
        return 0; /* Unknown key: decline the ticket, do a full handshake. */
    }
    if (EVP_DecryptInit_ex(ectx, EVP_aes_256_cbc(), NULL, key->aes_key, iv) != 1) return 0;
    if (HMAC_Init_ex(hctx, key->hmac_key, sizeof(key->hmac_key), EVP_sha256(), NULL) != 1) return 0;
    return 1;
}

/**
 * @brief Arm the server session cache and the shared ticket key so the
 *        context issues resumable tickets (prerequisite for 0-RTT).
 * @param ssl_ctx Context being configured.
 */
static void cwist_h3_setup_session_tickets(SSL_CTX *ssl_ctx) {
    SSL_CTX_set_session_cache_mode(ssl_ctx, SSL_SESS_CACHE_SERVER);

    pthread_once(&g_h3_ticket_key_ex_data_once, cwist_h3_ticket_key_ex_data_init);
    if (g_h3_ticket_key_ex_data_idx < 0) return;

    cwist_h3_ticket_key *key = (cwist_h3_ticket_key *)cwist_alloc(sizeof(*key));
    if (!key) return;
    if (RAND_bytes((uint8_t *)key, sizeof(*key)) != 1) {
        cwist_free(key);
        return; /* Fall back to the library-internal (per-worker) keys. */
    }
    SSL_CTX_set_ex_data(ssl_ctx, g_h3_ticket_key_ex_data_idx, key);
    SSL_CTX_set_tlsext_ticket_key_cb(ssl_ctx, cwist_h3_ticket_key_cb);
}

/**
 * @brief Free the per-context session-ticket key, if one was installed.
 * @param ssl_ctx Context whose ex-data slot was set by
 *                cwist_h3_setup_session_tickets(); may be NULL (no-op).
 *
 * Thread-safety: takes the pthread_once gate before reading
 * g_h3_ticket_key_ex_data_idx because this can run on a thread that never
 * armed the once itself (see the note in the body).
 */
static void cwist_h3_free_session_ticket_key(SSL_CTX *ssl_ctx) {
    if (!ssl_ctx) return;
    /* g_h3_ticket_key_ex_data_idx is only written once, from inside
     * cwist_h3_ticket_key_ex_data_init() via the pthread_once below -
     * but this function is also reached from cwist_http3_destroy_context()
     * (line ~2196), which can run on a thread that never called
     * cwist_h3_setup_session_tickets()/this same pthread_once itself (e.g.
     * a dedicated shutdown/admin thread tearing down a context another
     * thread created). Without calling pthread_once() here too, that
     * thread has no happens-before edge to the writer and is reading the
     * global race-free only by luck on the caller's platform/compiler.
     * pthread_once() is cheap after the first call, so just always take
     * this gate before reading the index. */
    pthread_once(&g_h3_ticket_key_ex_data_once, cwist_h3_ticket_key_ex_data_init);
    if (g_h3_ticket_key_ex_data_idx < 0) return;
    void *key = SSL_CTX_get_ex_data(ssl_ctx, g_h3_ticket_key_ex_data_idx);
    if (key) {
        SSL_CTX_set_ex_data(ssl_ctx, g_h3_ticket_key_ex_data_idx, NULL);
        cwist_free(key);
    }
}

/**
 * @brief Apply the common server-side SSL_CTX configuration.
 * @param early_data Non-zero to enable TLS 1.3 early data (0-RTT).
 * @return Always 0.  QUIC transport parameters are supplied by lsquic, not
 *         here.
 *
 * Pins TLS 1.3, installs default verify paths, the ALPN selection callback,
 * and the shared session-ticket key machinery.
 */
static int cwist_h3_ssl_ctx_init(SSL_CTX *ssl_ctx, int early_data) {
    SSL_CTX_set_min_proto_version(ssl_ctx, TLS1_3_VERSION);
    SSL_CTX_set_max_proto_version(ssl_ctx, TLS1_3_VERSION);
    SSL_CTX_set_default_verify_paths(ssl_ctx);
    SSL_CTX_set_alpn_select_cb(ssl_ctx, cwist_h3_alpn_select_cb, NULL);
    cwist_h3_setup_session_tickets(ssl_ctx);

    if (early_data) {
        SSL_CTX_set_early_data_enabled(ssl_ctx, 1);
    }

    /* Server-side QUIC transport parameters will be supplied by lsquic */
    return 0;
}

/**
 * @brief Create an HTTP/3 server context from a certificate/key pair.
 * @param ctx Receives the new context.
 * @param cert_path PEM file with the certificate chain.
 * @param key_path PEM file with the private key.
 * @return err.error.err_i16 == 0 on success; -1 on bad arguments, TLS setup
 *         failure, or OOM.  On failure all partial state is unwound and
 *         *ctx is untouched.
 *
 * Takes a process-wide lsquic reference (released by
 * cwist_http3_destroy_context()).
 */
cwist_error_t cwist_http3_init_context(cwist_http3_context **ctx, const char *cert_path,
                                       const char *key_path) {
    cwist_error_t err = make_error(CWIST_ERR_INT16);
    if (!ctx || !cert_path || !key_path) {
        err.error.err_i16 = -1;
        return err;
    }

    h3_global_init();

    const SSL_METHOD *method = TLS_method();
    SSL_CTX *ssl_ctx = SSL_CTX_new(method);
    if (!ssl_ctx) {
        h3_global_cleanup();
        err.error.err_i16 = -1;
        return err;
    }

    cwist_h3_ssl_ctx_init(ssl_ctx, 0);

    if (SSL_CTX_use_certificate_chain_file(ssl_ctx, cert_path) <= 0 ||
        SSL_CTX_use_PrivateKey_file(ssl_ctx, key_path, SSL_FILETYPE_PEM) <= 0) {
        cwist_h3_free_session_ticket_key(ssl_ctx);
        SSL_CTX_free(ssl_ctx);
        h3_global_cleanup();
        err.error.err_i16 = -1;
        return err;
    }

    if (cwist_tls_autoload_intermediates(ssl_ctx) < 0) {
        SSL_CTX_free(ssl_ctx);
        h3_global_cleanup();
        err.error.err_i16 = -1;
        return err;
    }

    if (SSL_CTX_check_private_key(ssl_ctx) != 1) {
        SSL_CTX_free(ssl_ctx);
        h3_global_cleanup();
        err.error.err_i16 = -1;
        return err;
    }

    *ctx = (cwist_http3_context *)cwist_alloc(sizeof(cwist_http3_context));
    if (!*ctx) {
        cwist_h3_free_session_ticket_key(ssl_ctx);
        SSL_CTX_free(ssl_ctx);
        h3_global_cleanup();
        err.error.err_i16 = -1;
        return err;
    }

    memset(*ctx, 0, sizeof(cwist_http3_context));
    (*ctx)->ssl_ctx = ssl_ctx;
    (*ctx)->udp_fd = -1;
    err.error.err_i16 = 0;
    return err;
}

/**
 * @brief Create an HTTP/3 server context with an ephemeral self-signed
 *        certificate ("localhost", RSA-2048, 1-year validity).
 * @param ctx Receives the new context.
 * @return err.error.err_i16 == 0 on success; -1 on TLS setup failure or OOM.
 *         On failure all partial state is unwound.
 *
 * Intended for tests and local deployments where provisioning a real
 * certificate is impractical.  Takes a process-wide lsquic reference
 * (released by cwist_http3_destroy_context()).
 */
cwist_error_t cwist_http3_init_context_ephemeral(cwist_http3_context **ctx) {
    cwist_error_t err = make_error(CWIST_ERR_INT16);
    if (!ctx) {
        err.error.err_i16 = -1;
        return err;
    }

    h3_global_init();

    const SSL_METHOD *method = TLS_method();
    SSL_CTX *ssl_ctx = SSL_CTX_new(method);
    if (!ssl_ctx) {
        h3_global_cleanup();
        err.error.err_i16 = -1;
        return err;
    }

    cwist_h3_ssl_ctx_init(ssl_ctx, 0);

    EVP_PKEY_CTX *pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, NULL);
    if (!pctx) {
        cwist_h3_free_session_ticket_key(ssl_ctx);
        SSL_CTX_free(ssl_ctx);
        h3_global_cleanup();
        err.error.err_i16 = -1;
        return err;
    }
    if (EVP_PKEY_keygen_init(pctx) <= 0 || EVP_PKEY_CTX_set_rsa_keygen_bits(pctx, 2048) <= 0) {
        EVP_PKEY_CTX_free(pctx);
        cwist_h3_free_session_ticket_key(ssl_ctx);
        SSL_CTX_free(ssl_ctx);
        h3_global_cleanup();
        err.error.err_i16 = -1;
        return err;
    }
    EVP_PKEY *pkey = NULL;
    if (EVP_PKEY_keygen(pctx, &pkey) <= 0 || !pkey) {
        EVP_PKEY_CTX_free(pctx);
        cwist_h3_free_session_ticket_key(ssl_ctx);
        SSL_CTX_free(ssl_ctx);
        h3_global_cleanup();
        err.error.err_i16 = -1;
        return err;
    }
    EVP_PKEY_CTX_free(pctx);

    X509 *x509 = X509_new();
    if (!x509) {
        EVP_PKEY_free(pkey);
        cwist_h3_free_session_ticket_key(ssl_ctx);
        SSL_CTX_free(ssl_ctx);
        h3_global_cleanup();
        err.error.err_i16 = -1;
        return err;
    }
    ASN1_INTEGER_set(X509_get_serialNumber(x509), 1);
    X509_gmtime_adj(X509_get_notBefore(x509), 0);
    X509_gmtime_adj(X509_get_notAfter(x509), 31536000L);
    X509_set_pubkey(x509, pkey);

    /* Self-signed subject */
    X509_NAME *subj = X509_get_subject_name(x509);
    X509_NAME_add_entry_by_txt(subj, "CN", MBSTRING_ASC, (const unsigned char *)"localhost", -1, -1,
                               0);
    X509_set_issuer_name(x509, subj);
    X509_sign(x509, pkey, EVP_sha256());

    if (SSL_CTX_use_certificate(ssl_ctx, x509) != 1 || SSL_CTX_use_PrivateKey(ssl_ctx, pkey) != 1) {
        X509_free(x509);
        EVP_PKEY_free(pkey);
        cwist_h3_free_session_ticket_key(ssl_ctx);
        SSL_CTX_free(ssl_ctx);
        h3_global_cleanup();
        err.error.err_i16 = -1;
        return err;
    }

    X509_free(x509);
    EVP_PKEY_free(pkey);

    SSL_CTX_set_alpn_select_cb(ssl_ctx, cwist_http3_alpn_select_cb, NULL);

    *ctx = (cwist_http3_context *)cwist_alloc(sizeof(cwist_http3_context));
    if (!*ctx) {
        cwist_h3_free_session_ticket_key(ssl_ctx);
        SSL_CTX_free(ssl_ctx);
        h3_global_cleanup();
        err.error.err_i16 = -1;
        return err;
    }

    memset(*ctx, 0, sizeof(cwist_http3_context));
    (*ctx)->ssl_ctx = ssl_ctx;
    (*ctx)->udp_fd = -1;
    err.error.err_i16 = 0;
    return err;
}

/**
 * @brief Graceful shutdown: send GOAWAY on every live connection (and stop
 *        accepting new ones), then give the engine one chance to flush the
 *        queued packets before teardown.
 *
 * lsquic_engine_cooldown() marks full connections going away (GOAWAY per
 * RFC 9114 Section 5.2); the ensuing lsquic_engine_destroy() closes them with
 * CONNECTION_CLOSE/H3_NO_ERROR instead of leaving clients hanging on
 * silence.  No-op when the engine was never created.
 */
static void cwist_h3_engine_graceful_stop(cwist_http3_context *ctx) {
    lsquic_engine_t *engine = (lsquic_engine_t *)ctx->engine;
    if (!engine) return;
    lsquic_engine_cooldown(engine);
    lsquic_engine_process_conns(engine);
    if (ctx->udp_fd >= 0 && lsquic_engine_has_unsent_packets(engine)) {
        lsquic_engine_send_unsent_packets(engine);
    }
}

/**
 * @brief Destroy a context: graceful engine stop, engine destroy, hset
 *        sweep, SSL_CTX and ticket-key teardown, context free.
 * @param ctx Context from cwist_http3_init_context*(); may be NULL (no-op).
 *
 * Releases the process-wide lsquic reference taken at init.  Must not be
 * called concurrently with cwist_http3_server_loop() on the same context.
 */
void cwist_http3_destroy_context(cwist_http3_context *ctx) {
    if (ctx) {
        if (ctx->engine) {
            cwist_h3_engine_graceful_stop(ctx);
            lsquic_engine_destroy((lsquic_engine_t *)ctx->engine);
            ctx->engine = NULL;
        }
        cwist_h3_hset_sweep(ctx);
        if (ctx->ssl_ctx) {
            cwist_h3_free_session_ticket_key(ctx->ssl_ctx);
            SSL_CTX_free(ctx->ssl_ctx);
            ctx->ssl_ctx = NULL;
        }
        cwist_free(ctx);
        h3_global_cleanup();
    }
}



#define CWIST_HTTP3_FRAME_DATA     0x00
#define CWIST_HTTP3_FRAME_HEADERS  0x01
#define CWIST_HTTP3_FRAME_SETTINGS 0x04
#define CWIST_HTTP3_FRAME_GOAWAY   0x07

static size_t h3_encode_varint(uint64_t value, unsigned char out[8]) {
    if (value <= 0x3f) {
        out[0] = (unsigned char)value;
        return 1;
    }
    if (value <= 0x3fff) {
        out[0] = (unsigned char)(0x40 | ((value >> 8) & 0x3f));
        out[1] = (unsigned char)(value & 0xff);
        return 2;
    }
    if (value <= 0x3fffffff) {
        out[0] = (unsigned char)(0x80 | ((value >> 24) & 0x3f));
        out[1] = (unsigned char)((value >> 16) & 0xff);
        out[2] = (unsigned char)((value >> 8) & 0xff);
        out[3] = (unsigned char)(value & 0xff);
        return 4;
    }
    out[0] = (unsigned char)(0xc0 | ((value >> 56) & 0x3f));
    out[1] = (unsigned char)((value >> 48) & 0xff);
    out[2] = (unsigned char)((value >> 40) & 0xff);
    out[3] = (unsigned char)((value >> 32) & 0xff);
    out[4] = (unsigned char)((value >> 24) & 0xff);
    out[5] = (unsigned char)((value >> 16) & 0xff);
    out[6] = (unsigned char)((value >> 8) & 0xff);
    out[7] = (unsigned char)(value & 0xff);
    return 8;
}

static int h3_decode_varint(const unsigned char *buf, size_t len, size_t *pos, uint64_t *value) {
    if (*pos >= len) return -1;
    unsigned char first = buf[*pos];
    size_t width = (size_t)1u << (first >> 6);
    if (*pos + width > len) return -1;

    uint64_t v = (uint64_t)(first & 0x3f);
    for (size_t i = 1; i < width; i++) {
        v = (v << 8) | buf[*pos + i];
    }
    *pos += width;
    *value = v;
    return 0;
}

static int h3_ssl_write_all(SSL *stream, const void *buf, size_t len) {
    const unsigned char *p = (const unsigned char *)buf;
    while (len > 0) {
        int n = SSL_write(stream, p, len);
        if (n <= 0) return -1;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static int h3_write_frame(SSL *stream, uint64_t type, const unsigned char *payload, size_t payload_len) {
    unsigned char header[16];
    unsigned char encoded[8];
    size_t header_len = 0;

    size_t n = h3_encode_varint(type, encoded);
    memcpy(header + header_len, encoded, n);
    header_len += n;
    n = h3_encode_varint(payload_len, encoded);
    memcpy(header + header_len, encoded, n);
    header_len += n;

    if (h3_ssl_write_all(stream, header, header_len) != 0) return -1;
    if (payload_len > 0 && payload) return h3_ssl_write_all(stream, payload, payload_len);
    return 0;
}

static int h3_read_frame(SSL *stream, uint64_t *type, uint64_t *payload_len,
                         unsigned char **payload, bool *closed) {
    *closed = false;
    unsigned char buf[16];

    int n = SSL_read(stream, buf, 1);
    if (n <= 0) {
        int err = SSL_get_error(stream, n);
        if (err == SSL_ERROR_ZERO_RETURN) {
            *closed = true;
        }
        return -1;
    }

    size_t got = 1;
    while (got < sizeof(buf)) {
        size_t pos = 0;
        uint64_t t;
        if (h3_decode_varint(buf, got, &pos, &t) == 0) {
            uint64_t l;
            size_t pos2 = pos;
            if (h3_decode_varint(buf, got, &pos2, &l) == 0) {
                *type = t;
                *payload_len = l;
                if (l == 0) {
                    *payload = NULL;
                    return 0;
                }
                *payload = (unsigned char *)malloc((size_t)l);
                if (!*payload) return -1;
                size_t payload_got = 0;
                while (payload_got < l) {
                    n = SSL_read(stream, *payload + payload_got, (int)(l - payload_got));
                    if (n <= 0) {
                        int ssl_err = SSL_get_error(stream, n);
                        if (ssl_err == SSL_ERROR_ZERO_RETURN) {
                            *closed = true;
                            free(*payload);
                            return -1;
                        }
                        if (ssl_err == SSL_ERROR_WANT_READ || ssl_err == SSL_ERROR_WANT_WRITE) {
                            usleep(1000);
                            continue;
                        }
                        free(*payload);
                        return -1;
                    }
                    payload_got += n;
                }
                return 0;
            }
        }

        n = SSL_read(stream, buf + got, 1);
        if (n <= 0) {
            int ssl_err = SSL_get_error(stream, n);
            if (ssl_err == SSL_ERROR_ZERO_RETURN) {
                *closed = true;
            }
            return -1;
        }
        got++;
    }
    return -1;
}

static int h3_send_settings(SSL *stream) {
    unsigned char settings[32];
    size_t pos = 0;
    /* SETTINGS_MAX_FIELD_SECTION_SIZE (0x06) = 16384 */
    pos += h3_encode_varint(0x06, settings + pos);
    pos += h3_encode_varint(16384, settings + pos);
    /* SETTINGS_QPACK_MAX_TABLE_CAPACITY (0x01) = 0 (no dynamic table) */
    pos += h3_encode_varint(0x01, settings + pos);
    pos += h3_encode_varint(0, settings + pos);
    return h3_write_frame(stream, CWIST_HTTP3_FRAME_SETTINGS, settings, pos);
}

static int h3_setup_server_control_streams(SSL *quic_conn) {
    /* Server control stream (unidirectional, type = 0x00) */
    SSL *ctrl = SSL_new_stream(quic_conn, SSL_STREAM_FLAG_UNI);
    if (!ctrl) return -1;

    unsigned char stream_type = 0x00;
    if (SSL_write(ctrl, &stream_type, 1) != 1) {
        SSL_free(ctrl);
        return -1;
    }

    if (h3_send_settings(ctrl) != 0) {
        SSL_free(ctrl);
        return -1;
    }

    SSL_stream_conclude(ctrl, 0);
    SSL_free(ctrl);

    /* Server QPACK decoder stream (unidirectional, type = 0x03) */
    SSL *decoder = SSL_new_stream(quic_conn, SSL_STREAM_FLAG_UNI);
    if (decoder) {
        stream_type = 0x03;
        SSL_write(decoder, &stream_type, 1);
        SSL_stream_conclude(decoder, 0);
        SSL_free(decoder);
    }

    return 0;
}

static void h3_handle_client_control_stream(SSL *stream) {
    unsigned char stream_type;
    int n = SSL_read(stream, &stream_type, 1);
    if (n != 1 || stream_type != 0x00) {
        return;
    }

    uint64_t ftype, flen;
    unsigned char *payload = NULL;
    bool closed = false;
    if (h3_read_frame(stream, &ftype, &flen, &payload, &closed) == 0) {
        if (ftype == CWIST_HTTP3_FRAME_SETTINGS) {
            /* Accept client settings */
        }
        free(payload);
    }
}

static void h3_handle_client_qpack_stream(SSL *stream) {
    unsigned char stream_type;
    int n = SSL_read(stream, &stream_type, 1);
    if (n != 1 || stream_type != 0x02) {
        return;
    }
    char discard[1024];
    while ((n = SSL_read(stream, discard, sizeof(discard))) > 0) {
        /* discard dynamic table instructions */
    }
}

static void h3_apply_header(cwist_http_request *req, const char *name, const char *value) {
    if (!req || !name || !value) return;

    if (strcmp(name, ":method") == 0) {
        req->method = cwist_http_string_to_method(value);
    } else if (strcmp(name, ":path") == 0) {
        cwist_sstring_assign(req->path, (char *)value);
    } else if (strcmp(name, ":authority") == 0 || strcmp(name, "host") == 0) {
        cwist_http_header_add(&req->headers, "host", value);
    } else if (strcmp(name, "content-length") == 0) {
        req->content_length = atol(value);
        cwist_http_header_add(&req->headers, name, value);
    } else if (strcmp(name, ":scheme") != 0 && name[0] != ':') {
        cwist_http_header_add(&req->headers, name, value);
    }
}

static int qpack_decode_integer(const unsigned char *buf, size_t len, size_t *pos, uint8_t prefix_bits, uint32_t *value) {
    if (*pos >= len || prefix_bits == 0 || prefix_bits > 8) return -1;
    uint8_t mask = (uint8_t)((1u << prefix_bits) - 1u);
    uint32_t n = buf[*pos] & mask;
    (*pos)++;
    if (n < mask) {
        *value = n;
        return 0;
    }

    uint32_t m = 0;
    while (*pos < len) {
        uint8_t b = buf[(*pos)++];
        if (m > 28) return -1;
        n += (uint32_t)(b & 0x7f) << m;
        if ((b & 0x80) == 0) {
            *value = n;
            return 0;
        }
        m += 7;
    }
    return -1;
}

static char *qpack_decode_string(const unsigned char *buf, size_t len, size_t *pos) {
    if (*pos >= len) return NULL;
    bool huffman = (buf[*pos] & 0x80) != 0;
    uint32_t str_len = 0;
    if (qpack_decode_integer(buf, len, pos, 7, &str_len) != 0) return NULL;
    if (*pos + str_len > len) return NULL;

    if (huffman) {
        size_t decoded_len = 0;
        char *out = h2_huffman_decode(buf + *pos, str_len, &decoded_len);
        *pos += str_len;
        return out;
    }

    char *out = (char *)malloc((size_t)str_len + 1);
    if (!out) return NULL;
    memcpy(out, buf + *pos, str_len);
    out[str_len] = '\0';
    *pos += str_len;
    return out;
}

static void h3_apply_minimal_request_headers(cwist_http_request *req,
                                             const unsigned char *buf,
                                             size_t len) {
    size_t pos = 0;
    while (pos < len) {
        uint64_t type = 0;
        uint64_t frame_len = 0;
        if (h3_decode_varint(buf, len, &pos, &type) != 0 ||
            h3_decode_varint(buf, len, &pos, &frame_len) != 0 ||
            pos + frame_len > len) {
            break;
        }

        if (type == CWIST_HTTP3_FRAME_HEADERS && frame_len >= 2) {
            const unsigned char *block = buf + pos;
            size_t block_len = (size_t)frame_len;
            size_t block_pos = 0;

            // Skip QPACK Encoded Field Section Prefix
            uint64_t ric = 0;
            if (h3_decode_varint(block, block_len, &block_pos, &ric) != 0) goto next_frame;
            if (block_pos >= block_len) goto next_frame;
            unsigned char base_first = block[block_pos];
            size_t base_width = (size_t)1u << (base_first >> 6);
            if (block_pos + base_width > block_len) goto next_frame;
            block_pos += base_width;

            // Parse field lines
            while (block_pos < block_len) {
                unsigned char b = block[block_pos];

                if (b & 0x80) {
                    // Indexed Field Line: 1 T XXXXXX
                    uint32_t idx = 0;
                    size_t tmp = block_pos;
                    if (qpack_decode_integer(block, block_len, &tmp, 6, &idx) != 0) break;
                    block_pos = tmp;
                    bool is_static = (b & 0x40) == 0;
                    if (is_static && idx < QPACK_STATIC_TABLE_COUNT) {
                        h3_apply_header(req, qpack_static_table[idx].name, qpack_static_table[idx].value);
                    }
                } else if ((b & 0xC0) == 0x40) {
                    // Literal Field Line with Post-Base Index: 01...
                    // Skip - not supported in minimal implementation
                    break;
                } else if ((b & 0xE0) == 0x20) {
                    // Literal Field Line with Literal Name: 001xxxxx
                    uint32_t name_len = 0;
                    size_t tmp = block_pos;
                    if (qpack_decode_integer(block, block_len, &tmp, 5, &name_len) != 0) break;
                    block_pos = tmp;
                    char *name = qpack_decode_string(block, block_len, &block_pos);
                    char *value = qpack_decode_string(block, block_len, &block_pos);
                    if (name && value) {
                        h3_apply_header(req, name, value);
                    }
                    free(name);
                    free(value);
                } else if ((b & 0xF0) == 0x00) {
                    // Literal Field Line with Name Reference: 0000 N IIII
                    uint32_t name_idx = 0;
                    size_t tmp = block_pos;
                    if (qpack_decode_integer(block, block_len, &tmp, 4, &name_idx) != 0) break;
                    block_pos = tmp;
                    bool is_static = (b & 0x08) == 0;
                    char *value = qpack_decode_string(block, block_len, &block_pos);
                    if (value && is_static && name_idx < QPACK_STATIC_TABLE_COUNT) {
                        h3_apply_header(req, qpack_static_table[name_idx].name, value);
                    }
                    free(value);
                } else {
                    // Unknown or unsupported
                    break;
                }
            }
        }
next_frame:
        pos += frame_len;
    }
}

static int h3_send_response(SSL *stream, cwist_http_response *res) {
    unsigned char header_block[8192];
    size_t block_len = qpack_encode_response_headers(res, header_block, sizeof(header_block));
    if (block_len == 0) return -1;

    if (h3_write_frame(stream, CWIST_HTTP3_FRAME_HEADERS, header_block, block_len) != 0) {
        return -1;
    }

    size_t body_len = 0;
    const unsigned char *body_data = NULL;
    if (res->use_file_stream) {
        body_len = res->file_stream_len;
    } else if (res->is_ptr_body) {
        body_len = res->ptr_body_len;
        body_data = (const unsigned char *)res->ptr_body;
    } else if (res->body) {
        body_len = res->body->size;
        body_data = (const unsigned char *)res->body->data;
    }

    if (res->use_file_stream && res->file_stream_fd >= 0) {
        off_t offset = res->file_stream_offset;
        size_t remaining = res->file_stream_len;
        while (remaining > 0) {
            size_t chunk = remaining > 16384 ? 16384 : remaining;
            unsigned char *chunk_buf = (unsigned char *)malloc(chunk);
            if (!chunk_buf) return -1;
            ssize_t r = pread(res->file_stream_fd, chunk_buf, chunk, offset);
            if (r <= 0) { free(chunk_buf); return -1; }
            if (h3_write_frame(stream, CWIST_HTTP3_FRAME_DATA, chunk_buf, (size_t)r) != 0) {
                free(chunk_buf); return -1;
            }
            free(chunk_buf);
            offset += r;
            remaining -= (size_t)r;
        }
    } else {
        if (body_len > 0) {
            if (h3_write_frame(stream, CWIST_HTTP3_FRAME_DATA, body_data, body_len) != 0) {
                return -1;
            }
        }
    }
    return 0;
}

struct h3_stream_thread_ctx {
    SSL *stream;
    cwist_http3_request_handler_func handler;
    void *user_ctx;
};

static void *h3_stream_thread_func(void *arg) {
    struct h3_stream_thread_ctx *ctx = (struct h3_stream_thread_ctx *)arg;
    SSL *stream = ctx->stream;
    cwist_http3_request_handler_func handler = ctx->handler;
    void *user_ctx = ctx->user_ctx;
    free(ctx);

    cwist_http_request *req = cwist_http_request_create();
    cwist_http_response *res = cwist_http_response_create();
    if (!req || !res) {
        goto stream_cleanup;
    }

    cwist_sstring_assign(req->version, "HTTP/3");

    unsigned char *body_buf = NULL;
    size_t body_len = 0;
    size_t body_cap = 0;
    bool headers_received = false;
    bool stream_closed = false;

    while (!stream_closed) {
        uint64_t frame_type, frame_len;
        unsigned char *payload = NULL;

        if (h3_read_frame(stream, &frame_type, &frame_len, &payload, &stream_closed) != 0) {
            break;
        }

        if (frame_type == CWIST_HTTP3_FRAME_HEADERS && !headers_received) {
            h3_apply_minimal_request_headers(req, payload, (size_t)frame_len);
            headers_received = true;
        } else if (frame_type == CWIST_HTTP3_FRAME_DATA && headers_received) {
            if (frame_len > 0 && payload) {
                size_t need = body_len + (size_t)frame_len;
                if (need > body_cap) {
                    size_t new_cap = body_cap ? body_cap * 2 : 4096;
                    while (new_cap < need) new_cap *= 2;
                    unsigned char *nb = (unsigned char *)realloc(body_buf, new_cap);
                    if (!nb) {
                        free(payload);
                        goto stream_cleanup;
                    }
                    body_buf = nb;
                    body_cap = new_cap;
                }
                memcpy(body_buf + body_len, payload, (size_t)frame_len);
                body_len += (size_t)frame_len;
            }
        } else if (frame_type == CWIST_HTTP3_FRAME_HEADERS && headers_received) {
            /* Trailer headers - ignore for now */
        }
        /* Other frame types are ignored for request streams */

        free(payload);

        if (req->content_length > 0 && body_len >= (size_t)req->content_length) {
            break;
        }
    }

    if (body_len > 0) {
        cwist_sstring_assign_len(req->body, (char *)body_buf, body_len);
    }

    if (handler && headers_received) {
        handler(user_ctx, req, res);
        h3_send_response(stream, res);
    } else if (!headers_received) {
        res->status_code = CWIST_HTTP_BAD_REQUEST;
        h3_send_response(stream, res);
    }

    SSL_stream_conclude(stream, 0);

stream_cleanup:
    if (body_buf) free(body_buf);
    cwist_http_request_destroy(req);
    cwist_http_response_destroy(res);
    SSL_free(stream);
    return NULL;
}

static void cwist_http3_handle_stream(SSL *stream, cwist_http3_request_handler_func handler, void *user_ctx) {
    struct h3_stream_thread_ctx *ctx = (struct h3_stream_thread_ctx *)malloc(sizeof(*ctx));
    if (!ctx) {
        SSL_free(stream);
        return;
    }
    ctx->stream = stream;
    ctx->handler = handler;
    ctx->user_ctx = user_ctx;

    pthread_t tid;
    if (pthread_create(&tid, NULL, h3_stream_thread_func, ctx) != 0) {
        free(ctx);
        SSL_free(stream);
        return;
    }
    pthread_detach(tid);
}

cwist_error_t cwist_http3_serve_connection(cwist_http3_connection *conn,
                                           void *user_ctx,
                                           cwist_http3_request_handler_func handler) {
    cwist_error_t err = make_error(CWIST_ERR_INT16);

    if (!conn || !conn->quic_ssl || !handler) {
        err.error.err_i16 = -1;
        return err;
    }

    /* Create server control streams before accepting client streams */
    h3_setup_server_control_streams(conn->quic_ssl);

    /* Accept and dispatch all incoming streams */
    while (1) {
        SSL *stream = SSL_accept_stream(conn->quic_ssl, SSL_STREAM_FLAG_NO_BLOCK);
        if (!stream) {
            int ssl_err = SSL_get_error(conn->quic_ssl, 0);
            if (ssl_err == SSL_ERROR_WANT_READ || ssl_err == SSL_ERROR_WANT_WRITE) {
                struct pollfd pfd = { .fd = conn->udp_fd, .events = POLLIN };
                poll(&pfd, 1, 100);
                continue;
            }
            if (ssl_err == SSL_ERROR_ZERO_RETURN) {
                break; /* Connection closed */
            }
            break; /* Other error */
        }

        int stype = SSL_get_stream_type(stream);
        if (stype == SSL_STREAM_TYPE_BIDI) {
            cwist_http3_handle_stream(stream, handler, user_ctx);
        } else {
            /* Unidirectional stream: control (0x00), push (0x01), or qpack encoder (0x02) */
            unsigned char stream_type_byte;
            int n = SSL_read(stream, &stream_type_byte, 1);
            if (n == 1) {
                if (stream_type_byte == 0x00) {
                    h3_handle_client_control_stream(stream);
                } else if (stream_type_byte == 0x02) {
                    h3_handle_client_qpack_stream(stream);
                }
            }
            SSL_free(stream);
        }
    }

    err.error.err_i16 = 0;
    return err;
}

cwist_error_t cwist_http3_server_loop(int udp_fd,
                                      cwist_http3_context *ctx,
                                      cwist_http3_request_handler_func handler,
                                      void *user_ctx) {
    cwist_error_t err = make_error(CWIST_ERR_INT16);
    if (udp_fd < 0 || !ctx || !ctx->ssl_ctx || !handler) {
        err.error.err_i16 = -1;
        return err;
    }

    struct lsquic_engine_settings settings;
    lsquic_engine_init_settings(&settings, LSENG_HTTP_SERVER);
    settings.es_support_push = ctx->push_enabled;
    settings.es_allow_migration = ctx->allow_migration ? ctx->allow_migration : 1;
    settings.es_max_delayed_0rtt_packets = 32;
    settings.es_datagrams = ctx->datagram_enabled;
    settings.es_ecn = 1;
    settings.es_pace_packets = 1;
    settings.es_optimistic_nat = 1;

    if (ctx->idle_timeout_ms > 0)
        settings.es_idle_timeout = (unsigned)(ctx->idle_timeout_ms / 1000);
    if (ctx->handshake_timeout_ms > 0)
        settings.es_handshake_to = (unsigned long)ctx->handshake_timeout_ms * 1000UL;
    if (ctx->ping_period_ms > 0)
        settings.es_ping_period = (unsigned)(ctx->ping_period_ms / 1000);
    if (ctx->noprogress_timeout_ms > 0)
        settings.es_noprogress_timeout = (unsigned)(ctx->noprogress_timeout_ms / 1000);

    char err_buf[256];
    if (lsquic_engine_check_settings(&settings, LSENG_HTTP_SERVER,
                                     err_buf, sizeof(err_buf)) != 0) {
        fprintf(stderr, "[HTTP/3] Invalid engine settings: %s\n", err_buf);
        err.error.err_i16 = -1;
        return err;
    }

    struct lsquic_engine_api api = {
        .ea_stream_if        = &cwist_h3_stream_if,
        .ea_stream_if_ctx    = ctx,
        .ea_packets_out      = cwist_h3_packets_out,
        .ea_packets_out_ctx  = &udp_fd,
        .ea_get_ssl_ctx      = cwist_h3_get_ssl_ctx,
        .ea_hsi_if           = &cwist_h3_hset_if,
        .ea_hsi_ctx          = NULL,
        .ea_settings         = &settings,
        .ea_alpn             = "h3",
    };

    lsquic_engine_t *engine = lsquic_engine_new(LSENG_HTTP_SERVER, &api);
    if (!engine) {
        err.error.err_i16 = -1;
        return err;
    }

    ctx->engine = engine;
    ctx->handler = handler;
    ctx->user_ctx = user_ctx;
    ctx->udp_fd = udp_fd;
    ctx->running = 1;

    printf("[HTTP/3] Listening on UDP socket %d\n", udp_fd);

    struct sockaddr_storage local_addr;
    socklen_t local_addr_len = sizeof(local_addr);
    memset(&local_addr, 0, sizeof(local_addr));
    if (getsockname(udp_fd, (struct sockaddr *)&local_addr, &local_addr_len) != 0) {
        local_addr_len = 0;
    }

    /* Make socket non-blocking for polling */
    int flags = fcntl(udp_fd, F_GETFL, 0);
    if (flags >= 0) fcntl(udp_fd, F_SETFL, flags | O_NONBLOCK);

    /* Enable ECN reception for congestion control feedback */
    int on = 1;
    setsockopt(udp_fd, IPPROTO_IP, IP_RECVTOS, &on, sizeof(on));
#ifdef IPV6_RECVTCLASS
    setsockopt(udp_fd, IPPROTO_IPV6, IPV6_RECVTCLASS, &on, sizeof(on));
#endif

    unsigned char *pkt_buf = malloc(65535);
    if (!pkt_buf) {
        lsquic_engine_destroy(engine);
        ctx->engine = NULL;
        err.error.err_i16 = -1;
        return err;
    }

#ifdef __linux__
    int epoll_fd = epoll_create1(0);
    if (epoll_fd < 0) {
        err.error.err_i16 = -1;
        lsquic_engine_destroy(engine);
        ctx->engine = NULL;
        return err;
    }
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = udp_fd;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, udp_fd, &ev) < 0) {
        close(epoll_fd);
        err.error.err_i16 = -1;
        lsquic_engine_destroy(engine);
        ctx->engine = NULL;
        return err;
    }
#endif
    settings.es_max_cfcw = 16 * 1024 * 1024;
    settings.es_max_sfcw = 8 * 1024 * 1024;
    settings.es_init_max_data = 16 * 1024 * 1024;
    settings.es_init_max_stream_data_bidi_remote = 8 * 1024 * 1024;
    settings.es_init_max_stream_data_bidi_local = 8 * 1024 * 1024;
    settings.es_init_max_stream_data_uni = 8 * 1024 * 1024;
    settings.es_init_max_streams_bidi = 256;
    settings.es_ecn = 1;
    settings.es_pace_packets = 1;
    settings.es_optimistic_nat = 1;

    while (ctx && ctx->running && atomic_load(&g_cwist_running)) {
        int diff = 1000; /* default 1 ms; let earliest_adv_tick drive it */
        if (lsquic_engine_earliest_adv_tick(engine, &diff)) {
            /* Enforce a small floor so pacing timers or back-to-back zero
             * ticks cannot turn this loop into a busy-wait. */
            if (diff < 1000)
                diff = 1000;
            else if (diff > 1000000)
                diff = 1000000;
        }

        SSL_set_fd(quic_conn, udp_fd);

        if (SSL_accept(quic_conn) <= 0) {
            SSL_free(quic_conn);
            usleep(10000);
            continue;
        }

        if (pret > 0) {
            if (pfd.revents & (POLLERR | POLLNVAL)) {
                fprintf(stderr, "[HTTP/3] UDP socket error, exiting loop.\n");
                break;
            }
            if (pfd.revents & POLLIN) {
#endif
                struct sockaddr_storage peer_addr;
                socklen_t peer_addr_len = sizeof(peer_addr);
                struct msghdr msg = {0};
                struct iovec iov = { pkt_buf, 65535 };
                msg.msg_name = &peer_addr;
                msg.msg_namelen = sizeof(peer_addr);
                msg.msg_iov = &iov;
                msg.msg_iovlen = 1;

        cwist_http3_serve_connection(&conn, user_ctx, handler);

                ssize_t nr = recvmsg(udp_fd, &msg, 0);
                if (nr > 0) {
                    int ecn = 0;
                    for (struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
                         cmsg != NULL;
                         cmsg = CMSG_NXTHDR(&msg, cmsg)) {
                        if (cmsg->cmsg_level == IPPROTO_IP &&
                            cmsg->cmsg_type == IP_TOS) {
                            ecn = *(int *)CMSG_DATA(cmsg) & 0x3;
                            break;
                        }
#ifdef IPV6_TCLASS
                        if (cmsg->cmsg_level == IPPROTO_IPV6 &&
                            cmsg->cmsg_type == IPV6_TCLASS) {
                            ecn = *(int *)CMSG_DATA(cmsg) & 0x3;
                            break;
                        }
#endif
                    }
                    lsquic_engine_packet_in(engine, pkt_buf, (size_t)nr,
                                            local_addr_len ? (struct sockaddr *)&local_addr : NULL,
                                            (struct sockaddr *)&peer_addr,
                                            ctx, ecn);
                } else if (nr < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                    if (errno == ECONNREFUSED || errno == ENETUNREACH ||
                        errno == EHOSTUNREACH) {
                        /* Transient error, keep going */
                    } else if (errno == EBADF) {
                        fprintf(stderr, "[HTTP/3] UDP socket closed.\n");
                        break;
                    }
                }
#ifdef __linux__
            }
#else
            }
        }
#endif

        lsquic_engine_process_conns(engine);
    }

    err.error.err_i16 = 0;
    return err;
}

#else

static cwist_error_t cwist_http3_quic_unavailable(void) {
    cwist_error_t err = make_error(CWIST_ERR_INT16);
    err.error.err_i16 = -1;
    return err;
}

cwist_error_t cwist_http3_init_context(cwist_http3_context **ctx,
                                       const char *cert_path,
                                       const char *key_path) {
    (void)cert_path;
    (void)key_path;
    if (ctx) *ctx = NULL;
    return cwist_http3_quic_unavailable();
}

cwist_error_t cwist_http3_init_context_ephemeral(cwist_http3_context **ctx) {
    if (ctx) *ctx = NULL;
    return cwist_http3_quic_unavailable();
}

/* ------------------------------------------------------------------ */
/* WebTransport API                                                   */
/* ------------------------------------------------------------------ */

void cwist_http3_set_webtransport_handler(cwist_http3_context *ctx,
                                          cwist_webtransport_handler_func handler) {
    if (ctx) ctx->wt_handler = handler;
}

/* ------------------------------------------------------------------ */
/* Resilience knobs                                                   */
/* ------------------------------------------------------------------ */

void cwist_http3_set_idle_timeout(cwist_http3_context *ctx, int ms) {
    if (ctx) ctx->idle_timeout_ms = ms > 0 ? ms : 0;
}

void cwist_http3_set_handshake_timeout(cwist_http3_context *ctx, int ms) {
    if (ctx) ctx->handshake_timeout_ms = ms > 0 ? ms : 0;
}

void cwist_http3_set_ping_period(cwist_http3_context *ctx, int ms) {
    if (ctx) ctx->ping_period_ms = ms > 0 ? ms : 0;
}

void cwist_http3_set_noprogress_timeout(cwist_http3_context *ctx, int ms) {
    if (ctx) ctx->noprogress_timeout_ms = ms > 0 ? ms : 0;
}

/* ------------------------------------------------------------------ */
/* Datagram API                                                       */
/* ------------------------------------------------------------------ */

void cwist_http3_set_datagram_enabled(cwist_http3_context *ctx, int enabled) {
    if (ctx) ctx->datagram_enabled = enabled;
}

void cwist_http3_set_datagram_callback(cwist_http3_context *ctx,
                                       void (*cb)(const void *data, size_t len, void *user_ctx),
                                       void *user_ctx) {
    if (ctx) {
        if (ctx->ssl_ctx) SSL_CTX_free(ctx->ssl_ctx);
        cwist_free(ctx);
    }
}

cwist_error_t cwist_http3_serve_connection(cwist_http3_connection *conn,
                                           void *user_ctx,
                                           cwist_http3_request_handler_func handler) {
    (void)conn;
    (void)user_ctx;
    (void)handler;
    return cwist_http3_quic_unavailable();
}

cwist_error_t cwist_http3_server_loop(int udp_fd,
                                      cwist_http3_context *ctx,
                                      cwist_http3_request_handler_func handler,
                                      void *user_ctx) {
    (void)udp_fd;
    (void)ctx;
    (void)handler;
    (void)user_ctx;
    return cwist_http3_quic_unavailable();
}

#endif /* CWIST_HAVE_OPENSSL_QUIC */
