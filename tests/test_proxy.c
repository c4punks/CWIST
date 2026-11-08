/**
 * @file test_proxy.c
 * @brief Reverse proxy tests: input validation, rule resolution, and
 *        end-to-end forwarding through forked upstream and proxy servers.
 */

#include <cwist/net/proxy/proxy.h>
#include <cwist/core/mem/alloc.h>
#include <cwist/core/sstring/sstring.h>
#include <cwist/net/http/http.h>
#include <cwist/sys/app/app.h>
#include "../src/net/proxy/proxy_internal.h"
#include "../src/net/http/tls_info.h"
#include <cwist/net/http/http3_client.h>
#include <curl/curl.h>

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define UPSTREAM_PORT 19961
#define PROXY_PORT 19962
#define FWD_PORT 19963
#define FWD_PORT2 19964
#define DEAD_PORT 19965
#define UP_PQ_PORT 19971       /* https upstream offering hybrid PQ key exchange */
#define UP_CLASSIC_PORT 19972  /* https upstream with classical groups only */
#define TLS_MIRROR_PORT 19974  /* https origin, TLS level mirrored upstream */
#define TLS_PROMOTE_PORT 19975 /* https origin, tls_auto_promote */

static char g_cert[256];
static char g_key[256];

static int g_failures = 0;

#define CHECK(cond, msg)                                            \
    do {                                                            \
        if (cond) {                                                 \
            printf("  ok   %s\n", msg);                             \
        } else {                                                    \
            printf("  FAIL %s (%s:%d)\n", msg, __FILE__, __LINE__); \
            g_failures++;                                           \
        }                                                           \
    } while (0)

/* --- Unit tests -------------------------------------------------------------- */

static void test_path_safety(void) {
    printf("path safety\n");
    CHECK(cwist_proxy_path_is_safe("/"), "root");
    CHECK(cwist_proxy_path_is_safe("/api/users/1"), "plain path");
    CHECK(cwist_proxy_path_is_safe("/a/.well-known/x"), "dot-prefixed segment");
    CHECK(cwist_proxy_path_is_safe("/a/b%20c"), "percent escape");
    CHECK(cwist_proxy_path_is_safe("/a/..."), "three dots is a name");
    CHECK(!cwist_proxy_path_is_safe("/api/../admin"), "dot-dot");
    CHECK(!cwist_proxy_path_is_safe("/api/.."), "trailing dot-dot");
    CHECK(!cwist_proxy_path_is_safe("/api/./x"), "single dot");
    CHECK(!cwist_proxy_path_is_safe("/api/%2e%2E/admin"), "encoded dot-dot");
    CHECK(!cwist_proxy_path_is_safe("/api/.%2e"), "mixed dot-dot");
    CHECK(!cwist_proxy_path_is_safe("/api%2fadmin"), "encoded slash");
    CHECK(!cwist_proxy_path_is_safe("/api%5Cadmin"), "encoded backslash");
    CHECK(!cwist_proxy_path_is_safe("/api\\admin"), "backslash");
    CHECK(!cwist_proxy_path_is_safe("/a%00b"), "encoded NUL");
    CHECK(!cwist_proxy_path_is_safe("/a%zz"), "bad escape");
    CHECK(!cwist_proxy_path_is_safe("/a%4"), "truncated escape");
    CHECK(!cwist_proxy_path_is_safe("/a b"), "space");
    CHECK(!cwist_proxy_path_is_safe("http://evil/"), "absolute form");
    CHECK(!cwist_proxy_path_is_safe(""), "empty");
    CHECK(cwist_proxy_query_is_safe("a=1&b=%20"), "query ok");
    CHECK(!cwist_proxy_query_is_safe("a=1\r\nX: y"), "query CRLF");
    CHECK(!cwist_proxy_query_is_safe("a=%g1"), "query bad escape");
}

static void test_header_validation(void) {
    printf("header validation\n");
    CHECK(cwist_proxy_header_name_ok("X-Request-Id"), "token name");
    CHECK(!cwist_proxy_header_name_ok("X Bad"), "space in name");
    CHECK(!cwist_proxy_header_name_ok("X:Bad"), "colon in name");
    CHECK(!cwist_proxy_header_name_ok(""), "empty name");
    CHECK(cwist_proxy_header_value_ok("a\tb c"), "htab and space");
    CHECK(cwist_proxy_header_value_ok("caf\xc3\xa9"), "obs-text");
    CHECK(!cwist_proxy_header_value_ok("a\r\nX-Injected: 1"), "CRLF");
    CHECK(!cwist_proxy_header_value_ok("a\nb"), "LF");
    CHECK(!cwist_proxy_header_value_ok("a\x01"), "control byte");
}

