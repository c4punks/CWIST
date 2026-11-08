/**
 * @file test_http3_defer.c
 * @brief cwist_async_defer() on the HTTP/3 path.
 *
 * Covers: a deferred stream completed from a scheduler job, the engine
 * serving another connection while a stream waits, completion before the
 * dispatch ack (inline respond), abort, the 504 timeout, and a stream
 * whose client leaves before the deferred response is ready.
 */

#include <cwist/net/http/async.h>
#include <cwist/net/http/http3_client.h>
#include <cwist/sys/app/app.h>
#include <cwist/sys/job/scheduler.h>

#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define TEST_PORT 19981

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

/* --- Server ---------------------------------------------------------------- */

static void respond_job(void *arg) {
    cwist_async *a = (cwist_async *)arg;
    cwist_async_respond(a, CWIST_HTTP_OK, "text/plain", "slow-ok", 7);
    cwist_async_release(a);
}

static void defer_with_delay(cwist_http_request *req, cwist_http_response *res, uint64_t ms) {
    cwist_async *a = cwist_async_defer(req, res);
    if (!a) {
        res->status_code = CWIST_HTTP_INTERNAL_ERROR;
        cwist_sstring_assign(res->body, "defer-failed");
        return;
    }
    cwist_scheduler_t *s = cwist_app_get_scheduler(req->app);
    cwist_async_retain(a);
    if (!s || !cwist_scheduler_schedule(s, respond_job, a, ms)) {
        cwist_async_release(a);
        cwist_async_abort(a, CWIST_HTTP_INTERNAL_ERROR);
    }
}

static void slow_handler(cwist_http_request *req, cwist_http_response *res) {
    defer_with_delay(req, res, 600);
}

static void fast_handler(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    cwist_sstring_assign(res->body, "fast-ok");
}

static void inline_handler(cwist_http_request *req, cwist_http_response *res) {
    cwist_async *a = cwist_async_defer(req, res);
    if (!a) {
        cwist_sstring_assign(res->body, "defer-failed");
        return;
    }
    /* Completes before the dispatch ack: the ack sends it. */
    cwist_async_respond(a, CWIST_HTTP_CREATED, "text/plain", "inline-ok", 9);
}

static void abort_handler(cwist_http_request *req, cwist_http_response *res) {
    cwist_async *a = cwist_async_defer(req, res);
    if (a) cwist_async_abort(a, CWIST_HTTP_SERVICE_UNAVAILABLE);
}

static void timeout_handler(cwist_http_request *req, cwist_http_response *res) {
    cwist_async *a = cwist_async_defer(req, res);
    if (a) cwist_async_set_timeout(a, 200); /* nobody responds */
}

static void respond_with_job(void *arg) {
    cwist_async *a = (cwist_async *)arg;
    cwist_http_response *r = cwist_http_response_create();
    r->status_code = CWIST_HTTP_ACCEPTED;
    cwist_http_header_add(&r->headers, "X-Built", "producer");
    cwist_sstring_assign(r->body, "built-ok");
    if (!cwist_async_respond_with(a, r)) cwist_http_response_destroy(r);
    cwist_async_release(a);
}

static void built_handler(cwist_http_request *req, cwist_http_response *res) {
    cwist_async *a = cwist_async_defer(req, res);
    if (!a) return;
    cwist_scheduler_t *s = cwist_app_get_scheduler(req->app);
    cwist_async_retain(a);
    if (!s || !cwist_scheduler_submit(s, respond_with_job, a)) {
        cwist_async_release(a);
        cwist_async_abort(a, CWIST_HTTP_INTERNAL_ERROR);
    }
}

static void run_server(void) {
    /* The app scheduler's threads start before cwist_app_listen() forks its
     * workers and do not survive the fork (TSan also refuses to start
     * threads after it), so keep the server single-process, as
     * test_async_defer does. */
    setenv("CWIST_WORKERS", "1", 1);
    cwist_app *app = cwist_app_create();
    cwist_app_use_scheduler(app, 2, 64);
    cwist_app_use_http3(app, true);
    cwist_app_get(app, "/slow", slow_handler);
    cwist_app_get(app, "/fast", fast_handler);
    cwist_app_get(app, "/inline", inline_handler);
    cwist_app_get(app, "/abort", abort_handler);
    cwist_app_get(app, "/timeout", timeout_handler);
    cwist_app_get(app, "/built", built_handler);
    _exit(cwist_app_listen(app, TEST_PORT) == 0 ? 0 : 3);
}

