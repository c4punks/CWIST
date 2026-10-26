/**
 * @file test_redis_timeout.c
 * @brief Deadline-bounded pooled Redis command tests against scripted,
 *        real-TCP fake RESP peers (no Redis server required).
 *
 * The fake peer interprets the command the client sends:
 *   ECHO tag              -> bulk tag
 *   STALL ms tag          -> sleep ms, then bulk tag
 *   TRICKLE ms tag        -> bulk tag written one byte every ms
 *   RAW bytes             -> bytes verbatim (malformed/typed replies)
 *   RAWCLOSE bytes        -> bytes verbatim, then close
 *   CLOSE                 -> close without replying
 *   REPLYCLOSE tag        -> bulk tag, then close (idle conn goes stale)
 *   UNSOL tag extra       -> bulk tag, then unsolicited extra bytes
 *   DOUBLE tag            -> bulk tag twice in a single write
 * A server-wide mode can stop reading (write stall) or reset the connection
 * after the first read (EPIPE/SIGPIPE).
 */
#include <cwist/core/mem/alloc.h>
#include <cwist/net/redis/cwist_redis.h>
#include <cwist/sys/err/cwist_err.h>

#include <arpa/inet.h>
#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

static int g_failures;
#define CHECK(cond)                                                           \
    do {                                                                      \
        if (!(cond)) {                                                        \
            fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            g_failures++;                                                     \
        }                                                                     \
    } while (0)

/* --- getaddrinfo() interposer: proves reconnect performs no DNS ---------- */

typedef int (*gai_fn)(const char *, const char *, const struct addrinfo *, struct addrinfo **);
static gai_fn g_real_gai;
static atomic_int g_dns_calls;
static atomic_bool g_dns_block;
static atomic_bool g_pipe_failure;
typedef int (*pipe_fn)(int *);
static pipe_fn g_real_pipe;

int pipe(int *fds) {
    if (atomic_load(&g_pipe_failure) || !g_real_pipe) {
        errno = EMFILE;
        return -1;
    }
    return g_real_pipe(fds);
}

int getaddrinfo(const char *node, const char *service, const struct addrinfo *hints,
                struct addrinfo **res) {
    atomic_fetch_add(&g_dns_calls, 1);
    if (atomic_load(&g_dns_block) || !g_real_gai) return EAI_FAIL;
    return g_real_gai(node, service, hints, res);
}

static void dns_init(void) {
    void *sym = dlsym(RTLD_NEXT, "getaddrinfo");
    memcpy(&g_real_gai, &sym, sizeof(g_real_gai));
    sym = dlsym(RTLD_NEXT, "pipe");
    memcpy(&g_real_pipe, &sym, sizeof(g_real_pipe));
}

/* --- time helpers -------------------------------------------------------- */

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec / 1e6;
}

/* poll() rather than nanosleep(): on background-QoS macOS, nanosleep and
 * condition timeouts are coalesced by up to ~150 ms, poll() stays precise. */
static void sleep_ms(long ms) {
    double end = now_ms() + (double)ms;
    for (double left = (double)ms; left > 0; left = end - now_ms())
        (void)poll(NULL, 0, (int)left + 1);
}

/* --- fake RESP peer ------------------------------------------------------ */

enum { MODE_NORMAL = 0, MODE_NOREAD = 1, MODE_RST_ON_READ = 2 };
#define LOG_MAX 8192
#define MAX_ARGS 8
#define SRV_BUF_MAX ((size_t)64 * 1024 * 1024)

typedef struct fake_server {
    int listen_fd;
    int port;
    pthread_t accept_thread;
    bool listener_open;
    atomic_bool stop_accept;
    atomic_bool stop;
    atomic_int mode;
    atomic_int accepted;
    atomic_int active;
    pthread_mutex_t mu;
    char *log[LOG_MAX];
    int nlog;
} fake_server;

typedef struct conn_ctx {
    fake_server *s;
    int fd;
} conn_ctx;

