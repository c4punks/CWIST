/* Public-dispatcher h2c replay regression; no private source or mocked I/O.
 * The seed is consumed from the real socket before serve_connection, modeling
 * accept sniffing deterministically, not exercising an app listener/reactor.
 * After its single request flight the client ONLY reads until the verdict;
 * in particular it never ACKs SETTINGS/PING or half-closes to wake the server.
 */
#include <cwist/net/http/http2.h>
#include <cwist/sys/app/shutdown.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define DEADLINE_MS 2500
static const unsigned char flight[] =
    "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n"
    /* Empty SETTINGS; END_HEADERS|END_STREAM HEADERS on streams 1 and 3.
     * HPACK static GET, http, /; literal authority localhost. Second path
     * is literal /two with indexed name :path. No Huffman/dynamic table. */
    "\x00\x00\x00\x04\x00\x00\x00\x00\x00"
    "\x00\x00\x0e\x01\x05\x00\x00\x00\x01"
    "\x82\x86\x84\x01\x09localhost"
    "\x00\x00\x13\x01\x05\x00\x00\x00\x03"
    "\x82\x86\x04\x04/two\x01\x09localhost";
/* A zero stream window forces the response writer to consume the following
 * WINDOW_UPDATE before it can send DATA. No later client write is permitted. */
static const unsigned char flow_flight[] =
    "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n"
    "\x00\x00\x06\x04\x00\x00\x00\x00\x00"
    "\x00\x04\x00\x00\x00\x00"
    "\x00\x00\x0e\x01\x05\x00\x00\x00\x01"
    "\x82\x86\x84\x01\x09localhost"
    "\x00\x00\x04\x08\x00\x00\x00\x00\x01"
    "\x00\x00\xff\xff";
static const char *const bodies[] = {"replayed stream one\n", "replayed stream three\n"};

static int64_t now_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        perror("clock_gettime");
        exit(EXIT_FAILURE);
    }
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int ready(int fd, short events, int64_t deadline) {
    for (;;) {
        int64_t left = deadline - now_ms();
        if (left <= 0) { errno = ETIMEDOUT; return -1; }
        struct pollfd p = {fd, events, 0};
        int rc = poll(&p, 1, (int)left);
        if (rc < 0 && errno == EINTR) continue;
        if (rc == 0) { errno = ETIMEDOUT; return -1; }
        if (rc < 0) return -1;
        if (p.revents & events) return 0;
        errno = ECONNRESET;
        return -1;
    }
}

