/**
 * @file lwip.c
 * @brief CWIST over lwIP's raw TCP API; see <cwist/sys/port/lwip.h>.
 *
 * Not part of libcwist: the firmware compiles this file next to lwIP so it
 * picks up the firmware's lwipopts.h. Uses only public CWIST API (parse,
 * dispatch, serialize), which keeps it independent of the socket runtime.
 *
 * One connection runs one exchange at a time:
 *
 *   recv -> rx buffer -> frame (Content-Length) -> parse -> dispatch
 *        -> serialize into tx -> tcp_write as tcp_sndbuf allows
 *        -> tcp_sent acks drain tx -> next pipelined request or close
 *
 * tx is written without TCP_WRITE_FLAG_COPY, so lwIP references it until
 * the peer acknowledges. Its storage is retained and reused by the next
 * response on the connection.
 *
 * With CWIST_LWIP_TLS and cfg.tls set, mbedTLS sits between the pcb and the
 * same rx/tx buffers: received ciphertext queues in tin and is decrypted
 * into rx by the pump; tx goes through mbedtls_ssl_write, whose records are
 * copied into lwIP (TCP_WRITE_FLAG_COPY) by the BIO, so the next response can
 * reuse the storage as soon as mbedTLS has taken all of it. The next pipelined
 * request is then picked up from tcp_sent, as on the plaintext path.
 */

#include <cwist/sys/port/lwip.h>

#include <cwist/core/mem/alloc.h>
#include <cwist/net/http/http.h>

#include "lwip/opt.h"
#include "lwip/sys.h"
#include "lwip/tcp.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#ifdef CWIST_LWIP_TLS
#include <cwist/sys/port.h>

#include "mbedtls/ctr_drbg.h"
#include "mbedtls/pk.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"

/* Received ciphertext not yet consumed by mbedTLS: one full record plus slack. */
#define CWIST_LWIP_TLS_IN_MAX (MBEDTLS_SSL_IN_CONTENT_LEN + 2048)
#endif

/* lwip/opt.h always defines both; testing for it keeps tools that analyze
 * this file without the firmware's lwIP tree (cppcheck) off the #error. */
#if defined(LWIP_TCP) && (!LWIP_TCP || !LWIP_CALLBACK_API)
#error "cwist lwIP adapter needs LWIP_TCP and LWIP_CALLBACK_API"
#endif

/* tcp_poll interval in TCP coarse-timer ticks (TCP_TMR_INTERVAL * 2 = 500 ms). */
#define CWIST_LWIP_POLL_TICKS 2

static const char CWIST_LWIP_100_CONTINUE[] = "HTTP/1.1 100 Continue\r\n\r\n";

typedef struct cwist_lwip_conn cwist_lwip_conn;

#ifdef CWIST_LWIP_TLS
struct cwist_lwip_tls {
    mbedtls_ssl_config conf;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_x509_crt cert;
    mbedtls_pk_context key;
};
#endif

struct cwist_lwip_server {
    cwist_app *app;
    cwist_lwip_config cfg;
    struct tcp_pcb *listen_pcb;
    cwist_lwip_conn *conns;
    size_t nconns;
};

struct cwist_lwip_conn {
    cwist_lwip_server *srv;
    struct tcp_pcb *pcb;
    cwist_lwip_conn *next;

    char *rx;
    size_t rx_len;
    size_t rx_cap;

    char *tx_store;      /* reusable serialized-response storage */
    size_t tx_cap;
    const char *tx;      /* active response; NULL when idle */
    size_t tx_len;
    size_t tx_queued;    /* bytes handed to tcp_write */
    size_t tx_acked;     /* bytes the peer acknowledged */
    size_t ack_skip;     /* acked bytes that belong to an interim 100 */

    bool close_after_tx; /* Connection: close, error reply, or peer FIN */
    bool peer_closed;
    bool continue_sent;
    u32_t last_active_ms;

#ifdef CWIST_LWIP_TLS
    mbedtls_ssl_context *ssl; /* NULL on a plaintext server */
    char *tin;                /* ciphertext queue: [tin_off, tin_len) unread */
    size_t tin_off;
    size_t tin_len;
    size_t tin_cap;
    bool tcp_fin;             /* TCP FIN seen; the TLS stream ends once tin drains */
    bool handshake_done;
#endif
};

typedef enum {
    FRAME_NEED_MORE,
    FRAME_COMPLETE,
    FRAME_ERROR,
} frame_result;

/* --- framing ---------------------------------------------------------------- */