/* --- Client ---------------------------------------------------------------- */

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static cwist_http3_client *new_client(void) {
    cwist_http3_client *c = cwist_http3_client_create();
    if (!c) return NULL;
    cwist_http3_client_set_insecure(c, 1);
    cwist_http3_client_set_server(c, "127.0.0.1", TEST_PORT);
    cwist_http3_client_set_timeout_ms(c, 5000);
    cwist_http3_client_set_conn_timeout_ms(c, 5000);
    return c;
}

typedef struct {
    const char *path;
    int status;
    char body[64];
    double done_ms;
} get_result;

static void h3_get(cwist_http3_client *c, get_result *r) {
    cwist_http_response *res = NULL;
    r->status = -1;
    r->body[0] = '\0';
    cwist_error_t e = cwist_http3_client_request(c, r->path, CWIST_HTTP_GET, NULL, NULL, 0, &res);
    if (cwist_error_is_ok(&e) && res) {
        r->status = (int)res->status_code;
        if (res->body && res->body->data) {
            snprintf(r->body, sizeof(r->body), "%.*s", (int)res->body->size, res->body->data);
        }
        cwist_http_response_destroy(res);
    }
    r->done_ms = now_ms();
}

static void *slow_thread(void *arg) {
    get_result *r = (get_result *)arg;
    cwist_http3_client *c = new_client();
    h3_get(c, r);
    cwist_http3_client_destroy(c);
    return NULL;
}

static int wait_server(void) {
    for (int i = 0; i < 100; i++) {
        cwist_http3_client *c = new_client();
        cwist_http3_client_set_timeout_ms(c, 300);
        cwist_http3_client_set_conn_timeout_ms(c, 300);
        get_result r = {.path = "/fast"};
        h3_get(c, &r);
        cwist_http3_client_destroy(c);
        if (r.status == 200) return 0;
        usleep(100000);
    }
    return -1;
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    setvbuf(stdout, NULL, _IONBF, 0);

    pid_t server = fork();
    if (server == 0) run_server();
    if (wait_server() != 0) {
        printf("  FAIL server did not start\n");
        kill(server, SIGTERM);
        waitpid(server, NULL, 0);
        return 1;
    }

    printf("HTTP/3 deferred responses\n");

    /* A waiting stream must not hold the engine: a second connection's fast
     * request finishes while the slow one is still deferred. */
    get_result slow = {.path = "/slow"};
    pthread_t t;
    double start = now_ms();
    pthread_create(&t, NULL, slow_thread, &slow);
    usleep(150000);
    cwist_http3_client *c = new_client();
    get_result fast = {.path = "/fast"};
    h3_get(c, &fast);
    pthread_join(t, NULL);
    CHECK(slow.status == 200 && strcmp(slow.body, "slow-ok") == 0, "deferred stream completes");
    CHECK(fast.status == 200 && strcmp(fast.body, "fast-ok") == 0, "other stream served");
    CHECK(fast.done_ms < slow.done_ms, "engine not blocked by the deferred stream");
    CHECK(slow.done_ms - start >= 550, "deferred response waited for its producer");

    get_result r = {.path = "/inline"};
    h3_get(c, &r);
    CHECK(r.status == 201 && strcmp(r.body, "inline-ok") == 0, "completion before the ack");

    r = (get_result){.path = "/built"};
    h3_get(c, &r);
    CHECK(r.status == 202 && strcmp(r.body, "built-ok") == 0, "respond_with from a producer");

    r = (get_result){.path = "/abort"};
    h3_get(c, &r);
    CHECK(r.status == 503, "abort status");

    r = (get_result){.path = "/timeout"};
    h3_get(c, &r);
    CHECK(r.status == 504, "deferred timeout -> 504");

    /* Several deferred streams on one connection, in sequence. */
    int ok = 0;
    for (int i = 0; i < 5; i++) {
        r = (get_result){.path = "/built"};
        h3_get(c, &r);
        if (r.status == 202) ok++;
    }
    CHECK(ok == 5, "repeated deferred streams on one connection");
    cwist_http3_client_destroy(c);

    /* Client leaves while its stream is deferred: the late completion must
     * be dropped, and the server must keep serving. */
    cwist_http3_client *gone = new_client();
    cwist_http3_client_set_timeout_ms(gone, 100);
    r = (get_result){.path = "/slow"};
    h3_get(gone, &r);
    cwist_http3_client_destroy(gone);
    usleep(800000); /* the producer completes after the client left */
    c = new_client();
    r = (get_result){.path = "/fast"};
    h3_get(c, &r);
    CHECK(r.status == 200, "server healthy after an abandoned deferred stream");
    cwist_http3_client_destroy(c);

    kill(server, SIGTERM);
    int st = 0;
    waitpid(server, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "server shut down cleanly");

    if (g_failures) {
        printf("test_http3_defer: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("test_http3_defer: all passed\n");
    return 0;
}