static int url_is(char *url, const char *want) {
    int ok = url && strcmp(url, want) == 0;
    if (!ok) printf("       got %s, want %s\n", url ? url : "(null)", want ? want : "(null)");
    cwist_free(url);
    return ok;
}

static int miss_is(unsigned short port, const char *host, const char *path, int want) {
    int status = 0;
    char *url = cwist_proxy_resolve_url(port, host, path, NULL, &status);
    int ok = !url && status == want;
    if (!ok) printf("       got %s/%d, want NULL/%d\n", url ? url : "(null)", status, want);
    cwist_free(url);
    return ok;
}

static void test_body_grow(void) {
    printf("buffered body growth\n");
    const size_t K = 1024, M = 1024 * 1024, MAX = 64 * M;
    CHECK(cwist_proxy_body_grow(0, 1, MAX) == 4 * K, "first chunk starts at 4 KiB");
    CHECK(cwist_proxy_body_grow(8 * K, 6 * K, MAX) == 8 * K, "fitting buffer is kept");
    CHECK(cwist_proxy_body_grow(64 * K, 64 * K + 1, MAX) == 96 * K, "small bodies grow x1.5");
    CHECK(cwist_proxy_body_grow(2 * M, 2 * M + 1, MAX) == 4 * M, "mid-size bodies grow x2");
    CHECK(cwist_proxy_body_grow(8 * M, 8 * M + 1, MAX) == 32 * M, "large bodies grow x4");
    CHECK(cwist_proxy_body_grow(32 * M, 32 * M + 1, MAX) == MAX, "growth clamps to the cap");
    CHECK(cwist_proxy_body_grow(MAX, MAX + 1, MAX) == 0, "over the cap is refused");
    CHECK(cwist_proxy_body_grow(0, 10 * M, MAX) >= 10 * M, "one large chunk fits at once");
    size_t cap = 0, steps = 0;
    for (size_t need = 1; need <= MAX; need = cap + 1) {
        cap = cwist_proxy_body_grow(cap, need, MAX);
        steps++;
        if (cap == MAX) break;
    }
    CHECK(cap == MAX && steps < 24, "reaching 64 MiB takes few reallocs");
    printf("       %zu growth steps to 64 MiB\n", steps);
}

