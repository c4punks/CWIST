/**
 * @file test_https_park.c
 * @brief TLS connections served through the HTTPS pool: idle parking, idle
 * expiry, and HTTP/2 responses that are deferred or rewritten by middleware.
 *
 * The pool runs a single thread (CWIST_WORKER_THREADS=1). A connection that
 * kept that thread while idle would stall every other one, so each case that
 * opens a second connection while the first is idle times out unless the
 * first was parked. Idle budgets are shortened so expiry is observable.
 */
#include <cwist/sys/app/app.h>
#include <cwist/sys/app/compress.h>
#include <cwist/net/http/https.h>
#include <cwist/net/http/async.h>
#include <cwist/core/mem/gc.h>
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

static const char *TEST_CERT = "example/othello-web/server.crt";
static const char *TEST_KEY = "example/othello-web/server.key";

#define IDLE_MS 1500
#define DEFER_BODY "deferred-h2-body"
#define BIG_LEN 4000

typedef struct {
    SSL_CTX *ctx;
    SSL *ssl;
    int fd;
} client_t;

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

/* --- routes --------------------------------------------------------------- */

static void hello_route(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    cwist_http_header_add(&res->headers, "Content-Type", "text/plain");
    cwist_sstring_assign(res->body, "hello");
}

static void *defer_worker(void *arg) {
    struct timespec ts = {.tv_sec = 0, .tv_nsec = 50 * 1000 * 1000};
    nanosleep(&ts, NULL);
    assert(cwist_async_respond((cwist_async *)arg, CWIST_HTTP_OK, "text/plain", DEFER_BODY,
                               strlen(DEFER_BODY)));
    return NULL;
}

/* Sets a Content-Length that does not match the body cwist_async_respond()
 * stores later: the HTTP/2 encoder must send the real length, once. */
static void defer_route(cwist_http_request *req, cwist_http_response *res) {
    cwist_http_header_add(&res->headers, "Content-Length", "999");
    cwist_async *a = cwist_async_defer(req, res);
    assert(a != NULL);
    pthread_t t;
    assert(pthread_create(&t, NULL, defer_worker, a) == 0);
    pthread_detach(t);
}

/* Sets the Content-Length of the uncompressed body; the compress middleware
 * then shrinks the body after the handler returns. */
static void big_route(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    char body[BIG_LEN + 1];
    memset(body, 'a', BIG_LEN);
    body[BIG_LEN] = '\0';
    cwist_http_header_add(&res->headers, "Content-Type", "text/plain");
    cwist_http_header_add(&res->headers, "Content-Length", "4000");
    cwist_sstring_assign_len(res->body, body, BIG_LEN);
}

static cwist_app *make_app(bool http2) {
    cwist_app *app = cwist_app_create();
    assert(app != NULL);
    if (http2) cwist_app_use_https2(app, true);
    cwist_error_t err = cwist_app_use_https(app, TEST_CERT, TEST_KEY);
    assert(cwist_error_is_ok(&err));
    cwist_app_use(app, cwist_mw_compress(64));
    cwist_app_get(app, "/hello", hello_route);
    cwist_app_get(app, "/defer", defer_route);
    cwist_app_get(app, "/big", big_route);
    return app;
}

/* --- client --------------------------------------------------------------- */

