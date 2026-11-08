/**
 * @file proxy.c
 * @brief Reverse proxy: rule table, request sanitizing and libcurl upstream.
 *
 * Front requests reach the proxy through the app router's internal fallback
 * hook, so a front app needs no routes. Every front protocol defers upstream
 * exchanges to a scheduler worker.
 */

#include <cwist/net/proxy/proxy.h>
#include <cwist/core/mem/alloc.h>
#include <cwist/core/sstring/sstring.h>
#include <cwist/net/http/async.h>
#include <cwist/net/http/http.h>
#include <cwist/sys/app/app.h>
#include <cwist/sys/job/scheduler.h>

#include "proxy_internal.h"
#include "../http/curl_global.h"
#include "../http/tls_info.h"
#include "../../sys/app/app_route_internal.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <curl/curl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

#define PROXY_DEFAULT_CONNECT_TIMEOUT_MS 5000
#define PROXY_DEFAULT_TIMEOUT_MS 30000
#define PROXY_DEFAULT_MAX_RESPONSE ((size_t)64 << 20)
#define PROXY_DEFAULT_WORKERS 8
#define PROXY_QUEUE_CAPACITY 4096
#define PROXY_MAX_CONN_TOKENS 16
#define PROXY_MAX_FRONT_APPS CWIST_MULTIPORT_MAX_PORTS
/* Upstream group list when post-quantum key exchange is required. One name
 * both OpenSSL 3.5+ and BoringSSL builds of libcurl understand. */
#define PROXY_PQ_GROUPS "X25519MLKEM768"

#define PROXY_DEFAULTS                                       \
    {.h2_enabled = false,                                    \
     .h3_enabled = false,                                    \
     .http_auto_promote = false,                             \
     .cert_path = NULL,                                      \
     .key_path = NULL,                                       \
     .preserve_host = false,                                 \
     .connect_timeout_ms = PROXY_DEFAULT_CONNECT_TIMEOUT_MS, \
     .timeout_ms = PROXY_DEFAULT_TIMEOUT_MS,                 \
     .max_response_bytes = PROXY_DEFAULT_MAX_RESPONSE,       \
     .workers = PROXY_DEFAULT_WORKERS,                       \
     .tls_auto_promote = false,                              \
     .upstream_ca_path = NULL}

cwist_proxy_config cwist_proxy = PROXY_DEFAULTS;

/* --- Rule table ------------------------------------------------------------ */

typedef struct proxy_rule {
    unsigned short port; /* origin port (explicit or the scheme default) */
    bool tls;            /* origin scheme is https */
    bool any_host;       /* cwist_proxy_port_forward(): every Host on the port */
    char *host;          /* lowercase origin host; NULL only when any_host */
    char *prefix;        /* "/" or "/a/b" (no trailing slash) */
    size_t prefix_len;
    char *target_base;   /* "http://host:port" */
    char *target_prefix; /* "" for "/", else "/v1" (no trailing slash) */
} proxy_rule;

static pthread_mutex_t g_proxy_rules_mu = PTHREAD_MUTEX_INITIALIZER;
static proxy_rule *g_proxy_rules;
static size_t g_proxy_rule_count;
static bool g_proxy_started;

/* Snapshot of cwist_proxy taken by cwist_proxy_listen(). */
static cwist_proxy_config g_proxy_cfg = PROXY_DEFAULTS;

static cwist_app *g_proxy_front_apps[PROXY_MAX_FRONT_APPS];
static size_t g_proxy_front_app_count;

static cwist_error_t proxy_result(int16_t code) {
    cwist_error_t err = make_error(CWIST_ERR_INT16);
    err.error.err_i16 = code;
    return err;
}

static char *proxy_strndup(const char *s, size_t n) {
    char *out = (char *)cwist_alloc(n + 1);
    if (!out) return NULL;
    memcpy(out, s, n);
    out[n] = '\0';
    return out;
}

static void proxy_rule_free(proxy_rule *r) {
    cwist_free(r->host);
    cwist_free(r->prefix);
    cwist_free(r->target_base);
    cwist_free(r->target_prefix);
    memset(r, 0, sizeof(*r));
}

static bool proxy_parse_port(const char *s, size_t len, unsigned short *out) {
    if (len == 0 || len > 5) return false;
    unsigned long v = 0;
    for (size_t i = 0; i < len; i++) {
        if (s[i] < '0' || s[i] > '9') return false;
        v = v * 10 + (unsigned long)(s[i] - '0');
    }
    if (v == 0 || v > 65535) return false;
    *out = (unsigned short)v;
    return true;
}

/* reg-name / IPv4 characters, or a bracketed IPv6 literal. */
static bool proxy_host_is_valid(const char *h, size_t len) {
    if (len == 0 || len > 253) return false;
    if (h[0] == '[') {
        if (h[len - 1] != ']' || len < 3) return false;
        for (size_t i = 1; i + 1 < len; i++) {
            char c = h[i];
            if (!(isxdigit((unsigned char)c) || c == ':' || c == '.')) return false;
        }
        return true;
    }
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)h[i];
        if (!(isalnum(c) || c == '-' || c == '.' || c == '_')) return false;
    }
    return true;
}

/*
 * Parse "scheme://host[:port][/]" the way a browser address bar shows it.
 * The scheme is required for origins (it picks TLS and the default port) and
 * optional for targets (http). No path, query, fragment or userinfo.
 */
