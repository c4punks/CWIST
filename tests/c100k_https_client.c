/**
 * @file c100k_https_client.c
 * @brief HTTPS C100k load client driven by scripts/ci/c100k_https.sh.
 *
 * Opens --conns TLS connections and keeps every one of them open at the same
 * time, then runs --rounds keep-alive request rounds over all of them:
 *
 *   1. ramp:   non-blocking connect + TLS handshake, at most --inflight
 *              handshakes pending per thread, until every connection is
 *              established or has failed.
 *   2. hold:   all connections stay open and idle for --hold seconds. A
 *              connection the server closes during the hold counts as dropped.
 *   3. rounds: one "GET <path>" per connection per round, sent on every
 *              connection before any response is awaited; each response must
 *              be HTTP 200 with the expected Content-Length body.
 *
 * Connections are spread round-robin over the --hosts addresses. Each
 * destination address has its own ~64k ephemeral-port space, so two loopback
 * addresses (127.0.0.1, 127.0.0.2) are enough for 100k connections against
 * a server bound to 0.0.0.0.
 *
 * Output is one "key=value" line per metric; the exit status is 0 only when
 * every connection was established, none dropped, and every request in every
 * round succeeded. Not a unit test (it needs a running server), so it is not
 * in TEST_TARGETS.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define MAX_HOSTS 16
#define RESP_BUF 512

typedef enum {
    ST_FREE,      /* not opened yet */
    ST_CONNECT,   /* TCP connect in progress */
    ST_HANDSHAKE, /* TLS handshake in progress */
    ST_IDLE,      /* established, no request outstanding */
    ST_SEND,      /* request partially written */
    ST_RECV,      /* waiting for / reading the response */
    ST_DEAD,      /* failed or closed */
} conn_state;

typedef struct {
    int fd;
    SSL *ssl;
    conn_state st;
    uint16_t host;
    uint16_t rlen;
    uint32_t sent;
    char rbuf[RESP_BUF];
} conn_t;

typedef struct {
    int id;
    int epfd;
    conn_t *conns;
    size_t n;
    size_t next_open;
    size_t pending; /* connects + handshakes in flight */
    size_t established;
    size_t failed_connect;
    size_t failed_handshake;
    size_t dropped;
    size_t round_ok;
    size_t round_fail;
    size_t round_left;
    double hs_ms_total;
    pthread_t thread;
} worker_t;

static struct sockaddr_in g_hosts[MAX_HOSTS];
static size_t g_nhosts;
static size_t g_conns = 100000;
static size_t g_threads = 4;
static size_t g_inflight = 512;
static int g_rounds = 2;
static int g_hold_s = 5;
static int g_phase_timeout_s = 180;
static const char *g_path = "/";
static size_t g_expect_len = 2;
static SSL_CTX *g_ctx;
static char g_req[256];
static size_t g_req_len;

static pthread_barrier_t g_barrier;

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void conn_kill(worker_t *w, conn_t *c) {
    if (c->ssl) {
        SSL_free(c->ssl);
        c->ssl = NULL;
    }
    if (c->fd >= 0) {
        epoll_ctl(w->epfd, EPOLL_CTL_DEL, c->fd, NULL);
        close(c->fd);
        c->fd = -1;
    }
    c->st = ST_DEAD;
}

static void conn_watch(worker_t *w, conn_t *c, uint32_t events) {
    struct epoll_event ev = {.events = events | EPOLLRDHUP, .data.ptr = c};
    if (epoll_ctl(w->epfd, EPOLL_CTL_MOD, c->fd, &ev) != 0) {
        epoll_ctl(w->epfd, EPOLL_CTL_ADD, c->fd, &ev);
    }
}