static const char *find_crlfcrlf(const char *buf, size_t len) {
    for (size_t i = 0; i + 3 < len; i++) {
        if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' && buf[i + 3] == '\n')
            return buf + i;
    }
    return NULL;
}

static bool header_is(const char *name, size_t name_len, const char *want) {
    size_t n = strlen(want);
    return name_len == n && strncasecmp(name, want, n) == 0;
}

/* Trim optional whitespace (RFC 9110 5.6.3) around a field value. */
static void trim_ows(const char **v, size_t *len) {
    while (*len > 0 && (**v == ' ' || **v == '\t')) (*v)++, (*len)--;
    while (*len > 0 && ((*v)[*len - 1] == ' ' || (*v)[*len - 1] == '\t')) (*len)--;
}

/**
 * Decide whether rx holds one complete request. Mirrors the parser's
 * Content-Length / Transfer-Encoding rules (RFC 9112 6.3) so the adapter
 * never hands the parser a different message boundary than it would pick.
 */
static frame_result frame_request(const cwist_lwip_conn *c, size_t *req_len, int *status,
                                  bool *expect_continue) {
    const cwist_lwip_config *cfg = &c->srv->cfg;
    *expect_continue = false;
    const char *end = find_crlfcrlf(c->rx, c->rx_len);
    if (!end) {
        if (c->rx_len > cfg->max_header_bytes) {
            *status = CWIST_HTTP_REQUEST_HEADER_FIELDS_TOO_LARGE;
            return FRAME_ERROR;
        }
        return FRAME_NEED_MORE;
    }
    size_t head_len = (size_t)(end - c->rx) + 4;
    if (head_len > cfg->max_header_bytes) {
        *status = CWIST_HTTP_REQUEST_HEADER_FIELDS_TOO_LARGE;
        return FRAME_ERROR;
    }

    bool cl_seen = false, te_seen = false;
    size_t cl = 0;
    const char *line = memchr(c->rx, '\n', head_len);
    while (line && (size_t)(line + 1 - c->rx) < head_len - 2) {
        line++;
        const char *eol = memchr(line, '\r', (size_t)(end + 2 - line));
        if (!eol) break;
        const char *colon = memchr(line, ':', (size_t)(eol - line));
        if (colon) {
            size_t name_len = (size_t)(colon - line);
            const char *v = colon + 1;
            size_t vlen = (size_t)(eol - v);
            trim_ows(&v, &vlen);
            if (header_is(line, name_len, "Content-Length")) {
                size_t n = 0;
                if (vlen == 0) goto bad_request;
                for (size_t i = 0; i < vlen; i++) {
                    if (v[i] < '0' || v[i] > '9' || n > (SIZE_MAX - 9) / 10) goto bad_request;
                    n = n * 10 + (size_t)(v[i] - '0');
                }
                if (cl_seen && n != cl) goto bad_request;
                cl_seen = true;
                cl = n;
            } else if (header_is(line, name_len, "Transfer-Encoding")) {
                te_seen = true;
            } else if (header_is(line, name_len, "Expect")) {
                *expect_continue = vlen == 12 && strncasecmp(v, "100-continue", 12) == 0;
            }
        }
        line = memchr(eol, '\n', (size_t)(end + 4 - eol));
    }

    if (te_seen) {
        if (cl_seen) goto bad_request;
        *status = CWIST_HTTP_NOT_IMPLEMENTED; /* chunked request bodies are not reassembled */
        return FRAME_ERROR;
    }
    if (cl > cfg->max_body_bytes) {
        *status = CWIST_HTTP_PAYLOAD_TOO_LARGE;
        return FRAME_ERROR;
    }
    if (c->rx_len - head_len < cl) return FRAME_NEED_MORE;
    *req_len = head_len + cl;
    return FRAME_COMPLETE;

bad_request:
    *status = CWIST_HTTP_BAD_REQUEST;
    return FRAME_ERROR;
}

/* --- connection lifecycle ---------------------------------------------------- */

static void conn_unlink(cwist_lwip_conn *c) {
    cwist_lwip_server *srv = c->srv;
    for (cwist_lwip_conn **pp = &srv->conns; *pp; pp = &(*pp)->next) {
        if (*pp == c) {
            *pp = c->next;
            srv->nconns--;
            break;
        }
    }
}

static void conn_free(cwist_lwip_conn *c) {
    conn_unlink(c);
    cwist_free(c->rx);
    cwist_free(c->tx_store);
#ifdef CWIST_LWIP_TLS
    if (c->ssl) {
        mbedtls_ssl_free(c->ssl);
        cwist_free(c->ssl);
    }
    cwist_free(c->tin);
#endif
    cwist_free(c);
}