static bool proxy_parse_authority(const char *url, bool require_scheme, bool *tls_out,
                                  char **host_out, unsigned short *port_out,
                                  size_t *authority_len_out, const char **authority_out) {
    bool tls = false;
    const char *rest = url;
    if (strncasecmp(url, "http://", 7) == 0) {
        rest = url + 7;
    } else if (strncasecmp(url, "https://", 8) == 0) {
        tls = true;
        rest = url + 8;
    } else if (require_scheme || strstr(url, "://")) {
        return false;
    }
    size_t len = strlen(rest);
    if (len > 0 && rest[len - 1] == '/') len--; /* tolerate "http://h:1/" */
    if (len == 0) return false;
    for (size_t i = 0; i < len; i++) {
        if (rest[i] == '/' || rest[i] == '?' || rest[i] == '#' || rest[i] == '@') return false;
    }
    const char *colon = NULL;
    if (rest[0] == '[') {
        const char *close = memchr(rest, ']', len);
        if (!close) return false;
        if ((size_t)(close - rest) + 1 < len) {
            if (close[1] != ':') return false;
            colon = close + 1;
        }
    } else {
        colon = memchr(rest, ':', len);
    }
    size_t hlen = colon ? (size_t)(colon - rest) : len;
    if (!proxy_host_is_valid(rest, hlen)) return false;
    unsigned short port = tls ? 443 : 80;
    if (colon && !proxy_parse_port(colon + 1, len - hlen - 1, &port)) return false;
    if (host_out) {
        char *h = proxy_strndup(rest, hlen);
        if (!h) return false;
        for (size_t i = 0; i < hlen; i++) h[i] = (char)tolower((unsigned char)h[i]);
        *host_out = h;
    }
    if (tls_out) *tls_out = tls;
    if (port_out) *port_out = port;
    if (authority_out) *authority_out = rest;
    if (authority_len_out) *authority_len_out = len;
    return true;
}

/* "http://host:port", "https://host:port" or "host:port" -> "scheme://host:port". */
static char *proxy_parse_target(const char *target) {
    bool tls = false;
    const char *authority = NULL;
    size_t len = 0;
    if (!proxy_parse_authority(target, false, &tls, NULL, NULL, &len, &authority)) return NULL;
    const char *scheme = tls ? "https" : "http";
    size_t out_len = strlen(scheme) + 3 + len;
    char *out = (char *)cwist_alloc(out_len + 1);
    if (!out) return NULL;
    snprintf(out, out_len + 1, "%s://%.*s", scheme, (int)len, authority);
    return out;
}

/* Normalize a configured route prefix: leading '/', no trailing '/' except root. */
static char *proxy_normalize_route(const char *route) {
    if (!route || route[0] != '/') return NULL;
    if (!cwist_proxy_path_is_safe(route) || strchr(route, '%')) return NULL;
    size_t len = strlen(route);
    while (len > 1 && route[len - 1] == '/') len--;
    return proxy_strndup(route, len);
}

static cwist_error_t proxy_add_rule(proxy_rule r, const char *origin_route, const char *target,
                                    const char *target_route) {
    r.prefix = proxy_normalize_route(origin_route);
    r.target_base = proxy_parse_target(target);
    char *tp = proxy_normalize_route(target_route);
    if (!r.prefix || !r.target_base || !tp) {
        cwist_free(tp);
        proxy_rule_free(&r);
        return proxy_result(-1);
    }
    r.prefix_len = strlen(r.prefix);
    if (strcmp(tp, "/") == 0) tp[0] = '\0';
    r.target_prefix = tp;

    pthread_mutex_lock(&g_proxy_rules_mu);
    int16_t rc = 0;
    if (g_proxy_started) {
        rc = -1;
    } else {
        for (size_t i = 0; i < g_proxy_rule_count; i++) {
            proxy_rule *o = &g_proxy_rules[i];
            if (o->port != r.port) continue;
            bool same_host =
                (o->any_host && r.any_host) || (o->host && r.host && strcmp(o->host, r.host) == 0);
            /* One listener cannot serve http:// and https:// origins at once. */
            if (o->tls != r.tls && !o->any_host && !r.any_host) {
                rc = -1;
                break;
            }
            if (same_host && strcmp(o->prefix, r.prefix) == 0) {
                rc = -1;
                break;
            }
        }
    }
    if (rc == 0) {
        proxy_rule *grown = (proxy_rule *)cwist_realloc(g_proxy_rules, (g_proxy_rule_count + 1) *
                                                                           sizeof(proxy_rule));
        if (!grown) {
            rc = -1;
        } else {
            g_proxy_rules = grown;
            g_proxy_rules[g_proxy_rule_count++] = r;
        }
    }
    pthread_mutex_unlock(&g_proxy_rules_mu);
    if (rc != 0) proxy_rule_free(&r);
    return proxy_result(rc);
}

cwist_error_t cwist_proxy_route(const char *origin, const char *origin_route, const char *target,
                                const char *target_route) {
    if (!origin || !origin_route || !target || !target_route) return proxy_result(-1);
    proxy_rule r;
    memset(&r, 0, sizeof(r));
    if (!proxy_parse_authority(origin, true, &r.tls, &r.host, &r.port, NULL, NULL)) {
        return proxy_result(-1);
    }
    return proxy_add_rule(r, origin_route, target, target_route);
}

cwist_error_t cwist_proxy_port_forward(unsigned short origin_port, unsigned short target_port) {
    if (origin_port == 0 || target_port == 0) return proxy_result(-1);
    char target[40];
    snprintf(target, sizeof(target), "http://127.0.0.1:%hu", target_port);
    proxy_rule r;
    memset(&r, 0, sizeof(r));
    r.port = origin_port;
    r.any_host = true;
    return proxy_add_rule(r, "/", target, "/");
}

/*
 * Split a Host header into its lowercase host and port. A Host without a port
 * means the scheme default, as a browser sends it. Returns false when the
 * Host is malformed; an absent Host yields an empty host.
 */
static bool proxy_host_key(const char *host, bool tls, char *out, size_t cap,
                           unsigned short *port_out) {
    out[0] = '\0';
    *port_out = tls ? 443 : 80;
    if (!host) return true;
    size_t len = strlen(host);
    const char *end = host + len;
    if (host[0] == '[') {
        const char *close = strchr(host, ']');
        if (!close) return false;
        end = close + 1;
        if (*end && *end != ':') return false;
    } else {
        const char *colon = strchr(host, ':');
        if (colon) end = colon;
    }
    if (*end == ':' && !proxy_parse_port(end + 1, strlen(end + 1), port_out)) return false;
    size_t hlen = (size_t)(end - host);
    if (hlen == 0) return true;
    if (hlen >= cap || !proxy_host_is_valid(host, hlen)) return false;
    for (size_t i = 0; i < hlen; i++) out[i] = (char)tolower((unsigned char)host[i]);
    out[hlen] = '\0';
    return true;
}

