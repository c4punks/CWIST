/**
 * @file test_log_async.c
 * @brief Tests for the background log writer (cwist_log_async_start()).
 *
 * stdout and stderr are pointed at temp files, so each check reads back
 * exactly what the writer thread wrote.
 */
#define _POSIX_C_SOURCE 200809L
#include <cwist/sys/app/logger.h>
#include <cwist/sys/app/middleware.h>
#include <cwist/net/http/http.h>
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define LOG_ASYNC_TEST_TSAN 1
#endif
#endif
#if defined(__SANITIZE_THREAD__)
#define LOG_ASYNC_TEST_TSAN 1
#endif
#ifdef LOG_ASYNC_TEST_TSAN
/* test_fork_child forks while the writer thread runs, and the child starts
 * its own writer. That is the CWIST_WORKERS case and fine under glibc, but
 * TSan refuses new threads after a multi-threaded fork unless told not to. */
const char *__tsan_default_options(void);
const char *__tsan_default_options(void) {
    return "die_after_fork=0";
}
#endif

#define THREADS 8
#define LINES_PER_THREAD 2000

typedef struct {
    int saved_out, saved_err;
    FILE *out, *err;
} capture_t;

static void capture_begin(capture_t *c) {
    fflush(stdout);
    fflush(stderr);
    c->out = tmpfile();
    c->err = tmpfile();
    assert(c->out && c->err);
    c->saved_out = dup(STDOUT_FILENO);
    c->saved_err = dup(STDERR_FILENO);
    assert(dup2(fileno(c->out), STDOUT_FILENO) >= 0);
    assert(dup2(fileno(c->err), STDERR_FILENO) >= 0);
}

/* The writer uses write(2) on the fds, so read the files through the fd. */
static char *slurp(FILE *f) {
    int fd = fileno(f);
    off_t n = lseek(fd, 0, SEEK_END);
    assert(n >= 0);
    char *buf = calloc((size_t)n + 1, 1);
    assert(buf);
    assert(pread(fd, buf, (size_t)n, 0) == n);
    fclose(f);
    return buf;
}

static void capture_end(capture_t *c, char **out, char **err) {
    fflush(stdout);
    fflush(stderr);
    assert(dup2(c->saved_out, STDOUT_FILENO) >= 0);
    assert(dup2(c->saved_err, STDERR_FILENO) >= 0);
    close(c->saved_out);
    close(c->saved_err);
    *out = slurp(c->out);
    *err = slurp(c->err);
}

static size_t count_lines(const char *s) {
    size_t n = 0;
    for (; *s; s++) n += *s == '\n';
    return n;
}

typedef struct {
    cwist_logger *log;
    int id;
} worker_arg_t;

static void *worker(void *p) {
    worker_arg_t *a = p;
    for (int i = 0; i < LINES_PER_THREAD; i++) {
        if (i % 10 == 0) {
            cwist_logger_warn(a->log, "t%d n%d", a->id, i);
        } else {
            cwist_logger_info(a->log, "t%d n%d", a->id, i);
        }
    }
    return NULL;
}

/* Every line is whole and each thread's lines arrive in order per stream. */
static void check_stream(const char *text, int stride_warn, size_t expect_lines) {
    int next[THREADS];
    for (int t = 0; t < THREADS; t++) next[t] = stride_warn ? 0 : 1;
    size_t lines = 0;
    const char *p = text;
    while (*p) {
        const char *nl = strchr(p, '\n');
        assert(nl);
        const char *msg = strstr(p, "async: t");
        assert(msg && msg < nl);
        int t = -1, n = -1;
        assert(sscanf(msg, "async: t%d n%d", &t, &n) == 2);
        assert(t >= 0 && t < THREADS);
        assert(n == next[t]);
        if (stride_warn) {
            next[t] += 10;
        } else {
            next[t]++;
            if (next[t] % 10 == 0) next[t]++;
        }
        lines++;
        p = nl + 1;
    }
    assert(lines == expect_lines);
}

