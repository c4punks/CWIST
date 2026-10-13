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

By default each access-log line, and each `cwist_logger_*` line, is written to stdout or stderr by the thread that handles the request. Call `cwist_log_async_start()` once at startup to have one background thread write them instead. The request thread then only formats the line and copies it into a buffer.

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


