/**
 * Regression coverage for issue #237 (wave B): an async (C1M) file-stream
 * response must park on write-readiness when the client drains slowly,
 * keeping the reactor thread free to serve other connections, and must
 * rearm the connection after the parked drain finishes.
 *
 * A tiny receive buffer on the file connection forces sendfile EAGAIN, so
 * the server has to park the remaining {fd, offset} state. The probe then
 * completes while the file is still undelivered — impossible if a reactor
 * thread were blocked in poll(POLLOUT) driving the file.
 */
#include <cwist/sys/app/app.h>
#include <cwist/sys/app/shutdown.h>
#include <cwist/sys/io/reactor.h>
#include <cwist/net/http/async.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define FILE_SIZE (512 * 1024)
#define IO_CAP (FILE_SIZE + 8192)

extern cwist_async_action_t cwist_app_http_handler_async(int, cwist_http_async_conn_t *);

static char g_file_path[] = "/tmp/cwist_async_file_park_XXXXXX";
static cwist_reactor_t *g_reactor;

static cwist_async_action_t dispatch(int fd, cwist_http_async_conn_t *conn) {
    if (!g_reactor) g_reactor = conn->reactor;
    return cwist_app_http_handler_async(fd, conn);
}

static void file_handler(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    int fd = open(g_file_path, O_RDONLY);
    assert(fd >= 0);
    struct stat st;
    assert(fstat(fd, &st) == 0);
    res->use_file_stream = true;
    res->file_stream_fd = fd;
    res->file_stream_len = (size_t)st.st_size;
    res->file_stream_offset = 0;
    res->file_stream_auto_close = true;
}

static void probe_handler(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    cwist_sstring_assign(res->body, "pong");
}

static void write_all(int fd, const char *data, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, data + off, len - off);
        assert(n > 0);
        off += (size_t)n;
    }
}

static void wake_for_shutdown(void *unused) {
    (void)unused;
}

int main(void) {
    /* Loaded CI machines: the drain below is deliberately slow. */
    alarm(60);
    signal(SIGPIPE, SIG_IGN);
    setenv("CWIST_WORKERS", "1", 1);
    setenv("CWIST_WORKER_THREADS", "1", 1);
    setenv("CWIST_C1M_MODE", "1", 1);
    atomic_store(&g_cwist_running, true);

    /* Patterned temp file so a content check catches any splice glitch. */
    int tfd = mkstemp(g_file_path);
    assert(tfd >= 0);
    static char pattern[FILE_SIZE];
    for (size_t i = 0; i < FILE_SIZE; i++) pattern[i] = (char)('A' + (i * 7 + i / 251) % 26);
    write_all(tfd, pattern, sizeof(pattern));
    close(tfd);

    cwist_app *app = cwist_app_create();
    assert(app);
    cwist_app_get(app, "/file", file_handler);
    cwist_app_get(app, "/probe", probe_handler);
    assert(cwist_http_pool_init() == 0);

    /* Connection A: file request. Capping BOTH buffers fixes the maximum
     * in-flight data far below the file size, so sendfile stalls and must
     * park (unix socket buffers would otherwise absorb the whole file). */
    int a[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, a) == 0);
    int bufopt = 4096;
    setsockopt(a[0], SOL_SOCKET, SO_SNDBUF, &bufopt, sizeof(bufopt));
    setsockopt(a[1], SOL_SOCKET, SO_RCVBUF, &bufopt, sizeof(bufopt));
    const char file_req[] = "GET /file HTTP/1.1\r\nHost: x\r\n\r\n";
    write_all(a[1], file_req, sizeof(file_req) - 1);
    assert(cwist_http_pool_submit_async(a[0], dispatch, app));

    /* Read until the response head arrived. Some body bytes may ride in
     * the same reads; the bulk of the file must still be undelivered when
     * the probe runs. */
    static char a_buf[IO_CAP];
    size_t a_used = 0;
    char *head_end = NULL;
    while (!head_end) {
        ssize_t n = read(a[1], a_buf + a_used, sizeof(a_buf) - 1 - a_used);
        assert(n > 0);
        a_used += (size_t)n;
        a_buf[a_used] = '\0';
        head_end = strstr(a_buf, "\r\n\r\n");
    }
    assert(strstr(a_buf, "200") != NULL);
    const size_t a_body = (size_t)(head_end - a_buf) + 4;
    assert(a_used - a_body < FILE_SIZE);

    /* Connection B, submitted mid-drain: completes promptly only if the
     * parked file writer left the reactor thread free. */
    int b[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, b) == 0);
    const char probe_req[] = "GET /probe HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
    write_all(b[1], probe_req, sizeof(probe_req) - 1);
    assert(cwist_http_pool_submit_async(b[0], dispatch, app));

    size_t a_body_when_b_done = 0;
    int b_done = 0;
    char b_buf[4096] = {0};
    size_t b_used = 0;
    while (!b_done || a_used - a_body < FILE_SIZE) {
        struct pollfd fds[2] = {{.fd = a[1], .events = POLLIN}, {.fd = b[1], .events = POLLIN}};
        int n = poll(fds, 2, 15000);
        assert(n > 0);
        /* Handle the probe first: read order must not bias the assertion
         * when both connections are ready in the same poll. */
        if (fds[1].revents & (POLLIN | POLLHUP)) {
            ssize_t got = read(b[1], b_buf + b_used, sizeof(b_buf) - 1 - b_used);
            if (got > 0) {
                b_used += (size_t)got;
            } else if (got == 0) {
                if (!b_done) a_body_when_b_done = a_used - a_body;
                b_done = 1;
            }
        }
        if (fds[1].revents & POLLERR) {
            assert(!"probe connection errored");
        }
        if (fds[0].revents & POLLIN) {
            ssize_t got = read(a[1], a_buf + a_used, sizeof(a_buf) - 1 - a_used);
            assert(got > 0);
            a_used += (size_t)got;
            /* Stay slow: the server must keep parking, not catch up. */
            struct timespec ts = {.tv_sec = 0, .tv_nsec = 10 * 1000 * 1000};
            nanosleep(&ts, NULL);
        }
    }
    assert(b_done);
    assert(strstr(b_buf, "pong") != NULL);
    /* The probe finished while the bulk of the file was still on the
     * server: the reactor thread was never blocked driving the file. */
    assert(a_body_when_b_done < FILE_SIZE);

    /* Full file content arrived intact. */
    assert(a_used - a_body == FILE_SIZE);
    assert(memcmp(a_buf + a_body, pattern, FILE_SIZE) == 0);

    /* The parked drain finished and rearmed the keep-alive connection. */
    write_all(a[1], probe_req, sizeof(probe_req) - 1);
    char a2[4096] = {0};
    size_t a2_used = 0;
    for (;;) {
        ssize_t got = read(a[1], a2 + a2_used, sizeof(a2) - 1 - a2_used);
        assert(got >= 0);
        if (got == 0) break;
        a2_used += (size_t)got;
        if (strstr(a2, "pong")) break;
    }
    assert(strstr(a2, "pong") != NULL);

    atomic_store(&g_cwist_running, false);
    cwist_reactor_post_t wake = {.cb = wake_for_shutdown};
    assert(cwist_reactor_post(g_reactor, &wake));
    cwist_http_pool_destroy();
    cwist_app_destroy(app);
    close(a[0]);
    close(a[1]);
    close(b[0]);
    close(b[1]);
    unlink(g_file_path);
    puts("async file park test passed");
    return 0;
}
