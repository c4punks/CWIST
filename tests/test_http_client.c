/**
 * @file test_http_client.c
 * @brief Loopback tests for cwist_http_client against a real CWIST server.
 *
 * A forked child runs a plain-HTTP CWIST server (classic mode, one worker)
 * on a free loopback port; the parent drives it through
 * cwist_http_client_request(). Failures are counted instead of asserted so
 * the server is always stopped and the client always destroyed.
 */
#define _POSIX_C_SOURCE 200809L
#include <cwist/sys/app/app.h>
#include <cwist/sys/app/shutdown.h>
#include <cwist/net/http/http_client.h>
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/prctl.h>
#endif

#define HELLO_BODY "hello from cwist"
#define REDIRECT_BODY "redirecting"
#define LARGE_POST_LEN 20000
#define CLOSED_PORT_TIMEOUT_MS 2000

static int g_failures = 0;

#define CHECK(cond, ...)                                         \
    do {                                                         \
        if (!(cond)) {                                           \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
            fprintf(stderr, __VA_ARGS__);                        \
            fputc('\n', stderr);                                 \
            g_failures++;                                        \
        }                                                        \
    } while (0)

/* Print a test's pass line only if it added no failures. */
static void report(int failures_before, const char *what) {
    if (g_failures == failures_before) printf("Passed %s\n", what);
}

/* --- server side --------------------------------------------------------- */

static void hello_handler(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    /* Not 200, so the test proves the status comes from the handler. */
    res->status_code = CWIST_HTTP_ACCEPTED;
    cwist_http_header_add(&res->headers, "X-Cwist-Test", "hello");
    cwist_sstring_assign(res->body, HELLO_BODY);
}

static void echo_handler(cwist_http_request *req, cwist_http_response *res) {
    size_t len = (req->body && req->body->data) ? req->body->size : 0;
    char prefix[32];
    snprintf(prefix, sizeof(prefix), "len=%zu;", len);
    res->status_code = CWIST_HTTP_CREATED;
    cwist_sstring_assign(res->body, prefix);
    if (len) cwist_sstring_append_len(res->body, req->body->data, len);
}

static void redirect_handler(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    res->status_code = CWIST_HTTP_FOUND;
    cwist_http_header_add(&res->headers, "Location", "/hello");
    cwist_sstring_assign(res->body, REDIRECT_BODY);
}

static void run_server(int port) {
#if defined(__linux__)
    /* Never outlive the test process, even if it aborts before cleanup. */
    prctl(PR_SET_PDEATHSIG, SIGKILL);
#endif
    setenv("CWIST_C1M_MODE", "0", 1);
    setenv("CWIST_WORKERS", "1", 1);
    cwist_app *app = cwist_app_create();
    if (!app) _exit(1);
    cwist_app_get(app, "/hello", hello_handler);
    cwist_app_post(app, "/echo", echo_handler);
    cwist_app_get(app, "/redirect", redirect_handler);
    g_cwist_drain_timeout_sec = 1;
    int rc = cwist_app_listen(app, port);
    cwist_app_destroy(app);
    _exit(rc == 0 ? 0 : 1);
}

/* --- process and socket helpers ------------------------------------------ */

/* Ask the kernel for an unused loopback port. Nothing listens on it once
 * this returns, which is also what the closed-port test relies on. */
static int pick_free_port(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = 0};
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t len = sizeof(addr);
    int port = -1;
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0 &&
        getsockname(fd, (struct sockaddr *)&addr, &len) == 0) {
        port = ntohs(addr.sin_port);
    }
    close(fd);
    return port;
}

static int can_connect(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return 0;
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port)};
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int ok = connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0;
    close(fd);
    return ok;
}

static void sleep_ms(long ms) {
    struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
    }
}

static long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Poll until the server accepts connections; fail fast if the child died
 * (for example because another process took the port first). */
static int wait_for_server(pid_t pid, int port) {
    for (int i = 0; i < 200; i++) {
        int status = 0;
        if (waitpid(pid, &status, WNOHANG) == pid) {
            fprintf(stderr, "FAIL: server exited during startup (status=%d)\n", status);
            return -1;
        }
        if (can_connect(port)) return 0;
        sleep_ms(25);
    }
    fprintf(stderr, "FAIL: server did not accept connections on port %d\n", port);
    return -1;
}