static int transfer(int fd, void *buf, size_t len, int writing, int64_t end) {
    unsigned char *p = buf;
    while (len) {
        if (ready(fd, writing ? POLLOUT : POLLIN, end) != 0) return -1;
        ssize_t n = writing ? write(fd, p, len) : read(fd, p, len);
        if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        if (n <= 0) { if (!n) errno = ECONNRESET; return -1; }
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

struct server {
    int fd;
    const unsigned char *flight;
    size_t seed;
    int64_t deadline;
    int seed_ok;
    unsigned calls;
    int bad_request;
};

static void handler(void *opaque, cwist_http_request *req, cwist_http_response *res) {
    struct server *s = opaque;
    int index = req->stream_id == 1 ? 0 : req->stream_id == 3 ? 1 : -1;
    ++s->calls;
    if (index < 0 || !req->path || !req->path->data ||
        strcmp(req->path->data, index == 0 ? "/" : "/two") != 0) {
        s->bad_request = 1;
        res->status_code = 500;
        return;
    }
    res->status_code = 200;
    cwist_http_response_set_body_ptr(res, bodies[index], strlen(bodies[index]));
}

static void *serve(void *opaque) {
    struct server *s = opaque;
    char seed[256];
    cwist_https_connection conn = {
        .fd = s->fd, .ssl = NULL, .read_buf = seed, .buf_len = 0,
        .negotiated_http2 = true, .negotiated_protocol = CWIST_HTTPS_PROTOCOL_HTTP2,
        .http2_sequenced_data = false
    };
    if (transfer(s->fd, seed, s->seed, 0, s->deadline) == 0 &&
        memcmp(seed, s->flight, s->seed) == 0) {
        s->seed_ok = 1;
        conn.buf_len = s->seed;
        cwist_error_t err = cwist_http2_serve_connection(&conn, s, handler);
        cwist_error_dispose(&err);
    }
    /* conn and seed are caller-owned; dispatcher does not close/free them. */
    return NULL;
}

struct response { int headers, ended; size_t used; unsigned char body[128]; };
static int responses(int fd, int64_t end, const char *name, unsigned streams) {
    struct response r[2] = {{0}};
    int settings = 0, settings_ack = 0;
    while (!r[0].ended || (streams == 2 && !r[1].ended) || !settings || !settings_ack) {
        unsigned char h[9], p[16384];
        if (transfer(fd, h, sizeof(h), 0, end) != 0) goto io_error;
        size_t n = ((size_t)h[0] << 16) | ((size_t)h[1] << 8) | h[2];
        uint32_t id = ((uint32_t)(h[5] & 127) << 24) | ((uint32_t)h[6] << 16) |
                      ((uint32_t)h[7] << 8) | h[8];
        if (n > sizeof(p)) goto protocol_error;
        if (transfer(fd, p, n, 0, end) != 0) goto io_error;
        if (h[3] == 4) {
            if (id || (h[4] & ~1) || ((h[4] & 1) ? n != 0 : n % 6 != 0)) goto protocol_error;
            if (h[4] & 1) settings_ack = 1; else settings = 1;
            continue;
        }
        if (h[3] == 6) { if (id || n != 8) goto protocol_error; continue; }
        if (h[3] == 8) { if (n != 4) goto protocol_error; continue; }
        /* GOAWAY, RST_STREAM and unexpected frames are failures, not progress. */
        if (id != 1 && (streams != 2 || id != 3)) goto protocol_error;
        struct response *v = &r[id == 1 ? 0 : 1];
        if (v->ended) goto protocol_error;
        if (h[3] == 1) {
            /* CWIST's current encoder starts status 200 with HPACK static
             * index 8 (0x88). This deliberately bounded oracle is NOT a
             * general HPACK decoder; encoding changes may need a test update. */
            if (v->headers || h[4] != 4 || !n || p[0] != 0x88) goto protocol_error;
            v->headers = 1;
        } else if (h[3] == 0) {
            if (!v->headers || (h[4] & ~1) || n > sizeof(v->body) - v->used) goto protocol_error;
            memcpy(v->body + v->used, p, n);
            v->used += n;
            if (h[4] & 1) v->ended = 1;
        } else goto protocol_error;
    }
    for (unsigned i = 0; i < streams; ++i) {
        if (!r[i].headers || r[i].used != strlen(bodies[i]) ||
            memcmp(r[i].body, bodies[i], r[i].used) != 0) goto protocol_error;
    }
    return 0;
io_error:
    fprintf(stderr, "%s: response read: %s (stream1 headers=%d bytes=%zu end=%d; stream3 headers=%d bytes=%zu end=%d)\n",
            name, strerror(errno), r[0].headers, r[0].used, r[0].ended,
            r[1].headers, r[1].used, r[1].ended);
    return -1;
protocol_error:
    fprintf(stderr, "%s: invalid response frame, status, body or stream state\n", name);
    return -1;
}

static int run_case(const char *name, const unsigned char *payload,
                    size_t len, size_t seed, unsigned streams) {
    if (len > 256 || seed > len || (streams != 1 && streams != 2)) return -1;
    int fd[2] = {-1, -1}, started = 0, rc = -1;
    pthread_t thread;
    struct server s = {.flight = payload, .seed = seed};
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fd) != 0) { perror("socketpair"); goto done; }
    for (int i = 0; i < 2; ++i) {
        int flags = fcntl(fd[i], F_GETFL, 0);
        if (flags < 0 || fcntl(fd[i], F_SETFL, flags | O_NONBLOCK) < 0) { perror("fcntl"); goto done; }
    }
    s.fd = fd[1];
    s.deadline = now_ms() + DEADLINE_MS;
    /* One logical flight, before the server starts. Short writes retry only
     * within this phase; absolutely no client sends occur in responses(). */
    unsigned char sendbuf[256];
    memcpy(sendbuf, payload, len);
    if (transfer(fd[0], sendbuf, len, 1, s.deadline) != 0) { perror("request flight"); goto done; }
    int err = pthread_create(&thread, NULL, serve, &s);
    if (err) { fprintf(stderr, "pthread_create: %s\n", strerror(err)); goto done; }
    started = 1;
    rc = responses(fd[0], s.deadline, name, streams);
done:
    /* Verdict is fixed BEFORE shutdown can make a buggy poll readable.
     * Wake both endpoints, then join before closing/reusing their descriptors.
     * Default SIGALRM bounds even a broken server's join to three seconds. */
    alarm(3);
    if (fd[0] >= 0) shutdown(fd[0], SHUT_RDWR);
    if (fd[1] >= 0) shutdown(fd[1], SHUT_RDWR);
    if (started) {
        int err = pthread_join(thread, NULL);
        if (err) { fprintf(stderr, "pthread_join: %s\n", strerror(err)); _Exit(EXIT_FAILURE); }
        if (!s.seed_ok || s.calls != streams || s.bad_request) rc = -1;
    }
    if (fd[0] >= 0) close(fd[0]);
    if (fd[1] >= 0) close(fd[1]);
    alarm(20);
    fprintf(stderr, "%s: %s (seed=%zu, dispatches=%u)\n", name, rc == 0 ? "PASS" : "FAIL", seed, s.calls);
    return rc;
}

int main(int argc, char **argv) {
    if (argc > 2 || (argc == 2 && strcmp(argv[1], "basic") && strcmp(argv[1], "flow"))) {
        fprintf(stderr, "usage: %s [basic|flow]\n", argv[0]);
        return EXIT_FAILURE;
    }
    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR || signal(SIGALRM, SIG_DFL) == SIG_ERR) {
        perror("signal"); return EXIT_FAILURE;
    }
    alarm(20);
    atomic_store(&g_cwist_running, 1);
    int failed = 0;
    if (argc == 1 || strcmp(argv[1], "basic") == 0) {
        failed |= run_case("socket-only control", flight, sizeof(flight) - 1, 0, 2) != 0;
        failed |= run_case("split first HEADERS header", flight, sizeof(flight) - 1, 24 + 9 + 5, 2) != 0;
        failed |= run_case("all requests prebuffered", flight, sizeof(flight) - 1, sizeof(flight) - 1, 2) != 0;
    }
    if (argc == 1 || strcmp(argv[1], "flow") == 0) {
        failed |= run_case("flow-control socket control", flow_flight, sizeof(flow_flight) - 1, 0, 1) != 0;
        failed |= run_case("flow-control update prebuffered", flow_flight, sizeof(flow_flight) - 1, sizeof(flow_flight) - 1, 1) != 0;
    }
    alarm(0);
    return failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