static void test_rules(void) {
    printf("rule registration and resolution\n");
    cwist_proxy_reset();

    cwist_error_t e =
        cwist_proxy_route("http://api.example.com:8080", "/api", "http://127.0.0.1:3000", "/v1");
    CHECK(cwist_error_is_ok(&e), "origin URL with port");
    e = cwist_proxy_route("http://API.Example.com:8080/", "/api/admin/", "127.0.0.1:3001", "/");
    CHECK(cwist_error_is_ok(&e), "same origin, longer prefix, bare target");
    e = cwist_proxy_route("http://localhost:8080", "/", "https://backend.internal:8443", "/x/");
    CHECK(cwist_error_is_ok(&e), "second origin on the same port");
    e = cwist_proxy_route("http://plain.example", "/", "http://127.0.0.1:3003", "/");
    CHECK(cwist_error_is_ok(&e), "default port 80");
    e = cwist_proxy_port_forward(9090, 4000);
    CHECK(cwist_error_is_ok(&e), "port forward");

    e = cwist_proxy_route("http://api.example.com:8080", "/api/", "http://127.0.0.1:3002", "/");
    CHECK(!cwist_error_is_ok(&e), "duplicate rule rejected");
    e = cwist_proxy_route("https://api.example.com:8080", "/tls", "http://127.0.0.1:1", "/");
    CHECK(!cwist_error_is_ok(&e), "http and https origins on one port rejected");
    e = cwist_proxy_route(":8080", "/", "http://127.0.0.1:1", "/");
    CHECK(!cwist_error_is_ok(&e), "host-less origin rejected");
    e = cwist_proxy_route("api.example.com:8080", "/", "http://127.0.0.1:1", "/");
    CHECK(!cwist_error_is_ok(&e), "scheme-less origin rejected");
    e = cwist_proxy_route("http://api.example.com:8080/app", "/", "http://127.0.0.1:1", "/");
    CHECK(!cwist_error_is_ok(&e), "origin with path rejected");
    e = cwist_proxy_route("http://api.example.com:0", "/", "http://127.0.0.1:1", "/");
    CHECK(!cwist_error_is_ok(&e), "port 0 rejected");
    e = cwist_proxy_route("http://a.test:8080", "api", "http://127.0.0.1:1", "/");
    CHECK(!cwist_error_is_ok(&e), "relative route rejected");
    e = cwist_proxy_route("http://a.test:8080", "/b", "http://127.0.0.1:1/path", "/");
    CHECK(!cwist_error_is_ok(&e), "target with path rejected");
    e = cwist_proxy_route("http://a.test:8080", "/b", "http://user@127.0.0.1:1", "/");
    CHECK(!cwist_error_is_ok(&e), "target with userinfo rejected");
    e = cwist_proxy_route("http://a.test:8080", "/b", "ftp://127.0.0.1:1", "/");
    CHECK(!cwist_error_is_ok(&e), "non-http scheme rejected");
    e = cwist_proxy_route("http://a.test:8080", "/b/../c", "http://127.0.0.1:1", "/");
    CHECK(!cwist_error_is_ok(&e), "unsafe origin route rejected");

    CHECK(url_is(cwist_proxy_resolve_url(8080, "api.example.com:8080", "/api/users", "x=1", NULL),
                 "http://127.0.0.1:3000/v1/users?x=1"),
          "prefix rewrite with query");
    CHECK(url_is(cwist_proxy_resolve_url(8080, "API.EXAMPLE.COM:8080", "/api", NULL, NULL),
                 "http://127.0.0.1:3000/v1"),
          "host is case-insensitive");
    CHECK(url_is(cwist_proxy_resolve_url(8080, "api.example.com:8080", "/api/", NULL, NULL),
                 "http://127.0.0.1:3000/v1/"),
          "prefix with trailing slash");
    CHECK(url_is(
              cwist_proxy_resolve_url(8080, "api.example.com:8080", "/api/admin/users", NULL, NULL),
              "http://127.0.0.1:3001/users"),
          "longest prefix wins");
    CHECK(url_is(cwist_proxy_resolve_url(8080, "localhost:8080", "/api/a", NULL, NULL),
                 "https://backend.internal:8443/x/api/a"),
          "each origin keeps its own routes");
    CHECK(url_is(cwist_proxy_resolve_url(80, "plain.example", "/p", NULL, NULL),
                 "http://127.0.0.1:3003/p"),
          "Host without port means the scheme default");
    CHECK(url_is(cwist_proxy_resolve_url(9090, "anything:9090", "/any/path", "q", NULL),
                 "http://127.0.0.1:4000/any/path?q"),
          "port forward ignores Host");
    CHECK(miss_is(8080, "api.example.com:8080", "/apix", CWIST_HTTP_NOT_FOUND),
          "origin matches, route does not -> 404");
    CHECK(miss_is(8080, "127.0.0.1:8080", "/api", CWIST_HTTP_MISDIRECTED_REQUEST),
          "127.0.0.1 is not localhost -> 421");
    CHECK(miss_is(8080, "api.example.com", "/api", CWIST_HTTP_MISDIRECTED_REQUEST),
          "Host without port means :80, not :8080 -> 421");
    CHECK(miss_is(8080, "api.example.com:9999", "/api", CWIST_HTTP_MISDIRECTED_REQUEST),
          "different Host port -> 421");
    CHECK(miss_is(8080, NULL, "/api", CWIST_HTTP_MISDIRECTED_REQUEST), "missing Host -> 421");
    CHECK(miss_is(8080, "bad host", "/api", CWIST_HTTP_MISDIRECTED_REQUEST),
          "malformed Host -> 421");
    CHECK(miss_is(8080, "api.example.com:8080", "/api/../x", CWIST_HTTP_BAD_REQUEST),
          "unsafe path -> 400");

    CHECK(cwist_proxy.http_auto_promote == false && cwist_proxy.timeout_ms == 30000, "defaults");
    cwist_proxy.h3_enabled = true;
    cwist_proxy_reset();
    CHECK(cwist_proxy.h3_enabled == false, "reset restores defaults");
    CHECK(cwist_proxy_resolve_url(9090, NULL, "/", NULL, NULL) == NULL, "reset drops rules");
    CHECK(cwist_proxy_listen() == -1, "listen without rules fails");

    cwist_proxy_route("https://secure.test:8443", "/", "http://127.0.0.1:1", "/");
    CHECK(cwist_proxy_listen() == -1, "https origin without certificate fails");
    cwist_proxy_reset();
    cwist_proxy_route("http://a.test:8081", "/", "http://127.0.0.1:1", "/");
    cwist_proxy_route("https://b.test:8444", "/", "http://127.0.0.1:1", "/");
    cwist_proxy.cert_path = "x.pem";
    cwist_proxy.key_path = "x.key";
    CHECK(cwist_proxy_listen() == -1, "mixed schemes across ports fail");
    cwist_proxy_reset();
}

/* --- End-to-end ------------------------------------------------------------------ */

static const char *hdr(cwist_http_request *req, const char *k) {
    const char *v = cwist_http_header_get(req->headers, k);
    return v ? v : "-";
}

