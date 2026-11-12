/**
 * @file test_bdr_privacy.c
 * @brief Big Dumb Reply must not share caller-specific replies (issue #706).
 *
 * Runs a real server in classic mode and in C1M mode and checks, over the
 * wire, that:
 *  - a reply learned for one Cookie / Authorization is never served to
 *    another caller of the same path;
 *  - responses carrying Set-Cookie or Cache-Control private / no-store /
 *    no-cache are never learned;
 *  - a handler faster than latency_threshold_ms is never learned, whatever
 *    connection it arrives on;
 *  - an ordinary slow GET is still learned (the cache stays useful, and the
 *    checks above are not vacuous).
 *
 * Each handler counts its invocations; GET /count?p=<path> (a query string,
 * so never cached) reports the count for a path.
 *
 * The server runs once per row of a strength-2 orthogonal array over the
 * runtime knobs that pick different serving paths (classic vs C1M, io_uring
 * vs epoll, receive mode, thread count, batch size). Every pair of knob
 * values therefore meets at least once, which is what exposes a check that
 * one path has and another lacks -- #706 was a classic-only leak. Thread
 * counts go up to 64 to cover wide hosts.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE /* strcasestr */
#endif
#include <cwist/sys/app/app.h>
#include "../src/core/design/latin_square.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define TEST_HOST "127.0.0.1"

static int g_port;

enum { H_WHO, H_SETCOOKIE, H_PRIVATE, H_NOSTORE, H_NOCACHE, H_FAST, H_PUBLIC, H_COUNT };
static _Atomic int g_hits[H_COUNT];
static const char *const g_paths[H_COUNT] = {"/who",    "/setcookie", "/private", "/nostore",
                                             "/nocache", "/fast",     "/public"};

static void sleep_ms(long ms) {
    struct timespec d = {ms / 1000, (ms % 1000) * 1000000L};
    nanosleep(&d, NULL);
}

/* Slower than the default 10 ms learning threshold. */
#define SLOW_MS 30

static void who_handler(cwist_http_request *req, cwist_http_response *res) {
    atomic_fetch_add(&g_hits[H_WHO], 1);
    sleep_ms(SLOW_MS);
    const char *cookie = cwist_http_header_get(req->headers, "Cookie");
    const char *auth = cwist_http_header_get(req->headers, "Authorization");
    char body[256];
    snprintf(body, sizeof(body), "who=%s", cookie ? cookie : auth ? auth : "anon");
    cwist_sstring_assign(res->body, body);
}

static void setcookie_handler(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    atomic_fetch_add(&g_hits[H_SETCOOKIE], 1);
    sleep_ms(SLOW_MS);
    cwist_http_header_add(&res->headers, "Set-Cookie", "sid=first-caller");
    cwist_sstring_assign(res->body, "welcome");
}

static void cc_reply(cwist_http_response *res, int slot, const char *cc) {
    atomic_fetch_add(&g_hits[slot], 1);
    sleep_ms(SLOW_MS);
    cwist_http_header_add(&res->headers, "Cache-Control", cc);
    cwist_sstring_assign(res->body, "per-user");
}

static void private_handler(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    cc_reply(res, H_PRIVATE, "max-age=60, Private");
}

static void nostore_handler(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    cc_reply(res, H_NOSTORE, "no-store");
}

static void nocache_handler(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    cc_reply(res, H_NOCACHE, "no-cache=\"Set-Cookie\"");
}

static void fast_handler(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    atomic_fetch_add(&g_hits[H_FAST], 1);
    /* Well under the 10 ms threshold, but over the 0-4 ms threshold the old
     * fd-hash weighting could produce. */
    sleep_ms(4);
    cwist_sstring_assign(res->body, "fast");
}

static void public_handler(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    atomic_fetch_add(&g_hits[H_PUBLIC], 1);
    sleep_ms(SLOW_MS);
    cwist_sstring_assign(res->body, "public");
}

static void count_handler(cwist_http_request *req, cwist_http_response *res) {
    const char *p = cwist_query_map_get(req->query_params, "p");
    int n = -1;
    for (int i = 0; p && i < H_COUNT; i++) {
        if (strcmp(p, g_paths[i]) == 0) n = atomic_load(&g_hits[i]);
    }
    char body[32];
    snprintf(body, sizeof(body), "%d", n);
    cwist_sstring_assign(res->body, body);
}