/* Whether the origin URL of @p r is the one the client addressed. */
static bool proxy_origin_matches(const proxy_rule *r, const char *host) {
    if (r->any_host) return true;
    char hk[256];
    unsigned short hport;
    if (!proxy_host_key(host, r->tls, hk, sizeof(hk), &hport)) return false;
    return hport == r->port && strcmp(r->host, hk) == 0;
}

static bool proxy_prefix_matches(const proxy_rule *r, const char *path) {
    if (r->prefix_len == 1) return true; /* "/" */
    if (strncmp(path, r->prefix, r->prefix_len) != 0) return false;
    char next = path[r->prefix_len];
    return next == '\0' || next == '/';
}

/*
 * Caller holds no lock: rules are frozen once g_proxy_started is set.
 * On no match, *status_out is 421 when no origin on @p port is the addressed
 * one, or 404 when the origin matches but no route prefix does.
 */
static const proxy_rule *proxy_find_rule(unsigned short port, const char *host, const char *path,
                                         int *status_out) {
    const proxy_rule *best = NULL;
    bool origin_seen = false;
    for (size_t i = 0; i < g_proxy_rule_count; i++) {
        const proxy_rule *r = &g_proxy_rules[i];
        if (r->port != port || !proxy_origin_matches(r, host)) continue;
        origin_seen = true;
        if (!proxy_prefix_matches(r, path)) continue;
        /* A named origin beats the port-forward catch-all at equal length. */
        if (!best || r->prefix_len > best->prefix_len ||
            (r->prefix_len == best->prefix_len && !r->any_host && best->any_host)) {
            best = r;
        }
    }
    if (!best && status_out) {
        *status_out = origin_seen ? CWIST_HTTP_NOT_FOUND : CWIST_HTTP_MISDIRECTED_REQUEST;
    }
    return best;
}

static char *proxy_build_url(const proxy_rule *r, const char *path, const char *query) {
    const char *rest = (r->prefix_len == 1) ? path : path + r->prefix_len;
    size_t blen = strlen(r->target_base);
    size_t tlen = strlen(r->target_prefix);
    size_t rlen = strlen(rest);
    size_t qlen = (query && query[0]) ? strlen(query) : 0;
    size_t total = blen + tlen + rlen + 1 + (qlen ? qlen + 1 : 0);
    char *url = (char *)cwist_alloc(total + 1);
    if (!url) return NULL;
    size_t off = 0;
    memcpy(url + off, r->target_base, blen);
    off += blen;
    memcpy(url + off, r->target_prefix, tlen);
    off += tlen;
    memcpy(url + off, rest, rlen);
    off += rlen;
    if (tlen + rlen == 0) url[off++] = '/';
    if (qlen) {
        url[off++] = '?';
        memcpy(url + off, query, qlen);
        off += qlen;
    }
    url[off] = '\0';
    return url;
}

char *cwist_proxy_resolve_url(unsigned short port, const char *host, const char *path,
                              const char *query, int *status_out) {
    if (status_out) *status_out = CWIST_HTTP_BAD_REQUEST;
    if (!path || !cwist_proxy_path_is_safe(path) || !cwist_proxy_query_is_safe(query)) {
        return NULL;
    }
    pthread_mutex_lock(&g_proxy_rules_mu);
    const proxy_rule *r = proxy_find_rule(port, host, path, status_out);
    char *url = r ? proxy_build_url(r, path, query) : NULL;
    pthread_mutex_unlock(&g_proxy_rules_mu);
    return url;
}

/* --- Validation -------------------------------------------------------------- */

static int proxy_hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool cwist_proxy_path_is_safe(const char *path) {
    if (!path || path[0] != '/') return false;
    /* Walk segments, decoding only %2E so "%2e%2E" is caught as "..". */
    const char *p = path;
    while (*p) {
        /* p points at '/' */
        p++;
        size_t dots = 0;
        size_t seg_len = 0;
        while (*p && *p != '/') {
            unsigned char c = (unsigned char)*p;
            if (c <= 0x20 || c == 0x7f || c == '\\' || c == '#') return false;
            if (c == '%') {
                int hi = proxy_hexval(p[1]);
                int lo = hi < 0 ? -1 : proxy_hexval(p[2]);
                if (lo < 0) return false;
                int v = hi * 16 + lo;
                /* Encoded '/', '\', NUL and control bytes change meaning downstream. */
                if (v == '/' || v == '\\' || v < 0x20 || v == 0x7f) return false;
                if (v == '.') dots++;
                seg_len++;
                p += 3;
                continue;
            }
            if (c == '.') dots++;
            seg_len++;
            p++;
        }
        if (seg_len > 0 && dots == seg_len && seg_len <= 2) return false; /* "." / ".." */
    }
    return true;
}

bool cwist_proxy_query_is_safe(const char *query) {
    if (!query) return true;
    for (const unsigned char *p = (const unsigned char *)query; *p; p++) {
        if (*p <= 0x20 || *p == 0x7f || *p == '#') return false;
        if (*p == '%' && (proxy_hexval((char)p[1]) < 0 || proxy_hexval((char)p[2]) < 0))
            return false;
    }
    return true;
}

bool cwist_proxy_header_name_ok(const char *name) {
    if (!name || !name[0]) return false;
    for (const unsigned char *p = (const unsigned char *)name; *p; p++) {
        if (isalnum(*p)) continue;
        if (!strchr("!#$%&'*+-.^_`|~", *p)) return false;
    }
    return true;
}

bool cwist_proxy_header_value_ok(const char *value) {
    if (!value) return false;
    for (const unsigned char *p = (const unsigned char *)value; *p; p++) {
        if ((*p < 0x20 && *p != '\t') || *p == 0x7f) return false;
    }
    return true;
}