static void test_threads_whole_and_ordered(void) {
    cwist_logger *log = cwist_logger_create("async");
    assert(log);
    cwist_logger_set_colors(log, false);

    capture_t c;
    capture_begin(&c);
    /* A small ring, so callers have to wait for the writer many times. */
    assert(cwist_log_async_start(4096));
    assert(cwist_log_async_active());

    pthread_t th[THREADS];
    worker_arg_t args[THREADS];
    for (int t = 0; t < THREADS; t++) {
        args[t].log = log;
        args[t].id = t;
        assert(pthread_create(&th[t], NULL, worker, &args[t]) == 0);
    }
    for (int t = 0; t < THREADS; t++) pthread_join(th[t], NULL);
    cwist_log_async_flush();

    char *out, *err;
    capture_end(&c, &out, &err);
    check_stream(out, 0, (size_t)THREADS * LINES_PER_THREAD * 9 / 10);
    check_stream(err, 1, (size_t)THREADS * LINES_PER_THREAD / 10);
    assert(strstr(out, "] [INFO] async: t0 n1\n"));
    free(out);
    free(err);
    cwist_log_async_stop();
    assert(!cwist_log_async_active());
    cwist_logger_destroy(log);
    printf("Passed %d threads: lines whole and in order per stream\n", THREADS);
}

static void test_long_line_and_stop(void) {
    cwist_logger *log = cwist_logger_create("async");
    assert(log);
    cwist_logger_set_colors(log, false);
    capture_t c;
    capture_begin(&c);
    assert(cwist_log_async_start(256));

    char big[1000];
    memset(big, 'x', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';
    cwist_logger_info(log, "%s", big); /* larger than the ring: written synchronously */
    cwist_logger_info(log, "queued");
    cwist_log_async_stop(); /* writes "queued" before returning */
    cwist_logger_info(log, "after stop");

    char *out, *err;
    capture_end(&c, &out, &err);
    assert(strstr(out, big));
    char *q = strstr(out, "async: queued\n");
    char *a = strstr(out, "async: after stop\n");
    assert(q && a && q < a);
    assert(count_lines(out) == 3);
    free(out);
    free(err);
    cwist_logger_destroy(log);
    printf("Passed long line falls back, stop drains, then synchronous again\n");
}

static void dummy_next(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    res->status_code = CWIST_HTTP_OK;
    cwist_sstring_assign(res->body, "OK");
}

static void test_access_log_async(void) {
    cwist_http_request *req = cwist_http_request_create();
    cwist_http_response *res = cwist_http_response_create();
    req->client_fd = -1;
    capture_t c;
    capture_begin(&c);
    assert(cwist_log_async_start(0));
    cwist_mw_access_log(CWIST_LOG_JSON)(req, res, dummy_next);
    cwist_log_async_flush();
    char *out, *err;
    capture_end(&c, &out, &err);
    cwist_log_async_stop();
    assert(strstr(out, "\"method\":\"GET\""));
    assert(strstr(out, "\"status\":200"));
    assert(count_lines(out) == 1);
    free(out);
    free(err);
    cwist_http_request_destroy(req);
    cwist_http_response_destroy(res);
    printf("Passed access log goes through the async writer\n");
}

static void test_fork_child(void) {
    cwist_logger *log = cwist_logger_create("async");
    assert(log);
    cwist_logger_set_colors(log, false);
    capture_t c;
    capture_begin(&c);
    assert(cwist_log_async_start(0));
    cwist_logger_info(log, "parent before fork");
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        /* No writer thread survived the fork; the first line starts one and
         * exit() writes it out through the atexit hook. */
        cwist_logger_info(log, "from child");
        exit(0);
    }
    int status = 0;
    assert(waitpid(pid, &status, 0) == pid);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    cwist_logger_info(log, "parent after fork");
    cwist_log_async_flush();
    char *out, *err;
    capture_end(&c, &out, &err);
    cwist_log_async_stop();
    assert(strstr(out, "async: from child\n"));
    char *before = strstr(out, "async: parent before fork\n");
    char *after = strstr(out, "async: parent after fork\n");
    assert(before && after && before < after);
    /* The child dropped its copy of the parent's queue: written once. */
    assert(strstr(before + strlen("async: parent before fork\n"), "parent before fork") == NULL);
    assert(count_lines(out) == 3);
    free(out);
    free(err);
    cwist_logger_destroy(log);
    printf("Passed forked child gets its own writer\n");
}

int main(void) {
    test_threads_whole_and_ordered();
    test_long_line_and_stop();
    test_access_log_async();
    test_fork_child();
    printf("All async log tests passed.\n");
    return 0;
}
