#define _POSIX_C_SOURCE 200809L

#include <cwist/core/mem/alloc.h>
#include <cwist/core/mem/gc.h>
#include <cwist/net/proxy/proxy.h>
#include <cwist/sys/app/shutdown.h>
#include "transport_internal.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define PROXY_TRANSPORT_CONNECT_TIMEOUT_MS 5000
#define PROXY_TRANSPORT_IDLE_TIMEOUT_MS 120000
#define PROXY_TRANSPORT_POLL_MS 100
#define PROXY_UDP_MAX_SESSIONS 1024
#define PROXY_UDP_MAX_DGRAM 65535

typedef struct proxy_transport_rule {
    int transport;
    unsigned short origin_port;
    char *target_host;
    unsigned short target_port;
    int listen_fd;
    pthread_t thread;
    bool thread_started;
    struct proxy_transport_rule *next;
} proxy_transport_rule;

typedef struct proxy_tcp_conn {
    int client_fd;
    char *target_host;
    unsigned short target_port;
} proxy_tcp_conn;

typedef struct proxy_udp_session {
    int fd;
    struct sockaddr_storage client;
    socklen_t client_len;
    uint64_t last_active_ms;
    struct proxy_udp_session *next;
} proxy_udp_session;

static pthread_mutex_t g_transport_lock = PTHREAD_MUTEX_INITIALIZER;
static proxy_transport_rule *g_transport_rules;
static size_t g_transport_rule_count;
static atomic_int g_transport_running;
static int g_transport_connect_timeout_ms = PROXY_TRANSPORT_CONNECT_TIMEOUT_MS;

static uint64_t proxy_transport_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static bool proxy_transport_active(void) {
    return atomic_load(&g_transport_running) && atomic_load(&g_cwist_running);
}

static void proxy_transport_set_nonblock(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl >= 0) (void)fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

static int proxy_transport_listener(int transport, unsigned short port) {
    char service[8];
    snprintf(service, sizeof(service), "%hu", port);
    struct addrinfo hints = {
        .ai_family = AF_UNSPEC,
        .ai_socktype = transport == CWIST_PROXY_TRANSPORT_TCP ? SOCK_STREAM : SOCK_DGRAM,
        .ai_flags = AI_PASSIVE,
    };
    struct addrinfo *addrs = NULL;
    if (getaddrinfo(NULL, service, &hints, &addrs) != 0) return -1;
    int fd = -1;
    for (struct addrinfo *ai = addrs; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        int one = 1;
        (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        if (ai->ai_family == AF_INET6) {
            int zero = 0;
            (void)setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof(zero));
        }
        proxy_transport_set_nonblock(fd);
        if (bind(fd, ai->ai_addr, ai->ai_addrlen) == 0 &&
            (transport != CWIST_PROXY_TRANSPORT_TCP || listen(fd, 128) == 0)) {
            break;
        }
        close(fd);
        fd = -1;
    }
    freeaddrinfo(addrs);
    return fd;
}

static int proxy_transport_upstream(const char *target_host, unsigned short target_port,
                                    int transport) {
    char service[8];
    snprintf(service, sizeof(service), "%hu", target_port);
    struct addrinfo hints = {
        .ai_family = AF_UNSPEC,
        .ai_socktype = transport == CWIST_PROXY_TRANSPORT_TCP ? SOCK_STREAM : SOCK_DGRAM,
    };
    struct addrinfo *addrs = NULL;
    if (getaddrinfo(target_host, service, &hints, &addrs) != 0) return -1;
    int fd = -1;
    for (struct addrinfo *ai = addrs; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        proxy_transport_set_nonblock(fd);
        if (transport == CWIST_PROXY_TRANSPORT_UDP) {
            if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) break;
        } else if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0 || errno == EINPROGRESS) {
            struct pollfd pfd = {.fd = fd, .events = POLLOUT};
            int rc;
            do {
                rc = poll(&pfd, 1, g_transport_connect_timeout_ms);
            } while (rc < 0 && errno == EINTR);
            int error = 0;
            socklen_t error_len = sizeof(error);
            if (rc > 0 && getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &error_len) == 0 &&
                error == 0) {
                break;
            }
        }
        close(fd);
        fd = -1;
    }
    freeaddrinfo(addrs);
    return fd;
}