/* Grow rx to hold @p need bytes. @return false on allocation failure. */
static bool rx_reserve(cwist_lwip_conn *c, size_t need, size_t limit) {
    if (need <= c->rx_cap) return true;
    size_t cap = c->rx_cap ? c->rx_cap : 512;
    while (cap < need) cap *= 2;
    if (cap > limit + 1) cap = limit + 1;
    char *grown = cwist_realloc(c->rx, cap);
    if (!grown) return false;
    c->rx = grown;
    c->rx_cap = cap;
    return true;
}

static bool tx_reserve(cwist_lwip_conn *c, size_t need) {
    if (need <= c->tx_cap) return true;
    size_t cap = c->tx_cap ? c->tx_cap : 512;
    while (cap < need) {
        if (cap > SIZE_MAX / 2) {
            cap = need;
            break;
        }
        cap *= 2;
    }
    char *grown = cwist_realloc(c->tx_store, cap);
    if (!grown) return false;
    c->tx_store = grown;
    c->tx_cap = cap;
    return true;
}

static int conn_serialize_response(cwist_lwip_conn *c, cwist_http_response *res, size_t *out_len) {
    if (res->use_file_stream) return -1;
    char header_buf[CWIST_HTTP_MAX_HEADER_SIZE];
    size_t header_len = cwist_http_serialize_headers(res, header_buf, sizeof(header_buf));

    const void *body = NULL;
    size_t body_len = 0;
    if (res->stream_mode) {
        if (!res->stream_ended) cwist_http_response_stream_end(res);
        body = res->stream_buf ? res->stream_buf->data : NULL;
        body_len = res->stream_buf ? res->stream_buf->size : 0;
    } else if (res->is_ptr_body) {
        body = res->ptr_body;
        body_len = res->ptr_body_len;
    } else if (res->body && res->body->data) {
        body = res->body->data;
        body_len = res->body->size;
    }
    if (body_len > SIZE_MAX - header_len - 1 || !tx_reserve(c, header_len + body_len + 1)) return -1;

    memcpy(c->tx_store, header_buf, header_len);
    if (body_len) memcpy(c->tx_store + header_len, body, body_len);
    c->tx_store[header_len + body_len] = '\0';
    *out_len = header_len + body_len;
    return 0;
}

static void conn_detach_pcb(cwist_lwip_conn *c) {
    if (!c->pcb) return;
    tcp_arg(c->pcb, NULL);
    tcp_recv(c->pcb, NULL);
    tcp_sent(c->pcb, NULL);
    tcp_err(c->pcb, NULL);
    tcp_poll(c->pcb, NULL, 0);
}

/* Abort: lwIP frees the pcb; callers inside a pcb callback return ERR_ABRT. */
static void conn_abort(cwist_lwip_conn *c) {
    struct tcp_pcb *pcb = c->pcb;
    conn_detach_pcb(c);
    c->pcb = NULL;
    conn_free(c);
    if (pcb) tcp_abort(pcb);
}

/* Graceful close; falls back to abort when lwIP is out of memory.
 * @return ERR_OK, or ERR_ABRT when the pcb was aborted. */
static err_t conn_close(cwist_lwip_conn *c) {
    struct tcp_pcb *pcb = c->pcb;
    conn_detach_pcb(c);
    c->pcb = NULL;
    conn_free(c);
    if (pcb && tcp_close(pcb) != ERR_OK) {
        tcp_abort(pcb);
        return ERR_ABRT;
    }
    return ERR_OK;
}

/* --- sending ------------------------------------------------------------------ */

/* Hand as much of tx to lwIP as the send buffer allows.
 * @return ERR_OK, or ERR_ABRT when the connection was aborted. */
#ifdef CWIST_LWIP_TLS
static err_t tls_push(cwist_lwip_conn *c);
#endif

static err_t conn_push(cwist_lwip_conn *c) {
#ifdef CWIST_LWIP_TLS
    if (c->ssl) return tls_push(c);
#endif
    while (c->tx && c->tx_queued < c->tx_len) {
        size_t room = tcp_sndbuf(c->pcb);
        if (room == 0) break;
        size_t n = c->tx_len - c->tx_queued;
        if (n > room) n = room;
        if (n > 0xFFFF) n = 0xFFFF;
        u8_t flags = (c->tx_queued + n < c->tx_len) ? TCP_WRITE_FLAG_MORE : 0;
        err_t err = tcp_write(c->pcb, c->tx + c->tx_queued, (u16_t)n, flags);
        if (err == ERR_MEM) break; /* retried from tcp_sent / tcp_poll */
        if (err != ERR_OK) {
            conn_abort(c);
            return ERR_ABRT;
        }
        c->tx_queued += n;
    }
    tcp_output(c->pcb);
    return ERR_OK;
}

