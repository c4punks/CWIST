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
```c
cwist_app_use(app, cwist_mw_access_log(CWIST_LOG_COMBINED));
```

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
Inspects `Accept-Encoding`, compresses the response body with a registered backend (gzip/zstd), and adds `Content-Encoding`. Only compresses bodies at or above `min_body_size` bytes.
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