static bool conn_open(worker_t *w, conn_t *c) {
    c->fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (c->fd < 0) return false;
    int one = 1;
    setsockopt(c->fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    const struct sockaddr_in *sa = &g_hosts[c->host];
    if (connect(c->fd, (const struct sockaddr *)sa, sizeof(*sa)) != 0 && errno != EINPROGRESS) {
        close(c->fd);
        c->fd = -1;
        return false;
    }
    c->st = ST_CONNECT;
    struct epoll_event ev = {.events = EPOLLOUT | EPOLLRDHUP, .data.ptr = c};
    epoll_ctl(w->epfd, EPOLL_CTL_ADD, c->fd, &ev);
    return true;
}

/* Drive the TLS handshake; returns false when the connection failed. */
static bool conn_handshake(worker_t *w, conn_t *c) {
    if (!c->ssl) {
        c->ssl = SSL_new(g_ctx);
        if (!c->ssl || SSL_set_fd(c->ssl, c->fd) != 1) return false;
        SSL_set_connect_state(c->ssl);
        c->st = ST_HANDSHAKE;
    }
    int rc = SSL_do_handshake(c->ssl);
    if (rc == 1) {
        c->st = ST_IDLE;
        conn_watch(w, c, 0); /* EPOLLRDHUP only: notice server-side closes */
        return true;
    }
    int err = SSL_get_error(c->ssl, rc);
    if (err == SSL_ERROR_WANT_READ) {
        conn_watch(w, c, EPOLLIN);
        return true;
    }
    if (err == SSL_ERROR_WANT_WRITE) {
        conn_watch(w, c, EPOLLOUT);
        return true;
    }
    return false;
}

/* Parse "HTTP/1.1 200" plus Content-Length; returns 1 done ok, 0 need more,
 * -1 bad response. */
static int resp_check(conn_t *c) {
    c->rbuf[c->rlen] = '\0';
    char *end = strstr(c->rbuf, "\r\n\r\n");
    if (!end) return c->rlen >= RESP_BUF - 1 ? -1 : 0;
    if (strncmp(c->rbuf, "HTTP/1.1 200", 12) != 0) return -1;
    const char *cl = strcasestr(c->rbuf, "\r\ncontent-length:");
    if (!cl || cl > end) return -1;
    size_t len = strtoul(cl + 17, NULL, 10);
    size_t have = (size_t)(c->rlen - (end + 4 - c->rbuf));
    if (have < len) return 0;
    return len == g_expect_len && have == len ? 1 : -1;
}

/* Drive one request/response exchange; returns 1 done ok, 0 in progress,
 * -1 failed. */
static int conn_exchange(worker_t *w, conn_t *c) {
    if (c->st == ST_IDLE) {
        c->st = ST_SEND;
        c->sent = 0;
        c->rlen = 0;
    }
    if (c->st == ST_SEND) {
        while (c->sent < g_req_len) {
            int n = SSL_write(c->ssl, g_req + c->sent, (int)(g_req_len - c->sent));
            if (n > 0) {
                c->sent += (uint32_t)n;
                continue;
            }
            int err = SSL_get_error(c->ssl, n);
            if (err == SSL_ERROR_WANT_WRITE || err == SSL_ERROR_WANT_READ) {
                conn_watch(w, c, err == SSL_ERROR_WANT_WRITE ? EPOLLOUT : EPOLLIN);
                return 0;
            }
            return -1;
        }
        c->st = ST_RECV;
    }
    for (;;) {
        int n = SSL_read(c->ssl, c->rbuf + c->rlen, RESP_BUF - 1 - c->rlen);
        if (n > 0) {
            c->rlen += (uint16_t)n;
            int r = resp_check(c);
            if (r != 0) {
                if (r > 0) {
                    c->st = ST_IDLE;
                    conn_watch(w, c, 0);
                }
                return r;
            }
            continue;
        }
        int err = SSL_get_error(c->ssl, n);
        if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
            conn_watch(w, c, err == SSL_ERROR_WANT_WRITE ? EPOLLOUT : EPOLLIN);
            return 0;
        }
        return -1;
    }
}

