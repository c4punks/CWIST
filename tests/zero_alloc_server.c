/**
 * @file zero_alloc_server.c
 * @brief Plain-HTTP server driven by scripts/ci/zero_alloc_gate.sh (#432).
 *
 * GET / answers "ok".  GET /zero_alloc/arm and GET /zero_alloc/count arm and
 * read the allocation counter of the LD_PRELOAD shim
 * (tests/zero_alloc_counter.c), so the gate counts the heap allocations of
 * the keep-alive requests sent in between.  Run with CWIST_WORKERS=1: the
 * counter is per process.
 * Not a unit test (it runs until SIGTERM), so it is not in TEST_TARGETS.
 */
#define _GNU_SOURCE
#include <cwist/sys/app/app.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>

static void (*g_arm)(void);
static long (*g_disarm)(void);

static void ok_route(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    cwist_sstring_assign(res->body, "ok");
}

static void arm_route(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    g_arm();
    cwist_sstring_assign(res->body, "armed");
}

static void count_route(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    char buf[32];
    snprintf(buf, sizeof(buf), "%ld", g_disarm());
    cwist_sstring_assign(res->body, buf);
}

int main(int argc, char **argv) {
    g_arm = (void (*)(void))dlsym(RTLD_DEFAULT, "cwist_zero_alloc_arm");
    g_disarm = (long (*)(void))dlsym(RTLD_DEFAULT, "cwist_zero_alloc_disarm");
    if (!g_arm || !g_disarm) {
        fprintf(stderr, "zero_alloc_server: run with LD_PRELOAD=<zero_alloc_counter shim>\n");
        return 2;
    }
    cwist_app *app = cwist_app_create();
    if (!app) return 1;
    cwist_app_get(app, "/", ok_route);
    cwist_app_get(app, "/zero_alloc/arm", arm_route);
    cwist_app_get(app, "/zero_alloc/count", count_route);
    int rc = cwist_app_listen(app, argc > 1 ? atoi(argv[1]) : 19600);
    cwist_app_destroy(app);
    return rc;
}