static int srv_send(int fd, const void *buf, size_t len) {
    const char *p = buf;
    while (len) {
        ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static char *bulk_frame(const char *s, size_t len, size_t *out_len) {
    char hdr[32];
    int h = snprintf(hdr, sizeof(hdr), "$%zu\r\n", len);
    size_t total = (size_t)h + len + 2;
    char *b = malloc(total);
    memcpy(b, hdr, (size_t)h);
    if (len) memcpy(b + h, s, len);
    memcpy(b + h + len, "\r\n", 2);
    *out_len = total;
    return b;
}

static int srv_bulk(int fd, const char *s, size_t len) {
    size_t n = 0;
    char *b = bulk_frame(s, len, &n);
    int rc = srv_send(fd, b, n);
    free(b);
    return rc;
}

static void srv_log(fake_server *s, const char *name, size_t nlen, const char *last, size_t llen) {
    if (llen > 64) llen = 64;
    char *e = malloc(nlen + llen + 2);
    memcpy(e, name, nlen);
    e[nlen] = ' ';
    memcpy(e + nlen + 1, last, llen);
    e[nlen + 1 + llen] = '\0';
    pthread_mutex_lock(&s->mu);
    if (s->nlog < LOG_MAX)
        s->log[s->nlog++] = e;
    else
        free(e);
    pthread_mutex_unlock(&s->mu);
}

/* Count log entries equal to @p entry (or starting with it if prefix). */
static int log_count(fake_server *s, const char *entry, bool prefix) {
    int n = 0;
    pthread_mutex_lock(&s->mu);
    for (int i = 0; i < s->nlog; i++) {
        if (prefix ? strncmp(s->log[i], entry, strlen(entry)) == 0 : strcmp(s->log[i], entry) == 0)
            n++;
    }
    pthread_mutex_unlock(&s->mu);
    return n;
}

static bool wait_log(fake_server *s, const char *entry, int ms) {
    double end = now_ms() + ms;
    while (now_ms() < end) {
        if (log_count(s, entry, false) > 0) return true;
        sleep_ms(2);
    }
    return false;
}

static int wait_accepted(fake_server *s, int want, int ms) {
    double end = now_ms() + ms;
    while (atomic_load(&s->accepted) < want && now_ms() < end) sleep_ms(2);
    return atomic_load(&s->accepted);
}

static void srv_stall(fake_server *s, long ms) {
    double end = now_ms() + (double)ms;
    while (!atomic_load(&s->stop) && now_ms() < end) sleep_ms(2);
}

static long arg_num(const char *p, size_t n) {
    char tmp[32];
    if (n >= sizeof(tmp)) return 0;
    memcpy(tmp, p, n);
    tmp[n] = '\0';
    return strtol(tmp, NULL, 10);
}

/* 1 = parsed, 0 = need more bytes, -1 = malformed. */
static int parse_num_line(const char *b, size_t len, size_t *pos, char prefix, long long *out) {
    if (*pos >= len) return 0;
    if (b[*pos] != prefix) return -1;
    const char *nl = memchr(b + *pos, '\n', len - *pos);
    if (!nl) return (len - *pos > 32) ? -1 : 0;
    size_t end = (size_t)(nl - b);
    if (end < *pos + 2 || b[end - 1] != '\r') return -1;
    char tmp[32];
    size_t n = end - 1 - (*pos + 1);
    if (n == 0 || n >= sizeof(tmp)) return -1;
    memcpy(tmp, b + *pos + 1, n);
    tmp[n] = '\0';
    char *ep = NULL;
    long long v = strtoll(tmp, &ep, 10);
    if (*ep || v < 0) return -1;
    *out = v;
    *pos = end + 1;
    return 1;
}

static int parse_cmd(const char *b, size_t len, size_t *used, const char **args, size_t *lens,
                     int *argc) {
    size_t pos = 0;
    long long n = 0;
    int r = parse_num_line(b, len, &pos, '*', &n);
    if (r <= 0) return r;
    if (n < 1 || n > MAX_ARGS) return -1;
    for (long long i = 0; i < n; i++) {
        long long l = 0;
        r = parse_num_line(b, len, &pos, '$', &l);
        if (r <= 0) return r;
        if ((unsigned long long)l > SRV_BUF_MAX) return -1;
        if (len - pos < (size_t)l + 2) return 0;
        if (b[pos + (size_t)l] != '\r' || b[pos + (size_t)l + 1] != '\n') return -1;
        args[i] = b + pos;
        lens[i] = (size_t)l;
        pos += (size_t)l + 2;
    }
    *argc = (int)n;
    *used = pos;
    return 1;
}

/* Returns false when the connection must be closed. */
static bool srv_exec(fake_server *s, int fd, int argc, const char **a, const size_t *l) {
    srv_log(s, a[0], l[0], a[argc - 1], l[argc - 1]);
#define IS(x) (l[0] == strlen(x) && memcmp(a[0], (x), l[0]) == 0)
    if (IS("ECHO") && argc == 2) return srv_bulk(fd, a[1], l[1]) == 0;
    if (IS("STALL") && argc == 3) {
        srv_stall(s, arg_num(a[1], l[1]));
        return srv_bulk(fd, a[2], l[2]) == 0;
    }
    if (IS("TRICKLE") && argc == 3) {
        long ms = arg_num(a[1], l[1]);
        size_t n = 0;
        char *b = bulk_frame(a[2], l[2], &n);
        bool ok = true;
        for (size_t i = 0; i < n && ok; i++) {
            ok = srv_send(fd, b + i, 1) == 0;
            if (ok && i + 1 < n) srv_stall(s, ms);
        }
        free(b);
        return ok;
    }
    if (IS("RAW") && argc == 2) return srv_send(fd, a[1], l[1]) == 0;
    if (IS("RAWCLOSE") && argc == 2) {
        srv_send(fd, a[1], l[1]);
        return false;
    }
    if (IS("CLOSE")) return false;
    if (IS("REPLYCLOSE") && argc == 2) {
        srv_bulk(fd, a[1], l[1]);
        return false;
    }
    if (IS("UNSOL") && argc == 3) {
        if (srv_bulk(fd, a[1], l[1]) != 0) return false;
        srv_stall(s, 30);
        return srv_send(fd, a[2], l[2]) == 0;
    }
    if (IS("DOUBLE") && argc == 2) {
        size_t n = 0;
        char *b = bulk_frame(a[1], l[1], &n);
        char *two = malloc(n * 2);
        memcpy(two, b, n);
        memcpy(two + n, b, n);
        int rc = srv_send(fd, two, n * 2);
        free(two);
        free(b);
        return rc == 0;
    }
#undef IS
    return srv_send(fd, "-ERR unknown\r\n", 14) == 0;
}

static void *srv_conn(void *arg) {
    conn_ctx *c = arg;
    fake_server *s = c->s;
    int fd = c->fd;
    free(c);
    size_t cap = 65536, len = 0;
    char *buf = malloc(cap);
    bool open = true;
    while (open && !atomic_load(&s->stop)) {
        if (atomic_load(&s->mode) == MODE_NOREAD) {
            sleep_ms(5);
            continue;
        }
        struct pollfd p = {fd, POLLIN, 0};
        if (poll(&p, 1, 20) <= 0) continue;
        if (len == cap) {
            if (cap >= SRV_BUF_MAX) break;
            cap *= 2;
            buf = realloc(buf, cap);
        }
        ssize_t n = recv(fd, buf + len, cap - len, 0);
        if (n <= 0) break;
        if (atomic_load(&s->mode) == MODE_RST_ON_READ) {
            struct linger lg = {1, 0};
            setsockopt(fd, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg));
            close(fd);
            fd = -1;
            break;
        }
        len += (size_t)n;
        for (;;) {
            const char *args[MAX_ARGS];
            size_t lens[MAX_ARGS];
            int argc = 0;
            size_t used = 0;
            int r = parse_cmd(buf, len, &used, args, lens, &argc);
            if (r < 0) open = false;
            if (r <= 0) break;
            if (!srv_exec(s, fd, argc, args, lens)) {
                open = false;
                break;
            }
            memmove(buf, buf + used, len - used);
            len -= used;
        }
    }
    if (fd >= 0) {
        shutdown(fd, SHUT_RDWR);
        close(fd);
    }
    free(buf);
    atomic_fetch_sub(&s->active, 1);
    return NULL;
}

static void *srv_accept(void *arg) {
    fake_server *s = arg;
    while (!atomic_load(&s->stop_accept)) {
        struct pollfd p = {s->listen_fd, POLLIN, 0};
        if (poll(&p, 1, 20) <= 0) continue;
        int fd = accept(s->listen_fd, NULL, NULL);
        if (fd < 0) continue;
#ifdef SO_NOSIGPIPE
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
        atomic_fetch_add(&s->accepted, 1);
        atomic_fetch_add(&s->active, 1);
        conn_ctx *c = malloc(sizeof(*c));
        c->s = s;
        c->fd = fd;
        pthread_t t;
        if (pthread_create(&t, NULL, srv_conn, c) != 0) {
            close(fd);
            free(c);
            atomic_fetch_sub(&s->active, 1);
            continue;
        }
        pthread_detach(t);
    }
    return NULL;
}

static fake_server *server_start(void) {
    fake_server *s = calloc(1, sizeof(*s));
    pthread_mutex_init(&s->mu, NULL);
    s->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(s->listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = 0;
    if (bind(s->listen_fd, (struct sockaddr *)&sa, sizeof(sa)) != 0 ||
        listen(s->listen_fd, 64) != 0) {
        perror("fake server");
        exit(2);
    }
    socklen_t sl = sizeof(sa);
    getsockname(s->listen_fd, (struct sockaddr *)&sa, &sl);
    s->port = ntohs(sa.sin_port);
    s->listener_open = true;
    pthread_create(&s->accept_thread, NULL, srv_accept, s);
    return s;
}

static void server_close_listener(fake_server *s) {
    if (!s->listener_open) return;
    atomic_store(&s->stop_accept, true);
    pthread_join(s->accept_thread, NULL);
    close(s->listen_fd);
    s->listener_open = false;
}

static void server_stop(fake_server *s) {
    server_close_listener(s);
    atomic_store(&s->stop, true);
    double end = now_ms() + 5000;
    while (atomic_load(&s->active) > 0 && now_ms() < end) sleep_ms(5);
    CHECK(atomic_load(&s->active) == 0);
    for (int i = 0; i < s->nlog; i++) free(s->log[i]);
    pthread_mutex_destroy(&s->mu);
    free(s);
}

/* --- client helpers ------------------------------------------------------ */

static cwist_redis_reply_t g_sentinel;

static int ecode(cwist_error_t e) {
    return e.errtype == CWIST_ERR_INT16 ? (int)e.error.err_i16 : INT_MIN;
}

static int cmdv(cwist_redis_pool_t *p, unsigned timeout, cwist_redis_reply_t **reply, size_t argc,
                const char **argv) {
    size_t lens[MAX_ARGS];
    for (size_t i = 0; i < argc; i++) lens[i] = strlen(argv[i]);
    *reply = &g_sentinel;
    cwist_error_t e = cwist_redis_pool_command_argv_reply_timeout(
        p, argc, (const void *const *)argv, lens, timeout, reply);
    return ecode(e);
}

#define CMD(p, t, r, ...)                                                             \
    cmdv((p), (t), (r), sizeof((const char *[]){__VA_ARGS__}) / sizeof(const char *), \
         (const char *[]){__VA_ARGS__})

static int raw_cmd(cwist_redis_pool_t *p, unsigned timeout, const char *bytes, size_t len,
                   cwist_redis_reply_t **reply, const char *verb) {
    const void *argv[] = {verb, bytes};
    size_t lens[] = {strlen(verb), len};
    *reply = &g_sentinel;
    return ecode(cwist_redis_pool_command_argv_reply_timeout(p, 2, argv, lens, timeout, reply));
}

static bool is_bulk(const cwist_redis_reply_t *r, const char *s) {
    return r && r != &g_sentinel && r->type == '$' && r->str && r->len == strlen(s) &&
           memcmp(r->str, s, r->len) == 0;
}

static void reply_drop(cwist_redis_reply_t *r) {
    if (r != &g_sentinel) cwist_redis_reply_free(r);
}

/* Run ECHO tag and expect it back. */
static bool echo_ok(cwist_redis_pool_t *p, const char *tag) {
    cwist_redis_reply_t *r = NULL;
    int c = CMD(p, 2000, &r, "ECHO", tag);
    bool ok = c == 0 && is_bulk(r, tag);
    if (!ok) fprintf(stderr, "  echo %s -> code %d\n", tag, c);
    reply_drop(r);
    return ok;
}

typedef struct legacy_job {
    cwist_redis_pool_t *p;
    const char *argv[3];
    size_t argc;
    char *out;
    int code;
    atomic_bool done;
} legacy_job;

static void *legacy_thread(void *arg) {
    legacy_job *j = arg;
    size_t lens[3];
    for (size_t i = 0; i < j->argc; i++) lens[i] = strlen(j->argv[i]);
    size_t out_len = 0;
    cwist_error_t e = cwist_redis_pool_command_argv(j->p, j->argc, (const void *const *)j->argv,
                                                    lens, &j->out, &out_len);
    j->code = ecode(e);
    atomic_store(&j->done, true);
    return NULL;
}

/* --- tests --------------------------------------------------------------- */

static void test_invalid_inputs(void) {
    fake_server *s = server_start();
    cwist_redis_pool_t *p = cwist_redis_pool_create("127.0.0.1", s->port, 1);
    CHECK(p != NULL);
    const void *argv[] = {"ECHO", "x"};
    size_t lens[] = {4, 1};
    cwist_redis_reply_t *r = NULL;
#define EXPECT_EINVAL(call)          \
    do {                             \
        r = &g_sentinel;             \
        cwist_error_t e_ = (call);   \
        CHECK(ecode(e_) == -EINVAL); \
        CHECK(r == NULL);            \
    } while (0)
    EXPECT_EINVAL(cwist_redis_pool_command_argv_reply_timeout(NULL, 2, argv, lens, 100, &r));
    EXPECT_EINVAL(cwist_redis_pool_command_argv_reply_timeout(p, 0, argv, lens, 100, &r));
    EXPECT_EINVAL(cwist_redis_pool_command_argv_reply_timeout(p, 2, NULL, lens, 100, &r));
    EXPECT_EINVAL(cwist_redis_pool_command_argv_reply_timeout(p, 2, argv, NULL, 100, &r));
    EXPECT_EINVAL(cwist_redis_pool_command_argv_reply_timeout(p, 2, argv, lens, 0, &r));
    EXPECT_EINVAL(
        cwist_redis_pool_command_argv_reply_timeout(p, (size_t)1 << 40, argv, lens, 100, &r));
    {
        const void *nv[] = {"ECHO", NULL};
        size_t nl[] = {4, 3};
        EXPECT_EINVAL(cwist_redis_pool_command_argv_reply_timeout(p, 2, nv, nl, 100, &r));
    }
    {
        size_t huge[] = {4, SIZE_MAX};
        EXPECT_EINVAL(cwist_redis_pool_command_argv_reply_timeout(p, 2, argv, huge, 100, &r));
    }
    {
        size_t ov[] = {SIZE_MAX / 2 + 1, SIZE_MAX / 2 + 1};
        EXPECT_EINVAL(cwist_redis_pool_command_argv_reply_timeout(p, 2, argv, ov, 100, &r));
    }
#undef EXPECT_EINVAL
    CHECK(ecode(cwist_redis_pool_command_argv_reply_timeout(p, 2, argv, lens, 100, NULL)) ==
          -EINVAL);
    sleep_ms(50);
    CHECK(log_count(s, "", true) == 0); /* nothing reached the peer */

    /* A NULL pointer with zero length is a valid empty argument. */
    const void *ev[] = {"ECHO", NULL};
    size_t el[] = {4, 0};
    r = &g_sentinel;
    CHECK(ecode(cwist_redis_pool_command_argv_reply_timeout(p, 2, ev, el, 1000, &r)) == 0);
    CHECK(r && r != &g_sentinel && r->type == '$' && r->str && r->len == 0);
    reply_drop(r);
    cwist_redis_pool_destroy(p);
    server_stop(s);
}

static void test_typed_replies(void) {
    fake_server *s = server_start();
    cwist_redis_pool_t *p = cwist_redis_pool_create("127.0.0.1", s->port, 1);
    CHECK(p != NULL);
    int base = wait_accepted(s, 1, 1000);
    cwist_redis_reply_t *r = NULL;

    CHECK(CMD(p, 1000, &r, "ECHO", "hello") == 0);
    CHECK(is_bulk(r, "hello"));
    reply_drop(r);

    CHECK(raw_cmd(p, 1000, ":42\r\n", 5, &r, "RAW") == 0);
    CHECK(r && r != &g_sentinel && r->type == ':' && r->integer == 42);
    reply_drop(r);

    const char *mn = ":-9223372036854775808\r\n";
    CHECK(raw_cmd(p, 1000, mn, strlen(mn), &r, "RAW") == 0);
    CHECK(r && r != &g_sentinel && r->type == ':' && r->integer == LLONG_MIN);
    reply_drop(r);

    CHECK(raw_cmd(p, 1000, "+OK\r\n", 5, &r, "RAW") == 0);
    CHECK(r && r != &g_sentinel && r->type == '+' && r->str && strcmp(r->str, "OK") == 0 &&
          r->len == 2);
    reply_drop(r);

    CHECK(raw_cmd(p, 1000, "$3\r\na\0b\r\n", 9, &r, "RAW") == 0);
    CHECK(r && r != &g_sentinel && r->type == '$' && r->len == 3 && r->str &&
          memcmp(r->str, "a\0b", 3) == 0 && r->str[3] == '\0');
    reply_drop(r);

    CHECK(raw_cmd(p, 1000, "$-1\r\n", 5, &r, "RAW") == 0);
    CHECK(r && r != &g_sentinel && r->type == '$' && r->str == NULL && r->integer == -1);
    reply_drop(r);

    CHECK(raw_cmd(p, 1000, "*-1\r\n", 5, &r, "RAW") == 0);
    CHECK(r && r != &g_sentinel && r->type == '*' && r->elements == 0 && r->element == NULL &&
          r->integer == -1);
    reply_drop(r);

    CHECK(raw_cmd(p, 1000, "*0\r\n", 4, &r, "RAW") == 0);
    CHECK(r && r != &g_sentinel && r->type == '*' && r->elements == 0 && r->integer == 0);
    reply_drop(r);

    const char *nested = "*3\r\n:1\r\n-ERR inner\r\n*1\r\n$1\r\nx\r\n";
    CHECK(raw_cmd(p, 1000, nested, strlen(nested), &r, "RAW") == 0);
    CHECK(r && r != &g_sentinel && r->type == '*' && r->elements == 3);
    if (r && r != &g_sentinel && r->elements == 3) {
        CHECK(r->element[0]->type == ':' && r->element[0]->integer == 1);
        CHECK(r->element[1]->type == '-' && strcmp(r->element[1]->str, "ERR inner") == 0);
        CHECK(r->element[2]->type == '*' && r->element[2]->elements == 1 &&
              is_bulk(r->element[2]->element[0], "x"));
    }
    reply_drop(r);

    /* A top-level Redis error reply fails with -EBADMSG and NULL reply, but
     * the connection is still in sync and is reused (no reconnect). */
    CHECK(raw_cmd(p, 1000, "-ERR boom\r\n", 11, &r, "RAW") == -EBADMSG);
    CHECK(r == NULL);
    CHECK(echo_ok(p, "after-error"));
    sleep_ms(30);
    CHECK(atomic_load(&s->accepted) == base);

    cwist_redis_pool_destroy(p);
    server_stop(s);
}

static void test_stall_deadline_and_stale_reply(void) {
    fake_server *s = server_start();
    cwist_redis_pool_t *p = cwist_redis_pool_create("127.0.0.1", s->port, 1);
    CHECK(p != NULL);
    int base = wait_accepted(s, 1, 1000);
    cwist_redis_reply_t *r = NULL;

    double t0 = now_ms();
    int c = CMD(p, 150, &r, "STALL", "1200", "late-a");
    double el = now_ms() - t0;
    CHECK(c == -ETIMEDOUT);
    CHECK(r == NULL);
    CHECK(el >= 140 && el < 450);
    fprintf(stderr, "  stall: code %d after %.1f ms\n", c, el);

    /* The timed-out connection must be discarded: the next command gets its
     * own reply on a fresh connection, never the late "late-a". */
    CHECK(echo_ok(p, "b"));
    CHECK(wait_accepted(s, base + 1, 1000) == base + 1);

    /* A slot discarded by the timed API stays usable by the legacy API. */
    CHECK(CMD(p, 100, &r, "STALL", "1000", "late-l") == -ETIMEDOUT);
    {
        legacy_job lj = {.p = p, .argv = {"ECHO", "legacy-after"}, .argc = 2};
        legacy_thread(&lj);
        CHECK(lj.out && strcmp(lj.out, "legacy-after") == 0);
        cwist_free(lj.out);
    }

    /* Even after the late replies have been written by the peer. */
    sleep_ms(1250);
    CHECK(echo_ok(p, "c"));
    CHECK(log_count(s, "STALL late-a", false) == 1); /* sent once, never retried */

    /* EOF after the command was sent is ambiguous: fail, never retry. */
    CHECK(CMD(p, 1000, &r, "CLOSE", "once") == -EIO);
    CHECK(r == NULL);
    sleep_ms(30);
    CHECK(log_count(s, "CLOSE once", false) == 1);
    CHECK(echo_ok(p, "after-close"));

    cwist_redis_pool_destroy(p);
    server_stop(s);
}

static void test_trickle_deadline(void) {
    fake_server *s = server_start();
    cwist_redis_pool_t *p = cwist_redis_pool_create("127.0.0.1", s->port, 1);
    CHECK(p != NULL);
    cwist_redis_reply_t *r = NULL;

    /* ~23 bytes at 40 ms/byte: every recv makes progress, so only a total
     * deadline (not a per-read timeout) can bound this. */
    double t0 = now_ms();
    int c = CMD(p, 200, &r, "TRICKLE", "40", "abcdefghijklmnop");
    double el = now_ms() - t0;
    CHECK(c == -ETIMEDOUT);
    CHECK(r == NULL);
    CHECK(el >= 190 && el < 500);
    fprintf(stderr, "  trickle: code %d after %.1f ms\n", c, el);

    /* A trickle that completes within the budget succeeds. */
    CHECK(CMD(p, 2000, &r, "TRICKLE", "5", "xyz") == 0);
    CHECK(is_bulk(r, "xyz"));
    reply_drop(r);
    CHECK(echo_ok(p, "after-trickle"));

    cwist_redis_pool_destroy(p);
    server_stop(s);
}

static void test_write_deadline(void) {
    fake_server *s = server_start();
    cwist_redis_pool_t *p = cwist_redis_pool_create("127.0.0.1", s->port, 1);
    CHECK(p != NULL);
    size_t big_len = (size_t)16 * 1024 * 1024;
    char *big = malloc(big_len);
    memset(big, 'w', big_len);
    cwist_redis_reply_t *r = NULL;

    atomic_store(&s->mode, MODE_NOREAD);
    double t0 = now_ms();
    int c = raw_cmd(p, 200, big, big_len, &r, "ECHO");
    double el = now_ms() - t0;
    atomic_store(&s->mode, MODE_NORMAL);
    CHECK(c == -ETIMEDOUT);
    CHECK(r == NULL);
    CHECK(el >= 190 && el < 500);
    fprintf(stderr, "  write stall: code %d after %.1f ms\n", c, el);

    /* The partially written frame must never be completed or reused. */
    CHECK(echo_ok(p, "after-write-stall"));
    sleep_ms(50);
    CHECK(log_count(s, "ECHO www", true) == 0);
    free(big);
    cwist_redis_pool_destroy(p);
    server_stop(s);
}

static void test_sigpipe_contained(void) {
    fake_server *s = server_start();
    cwist_redis_pool_t *p = cwist_redis_pool_create("127.0.0.1", s->port, 1);
    CHECK(p != NULL);
    size_t big_len = (size_t)16 * 1024 * 1024;
    char *big = malloc(big_len);
    memset(big, 'p', big_len);
    cwist_redis_reply_t *r = NULL;

    /* SIGPIPE has its default (fatal) disposition here; the peer resets the
     * connection mid-write, so an uncontained EPIPE would kill the test. */
    atomic_store(&s->mode, MODE_RST_ON_READ);
    int c = raw_cmd(p, 3000, big, big_len, &r, "ECHO");
    atomic_store(&s->mode, MODE_NORMAL);
    CHECK(c == -EIO);
    CHECK(r == NULL);
    fprintf(stderr, "  reset mid-write: code %d (process alive)\n", c);
    CHECK(echo_ok(p, "after-reset"));
    free(big);
    cwist_redis_pool_destroy(p);
    server_stop(s);
}

static void test_pool_saturation(void) {
    fake_server *s = server_start();
    cwist_redis_pool_t *p = cwist_redis_pool_create("127.0.0.1", s->port, 1);
    CHECK(p != NULL);
    cwist_redis_reply_t *r = NULL;

    /* A legacy caller holds the only connection. */
    legacy_job a = {.p = p, .argv = {"STALL", "700", "holdA"}, .argc = 3};
    pthread_t ta;
    pthread_create(&ta, NULL, legacy_thread, &a);
    CHECK(wait_log(s, "STALL holdA", 1000));

    double t0 = now_ms();
    int c = CMD(p, 120, &r, "ECHO", "tagT");
    double el = now_ms() - t0;
    CHECK(c == -ETIMEDOUT);
    CHECK(r == NULL);
    CHECK(el >= 110 && el < 200); /* acquisition wait is not timer-coalesced */
    fprintf(stderr, "  saturated acquire: code %d after %.1f ms\n", c, el);

    pthread_join(ta, NULL);
    CHECK(a.out && strcmp(a.out, "holdA") == 0);
    cwist_free(a.out);
    sleep_ms(30);
    CHECK(log_count(s, "ECHO tagT", false) == 0); /* never sent */

    /* A waiter whose budget outlasts the holder gets the slot. */
    legacy_job b = {.p = p, .argv = {"STALL", "150", "holdB"}, .argc = 3};
    pthread_t tb;
    pthread_create(&tb, NULL, legacy_thread, &b);
    CHECK(wait_log(s, "STALL holdB", 1000));
    t0 = now_ms();
    c = CMD(p, 3000, &r, "ECHO", "tagW");
    el = now_ms() - t0;
    CHECK(c == 0 && is_bulk(r, "tagW"));
    CHECK(el >= 80);
    reply_drop(r);
    pthread_join(tb, NULL);
    cwist_free(b.out);

    /* Timed waiters giving up must not strand a legacy waiter. */
    legacy_job h = {.p = p, .argv = {"STALL", "300", "holdC"}, .argc = 3};
    legacy_job w = {.p = p, .argv = {"ECHO", "tagL"}, .argc = 2};
    pthread_t th, tw;
    pthread_create(&th, NULL, legacy_thread, &h);
    CHECK(wait_log(s, "STALL holdC", 1000));
    pthread_create(&tw, NULL, legacy_thread, &w);
    sleep_ms(20);
    for (int i = 0; i < 3; i++) {
        double tx = now_ms();
        c = CMD(p, 40, &r, "ECHO", "tagX");
        double ex = now_ms() - tx;
        if (c != -ETIMEDOUT || ex >= 120)
            fprintf(stderr, "  timed waiter %d: code %d after %.1f ms\n", i, c, ex);
        CHECK(c == -ETIMEDOUT);
        CHECK(ex >= 35 && ex < 120);
        CHECK(r == NULL);
        reply_drop(r);
    }
    pthread_join(th, NULL);
    double end = now_ms() + 2000;
    while (!atomic_load(&w.done) && now_ms() < end) sleep_ms(5);
    CHECK(atomic_load(&w.done));
    if (atomic_load(&w.done)) {
        pthread_join(tw, NULL);
        CHECK(w.out && strcmp(w.out, "tagL") == 0);
        cwist_free(w.out);
    }
    cwist_free(h.out);
    CHECK(echo_ok(p, "after-saturation"));
    cwist_redis_pool_destroy(p);
    server_stop(s);
}

static void test_no_wake_pipe_deadline(void) {
    fake_server *s = server_start();
    atomic_store(&g_pipe_failure, true);
    cwist_redis_pool_t *p = cwist_redis_pool_create("127.0.0.1", s->port, 1);
    atomic_store(&g_pipe_failure, false);
    CHECK(p != NULL);
    legacy_job job = {.p = p, .argv = {"STALL", "500", "noPipe"}, .argc = 3};
    pthread_t thread;
    pthread_create(&thread, NULL, legacy_thread, &job);
    CHECK(wait_log(s, "STALL noPipe", 1000));
    double start = now_ms();
    cwist_redis_reply_t *reply = NULL;
    CHECK(CMD(p, 120, &reply, "ECHO", "timed") == -ETIMEDOUT);
    double elapsed = now_ms() - start;
    fprintf(stderr, "  no wake pipe: %.1f ms\n", elapsed);
    CHECK(elapsed >= 110 && elapsed < 200);
    CHECK(reply == NULL);
    pthread_join(thread, NULL);
    cwist_free(job.out);
    cwist_redis_pool_destroy(p);
    server_stop(s);
}

static void test_reconnect_cached_peer(void) {
    fake_server *s = server_start();
    atomic_store(&g_dns_calls, 0);
    cwist_redis_pool_t *p = cwist_redis_pool_create("localhost", s->port, 2);
    CHECK(p != NULL);
    CHECK(atomic_load(&g_dns_calls) > 0); /* interposer is live */
    int base = wait_accepted(s, 2, 1000);
    CHECK(base == 2);
    cwist_redis_reply_t *r = NULL;

    /* From here on any resolver call fails and is counted. */
    atomic_store(&g_dns_calls, 0);
    atomic_store(&g_dns_block, true);

    CHECK(CMD(p, 1000, &r, "REPLYCLOSE", "r1") == 0);
    CHECK(is_bulk(r, "r1"));
    reply_drop(r);
    sleep_ms(50); /* peer FIN reaches the idle pooled connection */

    for (int i = 0; i < 4; i++) CHECK(echo_ok(p, "x2"));
    CHECK(wait_accepted(s, base + 1, 1000) == base + 1);
    sleep_ms(30);
    CHECK(atomic_load(&s->accepted) == base + 1);
    CHECK(atomic_load(&g_dns_calls) == 0);
    atomic_store(&g_dns_block, false);
    cwist_redis_pool_destroy(p);
    server_stop(s);
}

static void test_reconnect_refused_is_bounded(void) {
    fake_server *s = server_start();
    cwist_redis_pool_t *p = cwist_redis_pool_create("127.0.0.1", s->port, 1);
    CHECK(p != NULL);
    cwist_redis_reply_t *r = NULL;
    CHECK(CMD(p, 1000, &r, "REPLYCLOSE", "bye") == 0);
    reply_drop(r);
    server_close_listener(s);
    sleep_ms(50);
    double t0 = now_ms();
    int c = CMD(p, 500, &r, "ECHO", "nobody");
    double el = now_ms() - t0;
    CHECK(c == -EIO);
    CHECK(r == NULL);
    CHECK(el < 500);
    fprintf(stderr, "  refused reconnect: code %d after %.1f ms\n", c, el);
    cwist_redis_pool_destroy(p);
    server_stop(s);
}

static void test_unsolicited_bytes_never_leak(void) {
    fake_server *s = server_start();
    cwist_redis_pool_t *p = cwist_redis_pool_create("127.0.0.1", s->port, 1);
    CHECK(p != NULL);
    int base = wait_accepted(s, 1, 1000);
    cwist_redis_reply_t *r = NULL;

    CHECK(CMD(p, 1000, &r, "UNSOL", "u1", ":666\r\n") == 0);
    CHECK(is_bulk(r, "u1"));
    reply_drop(r);
    sleep_ms(80); /* unsolicited ":666" lands on the idle connection */
    CHECK(CMD(p, 1000, &r, "ECHO", "u2") == 0);
    CHECK(is_bulk(r, "u2"));
    reply_drop(r);
    CHECK(wait_accepted(s, base + 1, 1000) == base + 1);

    /* Two replies for one command: the extra one must never be returned to a
     * later caller. */
    CHECK(CMD(p, 1000, &r, "DOUBLE", "d1") == 0);
    CHECK(is_bulk(r, "d1"));
    reply_drop(r);
    sleep_ms(30);
    CHECK(CMD(p, 1000, &r, "ECHO", "d2") == 0);
    CHECK(is_bulk(r, "d2"));
    reply_drop(r);
    CHECK(echo_ok(p, "d3"));
    cwist_redis_pool_destroy(p);
    server_stop(s);
}

static void test_malformed_replies(void) {
    fake_server *s = server_start();
    cwist_redis_pool_t *p = cwist_redis_pool_create("127.0.0.1", s->port, 1);
    CHECK(p != NULL);
    char deep[256] = {0};
    for (int i = 0; i < 20; i++) strcat(deep, "*1\r\n");
    strcat(deep, ":1\r\n");
    char longline[9100];
    longline[0] = '+';
    memset(longline + 1, 'a', 9000);
    memcpy(longline + 9001, "\r\n", 3);
    struct {
        const char *bytes;
        size_t len;
    } cases[] = {
        {"?x\r\n", 4},
        {":12a\r\n", 6},
        {":\r\n", 3},
        {": 1\r\n", 5},
        {":99999999999999999999\r\n", 23},
        {"$-2\r\n", 5},
        {"*-2\r\n", 5},
        {"$abc\r\n", 6},
        {"$2\r\nabXY", 8},
        {"*70000\r\n", 8},
        {"$999999999\r\n", 12},
        {deep, strlen(deep)},
        {longline, strlen(longline)},
        {"\r\n", 2},
        {":1\n", 3},
        {"+a\rb\r\n", 6},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        cwist_redis_reply_t *r = NULL;
        double t0 = now_ms();
        int c = raw_cmd(p, 1000, cases[i].bytes, cases[i].len, &r, "RAW");
        double el = now_ms() - t0;
        if (c != -EPROTO) fprintf(stderr, "  malformed case %zu -> code %d\n", i, c);
        CHECK(c == -EPROTO);
        CHECK(r == NULL);
        CHECK(el < 500);
        if (r != &g_sentinel) reply_drop(r);
        CHECK(echo_ok(p, "after-malformed"));
    }
    /* Peer EOF in the middle of a reply. */
    cwist_redis_reply_t *r = NULL;
    CHECK(raw_cmd(p, 1000, "$5\r\nab", 6, &r, "RAWCLOSE") == -EIO);
    CHECK(r == NULL);
    CHECK(echo_ok(p, "after-eof"));
    cwist_redis_pool_destroy(p);
    server_stop(s);
}

static void test_inherited_pool_rejected(void) {
    fake_server *s = server_start();
    cwist_redis_pool_t *p = cwist_redis_pool_create("127.0.0.1", s->port, 1);
    CHECK(p != NULL);
    fflush(NULL);
    pid_t pid = fork();
    if (pid == 0) {
        const void *argv[] = {"ECHO", "childtag"};
        size_t lens[] = {4, 8};
        cwist_redis_reply_t *r = &g_sentinel;
        cwist_error_t e = cwist_redis_pool_command_argv_reply_timeout(p, 2, argv, lens, 200, &r);
        _exit(ecode(e) == -EPERM && r == NULL ? 0 : 1);
    }
    int st = 0;
    CHECK(pid > 0 && waitpid(pid, &st, 0) == pid);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    sleep_ms(50);
    CHECK(log_count(s, "ECHO childtag", false) == 0);
    CHECK(echo_ok(p, "parent-still-works"));
    cwist_redis_pool_destroy(p);
    server_stop(s);
}

typedef struct cc_job {
    cwist_redis_pool_t *p;
    int id;
    int mismatches;
    int errors;
    int timeouts;
} cc_job;

static void *cc_thread(void *arg) {
    cc_job *j = arg;
    char tag[32];
    for (int i = 0; i < 40; i++) {
        snprintf(tag, sizeof(tag), "t%d-%d", j->id, i);
        cwist_redis_reply_t *r = NULL;
        if (i % 7 == 3) {
            int c = CMD(j->p, 40, &r, "STALL", "200", tag);
            if (c == -ETIMEDOUT && r == NULL)
                j->timeouts++;
            else
                j->errors++;
            if (c == 0) reply_drop(r);
        } else {
            int c = CMD(j->p, 3000, &r, "ECHO", tag);
            if (c != 0)
                j->errors++;
            else if (!is_bulk(r, tag))
                j->mismatches++;
            if (c == 0) reply_drop(r);
        }
    }
    return NULL;
}

static void test_concurrent_no_crosstalk(void) {
    fake_server *s = server_start();
    cwist_redis_pool_t *p = cwist_redis_pool_create("127.0.0.1", s->port, 3);
    CHECK(p != NULL);
    enum { N = 6 };
    pthread_t t[N];
    cc_job jobs[N];
    for (int i = 0; i < N; i++) {
        jobs[i] = (cc_job){.p = p, .id = i};
        pthread_create(&t[i], NULL, cc_thread, &jobs[i]);
    }
    int mism = 0, errs = 0, tos = 0;
    for (int i = 0; i < N; i++) {
        pthread_join(t[i], NULL);
        mism += jobs[i].mismatches;
        errs += jobs[i].errors;
        tos += jobs[i].timeouts;
    }
    fprintf(stderr, "  concurrent: mismatches %d errors %d timeouts %d\n", mism, errs, tos);
    CHECK(mism == 0);
    CHECK(errs == 0);
    CHECK(tos == N * 6);
    cwist_redis_pool_destroy(p);
    server_stop(s);
}

static const char *g_filter;

#define RUN(fn)                                                                                    \
    do {                                                                                           \
        if (g_filter && !strstr(#fn, g_filter)) break;                                             \
        int before_ = g_failures;                                                                  \
        fprintf(stderr, "[redis-timeout] %s\n", #fn);                                              \
        fn();                                                                                      \
        fprintf(stderr, "[redis-timeout] %s: %s\n", #fn, g_failures == before_ ? "ok" : "FAILED"); \
    } while (0)

/* Optional argv[1]: run only tests whose name contains it. */
int main(int argc, char **argv) {
    g_filter = argc > 1 ? argv[1] : NULL;
    signal(SIGPIPE, SIG_DFL);
    alarm(180);
    dns_init();
    RUN(test_invalid_inputs);
    RUN(test_typed_replies);
    RUN(test_stall_deadline_and_stale_reply);
    RUN(test_trickle_deadline);
    RUN(test_write_deadline);
    RUN(test_sigpipe_contained);
    RUN(test_pool_saturation);
    RUN(test_no_wake_pipe_deadline);
    RUN(test_reconnect_cached_peer);
    RUN(test_reconnect_refused_is_bounded);
    RUN(test_unsolicited_bytes_never_leak);
    RUN(test_malformed_replies);
    RUN(test_inherited_pool_rejected);
    RUN(test_concurrent_no_crosstalk);
    if (g_failures) {
        fprintf(stderr, "test_redis_timeout: %d check(s) failed\n", g_failures);
        return 1;
    }
    printf("test_redis_timeout: all checks passed\n");
    return 0;
}