/* --- Header filtering ---------------------------------------------------------- */

typedef struct proxy_conn_tokens {
    char tok[PROXY_MAX_CONN_TOKENS][64];
    size_t count;
} proxy_conn_tokens;

/* Collect the comma-separated tokens of every Connection header. */
static bool proxy_collect_connection_tokens(cwist_http_header_node *h, proxy_conn_tokens *out) {
    out->count = 0;
    for (; h; h = h->next) {
        if (!h->key || !h->key->data || strcasecmp(h->key->data, "connection") != 0) continue;
        const char *v = h->value && h->value->data ? h->value->data : "";
        while (*v) {
            while (*v == ' ' || *v == '\t' || *v == ',') v++;
            const char *s = v;
            while (*v && *v != ',') v++;
            const char *e = v;
            while (e > s && (e[-1] == ' ' || e[-1] == '\t')) e--;
            size_t n = (size_t)(e - s);
            if (n == 0) continue;
            if (n >= sizeof(out->tok[0]) || out->count == PROXY_MAX_CONN_TOKENS) return false;
            memcpy(out->tok[out->count], s, n);
            out->tok[out->count][n] = '\0';
            out->count++;
        }
    }
    return true;
}

static bool proxy_in_tokens(const proxy_conn_tokens *t, const char *name) {
    for (size_t i = 0; i < t->count; i++) {
        if (strcasecmp(t->tok[i], name) == 0) return true;
    }
    return false;
}

static bool proxy_in_list(const char *const *list, const char *name) {
    for (size_t i = 0; list[i]; i++) {
        if (strcasecmp(list[i], name) == 0) return true;
    }
    return false;
}

/* RFC 9110 section 7.6.1 hop-by-hop fields plus the de-facto ones. */
static const char *const k_proxy_hop_by_hop[] = {"connection",
                                                 "keep-alive",
                                                 "proxy-connection",
                                                 "te",
                                                 "trailer",
                                                 "transfer-encoding",
                                                 "upgrade",
                                                 "proxy-authorization",
                                                 "proxy-authenticate",
                                                 "http2-settings",
                                                 NULL};

/* Request fields the proxy owns or curl must compute. */
static const char *const k_proxy_req_drop[] = {"host",
                                               "content-length",
                                               "expect",
                                               "forwarded",
                                               "x-forwarded-for",
                                               "x-forwarded-host",
                                               "x-forwarded-proto",
                                               "x-real-ip",
                                               NULL};

/* --- Upstream exchange ------------------------------------------------------------ */

typedef struct proxy_job {
    cwist_async *async;
    char *url;
    const char *method; /* static string */
    bool head;
    struct curl_slist *headers;
    char *body;
    size_t body_len;
    unsigned short front_port;
    bool front_h3;
    /* Upstream TLS floor, derived from the front connection (https targets only). */
    long upstream_tls_min; /* CURL_SSLVERSION_* or CURL_SSLVERSION_DEFAULT */
    bool upstream_pq_only; /* offer only hybrid post-quantum key exchange */
} proxy_job;

static void proxy_job_free(proxy_job *job) {
    if (!job) return;
    cwist_free(job->url);
    curl_slist_free_all(job->headers);
    free(job->body);
    free(job);
}

typedef struct proxy_upstream_res {
    int status;
    cwist_http_header_node *headers;
    char *body;
    size_t len;
    size_t cap;
    size_t max;
    bool too_big;
    bool bad_header;
} proxy_upstream_res;

static size_t proxy_upstream_header_cb(char *ptr, size_t size, size_t nmemb, void *userp) {
    proxy_upstream_res *u = (proxy_upstream_res *)userp;
    size_t n = size * nmemb;
    size_t len = n;
    while (len > 0 && (ptr[len - 1] == '\n' || ptr[len - 1] == '\r')) len--;
    if (len == 0) return n;
    if (len > 5 && strncmp(ptr, "HTTP/", 5) == 0) {
        /* New status line (interim 1xx or the final one): restart the block. */
        cwist_http_header_free_all(u->headers);
        u->headers = NULL;
        const char *sp = memchr(ptr, ' ', len);
        if (!sp || (size_t)(sp - ptr) + 4 > len) {
            u->bad_header = true;
            return n;
        }
        int code = 0;
        for (int i = 1; i <= 3; i++) {
            if (sp[i] < '0' || sp[i] > '9') {
                u->bad_header = true;
                return n;
            }
            code = code * 10 + (sp[i] - '0');
        }
        u->status = code;
        return n;
    }
    if (ptr[0] == ' ' || ptr[0] == '\t') { /* obs-fold */
        u->bad_header = true;
        return n;
    }
    const char *colon = memchr(ptr, ':', len);
    if (!colon || colon == ptr || (size_t)(colon - ptr) >= 256) {
        u->bad_header = true;
        return n;
    }
    char name[256];
    size_t nlen = (size_t)(colon - ptr);
    memcpy(name, ptr, nlen);
    name[nlen] = '\0';
    const char *v = colon + 1;
    const char *ve = ptr + len;
    while (v < ve && (*v == ' ' || *v == '\t')) v++;
    while (ve > v && (ve[-1] == ' ' || ve[-1] == '\t')) ve--;
    char *value = proxy_strndup(v, (size_t)(ve - v));
    if (!value) {
        u->bad_header = true;
        return n;
    }
    if (!cwist_proxy_header_name_ok(name) || !cwist_proxy_header_value_ok(value)) {
        u->bad_header = true;
    } else {
        cwist_http_header_add(&u->headers, name, value);
    }
    cwist_free(value);
    return n;
}