static void ramp(worker_t *w) {
    double deadline = now_s() + g_phase_timeout_s;
    double start = now_s();
    struct epoll_event evs[1024];
    while ((w->next_open < w->n || w->pending > 0) && now_s() < deadline) {
        while (w->next_open < w->n && w->pending < g_inflight) {
            conn_t *c = &w->conns[w->next_open++];
            if (conn_open(w, c)) {
                w->pending++;
            } else {
                c->st = ST_DEAD;
                w->failed_connect++;
            }
        }
        int k = epoll_wait(w->epfd, evs, 1024, 100);
        for (int i = 0; i < k; i++) {
            conn_t *c = evs[i].data.ptr;
            if (c->st == ST_CONNECT) {
                int soerr = 0;
                socklen_t sl = sizeof(soerr);
                getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &soerr, &sl);
                if (soerr != 0 || (evs[i].events & (EPOLLERR | EPOLLHUP))) {
                    conn_kill(w, c);
                    w->pending--;
                    w->failed_connect++;
                    continue;
                }
            } else if (c->st != ST_HANDSHAKE) {
                if (c->st == ST_IDLE && (evs[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR))) {
                    conn_kill(w, c);
                    w->established--;
                    w->dropped++;
                }
                continue;
            }
            if (!conn_handshake(w, c)) {
                conn_kill(w, c);
                w->pending--;
                w->failed_handshake++;
            } else if (c->st == ST_IDLE) {
                w->pending--;
                w->established++;
            }
        }
    }
    (void)start;
    /* Whatever is still pending at the deadline failed. */
    for (size_t i = 0; i < w->n; i++) {
        conn_t *c = &w->conns[i];
        if (c->st == ST_CONNECT || c->st == ST_HANDSHAKE) {
            conn_kill(w, c);
            w->failed_handshake++;
        } else if (c->st == ST_FREE) {
            c->st = ST_DEAD;
            w->failed_connect++;
        }
    }
    w->pending = 0;
}

/* Idle phase: only server-side closes generate events. */
static void hold(worker_t *w, double until) {
    struct epoll_event evs[1024];
    while (now_s() < until) {
        int k = epoll_wait(w->epfd, evs, 1024, 100);
        for (int i = 0; i < k; i++) {
            conn_t *c = evs[i].data.ptr;
            if (c->st == ST_IDLE) {
                conn_kill(w, c);
                w->established--;
                w->dropped++;
            }
        }
    }
}

static void round_run(worker_t *w) {
    double deadline = now_s() + g_phase_timeout_s;
    w->round_left = 0;
    /* Write the request on every connection before awaiting any response,
     * so the server holds all of them busy at once. */
    for (size_t i = 0; i < w->n; i++) {
        conn_t *c = &w->conns[i];
        if (c->st != ST_IDLE) continue;
        int r = conn_exchange(w, c);
        if (r < 0) {
            conn_kill(w, c);
            w->round_fail++;
        } else if (r > 0) {
            w->round_ok++;
        } else {
            w->round_left++;
        }
    }
    struct epoll_event evs[1024];
    while (w->round_left > 0 && now_s() < deadline) {
        int k = epoll_wait(w->epfd, evs, 1024, 100);
        for (int i = 0; i < k; i++) {
            conn_t *c = evs[i].data.ptr;
            if (c->st != ST_SEND && c->st != ST_RECV) {
                if (c->st == ST_IDLE && (evs[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR))) {
                    conn_kill(w, c);
                    w->dropped++;
                }
                continue;
            }
            int r = conn_exchange(w, c);
            if (r == 0) continue;
            w->round_left--;
            if (r > 0) {
                w->round_ok++;
            } else {
                conn_kill(w, c);
                w->round_fail++;
            }
        }
    }
    for (size_t i = 0; i < w->n; i++) {
        conn_t *c = &w->conns[i];
        if (c->st == ST_SEND || c->st == ST_RECV) {
            conn_kill(w, c);
            w->round_fail++;
        }
    }
    w->round_left = 0;
}