static err_t conn_send(cwist_lwip_conn *c, const char *buf, size_t len, bool close_after) {
    c->tx = buf;
    c->tx_len = len;
    c->tx_queued = 0;
    c->tx_acked = 0;
    c->close_after_tx = c->close_after_tx || close_after;
    return conn_push(c);
}

/* Serialize a bodyless-by-default error reply and close after it is sent. */
static err_t conn_send_error(cwist_lwip_conn *c, int status) {
    c->rx_len = 0; /* the stream position is unknown past a framing error */
    cwist_http_response *res = cwist_http_response_create();
    size_t out_len = 0;
    int rc = -1;
    if (res) {
        const char *reason = cwist_http_status_reason(status);
        res->status_code = (cwist_http_status_t)status;
        res->keep_alive = false;
        if (reason) cwist_http_response_set_body_ptr(res, reason, strlen(reason));
        rc = conn_serialize_response(c, res, &out_len);
        cwist_http_response_destroy(res);
    }
    if (rc != 0) {
        conn_abort(c);
        return ERR_ABRT;
    }
    return conn_send(c, c->tx_store, out_len, true);
}

/* Length of the status line + headers + CRLFCRLF in a serialized response. */
static size_t response_head_len(const char *buf, size_t len) {
    const char *end = find_crlfcrlf(buf, len);
    return end ? (size_t)(end - buf) + 4 : len;
}

/* --- request processing ------------------------------------------------------- */

/* Answer the next complete request in rx, if any and if idle.
 * @return ERR_OK, or ERR_ABRT when the connection was aborted. */
static err_t conn_process(cwist_lwip_conn *c) {
    if (c->tx || !c->pcb) return ERR_OK;
    if (c->rx_len == 0) {
        if (c->peer_closed) return conn_close(c);
        return ERR_OK;
    }

    size_t req_len = 0;
    int status = 0;
    bool expect_continue = false;
    switch (frame_request(c, &req_len, &status, &expect_continue)) {
    case FRAME_ERROR:
        return conn_send_error(c, status);
    case FRAME_NEED_MORE:
        if (c->peer_closed) return conn_close(c); /* truncated request */
        if (expect_continue && !c->continue_sent) {
            c->continue_sent = true;
#ifdef CWIST_LWIP_TLS
            if (c->ssl) { /* must go through the record layer, like any reply */
                return conn_send(c, CWIST_LWIP_100_CONTINUE,
                                 sizeof(CWIST_LWIP_100_CONTINUE) - 1, false);
            }
#endif
            if (tcp_write(c->pcb, CWIST_LWIP_100_CONTINUE, sizeof(CWIST_LWIP_100_CONTINUE) - 1, 0) ==
                ERR_OK) {
                c->ack_skip += sizeof(CWIST_LWIP_100_CONTINUE) - 1;
                tcp_output(c->pcb);
            }
        }
        return ERR_OK;
    case FRAME_COMPLETE:
        break;
    }

    cwist_http_request *req = cwist_http_parse_request_len(c->rx, req_len);
    memmove(c->rx, c->rx + req_len, c->rx_len - req_len);
    c->rx_len -= req_len;
    c->continue_sent = false;
    if (!req) return conn_send_error(c, CWIST_HTTP_BAD_REQUEST);

    cwist_http_response *res = cwist_http_response_create();
    if (!res) {
        cwist_http_request_destroy(req);
        return conn_send_error(c, CWIST_HTTP_INTERNAL_ERROR);
    }
    cwist_app_dispatch(c->srv->app, req, res);
    if (res->stream_mode && !res->stream_ended) cwist_http_response_stream_end(res);

    bool keep = c->srv->cfg.keep_alive && req->keep_alive && res->keep_alive && !c->peer_closed;
    bool head = req->method == CWIST_HTTP_HEAD;
    cwist_http_request_destroy(req);
    res->keep_alive = keep;

    size_t out_len = 0;
    int rc = conn_serialize_response(c, res, &out_len);
    cwist_http_response_destroy(res);
    if (rc != 0) return conn_send_error(c, CWIST_HTTP_INTERNAL_ERROR);
    if (head) out_len = response_head_len(c->tx_store, out_len);
    return conn_send(c, c->tx_store, out_len, !keep);
}