static size_t proxy_upstream_write_cb(void *data, size_t size, size_t nmemb, void *userp) {
    proxy_upstream_res *u = (proxy_upstream_res *)userp;
    size_t n = size * nmemb;
    if (n > u->max - u->len) {
        u->too_big = true;
        return 0; /* aborts the transfer */
    }
    if (u->len + n > u->cap) {
        size_t cap = u->cap ? u->cap : 16384;
        while (cap < u->len + n) cap *= 2;
        if (cap > u->max) cap = u->max;
        char *grown = (char *)realloc(u->body, cap);
        if (!grown) return 0;
        u->body = grown;
        u->cap = cap;
    }
    memcpy(u->body + u->len, data, n);
    u->len += n;
    return n;
}

static void proxy_body_free_cb(const void *ptr, size_t len, void *ctx) {
    (void)len;
    (void)ctx;
    free((void *)ptr);
}

static void proxy_set_text(cwist_http_response *res, int status, const char *msg) {
    cwist_http_header_free_all(res->headers);
    res->headers = NULL;
    res->status_code = (cwist_http_status_t)status;
    cwist_http_header_add(&res->headers, "Content-Type", "text/plain");
    cwist_sstring_assign(res->body, msg);
}

static void proxy_apply_alt_svc(cwist_http_response *res, unsigned short port, bool front_h3) {
    if (front_h3 || !g_proxy_cfg.h3_enabled) return;
    if (g_proxy_cfg.http_auto_promote) {
        char v[48];
        snprintf(v, sizeof(v), "h3=\":%hu\"; ma=86400", port);
        cwist_http_header_add(&res->headers, "Alt-Svc", v);
    } else {
        /* An explicit value also stops the TLS writers from injecting one. */
        cwist_http_header_add(&res->headers, "Alt-Svc", "clear");
    }
}

static __thread CURL *t_proxy_curl;

/* Run the upstream exchange and write the outcome into @p res. */
static void proxy_execute(proxy_job *job, cwist_http_response *res) {
    if (!t_proxy_curl) t_proxy_curl = curl_easy_init();
    CURL *curl = t_proxy_curl;
    if (!curl) {
        proxy_set_text(res, CWIST_HTTP_BAD_GATEWAY, "502 Bad Gateway");
        return;
    }
    curl_easy_reset(curl);

    proxy_upstream_res u;
    memset(&u, 0, sizeof(u));
    u.max = g_proxy_cfg.max_response_bytes;

    curl_easy_setopt(curl, CURLOPT_URL, job->url);
#if LIBCURL_VERSION_NUM >= 0x075500 /* CURLOPT_PROTOCOLS_STR is an enum, not a macro */
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
#else
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS, (long)(CURLPROTO_HTTP | CURLPROTO_HTTPS));
#endif
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, (long)CURL_HTTP_VERSION_1_1);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, (long)g_proxy_cfg.connect_timeout_ms);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, (long)g_proxy_cfg.timeout_ms);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, job->headers);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, proxy_upstream_header_cb);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &u);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, proxy_upstream_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &u);
    curl_easy_setopt(curl, CURLOPT_MAXFILESIZE_LARGE, (curl_off_t)g_proxy_cfg.max_response_bytes);
    if (job->upstream_tls_min != CURL_SSLVERSION_DEFAULT) {
        curl_easy_setopt(curl, CURLOPT_SSLVERSION, job->upstream_tls_min);
    }
    if (job->upstream_pq_only) {
        /* Only hybrid groups on offer: an upstream without them fails the
         * handshake (502) instead of silently downgrading key exchange. */
        curl_easy_setopt(curl, CURLOPT_SSL_EC_CURVES, PROXY_PQ_GROUPS);
    }
    if (g_proxy_cfg.upstream_ca_path) {
        curl_easy_setopt(curl, CURLOPT_CAINFO, g_proxy_cfg.upstream_ca_path);
    }
    if (job->head) {
        curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
    } else if (job->body_len > 0 || strcmp(job->method, "POST") == 0 ||
               strcmp(job->method, "PUT") == 0 || strcmp(job->method, "PATCH") == 0) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, job->body ? job->body : "");
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)job->body_len);
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, job->method);
    } else if (strcmp(job->method, "GET") != 0) {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, job->method);
    }

    CURLcode cc = curl_easy_perform(curl);
    /* Drop references to job/stack state before the handle is reused. */
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, NULL);

    if (cc != CURLE_OK || u.too_big || u.bad_header || u.status < 200 || u.status > 999) {
        bool timeout = (cc == CURLE_OPERATION_TIMEDOUT);
        cwist_http_header_free_all(u.headers);
        free(u.body);
        if (timeout) {
            proxy_set_text(res, CWIST_HTTP_GATEWAY_TIMEOUT, "504 Gateway Timeout");
        } else {
            proxy_set_text(res, CWIST_HTTP_BAD_GATEWAY, "502 Bad Gateway");
        }
        return;
    }

    proxy_conn_tokens tokens;
    bool tokens_ok = proxy_collect_connection_tokens(u.headers, &tokens);
    if (!tokens_ok) {
        cwist_http_header_free_all(u.headers);
        free(u.body);
        proxy_set_text(res, CWIST_HTTP_BAD_GATEWAY, "502 Bad Gateway");
        return;
    }

    cwist_http_header_free_all(res->headers);
    res->headers = NULL;
    res->status_code = (cwist_http_status_t)u.status;
    /* Keep Content-Length where no body follows it, so HEAD and 304 stay
     * truthful; elsewhere the front writer computes it from the body. */
    bool keep_clen = job->head || u.status == 304;
    for (cwist_http_header_node *h = u.headers; h; h = h->next) {
        const char *k = h->key->data;
        if (proxy_in_list(k_proxy_hop_by_hop, k) || proxy_in_tokens(&tokens, k)) continue;
        if (strcasecmp(k, "alt-svc") == 0) continue;
        if (!keep_clen && strcasecmp(k, "content-length") == 0) continue;
        cwist_http_header_add(&res->headers, k, h->value->data);
    }
    cwist_http_header_free_all(u.headers);

    proxy_apply_alt_svc(res, job->front_port, job->front_h3);

    if (u.len > 0 && !job->head) {
        cwist_http_response_set_body_ptr_managed(res, u.body, u.len, proxy_body_free_cb, NULL);
    } else {
        free(u.body);
    }
}

