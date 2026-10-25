/**
 * @file otel.c
 * @brief OpenTelemetry tracing middleware (#402).
 *
 * The middleware records one SERVER span per request and pushes it onto a
 * bounded ring; a background thread drains the ring in batches and POSTs
 * them to the collector as OTLP/HTTP JSON.  Export is fail-open: a full ring
 * drops its oldest span and a failed POST drops the batch, so the collector
 * never slows or fails a request.
 *
 * The exporter thread is started by the first span a process records, and a
 * pthread_atfork child hook forgets the parent's thread and spans, so every
 * worker process exports its own spans.
 */
#define _POSIX_C_SOURCE 200809L
#include <cwist/sys/app/middleware.h>
#include <cwist/core/mem/alloc.h>
#include <cwist/net/http/http.h>
#include <cwist/net/http/http_client.h>
#include <cjson/cJSON.h>
#include <openssl/rand.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define OTEL_DEFAULT_ENDPOINT "http://localhost:4318/v1/traces"
#define OTEL_DEFAULT_SERVICE "cwist"
#define OTEL_DEFAULT_QUEUE 512U
#define OTEL_DEFAULT_BATCH 64U
#define OTEL_EXPORT_TIMEOUT_MS 2000
#define OTEL_TRACE_ID_HEX 32U
#define OTEL_SPAN_ID_HEX 16U

typedef struct {
    char trace_id[OTEL_TRACE_ID_HEX + 1];
    char span_id[OTEL_SPAN_ID_HEX + 1];
    char parent_span_id[OTEL_SPAN_ID_HEX + 1]; /* "" for a root span */
    cwist_http_method_t method;
    char path[256];
    int status;
    uint64_t start_ns;
    uint64_t end_ns;
} otel_span_t;

/* Configuration is fixed by the first cwist_mw_otel_trace() call; the
 * middleware is a plain function pointer, so there is one exporter per
 * process.  g_otel_lock guards the ring and the writer state. */
static struct {
    char *endpoint;
    char *service_name;
    size_t batch;
    otel_span_t *ring;
    size_t cap;
    size_t head;
    size_t count;
    bool configured;
    bool exporter_alive;
    pthread_t exporter;
} g_otel;
static pthread_mutex_t g_otel_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_otel_work = PTHREAD_COND_INITIALIZER;

/* Trace context of the requests running on this thread, innermost first, for
 * cwist_mw_otel_get_trace_id() (same scheme as cwist_mw_jwt_get_claims()). */
typedef struct otel_ctx {
    const cwist_http_request *req;
    const char *trace_id;
    struct otel_ctx *outer;
} otel_ctx;
static _Thread_local otel_ctx *t_otel_ctx;