static int stop_server(pid_t pid) {
    if (kill(pid, SIGTERM) < 0 && errno != ESRCH) perror("kill SIGTERM");
    int status = 0;
    pid_t reaped = 0;
    for (int i = 0; i < 100; i++) {
        reaped = waitpid(pid, &status, WNOHANG);
        if (reaped == pid || (reaped < 0 && errno != EINTR)) break;
        sleep_ms(50);
    }
    if (reaped != pid) {
        fprintf(stderr, "FAIL: server did not exit after SIGTERM, sending SIGKILL\n");
        kill(pid, SIGKILL);
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        }
        return 1;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "FAIL: server exited abnormally (status=%d)\n", status);
        return 1;
    }
    return 0;
}

/* --- client-side checks -------------------------------------------------- */

static bool body_equals(const cwist_http_response *res, const char *want, size_t want_len) {
    size_t len = (res->body && res->body->data) ? res->body->size : 0;
    return len == want_len && (want_len == 0 || memcmp(res->body->data, want, want_len) == 0);
}

static cwist_http_response *request(cwist_http_client *client, const char *url,
                                    cwist_http_method_t method, const char *body, size_t body_len) {
    cwist_http_response *res = NULL;
    cwist_error_t err = cwist_http_client_request(client, url, method, NULL, body, body_len, &res);
    bool ok = cwist_error_is_ok(&err);
    cwist_error_dispose(&err);
    CHECK(ok, "request to %s reported an error", url);
    CHECK(ok == (res != NULL), "request to %s: response pointer does not match the result", url);
    if (!ok && res) {
        cwist_http_response_destroy(res);
        res = NULL;
    }
    return res;
}

static void test_get(cwist_http_client *client, const char *base) {
    int before = g_failures;
    char url[128];
    snprintf(url, sizeof(url), "%s/hello", base);
    cwist_http_response *res = request(client, url, CWIST_HTTP_GET, NULL, 0);
    if (!res) return;
    CHECK(res->status_code == CWIST_HTTP_ACCEPTED, "GET: status %d, want 202",
          (int)res->status_code);
    CHECK(body_equals(res, HELLO_BODY, strlen(HELLO_BODY)), "GET: unexpected body");
    const char *h = cwist_http_header_get(res->headers, "X-Cwist-Test");
    CHECK(h && strcmp(h, "hello") == 0, "GET: handler header missing");
    cwist_http_response_destroy(res);
    report(before, "GET status, body and header");
}

static void post_and_expect_echo(cwist_http_client *client, const char *url, const char *body,
                                 size_t body_len) {
    cwist_http_response *res = request(client, url, CWIST_HTTP_POST, body, body_len);
    if (!res) return;
    CHECK(res->status_code == CWIST_HTTP_CREATED, "POST: status %d, want 201",
          (int)res->status_code);
    char prefix[32];
    int plen = snprintf(prefix, sizeof(prefix), "len=%zu;", body_len);
    size_t got = (res->body && res->body->data) ? res->body->size : 0;
    CHECK(got == (size_t)plen + body_len, "POST: echoed %zu bytes, want %zu", got,
          (size_t)plen + body_len);
    if (got == (size_t)plen + body_len) {
        CHECK(memcmp(res->body->data, prefix, (size_t)plen) == 0,
              "POST: server saw a different body length");
        CHECK(memcmp(res->body->data + plen, body, body_len) == 0,
              "POST: server saw different body bytes");
    }
    cwist_http_response_destroy(res);
}

static void test_post(cwist_http_client *client, const char *base) {
    int before = g_failures;
    char url[128];
    snprintf(url, sizeof(url), "%s/echo", base);

    const char *small = "name=cwist&note=a b+c%20d";
    post_and_expect_echo(client, url, small, strlen(small));

    /* Large enough to arrive in several reads on the server side. */
    static char large[LARGE_POST_LEN];
    for (size_t i = 0; i < sizeof(large); i++) large[i] = (char)('a' + (i * 31) % 26);
    post_and_expect_echo(client, url, large, sizeof(large));
    report(before, "POST body reaches the handler");
}