/* --- Scheduler (per process: workers may fork after setup) ------------------------- */

static pthread_mutex_t g_proxy_sched_mu = PTHREAD_MUTEX_INITIALIZER;
static cwist_scheduler_t *g_proxy_sched;
static pid_t g_proxy_sched_pid;

static cwist_scheduler_t *proxy_scheduler(void) {
    pid_t pid = getpid();
    pthread_mutex_lock(&g_proxy_sched_mu);
    if (!g_proxy_sched || g_proxy_sched_pid != pid) {
        /* A scheduler inherited across fork has no threads; leak it. */
        g_proxy_sched = cwist_scheduler_create(g_proxy_cfg.workers, PROXY_QUEUE_CAPACITY);
        g_proxy_sched_pid = pid;
    }
    cwist_scheduler_t *s = g_proxy_sched;
    pthread_mutex_unlock(&g_proxy_sched_mu);
    return s;
}

static void proxy_job_run(void *arg) {
    proxy_job *job = (proxy_job *)arg;
    cwist_async *a = job->async;
    cwist_http_response *res = cwist_http_response_create();
    if (!res) {
        cwist_async_abort(a, CWIST_HTTP_BAD_GATEWAY);
    } else {
        proxy_execute(job, res);
        if (!cwist_async_respond_with(a, res)) cwist_http_response_destroy(res);
    }
    cwist_async_release(a);
    proxy_job_free(job);
}

/* --- Front dispatch ------------------------------------------------------------------ */

static bool proxy_is_front_app(cwist_app *app) {
    for (size_t i = 0; i < g_proxy_front_app_count; i++) {
        if (g_proxy_front_apps[i] == app) return true;
    }
    return false;
}

static const char *proxy_req_path(cwist_http_request *req) {
    return (req->path && req->path->data) ? req->path->data : "/";
}

/* Front apps have no routes of their own: the proxy answers every request,
 * including the 400/404/421 refusals. */
static bool proxy_fallback_match(cwist_app *app, cwist_http_request *req) {
    return app && req && proxy_is_front_app(app);
}

static bool proxy_append_header(struct curl_slist **list, const char *name, const char *value) {
    size_t n = strlen(name) + strlen(value) + 3;
    char *line = (char *)malloc(n);
    if (!line) return false;
    if (value[0]) {
        snprintf(line, n, "%s: %s", name, value);
    } else {
        snprintf(line, n, "%s;", name); /* curl syntax for an empty value */
    }
    struct curl_slist *next = curl_slist_append(*list, line);
    free(line);
    if (!next) return false;
    *list = next;
    return true;
}

static bool proxy_peer_ip(int fd, char *out, size_t cap) {
    if (fd < 0) return false;
    int type = 0;
    socklen_t tl = sizeof(type);
    if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &tl) != 0 || type != SOCK_STREAM) return false;
    struct sockaddr_storage ss;
    socklen_t sl = sizeof(ss);
    if (getpeername(fd, (struct sockaddr *)&ss, &sl) != 0) return false;
    if (ss.ss_family == AF_INET) {
        return inet_ntop(AF_INET, &((struct sockaddr_in *)&ss)->sin_addr, out, (socklen_t)cap);
    }
    if (ss.ss_family == AF_INET6) {
        return inet_ntop(AF_INET6, &((struct sockaddr_in6 *)&ss)->sin6_addr, out, (socklen_t)cap);
    }
    return false;
}

/*
 * Upstream TLS policy. An http:// target terminates TLS here (stripped).
 * An https:// target keeps at least the front connection's level: the same
 * minimum version, and hybrid post-quantum key exchange only when the client
 * negotiated it. tls_auto_promote raises every https upstream to TLS 1.3 with
 * post-quantum key exchange regardless of the client.
 */
static void proxy_set_upstream_tls(cwist_http_request *req, proxy_job *job) {
    job->upstream_tls_min = CURL_SSLVERSION_DEFAULT;
    job->upstream_pq_only = false;
    if (strncmp(job->url, "https://", 8) != 0) return;
    if (g_proxy_cfg.tls_auto_promote) {
        job->upstream_tls_min = CURL_SSLVERSION_TLSv1_3;
        job->upstream_pq_only = true;
        return;
    }
    cwist_tls_info front;
    if (!cwist_http_request_tls_info(req, &front)) return;
    if (front.version >= 0x0304) {
        job->upstream_tls_min = CURL_SSLVERSION_TLSv1_3;
    } else if (front.version == 0x0303) {
        job->upstream_tls_min = CURL_SSLVERSION_TLSv1_2;
    }
    job->upstream_pq_only = cwist_tls_group_is_pq(front.group);
}