#ifdef CWIST_LWIP_TLS
/* --- TLS (mbedTLS over the pcb) ----------------------------------------------- */

/* BIO send: copy one chunk of ciphertext into lwIP's send buffer. */
static int tls_bio_send(void *ctx, const unsigned char *buf, size_t len) {
    cwist_lwip_conn *c = ctx;
    if (!c->pcb) return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    size_t room = tcp_sndbuf(c->pcb);
    if (room == 0) return MBEDTLS_ERR_SSL_WANT_WRITE;
    size_t n = len < room ? len : room;
    if (n > 0xFFFF) n = 0xFFFF;
    err_t err = tcp_write(c->pcb, buf, (u16_t)n, TCP_WRITE_FLAG_COPY);
    if (err == ERR_MEM) return MBEDTLS_ERR_SSL_WANT_WRITE; /* segment queue full */
    if (err != ERR_OK) return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    return (int)n;
}

/* BIO recv: hand mbedTLS queued ciphertext; 0 (EOF) once FIN arrived and it drained. */
static int tls_bio_recv(void *ctx, unsigned char *buf, size_t len) {
    cwist_lwip_conn *c = ctx;
    size_t avail = c->tin_len - c->tin_off;
    if (avail == 0) return c->tcp_fin ? 0 : MBEDTLS_ERR_SSL_WANT_READ;
    size_t n = len < avail ? len : avail;
    memcpy(buf, c->tin + c->tin_off, n);
    c->tin_off += n;
    if (c->tin_off == c->tin_len) c->tin_off = c->tin_len = 0;
    return (int)n;
}

/* Best-effort close_notify, then a graceful TCP close. */
static err_t tls_close(cwist_lwip_conn *c) {
    if (c->handshake_done) mbedtls_ssl_close_notify(c->ssl); /* WANT_WRITE: skip it */
    if (c->pcb) tcp_output(c->pcb);
    return conn_close(c);
}

/* tx fully taken by mbedTLS: retain its storage for the next response. The
 * next request is read from tcp_sent (tls_pump), never recursively from here. */
static err_t tls_tx_done(cwist_lwip_conn *c) {
    c->tx = NULL;
    c->tx_len = c->tx_queued = c->tx_acked = 0;
    if (c->close_after_tx) return tls_close(c);
    return ERR_OK;
}

/* Encrypt as much of tx as lwIP's send buffer takes. */
static err_t tls_push(cwist_lwip_conn *c) {
    while (c->tx && c->tx_queued < c->tx_len) {
        int r = mbedtls_ssl_write(c->ssl, (const unsigned char *)c->tx + c->tx_queued,
                                  c->tx_len - c->tx_queued);
        if (r == MBEDTLS_ERR_SSL_WANT_WRITE || r == MBEDTLS_ERR_SSL_WANT_READ) break;
        if (r < 0) {
            conn_abort(c);
            return ERR_ABRT;
        }
        c->tx_queued += (size_t)r; /* retried with the same pointer after WANT_WRITE */
    }
    if (c->pcb) tcp_output(c->pcb);
    if (c->tx && c->tx_queued == c->tx_len) return tls_tx_done(c);
    return ERR_OK;
}

/* Advance the handshake, decrypt into rx (within the request limits), and
 * answer whatever request that completes. */
static err_t tls_pump(cwist_lwip_conn *c) {
    if (!c->pcb) return ERR_OK;
    if (c->tx) return tls_push(c); /* one exchange at a time */
    if (!c->handshake_done) {
        int r = mbedtls_ssl_handshake(c->ssl);
        tcp_output(c->pcb);
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) return ERR_OK;
        if (r != 0) { /* bad client hello, no shared cipher, peer alert, ... */
            conn_abort(c);
            return ERR_ABRT;
        }
        c->handshake_done = true;
    }
    if (c->close_after_tx) return ERR_OK;

    size_t limit = c->srv->cfg.max_header_bytes + c->srv->cfg.max_body_bytes;
    while (c->rx_len < limit + 1) {
        size_t want = limit + 1 - c->rx_len;
        if (want > 2048) want = 2048;
        if (!rx_reserve(c, c->rx_len + want, limit)) {
            conn_abort(c);
            return ERR_ABRT;
        }
        int r = mbedtls_ssl_read(c->ssl, (unsigned char *)c->rx + c->rx_len, want);
        if (r > 0) {
            c->rx_len += (size_t)r;
            continue;
        }
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) break;
        if (r == 0 || r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || r == MBEDTLS_ERR_SSL_CONN_EOF) {
            c->peer_closed = true;
            break;
        }
        conn_abort(c); /* bad record MAC, unexpected message, ... */
        return ERR_ABRT;
    }
    tcp_output(c->pcb); /* alerts or renegotiation refusals */
    return conn_process(c);
}

