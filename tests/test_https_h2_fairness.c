/**
 * @file test_https_h2_fairness.c
 * @brief A busy HTTP/2 TLS connection must not hold the only HTTPS pool
 *        thread while another connection waits.
 *
 * The server runs one worker process with one HTTPS pool thread. Two busy
 * connections each keep 32 multiplexed requests to a 1 ms handler in flight
 * for two seconds, so the server always has their next request buffered:
 * they are never idle and never park. Meanwhile probe requests on fresh
 * connections must each complete well within the window. Before the pool
 * yield, a probe could wait behind a busy connection for most of the run.
 */
#define _POSIX_C_SOURCE 200809L
#include <cwist/app.h>
#include <curl/curl.h>

#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define TEST_PORT 19971
#define BUSY_SECONDS 2
#define MAX_WAIT_MS 300

static const char *g_cert = "/tmp/cwist_h2_fairness_cert.pem";
static const char *g_key = "/tmp/cwist_h2_fairness_key.pem";
static char g_url[64];
static char g_slow_url[64];

static void hello(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    cwist_sstring_assign(res->body, "ok");
}

static void slow(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    struct timespec d = {0, 1000000};
    nanosleep(&d, NULL);
    cwist_sstring_assign(res->body, "slow");
}

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static size_t discard(char *p, size_t s, size_t n, void *u) {
    (void)p;
    (void)u;
    return s * n;
}

static CURL *new_easy_url(const char *url) {
    CURL *e = curl_easy_init();
    curl_easy_setopt(e, CURLOPT_URL, url);
    curl_easy_setopt(e, CURLOPT_HTTP_VERSION, (long)CURL_HTTP_VERSION_2TLS);
    curl_easy_setopt(e, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(e, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(e, CURLOPT_WRITEFUNCTION, discard);
    curl_easy_setopt(e, CURLOPT_TIMEOUT_MS, 10000L);
    return e;
}

static CURL *new_easy(void) { return new_easy_url(g_url); }

static _Atomic int g_busy_done;
static _Atomic long g_busy_requests;

/* Busy load: BUSY_CONNS HTTP/2 connections, STREAMS requests in flight each. */
#define BUSY_CONNS 2
#define STREAMS 32
static void *busy_thread(void *arg) {
    (void)arg;
    CURLM *m = curl_multi_init();
    curl_multi_setopt(m, CURLMOPT_PIPELINING, CURLPIPE_MULTIPLEX);
    curl_multi_setopt(m, CURLMOPT_MAX_HOST_CONNECTIONS, 1L);
    curl_multi_setopt(m, CURLMOPT_MAX_CONCURRENT_STREAMS, (long)STREAMS);
    double end = now_ms() + BUSY_SECONDS * 1000.0;
    for (int i = 0; i < STREAMS; i++) {
        CURL *e = new_easy_url(g_slow_url);
        curl_easy_setopt(e, CURLOPT_PIPEWAIT, 1L);
        curl_multi_add_handle(m, e);
    }
    int running = 1;
    while (running) {
        curl_multi_perform(m, &running);
        CURLMsg *msg;
        int left;
        while ((msg = curl_multi_info_read(m, &left))) {
            if (msg->msg != CURLMSG_DONE) continue;
            CURL *e = msg->easy_handle;
            curl_multi_remove_handle(m, e);
            atomic_fetch_add(&g_busy_requests, 1);
            if (now_ms() < end) {
                curl_multi_add_handle(m, e); /* reuse: same connection */
                running = 1;
            } else {
                curl_easy_cleanup(e);
            }
        }
        if (running) curl_multi_poll(m, NULL, 0, 1, NULL);
    }
    curl_multi_cleanup(m);
    atomic_fetch_add(&g_busy_done, 1);
    return NULL;
}

static int make_cert(void) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:P-256 -nodes "
             "-keyout %s -out %s -days 1 -subj /CN=localhost >/dev/null 2>&1",
             g_key, g_cert);
    return system(cmd);
}

int main(void) {
    if (make_cert() != 0) {
        printf("openssl not available: skipping\n");
        return 0;
    }
    snprintf(g_url, sizeof(g_url), "https://127.0.0.1:%d/", TEST_PORT);
    snprintf(g_slow_url, sizeof(g_slow_url), "https://127.0.0.1:%d/slow", TEST_PORT);

    pid_t pid = fork();
    if (pid == 0) {
        setenv("CWIST_WORKERS", "1", 1);
        setenv("CWIST_WORKER_THREADS", "1", 1);
        cwist_app *app = cwist_app_create();
        cwist_app_get(app, "/", hello);
        cwist_app_get(app, "/slow", slow);
        cwist_app_use_https(app, g_cert, g_key);
        cwist_app_use_https2(app, true);
        _exit(cwist_app_listen(app, TEST_PORT) == 0 ? 0 : 1);
    }

    curl_global_init(CURL_GLOBAL_DEFAULT);
    int up = 0;
    for (int i = 0; i < 50 && !up; i++) {
        usleep(100000);
        CURL *e = new_easy();
        up = curl_easy_perform(e) == CURLE_OK;
        curl_easy_cleanup(e);
    }
    int failures = 0;
    if (!up) {
        fprintf(stderr, "FAIL: server did not come up\n");
        failures++;
        goto out;
    }

    pthread_t tids[BUSY_CONNS];
    for (int i = 0; i < BUSY_CONNS; i++) pthread_create(&tids[i], NULL, busy_thread, NULL);
    usleep(300000); /* let connection A get going */

    double worst = 0;
    int probes = 0;
    while (atomic_load(&g_busy_done) == 0 && probes < 10) {
        CURL *e = new_easy();
        curl_easy_setopt(e, CURLOPT_FRESH_CONNECT, 1L);
        double t0 = now_ms();
        CURLcode rc = curl_easy_perform(e);
        double dt = now_ms() - t0;
        curl_easy_cleanup(e);
        if (rc != CURLE_OK) {
            fprintf(stderr, "FAIL: probe request failed: %s\n", curl_easy_strerror(rc));
            failures++;
            break;
        }
        if (dt > worst) worst = dt;
        probes++;
        usleep(100000);
    }
    for (int i = 0; i < BUSY_CONNS; i++) pthread_join(tids[i], NULL);

    printf("busy connections served %ld requests; %d probes, slowest %.1f ms\n",
           atomic_load(&g_busy_requests), probes, worst);
    if (probes == 0 || atomic_load(&g_busy_requests) < 50) {
        fprintf(stderr, "FAIL: the busy connection or the probes did not run\n");
        failures++;
    }
    if (worst > MAX_WAIT_MS) {
        fprintf(stderr, "FAIL: a probe waited %.1f ms behind the busy connections (limit %d)\n",
                worst, MAX_WAIT_MS);
        failures++;
    }

out:
    kill(pid, SIGTERM);
    waitpid(pid, NULL, 0);
    curl_global_cleanup();
    unlink(g_cert);
    unlink(g_key);
    if (failures) return 1;
    printf("HTTPS HTTP/2 fairness test passed\n");
    return 0;
}
