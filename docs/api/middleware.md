# Middleware API

Middleware allows you to intercept and process requests before they reach the final handler.

## Functions

### `cwist_app_use`
Registers a global middleware. Middlewares are executed in the order they are registered.
```c
void cwist_app_use(cwist_app *app, cwist_middleware_func mw);
```

## Built-in Middlewares

### Request ID Middleware
Generates a unique ID for each request and adds it to the `X-Request-Id` header in both request and response.
```c
#include <cwist/sys/app/middleware.h>
cwist_app_use(app, cwist_mw_request_id(NULL));
```

### Access Log Middleware
Logs request details (method, path, status, latency) to stdout.
For a response deferred with `cwist_async_defer()` the status and size are not known when the chain returns; they are logged as `-` (`null` in the JSON format).
```c
cwist_app_use(app, cwist_mw_access_log(CWIST_LOG_COMBINED));
```

#### Async log output

By default each access-log line, and each `cwist_logger_*` line, is written to stdout or stderr by the thread that handles the request, using block-buffered stdio (a TTY destination stays line-buffered), so lines may reach the fd in batches rather than one at a time. Call `cwist_log_async_start()` once at startup to have one background thread write them instead — that is also the way to get immediate per-line delivery. The request thread then only formats the line and copies it into a buffer.

```c
#include <cwist/sys/app/logger.h>

cwist_log_async_start(0);   /* 0 = 256 KiB buffer per stream */
cwist_app_use(app, cwist_mw_access_log(CWIST_LOG_COMBINED));
cwist_app_listen(app, 8080);
cwist_log_async_stop();     /* writes what is still queued */
```

- **Lines:** each line stays whole, and lines on one stream keep the order they were logged in.
- **Full buffer:** when the buffer is full, the caller waits for space. No line is dropped.
- **Long lines:** a line larger than the buffer is written synchronously, so it can land out of order.
- **Exit:** lines still queued at `exit()` are written by an `atexit` hook. Call `cwist_log_async_flush()` before `_exit()`.
- **Workers:** forked workers (`CWIST_WORKERS`) each start their own writer on their first line.
- **Mixing:** output written with `printf` on the same stream is not ordered against queued lines.
- **WASM:** not available. `cwist_log_async_start()` returns `false` and logging stays synchronous.

### Rate Limiter Middleware
Limits the number of requests per minute from a single IP.
```c
cwist_app_use(app, cwist_mw_rate_limit_ip(60));
```

### Redis-backed shared rate limiter

The local limiter above keeps its buckets in one process. To share one
limit across independent server processes, give each process its own
**dedicated pool on DB 0, with the default user and no AUTH**, and use the
same prefix and limit. Replacement connections start fresh: AUTH, SELECT,
HELLO and other connection state are not replayed. Legacy calls that share
this pool may also receive fresh connections after a timed call discards one.
Use one prefix per rate policy: different limits with the same prefix
share one counter, not independent buckets.

```c
cwist_redis_pool_t *pool = cwist_redis_pool_create("127.0.0.1", 6379, 4);
cwist_rate_limit_redis_cfg_t cfg = {
    .pool = pool,
    .requests_per_minute = 60,
    .key_prefix = "my-service:rate:",
    .timeout_ms = 50,
    .fail_closed = true,
};
cwist_middleware_func limit = cwist_mw_rate_limit_ip_redis(&cfg);
/* Inside main: fail startup if registration did not succeed. */
if (!pool || !limit) return 1;
cwist_app_use(app, limit);
/* One process per instance: do not fork a live pool. */
cwist_app_listen_ex(app, 8080, 1, -1);
cwist_redis_pool_destroy(pool);
```

Each key is `prefix + peer-IP`. One Lua EVAL increments the counter and
sets a 60-second expiry on the first request. It repairs missing expiry
without extending a running window. The window starts at the first hit,
not a process-local clock boundary. Allowed and rejected attempts count.
Over-limit replies are 429 with a conservative `Retry-After: 60`.

With `fail_closed = false` (default), backend errors allow the request.
With `true`, errors return 503, not 429. This includes timeout, pool
exhaustion, malformed replies, and a missing socket peer. A failed command
is never retried: it might already have incremented the counter. There is
no local fallback counter.