static void upstream_echo(cwist_http_request *req, cwist_http_response *res) {
    char buf[2048];
    cwist_tls_info ti;
    char tls[48] = "tls=none";
    if (cwist_http_request_tls_info(req, &ti)) {
        snprintf(tls, sizeof(tls), "tls=%04x group=%04x", ti.version, ti.group);
    }
    snprintf(buf, sizeof(buf),
             "%s method=%s path=%s query=%s host=%s xff=%s xfh=%s xfp=%s secret=%s keepalive=%s "
             "accept=%s ctype=%s body=%s",
             tls, cwist_http_method_to_string(req->method), req->path->data,
             (req->query && req->query->data && req->query->data[0]) ? req->query->data : "-",
             hdr(req, "host"), hdr(req, "x-forwarded-for"), hdr(req, "x-forwarded-host"),
             hdr(req, "x-forwarded-proto"), hdr(req, "x-secret"), hdr(req, "keep-alive"),
             hdr(req, "accept"), hdr(req, "content-type"),
             (req->body && req->body->data && req->body->size) ? req->body->data : "-");
    cwist_http_header_add(&res->headers, "Content-Type", "text/plain");
    cwist_http_header_add(&res->headers, "Connection", "keep-alive, X-Up-Secret");
    cwist_http_header_add(&res->headers, "X-Up-Secret", "leak");
    cwist_http_header_add(&res->headers, "X-Up-Public", "visible");
    cwist_http_header_add(&res->headers, "Alt-Svc", "h3=\":1\"");
    cwist_sstring_assign(res->body, buf);
}

static void upstream_redirect(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    res->status_code = CWIST_HTTP_FOUND;
    cwist_http_header_add(&res->headers, "Location", "/v1/elsewhere");
}

static void upstream_teapot(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    res->status_code = CWIST_HTTP_IM_A_TEAPOT;
    cwist_sstring_assign(res->body, "short and stout");
}

static void upstream_big(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    static char big[200000];
    memset(big, 'z', sizeof(big));
    cwist_http_response_set_body_ptr(res, big, sizeof(big));
}

/* Each server runs in its own process group so stop_group() also reaches the
 * worker processes cwist_app_listen() forks (CWIST_WORKERS stays at its
 * default, keeping the multi-worker path under test). */
static pid_t spawn(void (*fn)(void)) {
    pid_t pid = fork();
    if (pid == 0) {
        setpgid(0, 0);
#if defined(__SANITIZE_THREAD__)
        setenv("CWIST_WORKERS", "1", 1);
#endif
        fn();
        _exit(0);
    }
    setpgid(pid, pid);
    return pid;
}

static void stop_group(pid_t pgid) {
    kill(-pgid, SIGTERM);
    waitpid(pgid, NULL, 0);
    /* Workers are grandchildren: wait until the whole group is gone so a
     * draining listener cannot join the next run's SO_REUSEPORT group. */
    for (int i = 0; i < 200 && kill(-pgid, 0) == 0; i++) usleep(50000);
    if (kill(-pgid, 0) == 0) kill(-pgid, SIGKILL);
}

static void run_upstream(void) {
    cwist_app *app = cwist_app_create();
    cwist_app_get(app, "/v1/echo", upstream_echo);
    cwist_app_post(app, "/v1/echo", upstream_echo);
    cwist_app_put(app, "/v1/echo", upstream_echo);
    cwist_app_get(app, "/v1/redirect", upstream_redirect);
    cwist_app_get(app, "/v1/teapot", upstream_teapot);
    cwist_app_get(app, "/v1/big", upstream_big);
    cwist_app_get(app, "/fwd/echo", upstream_echo);
    cwist_app_listen_ex(app, UPSTREAM_PORT, 1, -1);
}

/* TLS upstreams: both TLS 1.3 (PQC layer), one with hybrid PQ groups, one
 * restricted to classical groups. */
static void run_tls_upstream(int port, const char *groups) {
    cwist_app *app = cwist_app_create();
    cwist_app_use_pqc_layer(app, true);
    cwist_app_set_tls_groups(app, groups);
    cwist_error_t e = cwist_app_use_https(app, g_cert, g_key);
    if (!cwist_error_is_ok(&e)) _exit(4);
    cwist_app_get(app, "/echo", upstream_echo);
    cwist_app_listen_ex(app, port, 1, -1);
}

static void run_up_pq(void) {
    run_tls_upstream(UP_PQ_PORT, "X25519MLKEM768:X25519");
}

static void run_up_classic(void) {
    run_tls_upstream(UP_CLASSIC_PORT, "X25519:P-256");
}

static void add_tls_routes(const char *origin) {
    cwist_proxy_route(origin, "/pq", "https://localhost:19971", "/");
    cwist_proxy_route(origin, "/classic", "https://localhost:19972", "/");
    cwist_proxy_route(origin, "/plain", "http://127.0.0.1:19961", "/fwd");
    cwist_proxy.cert_path = g_cert;
    cwist_proxy.key_path = g_key;
    cwist_proxy.upstream_ca_path = g_cert; /* self-signed: the cert is its own CA */
    cwist_proxy.h2_enabled = true;
    cwist_proxy.h3_enabled = true;
}