/* Build the upstream request from @p req. Returns 0 or the HTTP status to answer. */
static int proxy_build_job(cwist_http_request *req, proxy_job *job) {
    switch (req->method) {
        case CWIST_HTTP_GET: job->method = "GET"; break;
        case CWIST_HTTP_POST: job->method = "POST"; break;
        case CWIST_HTTP_PUT: job->method = "PUT"; break;
        case CWIST_HTTP_DELETE: job->method = "DELETE"; break;
        case CWIST_HTTP_PATCH: job->method = "PATCH"; break;
        case CWIST_HTTP_HEAD: job->method = "HEAD"; break;
        case CWIST_HTTP_OPTIONS: job->method = "OPTIONS"; break;
        case CWIST_HTTP_CONNECT: return CWIST_HTTP_METHOD_NOT_ALLOWED;
        default: return CWIST_HTTP_NOT_IMPLEMENTED;
    }
    job->head = (req->method == CWIST_HTTP_HEAD);

    const char *path = proxy_req_path(req);
    const char *query = (req->query && req->query->data) ? req->query->data : NULL;
    if (!cwist_proxy_path_is_safe(path) || !cwist_proxy_query_is_safe(query)) {
        return CWIST_HTTP_BAD_REQUEST;
    }

    const char *host = cwist_http_header_get(req->headers, "host");
    if (host && !cwist_proxy_header_value_ok(host)) return CWIST_HTTP_BAD_REQUEST;
    char hk[256];
    unsigned short hport;
    if (!proxy_host_key(host, req->app->use_ssl || job->front_h3, hk, sizeof(hk), &hport)) {
        return CWIST_HTTP_BAD_REQUEST;
    }

    const char *upgrade = cwist_http_header_get(req->headers, "upgrade");
    if (upgrade && strcasestr(upgrade, "websocket")) return CWIST_HTTP_NOT_IMPLEMENTED;

    unsigned short port = (unsigned short)req->app->port;
    job->front_port = port;
    int miss = CWIST_HTTP_NOT_FOUND;
    job->url = cwist_proxy_resolve_url(port, host, path, query, &miss);
    if (!job->url) return miss;
    proxy_set_upstream_tls(req, job);

    proxy_conn_tokens tokens;
    if (!proxy_collect_connection_tokens(req->headers, &tokens)) return CWIST_HTTP_BAD_REQUEST;

    for (cwist_http_header_node *h = req->headers; h; h = h->next) {
        if (!h->key || !h->key->data) continue;
        const char *k = h->key->data;
        const char *v = (h->value && h->value->data) ? h->value->data : "";
        if (k[0] == ':') continue; /* stray pseudo-header */
        if (!cwist_proxy_header_name_ok(k) || !cwist_proxy_header_value_ok(v)) {
            return CWIST_HTTP_BAD_REQUEST;
        }
        if (proxy_in_list(k_proxy_hop_by_hop, k) || proxy_in_list(k_proxy_req_drop, k) ||
            proxy_in_tokens(&tokens, k))
            continue;
        if (!proxy_append_header(&job->headers, k, v)) return CWIST_HTTP_INTERNAL_ERROR;
    }
    /* "Name:" tells curl not to add a default the client did not send. */
    static const char *const k_proxy_curl_defaults[] = {"Accept", "Content-Type", NULL};
    for (size_t i = 0; k_proxy_curl_defaults[i]; i++) {
        if (cwist_http_header_get(req->headers, k_proxy_curl_defaults[i])) continue;
        char marker[32];
        snprintf(marker, sizeof(marker), "%s:", k_proxy_curl_defaults[i]);
        struct curl_slist *next = curl_slist_append(job->headers, marker);
        if (!next) return CWIST_HTTP_INTERNAL_ERROR;
        job->headers = next;
    }
    struct curl_slist *next = curl_slist_append(job->headers, "Expect:");
    if (!next) return CWIST_HTTP_INTERNAL_ERROR;
    job->headers = next;

    if (g_proxy_cfg.preserve_host && host && host[0] &&
        !proxy_append_header(&job->headers, "Host", host)) {
        return CWIST_HTTP_INTERNAL_ERROR;
    }
    char ip[INET6_ADDRSTRLEN];
    if (proxy_peer_ip(req->client_fd, ip, sizeof(ip)) &&
        !proxy_append_header(&job->headers, "X-Forwarded-For", ip)) {
        return CWIST_HTTP_INTERNAL_ERROR;
    }
    if (host && host[0] && !proxy_append_header(&job->headers, "X-Forwarded-Host", host)) {
        return CWIST_HTTP_INTERNAL_ERROR;
    }
    bool tls = req->app->use_ssl || job->front_h3;
    if (!proxy_append_header(&job->headers, "X-Forwarded-Proto", tls ? "https" : "http")) {
        return CWIST_HTTP_INTERNAL_ERROR;
    }

    if (req->body && req->body->data && req->body->size > 0) {
        job->body = (char *)malloc(req->body->size);
        if (!job->body) return CWIST_HTTP_INTERNAL_ERROR;
        memcpy(job->body, req->body->data, req->body->size);
        job->body_len = req->body->size;
    }
    return 0;
}

static void proxy_handler(cwist_http_request *req, cwist_http_response *res) {
    proxy_job *job = (proxy_job *)calloc(1, sizeof(*job));
    if (!job) {
        proxy_set_text(res, CWIST_HTTP_INTERNAL_ERROR, "500 Internal Server Error");
        return;
    }
    job->front_h3 = req->version && req->version->data && strcmp(req->version->data, "HTTP/3") == 0;
    int status = proxy_build_job(req, job);
    if (status != 0) {
        proxy_job_free(job);
        char msg[48];
        snprintf(msg, sizeof(msg), "%d %s", status, cwist_http_status_reason(status));
        proxy_set_text(res, status, msg);
        return;
    }

    cwist_scheduler_t *sched = proxy_scheduler();
    if (!sched) {
        proxy_job_free(job);
        proxy_set_text(res, CWIST_HTTP_SERVICE_UNAVAILABLE, "503 Service Unavailable");
        return;
    }
    /* Every front protocol defers, HTTP/3 included (engine completion queue). */
    cwist_async *a = cwist_async_defer(req, res);
    if (!a) {
        /* No completion path for this exchange (allocation failure, or an
         * HTTP/3 loop without a queue): forward inline rather than fail. */
        proxy_execute(job, res);
        proxy_job_free(job);
        return;
    }
    job->async = cwist_async_retain(a);
    if (!cwist_scheduler_submit(sched, proxy_job_run, job)) {
        cwist_async_release(a);
        proxy_job_free(job);
        cwist_async_abort(a, CWIST_HTTP_SERVICE_UNAVAILABLE);
    }
}

/* --- Listener setup -------------------------------------------------------------------- */