static bool proxy_transport_send_all(int fd, const unsigned char *buf, size_t len) {
    size_t off = 0;
    uint64_t deadline = proxy_transport_now_ms() + PROXY_TRANSPORT_IDLE_TIMEOUT_MS;
    while (off < len && proxy_transport_active()) {
        ssize_t n = send(fd, buf + off, len - off, MSG_NOSIGNAL);
        if (n > 0) {
            off += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            uint64_t now = proxy_transport_now_ms();
            if (now >= deadline) return false;
            uint64_t remaining = deadline - now;
            int timeout =
                remaining > PROXY_TRANSPORT_POLL_MS ? PROXY_TRANSPORT_POLL_MS : (int)remaining;
            struct pollfd pfd = {.fd = fd, .events = POLLOUT};
            if (poll(&pfd, 1, timeout) < 0 && errno != EINTR) return false;
            continue;
        }
        return false;
    }
    return off == len;
}

static void *proxy_tcp_relay(void *arg) {
    proxy_tcp_conn *conn = arg;
    int client_fd = conn->client_fd;
    char *target_host = conn->target_host;
    unsigned short target_port = conn->target_port;
    cwist_free(conn);
    int upstream_fd = proxy_transport_upstream(target_host, target_port, CWIST_PROXY_TRANSPORT_TCP);
    cwist_free(target_host);
    if (upstream_fd < 0) {
        close(client_fd);
        return NULL;
    }
    proxy_transport_set_nonblock(client_fd);
    bool client_open = true;
    bool upstream_open = true;
    unsigned char buf[16384];
    while (upstream_open && proxy_transport_active()) {
        struct pollfd fds[2] = {
            {.fd = client_open ? client_fd : -1, .events = POLLIN},
            {.fd = upstream_fd, .events = POLLIN},
        };
        int rc = poll(fds, 2, PROXY_TRANSPORT_POLL_MS);
        if (rc < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (rc == 0) continue;
        if (client_open && (fds[0].revents & (POLLIN | POLLHUP | POLLERR))) {
            ssize_t n = recv(client_fd, buf, sizeof(buf), 0);
            if (n > 0) {
                if (!proxy_transport_send_all(upstream_fd, buf, (size_t)n)) break;
            } else if (n == 0) {
                client_open = false;
                (void)shutdown(upstream_fd, SHUT_WR);
            } else if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
                break;
            }
        }
        if (upstream_open && (fds[1].revents & (POLLIN | POLLHUP | POLLERR))) {
            ssize_t n = recv(upstream_fd, buf, sizeof(buf), 0);
            if (n > 0) {
                if (!proxy_transport_send_all(client_fd, buf, (size_t)n)) break;
            } else if (n == 0) {
                upstream_open = false;
                (void)shutdown(client_fd, SHUT_WR);
            } else if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
                break;
            }
        }
    }
    close(client_fd);
    close(upstream_fd);
    return NULL;
}

static void *proxy_tcp_listener(void *arg) {
    proxy_transport_rule *rule = arg;
    while (proxy_transport_active()) {
        struct pollfd pfd = {.fd = rule->listen_fd, .events = POLLIN};
        int rc = poll(&pfd, 1, PROXY_TRANSPORT_POLL_MS);
        if (rc < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (rc == 0) continue;
        int client_fd = accept(rule->listen_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK || errno == ECONNABORTED)
                continue;
            break;
        }
        proxy_tcp_conn *conn = cwist_alloc(sizeof(*conn));
        if (!conn) {
            close(client_fd);
            continue;
        }
        conn->client_fd = client_fd;
        conn->target_host = cwist_strdup(rule->target_host);
        conn->target_port = rule->target_port;
        if (!conn->target_host) {
            close(client_fd);
            cwist_free(conn);
            continue;
        }
        cwist_gc_scope_disown(conn);
        cwist_gc_scope_disown(conn->target_host);
        pthread_t thread;
        if (pthread_create(&thread, NULL, proxy_tcp_relay, conn) != 0) {
            close(client_fd);
            cwist_free(conn->target_host);
            cwist_free(conn);
            continue;
        }
        pthread_detach(thread);
    }
    return NULL;
}

static bool proxy_udp_same_client(const proxy_udp_session *session,
                                  const struct sockaddr_storage *addr, socklen_t addr_len) {
    return session->client_len == addr_len && memcmp(&session->client, addr, addr_len) == 0;
}