The request-time budget covers pool wait, cached-peer reconnect, write,
and reply I/O. Zero selects 50 ms; the maximum is 60000 ms. This is a
blocking middleware, not an async reactor call. Scheduling delays are not
a real-time guarantee. Initial pool creation and DNS are outside this
budget. The first backend failure per config and process emits a warning
with the error code and policy to stderr. Keys and credentials are not logged.
The Redis command budget does not bound application logging.
Failed initial pool creation or invalid config returns NULL.

Create pools after fork, separately in each serving process. Inherited
pools are rejected by the timed API. Keep the pool alive until every call
has stopped, then destroy it. Do not register a pre-fork pool for automatic
`CWIST_WORKERS` children; this API does not add a worker-start hook. Eight
distinct immutable configs fit per process; equal configs reuse a slot.
Prefixes are copied and limited to 128 bytes.

Only the socket peer is used. Forwarded headers cannot choose the key.
A request without a connected TCP peer fd (for example, a QUIC request)
follows the missing-peer failure policy; this API does not extract QUIC
path metadata. IPv6 keys use the full address (/128), not a /64 prefix;
clients that rotate IPv6 addresses obtain separate buckets.
Behind a reverse proxy, this limits the proxy address unless a trusted
upstream preserves client source addresses. Redis RESP2 is plaintext: use
loopback, a private network, or a separate TLS tunnel. The default user's
ACL must allow EVAL, INCR, PTTL, and PEXPIRE. A server requiring AUTH makes
every call fail; the configured failure policy then applies to every request.
Eviction, failover, or external deletion
can reset counters; this is not a global consistency guarantee across
Redis data loss.

Run `make redis-rate-limit-e2e` with redis-server installed. The runner
starts its own loopback Redis and never silently skips. Two C processes
create separate pools and invoke the middleware with real TCP peer fds.
Their 40 attempts share seven admissions. The test also checks TTL,
expiry, namespaces, and failure policies. The normal unit target skips
only the live-Redis part when no test port is supplied.

### CORS Middleware
Enables Cross-Origin Resource Sharing (CORS) support.
- Adds `Access-Control-Allow-Origin: *` to all responses.
- Handles `OPTIONS` preflight requests with a 204 No Content status and appropriate headers, short-circuiting the request processing.
```c
cwist_app_use(app, cwist_mw_cors());
```

### Prometheus Metrics Middleware
Exposes a `/metrics` endpoint in Prometheus exposition format. Tracks request counts, latency histograms, and active connections.
```c
#include <cwist/sys/app/middleware.h>
cwist_app_use(app, cwist_mw_metrics());
```

### JWT Auth Middleware
Validates a `Bearer` token in the `Authorization` header using HMAC-SHA256. Responds with `401 Unauthorized` and short-circuits the chain on failure. Decoded claims are available to downstream handlers via `cwist_mw_jwt_get_claims()`.
```c
cwist_app_use(app, cwist_mw_jwt_auth("my-secret"));

// Inside a handler behind the middleware:
const cwist_jwt_claims *claims = cwist_mw_jwt_get_claims(req);
```
`secret` must be a null-terminated string that outlives the middleware invocations (typically a static or global string).

### Compression Middleware
Inspects `Accept-Encoding`, compresses the response body with a registered backend (gzip/zstd), and adds `Content-Encoding`. Only compresses bodies at or above `min_body_size` bytes. A response deferred with `cwist_async_defer()` is sent uncompressed: it belongs to its completion once the handler defers it.
```c
cwist_app_use(app, cwist_mw_compress(1024)); /* compress responses >= 1 KiB */
```

## Creating Custom Middleware
A middleware is a function that takes `req`, `res`, and a `next` callback.

**Thread-Safety Note**: Middlewares are executed in a multithreaded environment. Any access to shared resources must be synchronized.

```c
static pthread_mutex_t count_mutex = PTHREAD_MUTEX_INITIALIZER;
static int total_requests = 0;

void my_middleware(cwist_http_request *req, cwist_http_response *res, cwist_handler_func next) {
    pthread_mutex_lock(&count_mutex);
    total_requests++;
    pthread_mutex_unlock(&count_mutex);
    
    next(req, res);
}
```


