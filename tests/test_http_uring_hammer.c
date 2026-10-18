/**
 * @file test_http_uring_hammer.c
 * @brief HTTP/1.1 keep-alive hammer against the async (C1M) server.
 *
 * Forks a child running a cwist app on the default backend (io_uring where
 * available, accepting through the multishot IORING_OP_ACCEPT path) and
 * drives it with concurrent keep-alive clients. The Makefile also runs this
 * with CWIST_REACTOR_BACKEND=epoll, which exercises the silent fallback to
 * the one-shot POLL + accept4 loop, and with CWIST_ACCEPT_URING=0, which
 * refuses the multishot arm on the uring backend.
 */

#include <cwist/sys/app/app.h>
#include <cwist/sys/app/shutdown.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define TEST_PORT 19989
#define TEST_HOST "127.0.0.1"
#define THREADS 8
#define REQS_PER_THREAD 200

/* --- Server side --------------------------------------------------------- */

static void hello_handler(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    cwist_sstring_assign(res->body, "hammer-ok");
    cwist_http_header_add(&res->headers, "Content-Type", "text/plain");
    res->status_code = CWIST_HTTP_OK;
}

/* --- Client side --------------------------------------------------------- */

static int connect_to_server(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(TEST_PORT),
    };
    inet_pton(AF_INET, TEST_HOST, &addr.sin_addr);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int send_all(int fd, const char *req, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, req + sent, len - sent, 0);
        if (n <= 0) return -1;
        sent += (size_t)n;
    }
    return 0;
}

/* Read one full HTTP/1.x response (headers + Content-Length body). One
 * request in flight per connection, so nothing is pipelined. */
static int read_response(int fd, char *buf, size_t buf_size) {
    size_t total = 0;
    size_t need = 0; /* header_end + content_length once parsed */
    for (;;) {
        if (need == 0) {
            buf[total] = '\0';
            char *he = strstr(buf, "\r\n\r\n");
            if (he) {
                size_t header_end = (size_t)(he - buf) + 4;
                char *cl = strcasestr(buf, "Content-Length:");
                if (!cl || (size_t)(cl - buf) > header_end) return -1;
                need = header_end + (size_t)atoi(cl + 15);
            }
        }
        if (need > 0 && total >= need) return (int)total;
        if (total >= buf_size - 1) return -1;
        ssize_t n = recv(fd, buf + total, buf_size - 1 - total, 0);
        if (n <= 0) return -1;
        total += (size_t)n;
    }
}

static _Atomic int g_failures;

static void *client_thread(void *arg) {
    (void)arg;
    static const char req[] = "GET /hello HTTP/1.1\r\nHost: localhost\r\n\r\n";
    char buf[4096];
    int fd = connect_to_server();
    if (fd < 0) {
        atomic_fetch_add(&g_failures, 1);
        return NULL;
    }
    for (int i = 0; i < REQS_PER_THREAD; i++) {
        if (send_all(fd, req, sizeof(req) - 1) < 0 ||
            read_response(fd, buf, sizeof(buf)) < 0 || !strstr(buf, "HTTP/1.1 200") ||
            !strstr(buf, "hammer-ok")) {
            atomic_fetch_add(&g_failures, 1);
            break;
        }
    }
    close(fd);
    return NULL;
}

int main(void) {
    printf("Testing HTTP/1.1 keep-alive hammer (reactor accept path)...\n");

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return 1;
    }

    if (pid == 0) {
        /* Single worker process: the accept path under test is per-process. */
        setenv("CWIST_WORKERS", "1", 1);
        cwist_app *app = cwist_app_create();
        if (!app) _exit(1);
        cwist_app_get(app, "/hello", hello_handler);
        g_cwist_drain_timeout_sec = 1;
        int rc = cwist_app_listen(app, TEST_PORT);
        cwist_app_destroy(app);
        _exit(rc == 0 ? 0 : 1);
    }

    usleep(400000);

    pthread_t threads[THREADS];
    for (int i = 0; i < THREADS; i++) {
        if (pthread_create(&threads[i], NULL, client_thread, NULL) != 0) {
            atomic_fetch_add(&g_failures, 1);
            threads[i] = 0;
        }
    }
    for (int i = 0; i < THREADS; i++) {
        if (threads[i]) pthread_join(threads[i], NULL);
    }

    kill(pid, SIGTERM);
    int status;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "FAIL: server child exited abnormally (status=%d)\n", status);
        atomic_fetch_add(&g_failures, 1);
    }

    int failures = atomic_load(&g_failures);
    if (failures == 0) {
        printf("HTTP/1.1 keep-alive hammer (%d threads x %d requests): PASS\n", THREADS,
               REQS_PER_THREAD);
        return 0;
    }
    fprintf(stderr, "%d hammer failure(s).\n", failures);
    return 1;
}