static proxy_udp_session *proxy_udp_session_new(proxy_transport_rule *rule,
                                                const struct sockaddr_storage *client,
                                                socklen_t client_len, uint64_t now) {
    int fd =
        proxy_transport_upstream(rule->target_host, rule->target_port, CWIST_PROXY_TRANSPORT_UDP);
    if (fd < 0) return NULL;
    proxy_udp_session *session = cwist_alloc(sizeof(*session));
    if (!session) {
        close(fd);
        return NULL;
    }
    session->fd = fd;
    memcpy(&session->client, client, client_len);
    session->client_len = client_len;
    session->last_active_ms = now;
    session->next = NULL;
    return session;
}

static void proxy_udp_close_sessions(proxy_udp_session *session) {
    while (session) {
        proxy_udp_session *next = session->next;
        close(session->fd);
        cwist_free(session);
        session = next;
    }
}

static void *proxy_udp_listener(void *arg) {
    proxy_transport_rule *rule = arg;
    proxy_udp_session *sessions = NULL;
    size_t session_count = 0;
    unsigned char buf[PROXY_UDP_MAX_DGRAM];
    while (proxy_transport_active()) {
        struct pollfd fds[PROXY_UDP_MAX_SESSIONS + 1];
        proxy_udp_session *owners[PROXY_UDP_MAX_SESSIONS];
        size_t count = 0;
        fds[0] = (struct pollfd){.fd = rule->listen_fd, .events = POLLIN};
        for (proxy_udp_session *s = sessions; s && count < PROXY_UDP_MAX_SESSIONS; s = s->next) {
            owners[count] = s;
            fds[count + 1] = (struct pollfd){.fd = s->fd, .events = POLLIN};
            count++;
        }
        int rc = poll(fds, count + 1, PROXY_TRANSPORT_POLL_MS);
        uint64_t now = proxy_transport_now_ms();
        if (rc < 0 && errno != EINTR) break;
        if (rc > 0 && (fds[0].revents & (POLLIN | POLLERR))) {
            for (;;) {
                struct sockaddr_storage client;
                socklen_t client_len = sizeof(client);
                ssize_t n = recvfrom(rule->listen_fd, buf, sizeof(buf), 0,
                                     (struct sockaddr *)&client, &client_len);
                if (n < 0) {
                    if (errno == EINTR) continue;
                    break;
                }
                proxy_udp_session *session = sessions;
                proxy_udp_session *previous = NULL;
                while (session && !proxy_udp_same_client(session, &client, client_len)) {
                    previous = session;
                    session = session->next;
                }
                if (session) {
                    if (previous) {
                        previous->next = session->next;
                        session->next = sessions;
                        sessions = session;
                    }
                } else {
                    if (session_count >= PROXY_UDP_MAX_SESSIONS) {
                        proxy_udp_session **oldest = &sessions;
                        while ((*oldest)->next) oldest = &(*oldest)->next;
                        proxy_udp_session *victim = *oldest;
                        *oldest = NULL;
                        close(victim->fd);
                        cwist_free(victim);
                        session_count--;
                    }
                    session = proxy_udp_session_new(rule, &client, client_len, now);
                    if (session) {
                        session->next = sessions;
                        sessions = session;
                        session_count++;
                    }
                }
                if (session) {
                    session->last_active_ms = now;
                    (void)send(session->fd, buf, (size_t)n, MSG_NOSIGNAL);
                }
            }
        }
        for (size_t i = 0; i < count; i++) {
            if (!(fds[i + 1].revents & (POLLIN | POLLERR))) continue;
            proxy_udp_session *session = owners[i];
            ssize_t n = recv(session->fd, buf, sizeof(buf), 0);
            if (n > 0) {
                session->last_active_ms = now;
                (void)sendto(rule->listen_fd, buf, (size_t)n, MSG_NOSIGNAL,
                             (struct sockaddr *)&session->client, session->client_len);
            }
        }
        proxy_udp_session **link = &sessions;
        while (*link) {
            proxy_udp_session *session = *link;
            if (now - session->last_active_ms <= PROXY_TRANSPORT_IDLE_TIMEOUT_MS) {
                link = &session->next;
                continue;
            }
            *link = session->next;
            close(session->fd);
            cwist_free(session);
            session_count--;
        }
    }
    proxy_udp_close_sessions(sessions);
    return NULL;
}