static void *worker_main(void *arg) {
    worker_t *w = arg;
    ramp(w);
    pthread_barrier_wait(&g_barrier); /* ramp done everywhere */
    pthread_barrier_wait(&g_barrier); /* main has sampled; hold starts */
    hold(w, now_s() + g_hold_s);
    pthread_barrier_wait(&g_barrier);
    for (int r = 0; r < g_rounds; r++) {
        pthread_barrier_wait(&g_barrier); /* round start */
        round_run(w);
        pthread_barrier_wait(&g_barrier); /* round end */
    }
    pthread_barrier_wait(&g_barrier); /* main has sampled; close */
    for (size_t i = 0; i < w->n; i++) {
        if (w->conns[i].st != ST_DEAD && w->conns[i].st != ST_FREE) conn_kill(w, &w->conns[i]);
    }
    return NULL;
}

static size_t sum(worker_t *ws, size_t off) {
    size_t t = 0;
    for (size_t i = 0; i < g_threads; i++) t += *(size_t *)((char *)&ws[i] + off);
    return t;
}
#define SUM(field) sum(ws, offsetof(worker_t, field))

static void usage(const char *argv0) {
    fprintf(stderr,
            "usage: %s --hosts A[,B..] --port P [--conns N] [--threads T] [--inflight K]\n"
            "          [--rounds R] [--hold S] [--timeout S] [--path P] [--expect-len L]\n",
            argv0);
    exit(2);
}

static void parse_hosts(char *list, int port) {
    for (char *tok = strtok(list, ","); tok && g_nhosts < MAX_HOSTS; tok = strtok(NULL, ",")) {
        struct sockaddr_in *sa = &g_hosts[g_nhosts];
        memset(sa, 0, sizeof(*sa));
        sa->sin_family = AF_INET;
        sa->sin_port = htons((uint16_t)port);
        if (inet_pton(AF_INET, tok, &sa->sin_addr) != 1) {
            fprintf(stderr, "bad host %s\n", tok);
            exit(2);
        }
        g_nhosts++;
    }
}