static client_t client_open(cwist_app *app, bool http2) {
    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    https_pool_submit(sv[0], app->ssl_ctx, app->https_request_handler, app);

    client_t c = {.fd = sv[1]};
    /* Bound every read: a stalled server fails the test instead of hanging. */
    struct timeval tv = {.tv_sec = 5, .tv_usec = 0};
    assert(setsockopt(c.fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0);
    c.ctx = SSL_CTX_new(TLS_client_method());
    assert(c.ctx != NULL);
    SSL_CTX_set_verify(c.ctx, SSL_VERIFY_NONE, NULL);
    c.ssl = SSL_new(c.ctx);
    assert(c.ssl != NULL);
    assert(SSL_set_fd(c.ssl, c.fd) == 1);
    if (http2) {
        static const unsigned char alpn[] = "\x02h2";
        assert(SSL_set_alpn_protos(c.ssl, alpn, sizeof(alpn) - 1) == 0);
    }
    assert(SSL_connect(c.ssl) == 1);
    return c;
}

static void client_close(client_t *c) {
    SSL_free(c->ssl);
    SSL_CTX_free(c->ctx);
    close(c->fd);
}

static int ssl_read_exact(SSL *ssl, void *buf, size_t len) {
    unsigned char *p = buf;
    size_t off = 0;
    while (off < len) {
        int n = SSL_read(ssl, p + off, (int)(len - off));
        if (n <= 0) return -1;
        off += (size_t)n;
    }
    return 0;
}

static void ssl_write_all(SSL *ssl, const void *buf, size_t len) {
    const unsigned char *p = buf;
    size_t off = 0;
    while (off < len) {
        int n = SSL_write(ssl, p + off, (int)(len - off));
        assert(n > 0);
        off += (size_t)n;
    }
}

/* HTTP/1.1 GET; returns 0 and the body in @p body on a 200 response. */
static int h1_get(client_t *c, const char *path, char *body, size_t body_cap) {
    char req[256];
    int n = snprintf(req, sizeof(req),
                     "GET %s HTTP/1.1\r\nHost: localhost\r\nConnection: keep-alive\r\n\r\n", path);
    ssl_write_all(c->ssl, req, (size_t)n);

    char buf[8192];
    size_t total = 0;
    while (total < sizeof(buf) - 1) {
        int r = SSL_read(c->ssl, buf + total, (int)(sizeof(buf) - 1 - total));
        if (r <= 0) return -1;
        total += (size_t)r;
        buf[total] = '\0';
        char *end = strstr(buf, "\r\n\r\n");
        if (!end) continue;
        char *cl = strcasestr(buf, "\r\ncontent-length:");
        if (!cl) return -1;
        size_t want = (size_t)atol(cl + 17);
        size_t have = total - (size_t)(end + 4 - buf);
        if (have < want) continue;
        if (strncmp(buf, "HTTP/1.1 200", 12) != 0) return -1;
        snprintf(body, body_cap, "%.*s", (int)want, end + 4);
        return 0;
    }
    return -1;
}

/* True once the peer has closed the connection (EOF or reset). */
static bool wait_closed(client_t *c, int timeout_ms) {
    uint64_t deadline = now_ms() + (uint64_t)timeout_ms;
    while (now_ms() < deadline) {
        char b;
        errno = 0;
        int r = SSL_read(c->ssl, &b, 1);
        if (r > 0) continue;
        int e = SSL_get_error(c->ssl, r);
        /* SO_RCVTIMEO expiring is not a close. */
        if (e == SSL_ERROR_WANT_READ || errno == EAGAIN || errno == EWOULDBLOCK) continue;
        return true;
    }
    return false;
}

/* --- HTTP/2 client -------------------------------------------------------- */

typedef struct {
    unsigned char block[4096]; /* HEADERS block (no CONTINUATION expected) */
    size_t block_len;
    unsigned char data[8192];
    size_t data_len;
    bool ended;
    bool goaway;
} h2_resp_t;

static void h2_start(client_t *c) {
    static const unsigned char preface[] = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
    static const unsigned char settings[] = {0, 0, 0, 0x04, 0, 0, 0, 0, 0};
    ssl_write_all(c->ssl, preface, sizeof(preface) - 1);
    ssl_write_all(c->ssl, settings, sizeof(settings));
}

/* GET @p path on @p stream_id; @p gzip adds accept-encoding: gzip. */
static void h2_get(client_t *c, uint32_t stream_id, const char *path, bool gzip) {
    unsigned char block[128];
    size_t n = 0, plen = strlen(path);
    block[n++] = 0x82; /* :method GET */
    block[n++] = 0x87; /* :scheme https */
    block[n++] = 0x04; /* :path, literal without indexing */
    block[n++] = (unsigned char)plen;
    memcpy(block + n, path, plen);
    n += plen;
    if (gzip) {
        block[n++] = 0x0f; /* accept-encoding (static index 16) */
        block[n++] = 0x01;
        block[n++] = 0x04;
        memcpy(block + n, "gzip", 4);
        n += 4;
    }
    unsigned char hdr[9] = {0,
                            0,
                            (unsigned char)n,
                            0x01,
                            0x05, /* END_STREAM | END_HEADERS */
                            (unsigned char)(stream_id >> 24),
                            (unsigned char)(stream_id >> 16),
                            (unsigned char)(stream_id >> 8),
                            (unsigned char)stream_id};
    ssl_write_all(c->ssl, hdr, sizeof(hdr));
    ssl_write_all(c->ssl, block, n);
}

/* Reads frames until @p stream_id ends or a GOAWAY arrives. Other streams'
 * frames are skipped. Returns 0, or -1 on timeout or EOF. */
static int h2_read_response(client_t *c, uint32_t stream_id, h2_resp_t *r) {
    memset(r, 0, sizeof(*r));
    while (!r->ended) {
        unsigned char hdr[9];
        if (ssl_read_exact(c->ssl, hdr, sizeof(hdr)) != 0) return -1;
        uint32_t len = ((uint32_t)hdr[0] << 16) | ((uint32_t)hdr[1] << 8) | hdr[2];
        uint8_t type = hdr[3], flags = hdr[4];
        uint32_t sid = (((uint32_t)hdr[5] & 0x7f) << 24) | ((uint32_t)hdr[6] << 16) |
                       ((uint32_t)hdr[7] << 8) | hdr[8];
        unsigned char payload[16384];
        assert(len <= sizeof(payload));
        if (len > 0 && ssl_read_exact(c->ssl, payload, len) != 0) return -1;
        if (type == 0x7) {
            r->goaway = true;
            return 0;
        }
        if (sid != stream_id) continue;
        if (type == 0x1) {
            assert(flags & 0x4); /* END_HEADERS: the block fits one frame */
            assert(r->block_len + len <= sizeof(r->block));
            memcpy(r->block + r->block_len, payload, len);
            r->block_len += len;
        } else if (type == 0x0) {
            assert(r->data_len + len <= sizeof(r->data));
            memcpy(r->data + r->data_len, payload, len);
            r->data_len += len;
        }
        if ((type == 0x0 || type == 0x1) && (flags & 0x1)) r->ended = true;
    }
    return 0;
}

/* HPACK integer with an N-bit prefix (RFC 7541 section 5.1). */
static size_t hpack_int(const unsigned char *p, size_t len, int prefix, uint32_t *out) {
    uint32_t max = (1u << prefix) - 1;
    uint32_t v = p[0] & max;
    size_t i = 1;
    if (v == max) {
        int shift = 0;
        do {
            assert(i < len);
            v += (uint32_t)(p[i] & 0x7f) << shift;
            shift += 7;
        } while (p[i++] & 0x80);
    }
    *out = v;
    return i;
}

/* Non-Huffman string literal; cwist does not Huffman-encode. */
static size_t hpack_str(const unsigned char *p, size_t len, char *out, size_t cap) {
    assert((p[0] & 0x80) == 0);
    uint32_t n;
    size_t i = hpack_int(p, len, 7, &n);
    assert(i + n <= len && n < cap);
    memcpy(out, p + i, n);
    out[n] = '\0';
    return i + n;
}

typedef struct {
    int status;
    int content_length_count;
    long content_length;
    bool gzip;
} h2_headers_t;

/* Decodes the fields cwist emits: indexed :status, and literals without or
 * never indexed, with a static-table or literal name. */
static h2_headers_t h2_decode(const unsigned char *b, size_t len) {
    h2_headers_t h = {.content_length = -1};
    size_t i = 0;
    while (i < len) {
        unsigned char op = b[i];
        if (op & 0x80) {
            uint32_t idx;
            i += hpack_int(b + i, len - i, 7, &idx);
            if (idx >= 8 && idx <= 14) {
                static const int codes[] = {200, 204, 206, 304, 400, 404, 500};
                h.status = codes[idx - 8];
            }
            continue;
        }
        assert((op & 0xe0) != 0x20); /* no table size updates expected */
        int prefix = (op & 0x40) ? 6 : 4;
        uint32_t idx;
        i += hpack_int(b + i, len - i, prefix, &idx);
        char name[64], value[256];
        if (idx == 0) {
            i += hpack_str(b + i, len - i, name, sizeof(name));
        } else if (idx == 8) {
            snprintf(name, sizeof(name), ":status");
        } else if (idx == 26) {
            snprintf(name, sizeof(name), "content-encoding");
        } else if (idx == 28) {
            snprintf(name, sizeof(name), "content-length");
        } else {
            snprintf(name, sizeof(name), "#%u", idx);
        }
        i += hpack_str(b + i, len - i, value, sizeof(value));
        if (strcmp(name, ":status") == 0) h.status = atoi(value);
        if (strcasecmp(name, "content-length") == 0) {
            h.content_length_count++;
            h.content_length = atol(value);
        }
        if (strcasecmp(name, "content-encoding") == 0 && strcmp(value, "gzip") == 0) h.gzip = true;
    }
    return h;
}

/* --- cases ---------------------------------------------------------------- */

/* An idle HTTP/1.1 keep-alive connection must release the only pool thread:
 * a second connection is served at once, and the first resumes later. */
static void test_http1_idle_connection_is_parked(void) {
    printf("Testing HTTP/1.1 idle TLS connection parking...\n");
    cwist_app *app = make_app(false);
    char body[64];

    client_t a = client_open(app, false);
    assert(h1_get(&a, "/hello", body, sizeof(body)) == 0 && strcmp(body, "hello") == 0);

    uint64_t t0 = now_ms();
    client_t b = client_open(app, false);
    assert(h1_get(&b, "/hello", body, sizeof(body)) == 0 && strcmp(body, "hello") == 0);
    assert(now_ms() - t0 < IDLE_MS / 2);

    assert(h1_get(&a, "/hello", body, sizeof(body)) == 0 && strcmp(body, "hello") == 0);

    client_close(&a);
    client_close(&b);
    cwist_app_destroy(app);
    printf("Passed HTTP/1.1 idle TLS connection parking.\n");
}

/* A parked HTTP/1.1 connection is closed after the idle budget. */
static void test_http1_parked_connection_expires(void) {
    printf("Testing HTTP/1.1 parked connection expiry...\n");
    cwist_app *app = make_app(false);
    char body[64];

    client_t a = client_open(app, false);
    assert(h1_get(&a, "/hello", body, sizeof(body)) == 0);
    uint64_t t0 = now_ms();
    assert(wait_closed(&a, IDLE_MS + 3000));
    assert(now_ms() - t0 >= IDLE_MS - 200);

    client_close(&a);
    cwist_app_destroy(app);
    printf("Passed HTTP/1.1 parked connection expiry.\n");
}

/* The same for HTTP/2: the session survives parking (HPACK state included,
 * since the second request on stream 3 reuses it), and a second connection
 * is served while the first is idle. */
static void test_http2_idle_session_is_parked(void) {
    printf("Testing HTTP/2 idle TLS session parking...\n");
    cwist_app *app = make_app(true);
    h2_resp_t r;

    client_t a = client_open(app, true);
    h2_start(&a);
    h2_get(&a, 1, "/hello", false);
    assert(h2_read_response(&a, 1, &r) == 0 && !r.goaway);
    assert(r.data_len == 5 && memcmp(r.data, "hello", 5) == 0);

    uint64_t t0 = now_ms();
    client_t b = client_open(app, true);
    h2_start(&b);
    h2_get(&b, 1, "/hello", false);
    assert(h2_read_response(&b, 1, &r) == 0 && !r.goaway && r.data_len == 5);
    assert(now_ms() - t0 < IDLE_MS / 2);

    h2_get(&a, 3, "/hello", false);
    assert(h2_read_response(&a, 3, &r) == 0 && !r.goaway);
    assert(r.data_len == 5 && memcmp(r.data, "hello", 5) == 0);

    client_close(&a);
    client_close(&b);
    cwist_app_destroy(app);
    printf("Passed HTTP/2 idle TLS session parking.\n");
}

/* A parked HTTP/2 session that reaches its idle timeout sends GOAWAY and
 * then closes, as the in-thread idle expiry does. */
static void test_http2_parked_session_expires_with_goaway(void) {
    printf("Testing HTTP/2 parked session expiry...\n");
    cwist_app *app = make_app(true);
    h2_resp_t r;

    client_t a = client_open(app, true);
    h2_start(&a);
    h2_get(&a, 1, "/hello", false);
    assert(h2_read_response(&a, 1, &r) == 0 && !r.goaway);

    uint64_t t0 = now_ms();
    assert(h2_read_response(&a, 99, &r) == 0 && r.goaway);
    assert(now_ms() - t0 >= IDLE_MS - 200);
    assert(wait_closed(&a, 3000));

    client_close(&a);
    cwist_app_destroy(app);
    printf("Passed HTTP/2 parked session expiry.\n");
}

/* A deferred HTTP/2 response is sent without further client traffic, with
 * the real content-length (not the stale one the handler set), and the
 * session keeps serving afterwards. */
static void test_http2_deferred_response(void) {
    printf("Testing HTTP/2 deferred response over TLS...\n");
    cwist_app *app = make_app(true);
    h2_resp_t r;

    client_t a = client_open(app, true);
    h2_start(&a);
    h2_get(&a, 1, "/defer", false);
    assert(h2_read_response(&a, 1, &r) == 0 && !r.goaway);
    h2_headers_t h = h2_decode(r.block, r.block_len);
    assert(h.status == 200);
    assert(h.content_length_count == 1);
    assert(h.content_length == (long)strlen(DEFER_BODY));
    assert(r.data_len == strlen(DEFER_BODY) && memcmp(r.data, DEFER_BODY, r.data_len) == 0);

    h2_get(&a, 3, "/hello", false);
    assert(h2_read_response(&a, 3, &r) == 0 && !r.goaway && r.data_len == 5);

    client_close(&a);
    cwist_app_destroy(app);
    printf("Passed HTTP/2 deferred response over TLS.\n");
}

/* The compress middleware shrinks a body whose handler already set
 * Content-Length: the HTTP/2 content-length must match the DATA sent. */
static void test_http2_content_length_matches_compressed_body(void) {
    printf("Testing HTTP/2 content-length after compression...\n");
    cwist_app *app = make_app(true);
    h2_resp_t r;

    client_t a = client_open(app, true);
    h2_start(&a);
    h2_get(&a, 1, "/big", true);
    assert(h2_read_response(&a, 1, &r) == 0 && !r.goaway);
    h2_headers_t h = h2_decode(r.block, r.block_len);
    assert(h.status == 200);
    assert(h.gzip);
    assert(r.data_len < BIG_LEN);
    assert(h.content_length_count == 1);
    assert(h.content_length == (long)r.data_len);

    client_close(&a);
    cwist_app_destroy(app);
    printf("Passed HTTP/2 content-length after compression.\n");
}

/* Pool shutdown closes connections that are parked at that moment. */
static void test_pool_destroy_closes_parked_connections(void) {
    printf("Testing pool shutdown with parked connections...\n");
    cwist_app *app = make_app(true);
    char body[64];
    h2_resp_t r;

    client_t a = client_open(app, false);
    assert(h1_get(&a, "/hello", body, sizeof(body)) == 0);
    client_t b = client_open(app, true);
    h2_start(&b);
    h2_get(&b, 1, "/hello", false);
    assert(h2_read_response(&b, 1, &r) == 0 && !r.goaway);

    https_pool_destroy();
    assert(wait_closed(&a, 1000));
    assert(wait_closed(&b, 1000));

    client_close(&a);
    client_close(&b);
    cwist_app_destroy(app);
    printf("Passed pool shutdown with parked connections.\n");
}

#ifdef TEST_HTTPS_PARK_FULL_GC
/* Pool shutdown under full GC: the parked conn is untracked from the
 * connection registry before teardown, so the pool thread's exit sweep does
 * not close it a second time (ASan catches the double free). */
static void test_pool_destroy_closes_parked_http1_full_gc(void) {
    printf("Testing pool shutdown with a parked HTTP/1.1 connection under full GC...\n");
    cwist_app *app = make_app(false);
    char body[64];

    client_t a = client_open(app, false);
    assert(h1_get(&a, "/hello", body, sizeof(body)) == 0);
    https_pool_destroy();
    assert(wait_closed(&a, 1000));

    client_close(&a);
    cwist_app_destroy(app);
    printf("Passed pool shutdown with a parked HTTP/1.1 connection under full GC.\n");
}
#endif

int main(void) {
    signal(SIGPIPE, SIG_IGN);
#ifdef TEST_HTTPS_PARK_FULL_GC
    cwist_full_gc(true);
#endif
    /* Read once and cached by cwist, so set before anything starts. */
    setenv("CWIST_WORKER_THREADS", "1", 1);
    setenv("CWIST_HTTPS_IDLE_TIMEOUT_MS", "1500", 1);
    setenv("CWIST_HTTP2_IDLE_TIMEOUT_MS", "1500", 1);
    setenv("CWIST_HTTP2_GOAWAY_GRACE_MS", "200", 1);
    cwist_compress_register_backend(cwist_compress_backend_gzip());
    assert(https_pool_init() == 0);

    test_http1_idle_connection_is_parked();
    test_http1_parked_connection_expires();
    test_http2_idle_session_is_parked();
    test_http2_parked_session_expires_with_goaway();
    test_http2_deferred_response();
    test_http2_content_length_matches_compressed_body();
#ifdef TEST_HTTPS_PARK_FULL_GC
    test_pool_destroy_closes_parked_http1_full_gc();
    assert(https_pool_init() == 0);
#endif
    test_pool_destroy_closes_parked_connections();
    printf("All HTTPS parking tests passed!\n");
    return 0;
}