/* Queue received ciphertext; refuse it (lwIP redelivers) while the queue is full. */
static err_t tls_on_recv(cwist_lwip_conn *c, struct tcp_pcb *pcb, struct pbuf *p, err_t err) {
    if (!p) {
        c->tcp_fin = true;
        return tls_pump(c);
    }
    if (err != ERR_OK) {
        pbuf_free(p);
        return err;
    }
    c->last_active_ms = sys_now();
    if (c->tin_off > 0) { /* compact before growing */
        memmove(c->tin, c->tin + c->tin_off, c->tin_len - c->tin_off);
        c->tin_len -= c->tin_off;
        c->tin_off = 0;
    }
    size_t need = c->tin_len + p->tot_len;
    if (need > CWIST_LWIP_TLS_IN_MAX) return ERR_MEM;
    if (need > c->tin_cap) {
        size_t cap = c->tin_cap ? c->tin_cap : 1024;
        while (cap < need) cap *= 2;
        if (cap > CWIST_LWIP_TLS_IN_MAX) cap = CWIST_LWIP_TLS_IN_MAX;
        char *grown = cwist_realloc(c->tin, cap);
        if (!grown) {
            pbuf_free(p);
            conn_abort(c);
            return ERR_ABRT;
        }
        c->tin = grown;
        c->tin_cap = cap;
    }
    pbuf_copy_partial(p, c->tin + c->tin_len, p->tot_len, 0);
    c->tin_len += p->tot_len;
    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);
    return tls_pump(c);
}

/* CTR_DRBG entropy source: the firmware TRNG, nothing else. */
static int tls_entropy(void *ctx, unsigned char *out, size_t len) {
    (void)ctx;
    return cwist_port_entropy(out, len) == 0 ? 0 : MBEDTLS_ERR_CTR_DRBG_ENTROPY_SOURCE_FAILED;
}
#endif /* CWIST_LWIP_TLS */

/* --- lwIP callbacks ------------------------------------------------------------- */

static err_t on_sent(void *arg, struct tcp_pcb *pcb, u16_t len) {
    cwist_lwip_conn *c = arg;
    (void)pcb;
    if (!c) return ERR_OK;
    c->last_active_ms = sys_now();
#ifdef CWIST_LWIP_TLS
    if (c->ssl) return tls_pump(c); /* resume handshake / writes, then the next request */
#endif
    size_t acked = len;
    size_t skip = acked < c->ack_skip ? acked : c->ack_skip;
    c->ack_skip -= skip;
    acked -= skip;
    if (!c->tx) return ERR_OK;
    c->tx_acked += acked;
    if (c->tx_acked < c->tx_len) return conn_push(c);

    c->tx = NULL;
    c->tx_len = c->tx_queued = c->tx_acked = 0;
    if (c->close_after_tx) return conn_close(c);
    return conn_process(c); /* next pipelined request */
}

static err_t on_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err) {
    cwist_lwip_conn *c = arg;
    if (!c) {
        if (p) pbuf_free(p);
        return ERR_OK;
    }
#ifdef CWIST_LWIP_TLS
    if (c->ssl) return tls_on_recv(c, pcb, p, err);
#endif
    if (!p) { /* FIN */
        c->peer_closed = true;
        return conn_process(c);
    }
    if (err != ERR_OK) {
        pbuf_free(p);
        return err;
    }
    c->last_active_ms = sys_now();
    if (c->close_after_tx) { /* draining toward close: discard */
        tcp_recved(pcb, p->tot_len);
        pbuf_free(p);
        return ERR_OK;
    }

    /* One request is at most header cap + body cap; keep one spare request
     * worth of pipelined bytes while a response is in flight. */
    size_t limit = c->srv->cfg.max_header_bytes + c->srv->cfg.max_body_bytes;
    size_t need = c->rx_len + p->tot_len;
    if (need > limit + 1) {
        if (c->tx) return ERR_MEM; /* lwIP keeps p and redelivers it later */
        need = limit + 1;           /* enough to classify as 413/431 */
    }
    if (!rx_reserve(c, need, limit)) {
        pbuf_free(p);
        conn_abort(c);
        return ERR_ABRT;
    }
    size_t take = need - c->rx_len;
    pbuf_copy_partial(p, c->rx + c->rx_len, (u16_t)take, 0);
    c->rx_len += take;
    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);
    return conn_process(c);
}