static uint64_t otel_unix_nanos(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static bool otel_is_hex(const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

static bool otel_is_zero(const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (s[i] != '0') return false;
    }
    return true;
}

/* Random non-zero id of `nibbles` lowercase hex digits. */
static void otel_random_id(char *out, size_t nibbles) {
    static const char hex[] = "0123456789abcdef";
    uint8_t bytes[OTEL_TRACE_ID_HEX / 2];
    do {
        if (RAND_bytes(bytes, (int)(nibbles / 2)) != 1) memset(bytes, 0x5a, sizeof(bytes));
        for (size_t i = 0; i < nibbles / 2; i++) {
            out[2 * i] = hex[bytes[i] >> 4];
            out[2 * i + 1] = hex[bytes[i] & 0xF];
        }
        out[nibbles] = '\0';
    } while (otel_is_zero(out, nibbles));
}

/* W3C trace-context: "vv-<32 hex trace-id>-<16 hex parent-id>-ff", version
 * not "ff", ids not all zero.  Later versions may append "-..." fields. */
static bool parse_traceparent(const char *h, char *trace_id, char *parent_id) {
    if (!h || strlen(h) < 55) return false;
    if (!otel_is_hex(h, 2) || strncmp(h, "ff", 2) == 0 || h[2] != '-') return false;
    if (!otel_is_hex(h + 3, OTEL_TRACE_ID_HEX) || otel_is_zero(h + 3, OTEL_TRACE_ID_HEX) || h[35] != '-')
        return false;
    if (!otel_is_hex(h + 36, OTEL_SPAN_ID_HEX) || otel_is_zero(h + 36, OTEL_SPAN_ID_HEX) || h[52] != '-')
        return false;
    if (!otel_is_hex(h + 53, 2) || (h[55] != '\0' && (strncmp(h, "00", 2) == 0 || h[55] != '-')))
        return false;
    memcpy(trace_id, h + 3, OTEL_TRACE_ID_HEX);
    trace_id[OTEL_TRACE_ID_HEX] = '\0';
    memcpy(parent_id, h + 36, OTEL_SPAN_ID_HEX);
    parent_id[OTEL_SPAN_ID_HEX] = '\0';
    return true;
}

static cJSON *otel_attr(const char *key, cJSON *value, const char *type) {
    cJSON *a = cJSON_CreateObject();
    cJSON_AddStringToObject(a, "key", key);
    cJSON_AddItemToObject(cJSON_AddObjectToObject(a, "value"), type, value);
    return a;
}

/* OTLP/HTTP JSON body for `n` spans (uint64 fields are strings per the
 * protobuf JSON mapping). */
static char *otel_encode_spans(const otel_span_t *spans, size_t n) {
    cJSON *root = cJSON_CreateObject();
    cJSON *rs = cJSON_CreateObject();
    cJSON_AddItemToArray(cJSON_AddArrayToObject(root, "resourceSpans"), rs);
    cJSON *rattrs = cJSON_AddArrayToObject(cJSON_AddObjectToObject(rs, "resource"), "attributes");
    cJSON_AddItemToArray(rattrs,
                         otel_attr("service.name", cJSON_CreateString(g_otel.service_name), "stringValue"));
    cJSON *ss = cJSON_CreateObject();
    cJSON_AddItemToArray(cJSON_AddArrayToObject(rs, "scopeSpans"), ss);
    cJSON_AddStringToObject(cJSON_AddObjectToObject(ss, "scope"), "name", "cwist");
    cJSON *out = cJSON_AddArrayToObject(ss, "spans");

    for (size_t i = 0; i < n; i++) {
        const otel_span_t *s = &spans[i];
        const char *method = cwist_http_method_to_string(s->method);
        char num[24];
        cJSON *span = cJSON_CreateObject();
        cJSON_AddStringToObject(span, "traceId", s->trace_id);
        cJSON_AddStringToObject(span, "spanId", s->span_id);
        if (s->parent_span_id[0]) cJSON_AddStringToObject(span, "parentSpanId", s->parent_span_id);
        cJSON_AddStringToObject(span, "name", method);
        cJSON_AddNumberToObject(span, "kind", 2); /* SPAN_KIND_SERVER */
        snprintf(num, sizeof(num), "%llu", (unsigned long long)s->start_ns);
        cJSON_AddStringToObject(span, "startTimeUnixNano", num);
        snprintf(num, sizeof(num), "%llu", (unsigned long long)s->end_ns);
        cJSON_AddStringToObject(span, "endTimeUnixNano", num);
        cJSON *attrs = cJSON_AddArrayToObject(span, "attributes");
        cJSON_AddItemToArray(attrs, otel_attr("http.request.method", cJSON_CreateString(method),
                                         "stringValue"));
        cJSON_AddItemToArray(attrs, otel_attr("url.path", cJSON_CreateString(s->path), "stringValue"));
        snprintf(num, sizeof(num), "%d", s->status);
        cJSON_AddItemToArray(attrs, otel_attr("http.response.status_code", cJSON_CreateString(num),
                                         "intValue"));
        if (s->status >= 500) cJSON_AddNumberToObject(cJSON_AddObjectToObject(span, "status"), "code", 2);
        cJSON_AddItemToArray(out, span);
    }
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return body;
}

static void otel_export_batch(cwist_http_client *client, const otel_span_t *spans, size_t n) {
    char *body = otel_encode_spans(spans, n);
    if (!body) return;
    cwist_http_header_node *headers = NULL;
    cwist_http_header_add(&headers, "Content-Type", "application/json");
    cwist_http_response *res = NULL;
    cwist_http_client_request(client, g_otel.endpoint, CWIST_HTTP_POST, headers, body,
                              strlen(body), &res);
    if (res) cwist_http_response_destroy(res);
    cwist_http_header_free_all(headers);
    cJSON_free(body);
}

static void *otel_exporter_main(void *arg) {
    (void)arg;
    cwist_http_client *client = cwist_http_client_create();
    otel_span_t *batch = cwist_alloc_array(g_otel.batch, sizeof(*batch));
    if (client) cwist_http_client_set_timeout_ms(client, OTEL_EXPORT_TIMEOUT_MS);

    pthread_mutex_lock(&g_otel_lock);
    for (;;) {
        while (g_otel.count == 0) pthread_cond_wait(&g_otel_work, &g_otel_lock);
        size_t n = g_otel.count < g_otel.batch ? g_otel.count : g_otel.batch;
        for (size_t i = 0; i < n; i++) {
            if (batch) batch[i] = g_otel.ring[g_otel.head];
            g_otel.head = (g_otel.head + 1) % g_otel.cap;
        }
        g_otel.count -= n;
        pthread_mutex_unlock(&g_otel_lock);
        if (client && batch) otel_export_batch(client, batch, n);
        pthread_mutex_lock(&g_otel_lock);
    }
    return NULL;
}

static void otel_atfork_prepare(void) {
    pthread_mutex_lock(&g_otel_lock);
}

static void otel_atfork_parent(void) {
    pthread_mutex_unlock(&g_otel_lock);
}

/* The parent exports its own queued spans; the child starts empty and spawns
 * its exporter on its first span. */
static void otel_atfork_child(void) {
    g_otel.head = g_otel.count = 0;
    g_otel.exporter_alive = false;
    pthread_cond_t fresh = PTHREAD_COND_INITIALIZER;
    g_otel_work = fresh;
    pthread_mutex_unlock(&g_otel_lock);
}

static void otel_enqueue(const otel_span_t *span) {
    pthread_mutex_lock(&g_otel_lock);
    if (!g_otel.exporter_alive)
        g_otel.exporter_alive = pthread_create(&g_otel.exporter, NULL, otel_exporter_main, NULL) == 0;
    if (g_otel.exporter_alive) {
        if (g_otel.count == g_otel.cap) { /* full: drop the oldest */
            g_otel.head = (g_otel.head + 1) % g_otel.cap;
            g_otel.count--;
        }
        g_otel.ring[(g_otel.head + g_otel.count) % g_otel.cap] = *span;
        g_otel.count++;
        pthread_cond_signal(&g_otel_work);
    }
    pthread_mutex_unlock(&g_otel_lock);
}

static void otel_middleware(cwist_http_request *req, cwist_http_response *res,
                            cwist_handler_func next) {
    otel_span_t span = {.method = req->method, .start_ns = otel_unix_nanos()};
    if (!parse_traceparent(cwist_http_header_get(req->headers, "traceparent"), span.trace_id,
                           span.parent_span_id))
        otel_random_id(span.trace_id, OTEL_TRACE_ID_HEX);
    otel_random_id(span.span_id, OTEL_SPAN_ID_HEX);

    otel_ctx ctx = {.req = req, .trace_id = span.trace_id, .outer = t_otel_ctx};
    t_otel_ctx = &ctx;
    next(req, res);
    t_otel_ctx = ctx.outer;

    span.end_ns = otel_unix_nanos();
    span.status = (int)res->status_code;
    snprintf(span.path, sizeof(span.path), "%s",
             req->path && req->path->data ? req->path->data : "/");
    otel_enqueue(&span);
}

cwist_middleware_func cwist_mw_otel_trace(const cwist_otel_cfg_t *cfg) {
    pthread_mutex_lock(&g_otel_lock);
    if (!g_otel.configured) {
        const char *endpoint = cfg && cfg->endpoint ? cfg->endpoint : OTEL_DEFAULT_ENDPOINT;
        const char *service = cfg && cfg->service_name ? cfg->service_name : OTEL_DEFAULT_SERVICE;
        size_t cap = cfg && cfg->queue_cap ? cfg->queue_cap : OTEL_DEFAULT_QUEUE;
        size_t batch = cfg && cfg->batch_size ? cfg->batch_size : OTEL_DEFAULT_BATCH;
        g_otel.endpoint = cwist_strdup(endpoint);
        g_otel.service_name = cwist_strdup(service);
        g_otel.ring = cwist_alloc_array(cap, sizeof(*g_otel.ring));
        if (g_otel.endpoint && g_otel.service_name && g_otel.ring &&
            pthread_atfork(otel_atfork_prepare, otel_atfork_parent, otel_atfork_child) == 0) {
            g_otel.cap = cap;
            g_otel.batch = batch < cap ? batch : cap;
            g_otel.configured = true;
        } else {
            cwist_free(g_otel.endpoint);
            cwist_free(g_otel.service_name);
            cwist_free(g_otel.ring);
            g_otel.endpoint = g_otel.service_name = NULL;
            g_otel.ring = NULL;
        }
    }
    bool ok = g_otel.configured;
    pthread_mutex_unlock(&g_otel_lock);
    return ok ? otel_middleware : NULL;
}

const char *cwist_mw_otel_get_trace_id(const cwist_http_request *req) {
    if (!req) return NULL;
    for (const otel_ctx *ctx = t_otel_ctx; ctx; ctx = ctx->outer) {
        if (ctx->req == req) return ctx->trace_id;
    }
    return NULL;
}