static void test_redirect_followed(cwist_http_client *client, const char *base) {
    int before = g_failures;
    char url[128];
    snprintf(url, sizeof(url), "%s/redirect", base);
    cwist_http_client_set_follow_redirects(client, 1);
    cwist_http_response *res = request(client, url, CWIST_HTTP_GET, NULL, 0);
    if (!res) return;
    CHECK(res->status_code == CWIST_HTTP_ACCEPTED, "follow: status %d, want the target's 202",
          (int)res->status_code);
    CHECK(body_equals(res, HELLO_BODY, strlen(HELLO_BODY)), "follow: body is not the target's");
    /* Headers must describe the final response only, not the 302 hop. */
    CHECK(cwist_http_header_get(res->headers, "Location") == NULL,
          "follow: Location from the redirect hop leaked into the final response");
    int content_length_headers = 0;
    for (cwist_http_header_node *n = res->headers; n; n = n->next) {
        if (n->key && n->key->data && strcasecmp(n->key->data, "Content-Length") == 0) {
            content_length_headers++;
        }
    }
    CHECK(content_length_headers == 1, "follow: %d Content-Length headers, want 1",
          content_length_headers);
    cwist_http_response_destroy(res);
    report(before, "redirect following enabled");
}

static void test_redirect_not_followed(cwist_http_client *client, const char *base) {
    int before = g_failures;
    char url[128];
    snprintf(url, sizeof(url), "%s/redirect", base);
    cwist_http_client_set_follow_redirects(client, 0);
    cwist_http_response *res = request(client, url, CWIST_HTTP_GET, NULL, 0);
    cwist_http_client_set_follow_redirects(client, 1);
    if (!res) return;
    CHECK(res->status_code == CWIST_HTTP_FOUND, "no-follow: status %d, want 302",
          (int)res->status_code);
    const char *loc = cwist_http_header_get(res->headers, "Location");
    CHECK(loc && strcmp(loc, "/hello") == 0, "no-follow: Location header missing or wrong");
    CHECK(body_equals(res, REDIRECT_BODY, strlen(REDIRECT_BODY)),
          "no-follow: body is not the redirect handler's");
    cwist_http_response_destroy(res);
    report(before, "redirect following disabled");
}

static void test_closed_port(cwist_http_client *client) {
    int before = g_failures;
    int port = pick_free_port();
    CHECK(port > 0, "could not pick a closed port");
    if (port <= 0) return;
    char url[128];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/hello", port);

    cwist_http_client_set_timeout_ms(client, CLOSED_PORT_TIMEOUT_MS);
    cwist_http_response *res = NULL;
    long start = now_ms();
    cwist_error_t err = cwist_http_client_request(client, url, CWIST_HTTP_GET, NULL, NULL, 0, &res);
    long elapsed = now_ms() - start;
    bool ok = cwist_error_is_ok(&err);
    cwist_error_dispose(&err);

    CHECK(!ok, "request to closed port %d reported success", port);
    CHECK(res == NULL, "request to closed port returned a response");
    /* Refused connections fail at once; the bound is the configured
     * timeout plus scheduling slack, never the 30 s default. */
    CHECK(elapsed < CLOSED_PORT_TIMEOUT_MS + 1000, "closed port took %ld ms to fail", elapsed);
    if (res) cwist_http_response_destroy(res);
    if (g_failures == before) {
        printf("Passed closed port fails without hanging (%ld ms)\n", elapsed);
    }
}

int main(void) {
    printf("Testing cwist_http_client against a loopback CWIST server...\n");

    int port = pick_free_port();
    if (port <= 0) {
        fprintf(stderr, "FAIL: could not pick a loopback port\n");
        return 1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return 1;
    }
    if (pid == 0) run_server(port);

    if (wait_for_server(pid, port) != 0) {
        g_failures++;
        stop_server(pid);
        return 1;
    }

    cwist_http_client *client = cwist_http_client_create();
    CHECK(client != NULL, "cwist_http_client_create failed");
    if (client) {
        cwist_http_client_set_timeout_ms(client, 10000);
        char base[64];
        snprintf(base, sizeof(base), "http://127.0.0.1:%d", port);
        test_get(client, base);
        test_post(client, base);
        test_redirect_followed(client, base);
        test_redirect_not_followed(client, base);
    }

    g_failures += stop_server(pid);

    if (client) {
        test_closed_port(client);
        cwist_http_client_destroy(client);
    }

    if (g_failures) {
        fprintf(stderr, "test_http_client: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("All cwist_http_client tests passed!\n");
    return 0;
}
