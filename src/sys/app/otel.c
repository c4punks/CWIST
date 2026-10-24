/**
 * @file otel.c
 * @brief OpenTelemetry tracing middleware (#402).
 *
 * cwist_mw_otel_trace() configures a process-global OTel exporter and
 * returns a cwist_middleware_func that emits one span per request.
 *
 * Each span records:
 *   - W3C trace-id extracted from the incoming "traceparent" header, or a
 *     freshly generated one when no header is present.
 *   - HTTP method, route path, response status, and wall-clock duration.
 *
 * Export is fail-open: an unreachable collector never blocks or fails a
 * request.  When the internal bounded queue fills, the oldest entry is
 * dropped silently.
 *
 * Wire encoding: a minimal JSON-over-HTTP scaffold for v4.1; a follow-up
 * can replace the export_spans() call with binary OTLP protobuf without
 * changing the middleware or public API.
 */
#define _POSIX_C_SOURCE 200809L
#include <cwist/sys/app/middleware.h>
#include <cwist/core/mem/alloc.h>
#include <cwist/net/http/http.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define OTEL_DEFAULT_ENDPOINT "http://localhost:4318/v1/traces"
#define OTEL_DEFAULT_SERVICE  "cwist"
#define OTEL_DEFAULT_QUEUE    512U
#define OTEL_DEFAULT_BATCH    64U
#define OTEL_TRACE_ID_HEX     32U
#define OTEL_SPAN_ID_HEX      16U

/* ---- Span record -------------------------------------------------------- */

typedef struct {
    char   trace_id[OTEL_TRACE_ID_HEX + 1];
    char   span_id[OTEL_SPAN_ID_HEX + 1];
    char   parent_span_id[OTEL_SPAN_ID_HEX + 1];
    char   method[16];
    char   path[256];
    int    status;
    double duration_ms;
} otel_span_t;

/* ---- Bounded ring-buffer queue ------------------------------------------ */

typedef struct {
    otel_span_t    *buf;
    size_t          cap;
    size_t          head;
    size_t          count;
    pthread_mutex_t mu;
    pthread_cond_t  not_empty;
    int             stop;
} otel_queue_t;

static void q_push(otel_queue_t *q, const otel_span_t *s) {
    pthread_mutex_lock(&q->mu);
    if (q->count == q->cap) {
        q->head = (q->head + 1) % q->cap;
        q->count--;
    }
    q->buf[(q->head + q->count) % q->cap] = *s;
    q->count++;
    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->mu);
}

static int q_pop_batch(otel_queue_t *q, otel_span_t *out, size_t cap, size_t *n) {
    pthread_mutex_lock(&q->mu);
    while (q->count == 0 && !q->stop)
        pthread_cond_wait(&q->not_empty, &q->mu);
    if (q->count == 0) { pthread_mutex_unlock(&q->mu); return -1; }
    *n = q->count < cap ? q->count : cap;
    for (size_t i = 0; i < *n; i++) {
        out[i] = q->buf[q->head];
        q->head = (q->head + 1) % q->cap;
    }
    q->count -= *n;
    pthread_mutex_unlock(&q->mu);
    return 0;
}

/* ---- Process-global state ----------------------------------------------- */

static struct {
    cwist_otel_cfg_t cfg;
    otel_queue_t     queue;
    pthread_t        thread;
    pthread_once_t   init;
    int              active;
} g_otel = { .init = PTHREAD_ONCE_INIT };

/* ---- Helpers ------------------------------------------------------------ */

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static void gen_hex_rand(char *out, size_t nibbles) {
    static const char h[] = "0123456789abcdef";
    for (size_t i = 0; i < nibbles; i++)
        out[i] = h[rand() & 0xF];
    out[nibbles] = '\0';
}

static int parse_traceparent(const char *hdr, char *trace_id, char *span_id) {
    if (!hdr || strncmp(hdr, "00-", 3) != 0) return 0;
    const char *p = hdr + 3;
    if (strlen(p) < OTEL_TRACE_ID_HEX + 1 + OTEL_SPAN_ID_HEX + 3) return 0;
    memcpy(trace_id, p, OTEL_TRACE_ID_HEX);
    trace_id[OTEL_TRACE_ID_HEX] = '\0';
    memcpy(span_id, p + OTEL_TRACE_ID_HEX + 1, OTEL_SPAN_ID_HEX);
    span_id[OTEL_SPAN_ID_HEX] = '\0';
    return 1;
}

/* ---- OTLP export (JSON stub) -------------------------------------------- */

