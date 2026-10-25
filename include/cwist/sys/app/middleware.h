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

/**
 * @brief Configuration for cwist_mw_otel_trace().
 *
 * Every field is optional (NULL / 0 selects the default); the strings are
 * copied.
 */
typedef struct {
    const char *endpoint;     /**< OTLP/HTTP traces URL.
                               *   Default: "http://localhost:4318/v1/traces". */
    const char *service_name; /**< Resource attribute service.name. Default: "cwist". */
    size_t queue_cap;         /**< Spans buffered per process. Default: 512. */
    size_t batch_size;        /**< Spans per export request. Default: 64. */
} cwist_otel_cfg_t;

/**
 * @brief OpenTelemetry tracing middleware.
 *
 * Records one SERVER span per request covering the rest of the chain, with
 * the method, path, status and start/end time, and exports the spans to the
 * collector as OTLP/HTTP JSON from a background thread in each worker
 * process.  A valid W3C @c traceparent header makes the span a child of the
 * caller's span; otherwise a new trace starts.
 *
 * Export is fail-open: a full queue drops its oldest span and a failed export
 * drops the batch; requests are never slowed or failed by the collector.
 *
 * There is one exporter per process: the first call fixes the configuration
 * and later calls return the same middleware.
 *
 * @param cfg Exporter configuration, or NULL for the defaults.
 * @return The middleware, or NULL when memory could not be had.
 */
cwist_middleware_func cwist_mw_otel_trace(const cwist_otel_cfg_t *cfg);

/**
 * @brief Trace id of the span cwist_mw_otel_trace() opened for @p req.
 *
 * @param req The current request.
 * @return 32 lowercase hex characters (the caller's trace id when the request
 *         carried a valid @c traceparent, a fresh one otherwise), or NULL when
 *         @p req is not running behind the middleware on this thread.
 */
const char *cwist_mw_otel_get_trace_id(const cwist_http_request *req);

#endif