static void run_tls_mirror_proxy(void) {
    add_tls_routes("https://localhost:19974");
    _exit(cwist_proxy_listen() == 0 ? 0 : 3);
}

static void run_tls_promote_proxy(void) {
    add_tls_routes("https://localhost:19975");
    cwist_proxy.tls_auto_promote = true;
    _exit(cwist_proxy_listen() == 0 ? 0 : 3);
}

static void run_single_port_proxy(void) {
    cwist_proxy_route("http://front.test:19962", "/api", "http://127.0.0.1:19961", "/v1");
    cwist_proxy_route("http://front.test:19962", "/dead", "http://127.0.0.1:19965", "/");
    cwist_proxy.max_response_bytes = 100000;
    cwist_proxy.workers = 2;
    _exit(cwist_proxy_listen() == 0 ? 0 : 3);
}

static void run_multiport_proxy(void) {
    cwist_proxy_port_forward(FWD_PORT, UPSTREAM_PORT);
    cwist_proxy_route("http://only.test:19964", "/", "http://127.0.0.1:19961", "/fwd");
    cwist_proxy.http_auto_promote = true;
    cwist_proxy.h3_enabled = true; /* ephemeral HTTP/3 next to cleartext TCP */
    _exit(cwist_proxy_listen() == 0 ? 0 : 3);
}

static int connect_port(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((unsigned short)port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr *)&a, sizeof(a)) != 0) {
        close(fd);
        return -1;
    }
    struct timeval tv = {.tv_sec = 10, .tv_usec = 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    return fd;
}

static int wait_port(int port) {
    for (int i = 0; i < 100; i++) {
        int fd = connect_port(port);
        if (fd >= 0) {
            close(fd);
            return 0;
        }
        usleep(50000);
    }
    return -1;
}

/* Send @p raw and read until the peer closes; returns heap response or NULL. */
static char *exchange(int port, const char *raw) {
    int fd = connect_port(port);
    if (fd < 0) return NULL;
    size_t len = strlen(raw);
    if (write(fd, raw, len) != (ssize_t)len) {
        close(fd);
        return NULL;
    }
    size_t cap = 65536, used = 0;
    char *buf = malloc(cap);
    for (;;) {
        if (used + 4096 > cap) {
            cap *= 2;
            buf = realloc(buf, cap);
        }
        ssize_t n = read(fd, buf + used, cap - used - 1);
        if (n <= 0) break;
        used += (size_t)n;
    }
    buf[used] = '\0';
    close(fd);
    return buf;
}

static int has(const char *resp, const char *needle) {
    return resp && strstr(resp, needle) != NULL;
}

static int status_is(const char *resp, int code) {
    char want[16];
    snprintf(want, sizeof(want), " %d ", code);
    return resp && strncmp(resp, "HTTP/1.1", 8) == 0 && strncmp(resp + 8, want, 5) == 0;
}

static int has_header_ci(const char *resp, const char *name) {
    if (!resp) return 0;
    const char *end = strstr(resp, "\r\n\r\n");
    size_t n = strlen(name);
    for (const char *p = strstr(resp, "\r\n"); p && (!end || p < end); p = strstr(p + 2, "\r\n")) {
        if (strncasecmp(p + 2, name, n) == 0 && p[2 + n] == ':') return 1;
    }
    return 0;
}