static void on_err(void *arg, err_t err) {
    cwist_lwip_conn *c = arg;
    (void)err;
    if (!c) return;
    c->pcb = NULL; /* already freed by lwIP */
    conn_free(c);
}

static err_t on_poll(void *arg, struct tcp_pcb *pcb) {
    cwist_lwip_conn *c = arg;
    (void)pcb;
    if (!c) return ERR_OK;
    uint32_t timeout = c->srv->cfg.idle_timeout_ms;
    if (timeout && (u32_t)(sys_now() - c->last_active_ms) >= timeout) return conn_close(c);
    if (c->tx && c->tx_queued < c->tx_len) return conn_push(c); /* retry after ERR_MEM */
#ifdef CWIST_LWIP_TLS
    if (c->ssl) return tls_pump(c); /* refused ciphertext / a stalled handshake */
#endif
    return ERR_OK;
}

static err_t on_accept(void *arg, struct tcp_pcb *pcb, err_t err) {
    cwist_lwip_server *srv = arg;
    if (err != ERR_OK || !pcb) return ERR_VAL;
    if (srv->nconns >= srv->cfg.max_conns) {
        tcp_abort(pcb);
        return ERR_ABRT;
    }
    cwist_lwip_conn *c = cwist_alloc(sizeof(*c));
    if (!c) {
        tcp_abort(pcb);
        return ERR_ABRT;
    }
    memset(c, 0, sizeof(*c));
    c->srv = srv;
    c->pcb = pcb;
    c->last_active_ms = sys_now();
    c->next = srv->conns;
    srv->conns = c;
    srv->nconns++;

#ifdef CWIST_LWIP_TLS
    if (srv->cfg.tls) {
        c->ssl = cwist_alloc(sizeof(*c->ssl));
        if (c->ssl) mbedtls_ssl_init(c->ssl);
        if (!c->ssl || mbedtls_ssl_setup(c->ssl, &srv->cfg.tls->conf) != 0) {
            c->pcb = NULL;
            conn_free(c);
            tcp_abort(pcb);
            return ERR_ABRT;
        }
        mbedtls_ssl_set_bio(c->ssl, c, tls_bio_send, tls_bio_recv, NULL);
    }
#endif

    tcp_arg(pcb, c);
    tcp_recv(pcb, on_recv);
    tcp_sent(pcb, on_sent);
    tcp_err(pcb, on_err);
    tcp_poll(pcb, on_poll, CWIST_LWIP_POLL_TICKS);
    return ERR_OK;
}

/* --- public API ----------------------------------------------------------------- */

/**
 * @brief Fill @p cfg with the defaults documented on cwist_lwip_config.
 * @param cfg Configuration to initialize.
 */
void cwist_lwip_config_init(cwist_lwip_config *cfg) {
    if (!cfg) return;
    cfg->port = 80;
    cfg->max_conns = 4;
    cfg->max_header_bytes = 4096;
    cfg->max_body_bytes = 16384;
    cfg->idle_timeout_ms = 30000;
    cfg->keep_alive = true;
    cfg->tls = NULL;
}

/**
 * @brief Bind a listening tcp_pcb on @p cfg->port and serve @p app on it.
 * @param app Application to dispatch to; must outlive the server.
 * @param cfg Settings, copied; NULL means the defaults.
 * @return Server handle, or NULL on pcb or allocation failure.
 */
cwist_lwip_server *cwist_lwip_listen(cwist_app *app, const cwist_lwip_config *cfg) {
    if (!app) return NULL;
    cwist_lwip_server *srv = cwist_alloc(sizeof(*srv));
    if (!srv) return NULL;
    memset(srv, 0, sizeof(*srv));
    srv->app = app;
    if (cfg)
        srv->cfg = *cfg;
    else
        cwist_lwip_config_init(&srv->cfg);
    if (srv->cfg.max_conns == 0) srv->cfg.max_conns = 1;
#ifndef CWIST_LWIP_TLS
    if (srv->cfg.tls) goto fail; /* never fall back to plaintext silently */
#endif

    struct tcp_pcb *pcb = tcp_new_ip_type(IPADDR_TYPE_ANY);
    if (!pcb) goto fail;
#if SO_REUSE
    /* Restarting the listener must not wait out TIME_WAIT on connections the
     * server closed (it is the active closer for Connection: close). */
    ip_set_option(pcb, SOF_REUSEADDR);
#endif
    if (tcp_bind(pcb, IP_ANY_TYPE, srv->cfg.port) != ERR_OK) {
        tcp_close(pcb);
        goto fail;
    }
    struct tcp_pcb *lpcb = tcp_listen_with_backlog(pcb, srv->cfg.max_conns);
    if (!lpcb) {
        tcp_close(pcb);
        goto fail;
    }
    srv->listen_pcb = lpcb;
    tcp_arg(lpcb, srv);
    tcp_accept(lpcb, on_accept);
    return srv;

fail:
    cwist_free(srv);
    return NULL;
}