/* One request per connection; returns the body (or NULL) in @p buf. */
static const char *fetch(const char *path, const char *extra_headers, char *buf, size_t cap) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return NULL;
    struct timeval timeout = {5, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons((uint16_t)g_port)};
    inet_pton(AF_INET, TEST_HOST, &addr.sin_addr);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return NULL;
    }
    char req[512];
    int len = snprintf(req, sizeof(req),
                       "GET %s HTTP/1.1\r\nHost: localhost\r\n%sConnection: close\r\n\r\n", path,
                       extra_headers ? extra_headers : "");
    if (send(fd, req, (size_t)len, 0) != len) {
        close(fd);
        return NULL;
    }
    /* Stop at Content-Length rather than EOF: a reply replayed from the
     * cache carries the connection headers of the reply it was learned
     * from, so the server may keep the socket open despite
     * "Connection: close" (see docs/fixed-cache-status.md). */
    size_t used = 0;
    ssize_t n;
    const char *body = NULL;
    long want = -1;
    while (used + 1 < cap && (n = recv(fd, buf + used, cap - 1 - used, 0)) > 0) {
        used += (size_t)n;
        buf[used] = '\0';
        if (!body && (body = strstr(buf, "\r\n\r\n")) != NULL) {
            body += 4;
            const char *cl = strcasestr(buf, "\r\nContent-Length:");
            if (cl && cl < body) want = strtol(cl + 17, NULL, 10);
        }
        if (body && want >= 0 && (long)(buf + used - body) >= want) break;
    }
    buf[used] = '\0';
    close(fd);
    return body;
}

static int hits(const char *path) {
    char q[128], buf[4096];
    snprintf(q, sizeof(q), "/count?p=%s", path);
    const char *b = fetch(q, NULL, buf, sizeof(buf));
    return b ? atoi(b) : -1;
}

static int failures;
#define CHECK(cond, ...)                                                                           \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "FAIL [%s] ", mode);                                                   \
            fprintf(stderr, __VA_ARGS__);                                                          \
            fputc('\n', stderr);                                                                   \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

/* Runtime knobs varied by the matrix. Each factor has four levels; two-valued
 * knobs repeat their values, which keeps every pair of values covered. */
typedef struct {
    const char *name;
    const char *levels[4];
} knob_t;

static const knob_t k_knobs[] = {
    {"CWIST_C1M_MODE", {"0", "1", "0", "1"}},
    {"CWIST_WORKER_THREADS", {"1", "2", "16", "64"}},
    {"CWIST_REACTOR_BACKEND", {"uring", "epoll", "uring", "epoll"}},
    {"CWIST_HTTP_BATCH", {"1", "4", "16", "1024"}},
    {"CWIST_RX_URING", {"1", "0", "1", "0"}},
};
#define KNOB_COUNT (sizeof(k_knobs) / sizeof(k_knobs[0]))