static void test_end_to_end(void) {
    printf("end-to-end (single port)\n");
    char *r;

    r = exchange(PROXY_PORT, "GET /api/echo?a=1 HTTP/1.1\r\nHost: front.test:19962\r\n"
                             "X-Forwarded-For: 6.6.6.6\r\nX-Real-IP: 6.6.6.6\r\n"
                             "Connection: close, X-Secret\r\nX-Secret: s3cr3t\r\n"
                             "Keep-Alive: timeout=99\r\n\r\n");
    CHECK(status_is(r, 200), "GET forwarded");
    CHECK(has(r, "method=GET path=/v1/echo query=a=1"), "path rewritten, query kept");
    CHECK(has(r, "host=127.0.0.1:19961"), "Host set to target authority");
    CHECK(has(r, "xff=127.0.0.1 "), "X-Forwarded-For overwritten");
    CHECK(has(r, "xfh=front.test:19962"), "X-Forwarded-Host");
    CHECK(has(r, "xfp=http"), "X-Forwarded-Proto");
    CHECK(has(r, "secret=-"), "Connection-listed request header dropped");
    CHECK(has(r, "keepalive=-"), "hop-by-hop request header dropped");
    CHECK(has(r, "accept=-"), "no Accept invented");
    CHECK(has(r, "X-Up-Public: visible"), "end-to-end response header kept");
    CHECK(!has_header_ci(r, "X-Up-Secret"), "Connection-listed response header dropped");
    CHECK(!has_header_ci(r, "Alt-Svc"), "upstream Alt-Svc dropped, h3 off");
    free(r);

    r = exchange(PROXY_PORT,
                 "POST /api/echo HTTP/1.1\r\nHost: front.test:19962\r\nConnection: close\r\n"
                 "Content-Type: text/plain\r\nContent-Length: 11\r\n\r\nhello proxy");
    CHECK(status_is(r, 200) && has(r, "method=POST") && has(r, "body=hello proxy") &&
              has(r, "ctype=text/plain"),
          "POST body and Content-Type forwarded");
    free(r);

    r = exchange(
        PROXY_PORT,
        "GET /api/redirect HTTP/1.1\r\nHost: front.test:19962\r\nConnection: close\r\n\r\n");
    CHECK(status_is(r, 302) && has(r, "Location: /v1/elsewhere"),
          "redirect returned, not followed");
    free(r);

    r = exchange(PROXY_PORT,
                 "GET /api/teapot HTTP/1.1\r\nHost: front.test:19962\r\nConnection: close\r\n\r\n");
    CHECK(status_is(r, 418) && has(r, "short and stout"), "upstream status passed through");
    free(r);

    r = exchange(PROXY_PORT,
                 "GET /api/big HTTP/1.1\r\nHost: front.test:19962\r\nConnection: close\r\n\r\n");
    CHECK(status_is(r, 502), "oversized upstream body -> 502");
    free(r);

    r = exchange(PROXY_PORT,
                 "GET /dead/x HTTP/1.1\r\nHost: front.test:19962\r\nConnection: close\r\n\r\n");
    CHECK(status_is(r, 502), "unreachable upstream -> 502");
    free(r);

    r = exchange(
        PROXY_PORT,
        "GET /api/../admin HTTP/1.1\r\nHost: front.test:19962\r\nConnection: close\r\n\r\n");
    CHECK(status_is(r, 400), "dot-dot path -> 400");
    free(r);

    r = exchange(PROXY_PORT,
                 "GET /api%2fecho HTTP/1.1\r\nHost: front.test:19962\r\nConnection: close\r\n\r\n");
    CHECK(status_is(r, 400), "encoded slash -> 400");
    free(r);

    r = exchange(PROXY_PORT,
                 "GET /other HTTP/1.1\r\nHost: front.test:19962\r\nConnection: close\r\n\r\n");
    CHECK(status_is(r, 404), "unmatched path -> 404");
    free(r);

    r = exchange(PROXY_PORT,
                 "GET /api/echo HTTP/1.1\r\nHost: 127.0.0.1:19962\r\nConnection: close\r\n\r\n");
    CHECK(status_is(r, 421), "same port, other Host -> 421");
    free(r);

    r = exchange(PROXY_PORT,
                 "GET /api/echo HTTP/1.1\r\nHost: front.test:19962\r\nConnection: Upgrade\r\n"
                 "Upgrade: websocket\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                 "Sec-WebSocket-Version: 13\r\n\r\n");
    CHECK(status_is(r, 501), "WebSocket upgrade -> 501");
    free(r);

    /* Two requests on one keep-alive connection: the deferred path re-arms. */
    r = exchange(
        PROXY_PORT,
        "GET /api/echo?n=1 HTTP/1.1\r\nHost: front.test:19962\r\n\r\n"
        "GET /api/echo?n=2 HTTP/1.1\r\nHost: front.test:19962\r\nConnection: close\r\n\r\n");
    if (!(has(r, "query=n=1") && has(r, "query=n=2")))
        printf("       response: %s\n", r ? r : "(null)");
    CHECK(has(r, "query=n=1") && has(r, "query=n=2"), "pipelined keep-alive requests");
    CHECK(r && strstr(r, "query=n=1") < strstr(r, "query=n=2"), "responses stay in order");
    free(r);

    printf("end-to-end (multiport, port forward, auto promote)\n");
    r = exchange(FWD_PORT, "GET /fwd/echo HTTP/1.1\r\nHost: anything\r\nConnection: close\r\n\r\n");
    CHECK(status_is(r, 200) && has(r, "path=/fwd/echo"), "port forward keeps the path");
    CHECK(has(r, "Alt-Svc: h3=\":19963\""), "auto promote advertises h3 on the origin port");
    free(r);

    r = exchange(FWD_PORT2,
                 "GET /echo HTTP/1.1\r\nHost: only.test:19964\r\nConnection: close\r\n\r\n");
    CHECK(status_is(r, 200) && has(r, "path=/fwd/echo"), "virtual host rule on second port");
    free(r);

    r = exchange(FWD_PORT2,
                 "GET /echo HTTP/1.1\r\nHost: other.test:19964\r\nConnection: close\r\n\r\n");
    CHECK(status_is(r, 421), "Host naming no origin -> 421");
    free(r);
}

/* --- TLS level handling ------------------------------------------------------ */

typedef struct {
    char *data;
    size_t len;
} buf_t;

static size_t buf_write(void *p, size_t sz, size_t n, void *u) {
    buf_t *b = (buf_t *)u;
    size_t add = sz * n;
    char *grown = realloc(b->data, b->len + add + 1);
    if (!grown) return 0;
    b->data = grown;
    memcpy(b->data + b->len, p, add);
    b->len += add;
    b->data[b->len] = '\0';
    return add;
}

/* GET https://localhost:<port><path> offering only @p groups; returns the
 * status (or -1) and the body in *body (caller frees). */
static long tls_get(int port, const char *path, const char *groups, bool h2, char **body) {
    CURL *c = curl_easy_init();
    char url[128];
    snprintf(url, sizeof(url), "https://localhost:%d%s", port, path);
    buf_t b = {0};
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_CAINFO, g_cert);
    curl_easy_setopt(c, CURLOPT_SSL_EC_CURVES, groups);
    curl_easy_setopt(c, CURLOPT_SSLVERSION, (long)CURL_SSLVERSION_TLSv1_3);
    curl_easy_setopt(c, CURLOPT_HTTP_VERSION,
                     (long)(h2 ? CURL_HTTP_VERSION_2TLS : CURL_HTTP_VERSION_1_1));
    curl_easy_setopt(c, CURLOPT_TIMEOUT_MS, 10000L);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, buf_write);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &b);
    long status = -1;
    if (curl_easy_perform(c) == CURLE_OK) curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
    long ver = 0;
    curl_easy_getinfo(c, CURLINFO_HTTP_VERSION, &ver);
    if (h2 && status > 0 && ver != CURL_HTTP_VERSION_2_0) status = -2; /* not h2 */
    curl_easy_cleanup(c);
    *body = b.data;
    return status;
}