int main(int argc, char **argv) {
    char *hosts = NULL;
    int port = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!v) usage(argv[0]);
        if (!strcmp(a, "--hosts")) hosts = argv[++i];
        else if (!strcmp(a, "--port")) port = atoi(argv[++i]);
        else if (!strcmp(a, "--conns")) g_conns = strtoul(argv[++i], NULL, 10);
        else if (!strcmp(a, "--threads")) g_threads = strtoul(argv[++i], NULL, 10);
        else if (!strcmp(a, "--inflight")) g_inflight = strtoul(argv[++i], NULL, 10);
        else if (!strcmp(a, "--rounds")) g_rounds = atoi(argv[++i]);
        else if (!strcmp(a, "--hold")) g_hold_s = atoi(argv[++i]);
        else if (!strcmp(a, "--timeout")) g_phase_timeout_s = atoi(argv[++i]);
        else if (!strcmp(a, "--path")) g_path = argv[++i];
        else if (!strcmp(a, "--expect-len")) g_expect_len = strtoul(argv[++i], NULL, 10);
        else usage(argv[0]);
    }
    if (!hosts || port <= 0 || g_conns == 0 || g_threads == 0 || g_inflight == 0) usage(argv[0]);
    parse_hosts(hosts, port);

    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur < g_conns + 1024) {
        rl.rlim_cur = rl.rlim_max;
        setrlimit(RLIMIT_NOFILE, &rl);
        getrlimit(RLIMIT_NOFILE, &rl);
        if (rl.rlim_cur < g_conns + 1024) {
            fprintf(stderr, "RLIMIT_NOFILE %llu < conns %zu\n", (unsigned long long)rl.rlim_cur, g_conns);
            return 2;
        }
    }

    g_ctx = SSL_CTX_new(TLS_client_method());
    if (!g_ctx) return 2;
    SSL_CTX_set_verify(g_ctx, SSL_VERIFY_NONE, NULL);
    /* Keep client memory per connection small: 100k idle sessions with
     * their 16 KiB read/write buffers would cost the client ~3 GiB. */
    SSL_CTX_set_mode(g_ctx, SSL_MODE_RELEASE_BUFFERS);
    SSL_CTX_set_session_cache_mode(g_ctx, SSL_SESS_CACHE_OFF);
    SSL_CTX_set_options(g_ctx, SSL_OP_NO_TICKET);
    static const unsigned char alpn[] = "\x08http/1.1";
    SSL_CTX_set_alpn_protos(g_ctx, alpn, sizeof(alpn) - 1);
    g_req_len = (size_t)snprintf(g_req, sizeof(g_req),
                                 "GET %s HTTP/1.1\r\nHost: c100k\r\nConnection: keep-alive\r\n\r\n", g_path);

    conn_t *conns = calloc(g_conns, sizeof(*conns));
    worker_t *ws = calloc(g_threads, sizeof(*ws));
    if (!conns || !ws) return 2;
    for (size_t i = 0; i < g_conns; i++) {
        conns[i].fd = -1;
        conns[i].host = (uint16_t)(i % g_nhosts);
    }
    size_t per = g_conns / g_threads, rem = g_conns % g_threads, off = 0;
    pthread_barrier_init(&g_barrier, NULL, (unsigned)g_threads + 1);
    double t0 = now_s();
    for (size_t i = 0; i < g_threads; i++) {
        worker_t *w = &ws[i];
        w->id = (int)i;
        w->n = per + (i < rem ? 1 : 0);
        w->conns = conns + off;
        off += w->n;
        w->epfd = epoll_create1(EPOLL_CLOEXEC);
        if (w->epfd < 0 || pthread_create(&w->thread, NULL, worker_main, w) != 0) return 2;
    }

    pthread_barrier_wait(&g_barrier);
    size_t est = SUM(established);
    printf("ramp_seconds=%.2f\n", now_s() - t0);
    printf("established=%zu\nfailed_connect=%zu\nfailed_handshake=%zu\n", est, SUM(failed_connect),
           SUM(failed_handshake));
    fflush(stdout);
    /* Lets the driver script sample server state with every connection open. */
    if (getenv("C100K_READY_FILE")) {
        FILE *f = fopen(getenv("C100K_READY_FILE"), "w");
        if (f) {
            fprintf(f, "%zu\n", est);
            fclose(f);
        }
    }
    pthread_barrier_wait(&g_barrier);
    pthread_barrier_wait(&g_barrier);
    printf("held_after_idle=%zu\ndropped_idle=%zu\n", SUM(established), SUM(dropped));
    fflush(stdout);

    bool rounds_ok = true;
    for (int r = 0; r < g_rounds; r++) {
        size_t ok0 = SUM(round_ok), fail0 = SUM(round_fail);
        double rs = now_s();
        pthread_barrier_wait(&g_barrier);
        pthread_barrier_wait(&g_barrier);
        size_t ok = SUM(round_ok) - ok0, fail = SUM(round_fail) - fail0;
        double secs = now_s() - rs;
        printf("round%d_ok=%zu\nround%d_fail=%zu\nround%d_seconds=%.2f\nround%d_rps=%.0f\n", r + 1, ok, r + 1,
               fail, r + 1, secs, r + 1, secs > 0 ? (double)ok / secs : 0.0);
        fflush(stdout);
        if (ok != g_conns || fail != 0) rounds_ok = false;
    }
    if (getenv("C100K_DONE_FILE")) {
        FILE *f = fopen(getenv("C100K_DONE_FILE"), "w");
        if (f) fclose(f);
    }
    /* Give the driver script a moment to sample server RSS at peak load. */
    if (getenv("C100K_LINGER_S")) sleep((unsigned)atoi(getenv("C100K_LINGER_S")));
    pthread_barrier_wait(&g_barrier);
    for (size_t i = 0; i < g_threads; i++) pthread_join(ws[i].thread, NULL);

    bool pass = est == g_conns && SUM(dropped) == 0 && rounds_ok;
    printf("result=%s\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}