static void run_row(const cwist_latin_family_t *oa, uint64_t run, int port) {
    char mode[256];
    size_t used = (size_t)snprintf(mode, sizeof(mode), "run %llu:", (unsigned long long)run);
    const char *values[KNOB_COUNT];
    for (uint32_t k = 0; k < KNOB_COUNT; k++) {
        values[k] = k_knobs[k].levels[cwist_latin_oa_level(oa, run, k)];
        if (used < sizeof(mode)) {
            used += (size_t)snprintf(mode + used, sizeof(mode) - used, " %s=%s",
                                     k_knobs[k].name + 6 /* drop "CWIST_" */, values[k]);
        }
    }
    /* Only the classic path learns (the C1M path sends through the
     * coalescing writer and returns before the learn block). */
    bool expect_learning = strcmp(values[0], "0") == 0;
    int failures_before = failures;

    g_port = port;
    pid_t pid = fork();
    if (pid < 0) {
        failures++;
        return;
    }
    if (pid == 0) {
        for (uint32_t k = 0; k < KNOB_COUNT; k++) setenv(k_knobs[k].name, values[k], 1);
        /* Worker processes would each keep their own cache and counters. */
        setenv("CWIST_WORKERS", "1", 1);
        cwist_app *app = cwist_app_create();
        if (!app) _exit(1);
        cwist_app_get(app, "/who", who_handler);
        cwist_app_get(app, "/setcookie", setcookie_handler);
        cwist_app_get(app, "/private", private_handler);
        cwist_app_get(app, "/nostore", nostore_handler);
        cwist_app_get(app, "/nocache", nocache_handler);
        cwist_app_get(app, "/fast", fast_handler);
        cwist_app_get(app, "/public", public_handler);
        cwist_app_get(app, "/count", count_handler);
        int rc = cwist_app_listen(app, port);
        cwist_app_destroy(app);
        _exit(rc == 0 ? 0 : 1);
    }

    char buf[8192];
    const char *b = NULL;
    for (int i = 0; i < 50 && !b; i++) {
        sleep_ms(100);
        b = fetch("/count?p=/who", NULL, buf, sizeof(buf));
    }
    CHECK(b != NULL, "server did not come up");
    if (!b) goto out;

    /* 1. Cookie: alice teaches the path twice (enough to stabilize), bob
     *    must still get his own reply. */
    for (int i = 0; i < 3; i++) fetch("/who", "Cookie: sid=alice\r\n", buf, sizeof(buf));
    b = fetch("/who", "Cookie: sid=bob\r\n", buf, sizeof(buf));
    CHECK(b && strcmp(b, "who=sid=bob") == 0, "Cookie: bob got '%s'", b ? b : "(null)");

    /* 2. Same with Authorization, and an anonymous caller after both. */
    for (int i = 0; i < 3; i++) fetch("/who", "Authorization: Bearer A\r\n", buf, sizeof(buf));
    b = fetch("/who", "Authorization: Bearer B\r\n", buf, sizeof(buf));
    CHECK(b && strcmp(b, "who=Bearer B") == 0, "Authorization: B got '%s'", b ? b : "(null)");
    b = fetch("/who", NULL, buf, sizeof(buf));
    CHECK(b && strcmp(b, "who=anon") == 0, "anonymous got '%s'", b ? b : "(null)");

    /* 3. Responses that set a cookie or forbid shared caching, and a handler
     *    under the latency threshold: every request must reach the handler. */
    static const int slots[] = {H_SETCOOKIE, H_PRIVATE, H_NOSTORE, H_NOCACHE, H_FAST};
    for (size_t s = 0; s < sizeof(slots) / sizeof(slots[0]); s++) {
        const char *path = g_paths[slots[s]];
        for (int i = 0; i < 5; i++) fetch(path, NULL, buf, sizeof(buf));
        int n = hits(path);
        CHECK(n == 5, "%s reached the handler %d/5 times (reply was cached)", path, n);
    }

    /* 4. Control: an ordinary slow GET is learned after two identical
     *    replies and served from the cache afterwards, so the checks above
     *    are not vacuous. */
    for (int i = 0; i < 5; i++) {
        b = fetch("/public", NULL, buf, sizeof(buf));
        CHECK(b && strcmp(b, "public") == 0, "/public got '%s'", b ? b : "(null)");
    }
    if (expect_learning) {
        int n = hits("/public");
        CHECK(n >= 1 && n < 5, "/public reached the handler %d/5 times (expected cache hits)", n);
    }

out:
    kill(pid, SIGTERM);
    int status = 0;
    waitpid(pid, &status, 0);
    if (failures == failures_before) printf("Passed BDR privacy checks (%s)\n", mode);
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    /* Four levels and five factors: OA(16, 5, 4, 2) from three MOLS of
     * order 4 (GF(4)). */
    cwist_latin_family_t *oa = cwist_latin_family_create(4, KNOB_COUNT - 2);
    if (!oa) {
        fprintf(stderr, "no orthogonal array for %zu knobs\n", (size_t)KNOB_COUNT);
        return 1;
    }
    uint64_t runs = cwist_latin_oa_runs(oa);
    for (uint64_t run = 0; run < runs; run++) run_row(oa, run, 19900 + (int)run);
    cwist_latin_family_destroy(oa);
    if (failures) {
        fprintf(stderr, "%d BDR privacy check(s) failed\n", failures);
        return 1;
    }
    printf("All BDR privacy tests passed (%llu runs)!\n", (unsigned long long)runs);
    return 0;
}