#define PQ_GROUP "group=11ec" /* X25519MLKEM768 */
#define X25519_GROUP "group=001d"

static bool tls_pq_supported(void) {
    char *b = NULL;
    long st = tls_get(UP_PQ_PORT, "/echo", "X25519MLKEM768", false, &b);
    bool supported = st == 200 && has(b, PQ_GROUP);
    free(b);
    if (!supported) printf("  skip PQ assertions: libcurl cannot negotiate X25519MLKEM768\n");
    return supported;
}

static void test_tls_levels(void) {
    char *b = NULL;
    long st;
    bool pq = tls_pq_supported();

    printf("TLS level mirroring (no promotion)\n");
    if (pq) {
        st = tls_get(TLS_MIRROR_PORT, "/pq/echo", "X25519MLKEM768", false, &b);
        CHECK(st == 200 && has(b, "tls=0304") && has(b, PQ_GROUP),
              "PQ client -> PQ upstream keeps hybrid key exchange");
        free(b);
        st = tls_get(TLS_MIRROR_PORT, "/classic/echo", "X25519MLKEM768", false, &b);
        CHECK(st == 502, "PQ client -> classical-only upstream refused (no silent downgrade)");
        free(b);
    }
    st = tls_get(TLS_MIRROR_PORT, "/classic/echo", "X25519", false, &b);
    CHECK(st == 200 && has(b, "tls=0304") && has(b, X25519_GROUP),
          "classical client -> classical upstream allowed");
    free(b);
    if (pq) {
        st = tls_get(TLS_MIRROR_PORT, "/pq/echo", "X25519MLKEM768", true, &b);
        CHECK(st == 200 && has(b, PQ_GROUP), "same over HTTP/2 (TLS level read from the h2 conn)");
        free(b);
        st = tls_get(TLS_MIRROR_PORT, "/classic/echo", "X25519MLKEM768", true, &b);
        CHECK(st == 502, "HTTP/2 PQ client -> classical-only upstream refused");
        free(b);
    }
    st = tls_get(TLS_MIRROR_PORT, "/plain/echo", "X25519", false, &b);
    CHECK(st == 200 && has(b, "tls=none") && has(b, "xfp=https"),
          "http:// target: TLS stripped, X-Forwarded-Proto https");
    free(b);

    printf("TLS auto promotion\n");
    if (pq) {
        st = tls_get(TLS_PROMOTE_PORT, "/pq/echo", "X25519", false, &b);
        CHECK(st == 200 && has(b, PQ_GROUP), "classical client promoted to PQ upstream");
        free(b);
    }
    /* Holds without hybrid support too: the upstream leg then fails either way. */
    st = tls_get(TLS_PROMOTE_PORT, "/classic/echo", "X25519", false, &b);
    CHECK(st == 502, "promotion refuses an upstream without PQ key exchange");
    free(b);
    st = tls_get(TLS_PROMOTE_PORT, "/plain/echo", "X25519", false, &b);
    CHECK(st == 200 && has(b, "tls=none"), "promotion still strips TLS for http:// targets");
    free(b);
    if (pq) {
        st = tls_get(TLS_PROMOTE_PORT, "/pq/echo", "X25519MLKEM768", true, &b);
        CHECK(st == 200 && has(b, PQ_GROUP), "front accepts PQ clients over HTTP/2");
        free(b);
    }
}

