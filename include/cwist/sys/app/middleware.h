/**
 * @file middleware.h
 * @brief Built-in middleware factories.
 */

#ifndef __CWIST_MIDDLEWARE_H__
#define __CWIST_MIDDLEWARE_H__

#include <cwist/sys/app/app.h>
#include <cwist/sys/app/waf.h>
#include <cwist/security/jwt/jwt.h>

/**
 * @brief Request ID middleware.
 * @param header_name Optional override for the header (defaults to "X-Request-Id").
 */
cwist_middleware_func cwist_mw_request_id(const char *header_name);

/** @brief Access log middleware output formats. */
typedef enum { CWIST_LOG_COMMON, CWIST_LOG_COMBINED, CWIST_LOG_JSON } cwist_log_format_t;

/** @brief Access log middleware factory. */
cwist_middleware_func cwist_mw_access_log(cwist_log_format_t format);

/** @brief Fixed-window rate limiter middleware (per-IP). */
cwist_middleware_func cwist_mw_rate_limit_ip(int requests_per_minute);

/** @brief Reset the internal rate-limit IP bucket table (test helper). */
void cwist_mw_rate_limit_reset(void);

/** @brief Prometheus metrics collection middleware. */
cwist_middleware_func cwist_mw_metrics(void);

/**
 * @brief CORS middleware.
 * Adds CORS headers and handles OPTIONS requests (returns 204).
 */
cwist_middleware_func cwist_mw_cors(void);

/**
 * @brief JWT authentication middleware factory.
 *
 * Reads the Authorization header, expects "Bearer <token>".
 * On success the decoded claims are available via cwist_mw_jwt_get_claims().
 * On failure responds with 401 Unauthorized and short-circuits the chain.
 *
 * @param secret HMAC-SHA256 signing secret (null-terminated, must outlive the
 *               middleware invocations - typically a static/global string).
 * @return Middleware function pointer, or NULL when @p secret is NULL or
 *         eight distinct secrets are already registered in this process.
 */
cwist_middleware_func cwist_mw_jwt_auth(const char *secret);

/**
 * @brief Response compression middleware factory.
 *
 * Inspects Accept-Encoding, compresses the response body using a registered
 * backend, and adds the Content-Encoding header.
 *
 * @param min_body_size Minimum uncompressed body size (bytes) to trigger compression.
 * @return Middleware function pointer.
 */
cwist_middleware_func cwist_mw_compress(size_t min_body_size);

/**
 * @brief Retrieve JWT claims stored by cwist_mw_jwt_auth from a request.
 *
 * Only valid inside a handler that sits behind the JWT middleware, on the
 * thread running that request's middleware chain; the claims are released
 * when the chain returns to the middleware.
 *
 * @param req The current HTTP request.
 * @return Read-only pointer to the claims, or NULL if not authenticated.
 */
const cwist_jwt_claims *cwist_mw_jwt_get_claims(const cwist_http_request *req);

/* ---- OpenTelemetry tracing middleware ------------------------------------ */

/** @brief OTLP exporter protocol. */
typedef enum {
    CWIST_OTEL_PROTO_HTTP = 0, /**< OTLP/HTTP (binary protobuf). Default. */
    CWIST_OTEL_PROTO_GRPC,     /**< OTLP/gRPC. */
} cwist_otel_proto_t;

/**
 * @brief Configuration for cwist_mw_otel_trace().
 *
 * All string fields are borrowed: they must outlive every request that
 * passes through the middleware (typically static or global).
 */
typedef struct {
    const char       *endpoint;     /**< OTLP collector endpoint URL.
                                     *   Default: "http://localhost:4318/v1/traces". */
    const char       *service_name; /**< OTel resource service.name attribute.
                                     *   Default: "cwist". */
    cwist_otel_proto_t proto;       /**< Exporter wire protocol. */
    size_t            queue_cap;    /**< Max spans buffered before export.
                                     *   0 uses the built-in default (512). */
    size_t            batch_size;   /**< Spans per export batch.
                                     *   0 uses the built-in default (64). */
} cwist_otel_cfg_t;

/**
 * @brief Create an OpenTelemetry tracing middleware instance.
 *
 * Emits one span per request covering the full middleware chain.  Each
 * span records the HTTP method, route path, response status, and
 * duration.  The W3C @c traceparent header is read from the incoming
 * request; the extracted trace-id and parent-span-id are propagated into
 * @p req so downstream handlers can read them via
 * cwist_mw_otel_get_trace_id().
 *
 * Export failures are fail-open: a collector that is down or slow never
 * fails or slows a request.  When the internal queue is full, the oldest
 * span is silently dropped.
 *
 * @param cfg Exporter configuration, or NULL for built-in defaults.
 * @return Middleware function pointer, or NULL on allocation failure.
 */
cwist_middleware_func cwist_mw_otel_trace(const cwist_otel_cfg_t *cfg);

/**
 * @brief Read the trace-id injected by cwist_mw_otel_trace() for this request.
 *
 * Returns a 32-hex-character lowercase string (128-bit W3C trace-id) if a
 * @c traceparent header was present and valid; returns NULL otherwise.
 * Only valid inside handlers or middleware that run downstream of
 * cwist_mw_otel_trace().
 *
 * @param req The current HTTP request.
 * @return Borrowed pointer to the trace-id string, or NULL.
 */
const char *cwist_mw_otel_get_trace_id(const cwist_http_request *req);

#endif