cwist_error_t cwist_proxy_transport_add(int transport, unsigned short origin_port,
                                        const char *target_host, unsigned short target_port) {
    cwist_error_t err = make_error(CWIST_ERR_INT16);
    err.error.err_i16 = -1;
    if ((transport != CWIST_PROXY_TRANSPORT_TCP && transport != CWIST_PROXY_TRANSPORT_UDP) ||
        origin_port == 0 || target_port == 0 || !target_host || !target_host[0]) {
        return err;
    }
    proxy_transport_rule *rule = cwist_alloc(sizeof(*rule));
    if (!rule) return err;
    memset(rule, 0, sizeof(*rule));
    rule->listen_fd = -1;
    rule->target_host = cwist_alloc(strlen(target_host) + 1);
    if (!rule->target_host) {
        cwist_free(rule);
        return err;
    }
    strcpy(rule->target_host, target_host);
    rule->transport = transport;
    rule->origin_port = origin_port;
    rule->target_port = target_port;

    pthread_mutex_lock(&g_transport_lock);
    if (atomic_load(&g_transport_running)) {
        pthread_mutex_unlock(&g_transport_lock);
        cwist_free(rule->target_host);
        cwist_free(rule);
        return err;
    }
    for (proxy_transport_rule *existing = g_transport_rules; existing; existing = existing->next) {
        if (existing->transport == transport && existing->origin_port == origin_port) {
            pthread_mutex_unlock(&g_transport_lock);
            cwist_free(rule->target_host);
            cwist_free(rule);
            return err;
        }
    }
    rule->next = g_transport_rules;
    g_transport_rules = rule;
    g_transport_rule_count++;
    pthread_mutex_unlock(&g_transport_lock);
    err.error.err_i16 = 0;
    return err;
}

size_t cwist_proxy_transport_count(void) {
    pthread_mutex_lock(&g_transport_lock);
    size_t count = g_transport_rule_count;
    pthread_mutex_unlock(&g_transport_lock);
    return count;
}

int cwist_proxy_transport_start(void) {
    pthread_mutex_lock(&g_transport_lock);
    if (g_transport_rule_count == 0 || atomic_load(&g_transport_running)) {
        pthread_mutex_unlock(&g_transport_lock);
        return g_transport_rule_count == 0 ? 0 : -1;
    }
    g_transport_connect_timeout_ms = cwist_proxy.connect_timeout_ms > 0
                                         ? cwist_proxy.connect_timeout_ms
                                         : PROXY_TRANSPORT_CONNECT_TIMEOUT_MS;
    atomic_store(&g_transport_running, 1);
    proxy_transport_rule *failed = NULL;
    for (proxy_transport_rule *rule = g_transport_rules; rule; rule = rule->next) {
        rule->listen_fd = proxy_transport_listener(rule->transport, rule->origin_port);
        if (rule->listen_fd < 0) {
            failed = rule;
            break;
        }
        void *(*entry)(void *) =
            rule->transport == CWIST_PROXY_TRANSPORT_TCP ? proxy_tcp_listener : proxy_udp_listener;
        if (pthread_create(&rule->thread, NULL, entry, rule) != 0) {
            failed = rule;
            break;
        }
        rule->thread_started = true;
    }
    pthread_mutex_unlock(&g_transport_lock);
    if (!failed) return 0;
    cwist_proxy_transport_stop();
    return -1;
}

void cwist_proxy_transport_stop(void) {
    pthread_mutex_lock(&g_transport_lock);
    if (!atomic_load(&g_transport_running)) {
        pthread_mutex_unlock(&g_transport_lock);
        return;
    }
    atomic_store(&g_transport_running, 0);
    proxy_transport_rule *rules = g_transport_rules;
    for (proxy_transport_rule *rule = rules; rule; rule = rule->next) {
        if (rule->listen_fd >= 0) (void)shutdown(rule->listen_fd, SHUT_RDWR);
    }
    pthread_mutex_unlock(&g_transport_lock);
    for (proxy_transport_rule *rule = rules; rule; rule = rule->next) {
        if (rule->thread_started) {
            pthread_join(rule->thread, NULL);
            rule->thread_started = false;
        }
    }
    pthread_mutex_lock(&g_transport_lock);
    for (proxy_transport_rule *rule = rules; rule; rule = rule->next) {
        if (rule->listen_fd >= 0) {
            close(rule->listen_fd);
            rule->listen_fd = -1;
        }
    }
    pthread_mutex_unlock(&g_transport_lock);
}

void cwist_proxy_transport_reset(void) {
    pthread_mutex_lock(&g_transport_lock);
    proxy_transport_rule *rule = g_transport_rules;
    g_transport_rules = NULL;
    g_transport_rule_count = 0;
    pthread_mutex_unlock(&g_transport_lock);
    while (rule) {
        proxy_transport_rule *next = rule->next;
        cwist_free(rule->target_host);
        cwist_free(rule);
        rule = next;
    }
}