/* --- HTTP/3 front ------------------------------------------------------------ */

static int h3_get(int port, const char *authority_host, const char *path, char *out, size_t cap) {
    cwist_http3_client *c = cwist_http3_client_create();
    cwist_http3_client_set_insecure(c, 1);
    cwist_http3_client_set_server(c, authority_host, (uint16_t)port);
    cwist_http3_client_set_timeout_ms(c, 5000);
    cwist_http3_client_set_conn_timeout_ms(c, 5000);
    cwist_http_response *res = NULL;
    int status = -1;
    out[0] = '\0';
    cwist_error_t e = cwist_http3_client_request(c, path, CWIST_HTTP_GET, NULL, NULL, 0, &res);
    if (cwist_error_is_ok(&e) && res) {
        status = (int)res->status_code;
        if (res->body && res->body->data) {
            snprintf(out, cap, "%.*s", (int)res->body->size, res->body->data);
        }
        cwist_http_response_destroy(res);
    }
    cwist_http3_client_destroy(c);
    return status;
}

static void test_h3_front(void) {
    printf("HTTP/3 front (deferred through the engine queue)\n");
    char body[2048];
    int st = h3_get(FWD_PORT, "127.0.0.1", "/fwd/echo?via=h3", body, sizeof(body));
    CHECK(st == 200 && has(body, "path=/fwd/echo query=via=h3"), "port forward over HTTP/3");
    st = h3_get(TLS_MIRROR_PORT, "localhost", "/plain/echo", body, sizeof(body));
    CHECK(st == 200 && has(body, "xfp=https"), "https origin over HTTP/3, TLS stripped");
    st = h3_get(TLS_MIRROR_PORT, "localhost", "/pq/echo", body, sizeof(body));
    CHECK(st == 200 && has(body, "tls=0304"), "HTTP/3 -> https upstream at TLS 1.3");
}

static int make_cert(void) {
    static char dir[] = "/tmp/cwist-proxy-test-XXXXXX";
    if (!mkdtemp(dir)) return -1;
    snprintf(g_cert, sizeof(g_cert), "%s/cert.pem", dir);
    snprintf(g_key, sizeof(g_key), "%s/key.pem", dir);
    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
             "openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:P-256 -nodes -days 2 "
             "-keyout %s -out %s -subj /CN=localhost "
             "-addext subjectAltName=DNS:localhost,IP:127.0.0.1 >/dev/null 2>&1",
             g_key, g_cert);
    return system(cmd) == 0 ? 0 : -1;
}

static void remove_cert(void) {
    unlink(g_cert);
    unlink(g_key);
    char *slash = strrchr(g_cert, '/');
    if (slash) {
        *slash = '\0';
        rmdir(g_cert);
    }
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    setvbuf(stdout, NULL, _IONBF, 0);
    test_path_safety();
    test_header_validation();
    test_body_grow();
    test_rules();

    if (make_cert() != 0) {
        printf("  FAIL could not create a test certificate (openssl CLI)\n");
        return 1;
    }
    curl_global_init(CURL_GLOBAL_DEFAULT);

    pid_t up = spawn(run_upstream);
    pid_t px = spawn(run_single_port_proxy);
    pid_t mp = spawn(run_multiport_proxy);
    pid_t upq = spawn(run_up_pq);
    pid_t ucl = spawn(run_up_classic);
    pid_t tm = spawn(run_tls_mirror_proxy);
    pid_t tp = spawn(run_tls_promote_proxy);
    if (wait_port(UPSTREAM_PORT) != 0 || wait_port(PROXY_PORT) != 0 || wait_port(FWD_PORT) != 0 ||
        wait_port(FWD_PORT2) != 0 || wait_port(UP_PQ_PORT) != 0 ||
        wait_port(UP_CLASSIC_PORT) != 0 || wait_port(TLS_MIRROR_PORT) != 0 ||
        wait_port(TLS_PROMOTE_PORT) != 0) {
        printf("  FAIL servers did not start\n");
        g_failures++;
    } else {
        test_end_to_end();
        test_tls_levels();
        test_h3_front();
    }
    stop_group(tp);
    stop_group(tm);
    stop_group(ucl);
    stop_group(upq);
    stop_group(px);
    stop_group(mp);
    stop_group(up);
    curl_global_cleanup();
    remove_cert();

    if (g_failures) {
        printf("test_proxy: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("test_proxy: all passed\n");
    return 0;
}