/**
 * @brief Stop listening, abort every open connection, and free @p srv.
 * @param srv Server returned by cwist_lwip_listen(); NULL is a no-op.
 */
void cwist_lwip_close(cwist_lwip_server *srv) {
    if (!srv) return;
    if (srv->listen_pcb) {
        tcp_arg(srv->listen_pcb, NULL);
        tcp_accept(srv->listen_pcb, NULL);
        tcp_close(srv->listen_pcb);
        srv->listen_pcb = NULL;
    }
    while (srv->conns) conn_abort(srv->conns);
    cwist_free(srv);
}

/**
 * @brief Number of connections the server currently holds open.
 * @param srv Server handle.
 * @return Open connection count (0 for NULL).
 */
size_t cwist_lwip_active_connections(const cwist_lwip_server *srv) {
    return srv ? srv->nconns : 0;
}

/**
 * @brief Create a TLS server context from a certificate chain and private key.
 * @param cert Certificate chain, leaf first (PEM with its NUL counted, or DER).
 * @param cert_len Length of @p cert.
 * @param key Matching private key (PEM with its NUL counted, or DER).
 * @param key_len Length of @p key.
 * @return Context, or NULL on parse/pairing/seeding/allocation failure or
 *         when built without CWIST_LWIP_TLS.
 */
cwist_lwip_tls *cwist_lwip_tls_create(const unsigned char *cert, size_t cert_len,
                                      const unsigned char *key, size_t key_len) {
#ifdef CWIST_LWIP_TLS
    static const unsigned char pers[] = "cwist-lwip-tls";
    if (!cert || !key) return NULL;
    cwist_lwip_tls *tls = cwist_alloc(sizeof(*tls));
    if (!tls) return NULL;
    mbedtls_ssl_config_init(&tls->conf);
    mbedtls_ctr_drbg_init(&tls->drbg);
    mbedtls_x509_crt_init(&tls->cert);
    mbedtls_pk_init(&tls->key);
    if (mbedtls_ctr_drbg_seed(&tls->drbg, tls_entropy, NULL, pers, sizeof(pers) - 1) != 0 ||
        mbedtls_x509_crt_parse(&tls->cert, cert, cert_len) != 0 ||
        mbedtls_pk_parse_key(&tls->key, key, key_len, NULL, 0, mbedtls_ctr_drbg_random,
                             &tls->drbg) != 0 ||
        mbedtls_pk_check_pair(&tls->cert.pk, &tls->key, mbedtls_ctr_drbg_random, &tls->drbg) != 0 ||
        mbedtls_ssl_config_defaults(&tls->conf, MBEDTLS_SSL_IS_SERVER, MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT) != 0 ||
        mbedtls_ssl_conf_own_cert(&tls->conf, &tls->cert, &tls->key) != 0) {
        cwist_lwip_tls_destroy(tls);
        return NULL;
    }
    mbedtls_ssl_conf_rng(&tls->conf, mbedtls_ctr_drbg_random, &tls->drbg);
    return tls;
#else
    (void)cert;
    (void)cert_len;
    (void)key;
    (void)key_len;
    return NULL;
#endif
}

/**
 * @brief Free a TLS context; close every server using it first.
 * @param tls Context from cwist_lwip_tls_create(); NULL is a no-op.
 */
void cwist_lwip_tls_destroy(cwist_lwip_tls *tls) {
#ifdef CWIST_LWIP_TLS
    if (!tls) return;
    mbedtls_ssl_config_free(&tls->conf);
    mbedtls_x509_crt_free(&tls->cert);
    mbedtls_pk_free(&tls->key);
    mbedtls_ctr_drbg_free(&tls->drbg);
    cwist_free(tls);
#else
    (void)tls;
#endif
}