static void export_spans(const otel_span_t *spans, size_t n) {
    if (!n) return;
    const char *svc = g_otel.cfg.service_name ? g_otel.cfg.service_name
                                               : OTEL_DEFAULT_SERVICE;
    char body[65536];
    int pos = snprintf(body, sizeof(body),
        "{\"resourceSpans\":[{\"resource\":{\"attributes\":["
        "{\"key\":\"service.name\",\"value\":{\"stringValue\":\"%s\"}}]},"
        "\"scopeSpans\":[{\"spans\":[", svc);
    for (size_t i = 0; i < n && pos < (int)sizeof(body) - 512; i++) {
        const otel_span_t *s = &spans[i];
        pos += snprintf(body + pos, sizeof(body) - (size_t)pos,
            "%s{\"traceId\":\"%s\",\"spanId\":\"%s\","
            "\"parentSpanId\":\"%s\",\"name\":\"%s %s\",\"kind\":2,"
            "\"attributes\":["
            "{\"key\":\"http.method\",\"value\":{\"stringValue\":\"%s\"}},"
            "{\"key\":\"http.target\",\"value\":{\"stringValue\":\"%s\"}},"
            "{\"key\":\"http.status_code\",\"value\":{\"intValue\":%d}}]}",
            i ? "," : "",
            s->trace_id, s->span_id, s->parent_span_id,
            s->method, s->path, s->method, s->path, s->status);
    }
    if (pos < (int)sizeof(body) - 16)
        pos += snprintf(body + pos, sizeof(body) - (size_t)pos, "]}]}]}");
    (void)body; /* TODO: POST to g_otel.cfg.endpoint via cwist HTTP client. */
}

static void *exporter_thread(void *arg) {
    (void)arg;
    size_t batch = g_otel.cfg.batch_size ? g_otel.cfg.batch_size : OTEL_DEFAULT_BATCH;
    otel_span_t *tmp = (otel_span_t *)cwist_alloc_array(batch, sizeof(otel_span_t));
    if (!tmp) return NULL;
    size_t n;
    while (q_pop_batch(&g_otel.queue, tmp, batch, &n) == 0)
        export_spans(tmp, n);
    cwist_free(tmp);
    return NULL;
}

/* ---- Middleware function ------------------------------------------------ */

static const char TRACE_ID_HDR[] = "X-Cwist-Trace-Id";

static void otel_middleware(cwist_http_request *req, cwist_http_response *res,
                            void (*next)(cwist_http_request *, cwist_http_response *)) {
    double t0 = now_ms();

    otel_span_t span;
    memset(&span, 0, sizeof(span));

    const char *tp = cwist_http_header_get(req->headers, "traceparent");
    if (!parse_traceparent(tp, span.trace_id, span.parent_span_id))
        gen_hex_rand(span.trace_id, OTEL_TRACE_ID_HEX);
    gen_hex_rand(span.span_id, OTEL_SPAN_ID_HEX);

    /* Propagate trace-id into the request for downstream handlers. */
    cwist_http_header_add(&req->headers, TRACE_ID_HDR, span.trace_id);

    next(req, res);

    span.duration_ms = now_ms() - t0;
    span.status      = res->status;
    const char *path = req->path ? cwist_sstring_cstr(req->path) : "/";
    snprintf(span.path, sizeof(span.path), "%s", path ? path : "/");
    static const char *methods[] = {"GET","POST","PUT","DELETE","PATCH"};
    static const int method_ids[] = {
        CWIST_HTTP_GET, CWIST_HTTP_POST, CWIST_HTTP_PUT,
        CWIST_HTTP_DELETE, CWIST_HTTP_PATCH
    };
    const char *mname = "OTHER";
    for (int i = 0; i < 5; i++) {
        if (req->method == method_ids[i]) { mname = methods[i]; break; }
    }
    snprintf(span.method, sizeof(span.method), "%s", mname);

    q_push(&g_otel.queue, &span);
}

/* ---- Public API --------------------------------------------------------- */

cwist_middleware_func cwist_mw_otel_trace(const cwist_otel_cfg_t *cfg) {
    if (!g_otel.active) {
        size_t qcap = (cfg && cfg->queue_cap) ? cfg->queue_cap : OTEL_DEFAULT_QUEUE;
        g_otel.queue.buf = (otel_span_t *)cwist_alloc_array(qcap, sizeof(otel_span_t));
        if (!g_otel.queue.buf) return NULL;
        g_otel.queue.cap  = qcap;
        g_otel.queue.head = 0;
        g_otel.queue.count = 0;
        g_otel.queue.stop  = 0;
        pthread_mutex_init(&g_otel.queue.mu, NULL);
        pthread_cond_init(&g_otel.queue.not_empty, NULL);

        if (cfg) g_otel.cfg = *cfg;
        else {
            g_otel.cfg.endpoint     = OTEL_DEFAULT_ENDPOINT;
            g_otel.cfg.service_name = OTEL_DEFAULT_SERVICE;
            g_otel.cfg.proto        = CWIST_OTEL_PROTO_HTTP;
        }

        if (pthread_create(&g_otel.thread, NULL, exporter_thread, NULL) != 0) {
            cwist_free(g_otel.queue.buf);
            return NULL;
        }
        g_otel.active = 1;
    }
    return otel_middleware;
}

const char *cwist_mw_otel_get_trace_id(const cwist_http_request *req) {
    if (!req) return NULL;
    return cwist_http_header_get(req->headers, TRACE_ID_HDR);
}