static cwist_app *proxy_build_front_app(bool tls) {
    cwist_app *app = cwist_app_create();
    if (!app) return NULL;
    /* Before the TLS contexts exist, so both the TCP and the QUIC context
     * get hybrid post-quantum groups and TLS 1.3. */
    if (g_proxy_cfg.tls_auto_promote) cwist_app_use_pqc_layer(app, true);
    if (tls) {
        cwist_error_t e = cwist_app_use_https(app, g_proxy_cfg.cert_path, g_proxy_cfg.key_path);
        if (!cwist_error_is_ok(&e)) {
            fprintf(stderr, "cwist_proxy: TLS setup failed for %s\n", g_proxy_cfg.cert_path);
            cwist_app_destroy(app);
            return NULL;
        }
    }
    if (g_proxy_cfg.h2_enabled) {
        cwist_error_t e = tls ? cwist_app_use_https2(app, true) : cwist_app_use_http2(app, true);
        if (!cwist_error_is_ok(&e)) {
            fprintf(stderr, "cwist_proxy: HTTP/2 is not available in this build\n");
            cwist_app_destroy(app);
            return NULL;
        }
    }
    if (g_proxy_cfg.h3_enabled) {
        cwist_error_t e = tls ? cwist_app_use_https3(app, true) : cwist_app_use_http3(app, true);
        if (!cwist_error_is_ok(&e)) {
            fprintf(stderr, "cwist_proxy: HTTP/3 is not available in this build\n");
            cwist_app_destroy(app);
            return NULL;
        }
    }
    return app;
}

static void proxy_snapshot_config(void) {
    g_proxy_cfg = cwist_proxy;
    if (g_proxy_cfg.connect_timeout_ms <= 0)
        g_proxy_cfg.connect_timeout_ms = PROXY_DEFAULT_CONNECT_TIMEOUT_MS;
    if (g_proxy_cfg.timeout_ms <= 0) g_proxy_cfg.timeout_ms = PROXY_DEFAULT_TIMEOUT_MS;
    if (g_proxy_cfg.max_response_bytes == 0)
        g_proxy_cfg.max_response_bytes = PROXY_DEFAULT_MAX_RESPONSE;
    if (g_proxy_cfg.workers == 0) g_proxy_cfg.workers = PROXY_DEFAULT_WORKERS;
    if (!g_proxy_cfg.cert_path || !g_proxy_cfg.key_path) {
        g_proxy_cfg.cert_path = NULL;
        g_proxy_cfg.key_path = NULL;
    }
}

int cwist_proxy_listen(void) {
    unsigned short ports[CWIST_MULTIPORT_MAX_PORTS];
    size_t port_count = 0;

    pthread_mutex_lock(&g_proxy_rules_mu);
    if (g_proxy_started || g_proxy_rule_count == 0) {
        pthread_mutex_unlock(&g_proxy_rules_mu);
        return -1;
    }
    for (size_t i = 0; i < g_proxy_rule_count; i++) {
        bool seen = false;
        for (size_t j = 0; j < port_count; j++) {
            if (ports[j] == g_proxy_rules[i].port) seen = true;
        }
        if (seen) continue;
        if (port_count == CWIST_MULTIPORT_MAX_PORTS) {
            pthread_mutex_unlock(&g_proxy_rules_mu);
            fprintf(stderr, "cwist_proxy: more than %d origin ports\n", CWIST_MULTIPORT_MAX_PORTS);
            return -1;
        }
        ports[port_count++] = g_proxy_rules[i].port;
    }
    /* The origin schemes decide TLS. Multiport sub-apps inherit the root's
     * TLS setup, so every named origin must share one scheme; port forwards
     * are scheme-agnostic and follow it (or the presence of a certificate). */
    bool has_http = false, has_https = false;
    for (size_t i = 0; i < g_proxy_rule_count; i++) {
        if (g_proxy_rules[i].any_host) continue;
        if (g_proxy_rules[i].tls) {
            has_https = true;
        } else {
            has_http = true;
        }
    }
    g_proxy_started = true;
    pthread_mutex_unlock(&g_proxy_rules_mu);

    proxy_snapshot_config();
    bool have_cert = g_proxy_cfg.cert_path != NULL;
    if (has_http && has_https) {
        fprintf(stderr,
                "cwist_proxy: http:// and https:// origins cannot be served together yet\n");
        return -1;
    }
    if (has_https && !have_cert) {
        fprintf(stderr, "cwist_proxy: https:// origins need cwist_proxy.cert_path/key_path\n");
        return -1;
    }
    if (has_http && have_cert) {
        fprintf(stderr, "cwist_proxy: certificate configured but every origin is http://\n");
        return -1;
    }
    bool tls = has_https || (!has_http && have_cert);

    cwist_app *root = proxy_build_front_app(tls);
    if (!root) return -1;
    root->port = ports[0];
    g_proxy_front_apps[0] = root;
    g_proxy_front_app_count = 1;
    for (size_t i = 1; i < port_count; i++) {
        cwist_app *sub = cwist_multiport_get_app(&root, ports[i]);
        if (!sub) {
            cwist_app_destroy(root);
            g_proxy_front_app_count = 0;
            return -1;
        }
        g_proxy_front_apps[g_proxy_front_app_count++] = sub;
    }

    cwist_curl_global_acquire();
    cwist_app_internal_set_fallback(proxy_fallback_match, proxy_handler);

    int rc;
    if (port_count == 1) {
        rc = cwist_app_listen(root, ports[0]);
    } else {
        cwist_multiport_t extra = cwist_create_multiport_from_array(ports + 1, port_count - 1);
        rc = cwist_app_multiport(&root, ports[0], extra);
    }

    cwist_app_internal_set_fallback(NULL, NULL);
    g_proxy_front_app_count = 0;
    cwist_app_destroy(root); /* also releases detached multiport sub-apps */
    cwist_curl_global_release();
    return rc;
}

void cwist_proxy_reset(void) {
    pthread_mutex_lock(&g_proxy_rules_mu);
    for (size_t i = 0; i < g_proxy_rule_count; i++) proxy_rule_free(&g_proxy_rules[i]);
    cwist_free(g_proxy_rules);
    g_proxy_rules = NULL;
    g_proxy_rule_count = 0;
    g_proxy_started = false;
    pthread_mutex_unlock(&g_proxy_rules_mu);
    cwist_proxy_config defaults = PROXY_DEFAULTS;
    cwist_proxy = defaults;
    g_proxy_cfg = defaults;
}
