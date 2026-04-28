# Framework & App API

*Header:* `<cwist/sys/app/app.h>` (Proposed)

High-level abstractions for building web applications quickly.

### `cwist_app_create`
```c
cwist_app *cwist_app_create(void);
```
Initializes a new web application instance with default security settings.

### `cwist_app_use_https`
```c
cwist_error_t cwist_app_use_https(cwist_app *app, const char *cert_path, const char *key_path);
```
Enables HTTPS for the application.

### `cwist_app_use_https2` / `cwist_use_https2`
```c
cwist_error_t cwist_app_use_https2(cwist_app *app, bool enabled);

// Convenience macro when your app variable is named `app`
cwist_use_https2(true);
```
Keeps the default HTTPS request path in legacy `HTTP/1.1` mode unless explicitly enabled.
When enabled, CWIST rebuilds the TLS context with an HTTP/2-compatible TLS profile, negotiates `h2` through ALPN, and swaps in the HTTPS/2 request-handler slot for the app.
The HTTP/2 handler feeds decoded requests into the existing routing/middleware stack, so handler code does not need to change.
The current implementation is intentionally narrow: one request stream is handled at a time per TLS connection, with support for the standard client preface, `SETTINGS`, `HEADERS`, `CONTINUATION`, `DATA`, `PING`, and `GOAWAY`.

### `cwist_app_use_db`
```c
cwist_error_t cwist_app_use_db(cwist_app *app, const char *db_path);
cwist_db *cwist_app_get_db(cwist_app *app);
```
Opens (or reopens) a SQLite database and keeps the handle on the `cwist_app`. Each incoming request automatically receives the pointer via `req->db`, so handlers can run queries without wiring globals:
```c
void profile_handler(cwist_http_request *req, cwist_http_response *res) {
    cJSON *rows = NULL;
    if (cwist_db_query(req->db, "SELECT name FROM profiles LIMIT 1", &rows).error.err_i16 == 0) {
        char *json = cJSON_PrintUnformatted(rows);
        cwist_sstring_assign(res->body, json);
        free(json);
        cJSON_Delete(rows);
    }
}
```

## Routing

### `cwist_app_get` / `cwist_app_post`
Registers standard HTTP handlers. Supports path parameters using `:name` syntax.

```c
void user_handler(cwist_http_request *req, cwist_http_response *res) {
    char *user_id = cwist_query_map_get(req->path_params, "id");
    // ...
}

cwist_app_get(app, "/users/:id", user_handler);
```

### `cwist_app_get_ex` / `_post_ex` / `_put_ex` / `_delete_ex` / `_patch_ex`
Register a route whose handler also receives a context pointer, so state can
be bound to a route without globals (for example a closure from a language
binding). Matching, `:param` paths, middleware and every transport behave
exactly as for `cwist_app_get()`.

```c
typedef void (*cwist_handler_ex_func)(void *user_ctx, cwist_http_request *req,
                                      cwist_http_response *res);
typedef void (*cwist_handler_ctx_destroy_func)(void *user_ctx);

cwist_error_t cwist_app_get_ex(cwist_app *app, const char *path, cwist_handler_ex_func handler,
                               void *user_ctx, cwist_handler_ctx_destroy_func destroy);
```

Ownership of `user_ctx` passes to the app on every call. If `destroy` is not
NULL it runs exactly once with `user_ctx`:

- when the app is destroyed;
- when the same method and path (one without `:param` segments) is
  registered again, by any routing function, unless the new registration
  passes the same non-NULL `user_ctx`;
- immediately, when the registration fails. The function then returns INT16
  `-1`; success is INT16 `0`.

Multiport sub-apps share the context without owning it. Pass `destroy = NULL`
to keep ownership with the caller. The handler can run on several worker
threads at once, so a shared context needs its own synchronization.

```c
typedef struct { const char *greeting; } greeter;

static void greet(void *user_ctx, cwist_http_request *req, cwist_http_response *res) {
    const greeter *g = user_ctx;
    (void)req;
    cwist_sstring_assign(res->body, g->greeting);
}

greeter *g = cwist_alloc(sizeof(*g));
g->greeting = "hello";
cwist_app_get_ex(app, "/greet", greet, g, cwist_free);
```

### `cwist_app_dispatch_memory`
Runs the full router/middleware/handler pipeline on a raw HTTP/1.x request held
in a memory buffer — no socket, thread, or event loop involved. The response is
serialized (status line + headers + body) into a freshly allocated buffer the
caller frees with `cwist_free()`. One-shot `Connection: close` semantics; this
is the in-memory entry point for embedded transports such as WASM hosts.

```c
char *res_buf;
size_t res_len;
const char *req = "GET /users/7 HTTP/1.1\r\nHost: x\r\n\r\n";
if (cwist_app_dispatch_memory(app, req, strlen(req), &res_buf, &res_len) == 0) {
    fwrite(res_buf, 1, res_len, stdout);
    cwist_free(res_buf);
}
```


### `cwist_app_ws`
Registers a WebSocket handler.
```c
void cwist_app_ws(cwist_app *app, const char *path, cwist_ws_handler_func handler);
```

Routes without parameters are stored in a hash table for O(1) lookups while parameterized patterns fall back to sequential matching.

### `cwist_endpoint_opt_t`
Each route can be annotated with a bitmask of behavioral flags:

| Flag | Description |
| ---- | ----------- |
| `CWIST_DYNAMIC` | Default dynamic handler behavior. |
| `CWIST_ENDPOINT_FIXED` | Persist the first response in the Big Dumb Reply cache and reuse it immediately on subsequent hits. |
| `CWIST_ENDPOINT_FILE` | Hint that the endpoint streams files, enabling Linux/BSD `sendfile` fast paths. |

Use the `_opt` helpers to set these flags:

```c
cwist_app_get_opt(app, "/feed", feed_handler, CWIST_ENDPOINT_FIXED);
cwist_app_get_opt(app,
                  "/download/:id",
                  download_handler,
                  CWIST_DYNAMIC | CWIST_ENDPOINT_FILE);
```

Flags may be OR-ed together to combine behaviors (e.g., `fixed + file`).

## Static Assets

### `cwist_app_static`
```c
cwist_error_t cwist_app_static(cwist_app *app, const char *url_prefix, const char *dir);
```
Mounts a directory (path normalization + traversal guards) at a URL prefix. Static responses still pass through the middleware chain and use `cwist_http_response_send_file` for MIME detection, traversal protection, and HEAD-aware `Content-Length`.

## Error Handling

### `cwist_app_set_error_handler`
Registers a global error handler for status codes >= 400 (e.g., 404 Not Found).
```c
void cwist_app_set_error_handler(cwist_app *app, cwist_error_handler_func handler);
```

### Unused Variables
Use the `CWIST_UNUSED()` macro to suppress compiler warnings for unused handler parameters.
```c
void my_handler(cwist_http_request *req, cwist_http_response *res) {
    CWIST_UNUSED(req);
    cwist_sstring_assign(res->body, "Hello");
}
```

## Concurrency & Thread-Safety

- **Multithreading**: `cwist_app_listen` starts a multithreaded server (one thread per request). Handlers and middleware MUST be thread-safe.
- **Global State**: Avoid using global variables in your handlers. If you need shared state, ensure it is protected by appropriate synchronization primitives (e.g., `pthread_mutex_t`).
- **Context Passing**: Each handler receives `req->app`/`req->db` for shared infrastructure, while `req->private_data` remains available for middleware-to-middleware communication.

## Memory Ownership Rules

- **Framework-Owned**: `cwist_http_request` and `cwist_http_response` objects passed to handlers are owned by the framework. Do NOT destroy them inside the handler.
- **Borrowed Strings**: Strings returned by `cwist_http_header_get` or `cwist_json_get_raw` are borrowed. If you need to keep them after the builder or request is destroyed, you must copy them.
- **Explicit Destruction**: Objects created with `_create` (e.g., `cwist_json_builder_create`, `cwist_websocket_upgrade`) MUST be destroyed by the caller using the corresponding `_destroy` function.

### `cwist_app_listen`
```c
int cwist_app_listen(cwist_app *app, int port);
```
Starts the server loop on the specified port.

## In-Memory Test Client

*Header:* `<cwist/sys/app/test_client.h>`

`cwist_test_client` dispatches synthetic HTTP requests directly through the
app's router and middleware without opening a socket, so integration tests
run in-process with no ports and no teardown race.

### `cwist_test_client_create` / `cwist_test_client_destroy`
```c
cwist_test_client *cwist_test_client_create(cwist_app *app);
void               cwist_test_client_destroy(cwist_test_client *client);
```
Creates and destroys a test client bound to `app`. The client keeps a
per-instance cookie jar that is carried automatically on subsequent requests.

### Request helpers
```c
cwist_http_response *cwist_test_client_get(cwist_test_client *client, const char *path);
cwist_http_response *cwist_test_client_post(cwist_test_client *client, const char *path,
                                            const char *body, const char *content_type);
cwist_http_response *cwist_test_client_post_json(cwist_test_client *client, const char *path,
                                                 const char *json_body);
cwist_http_response *cwist_test_client_put(cwist_test_client *client, const char *path,
                                           const char *body, const char *content_type);
cwist_http_response *cwist_test_client_delete(cwist_test_client *client, const char *path);
cwist_http_response *cwist_test_client_patch(cwist_test_client *client, const char *path,
                                             const char *body, const char *content_type);
```
Each returns a `cwist_http_response *` owned by the caller; free with
`cwist_http_response_destroy()`.

For full control over headers, cookies, and query string use:
```c
cwist_http_response *cwist_test_client_request_ex(cwist_test_client *client,
                                                  cwist_http_method_t method, const char *path,
                                                  const cwist_test_client_request_options *opts);
```

### Cookie jar
```c
void        cwist_test_client_set_cookie(cwist_test_client *client,
                                         const char *name, const char *value, const char *path);
const char *cwist_test_client_get_cookie(cwist_test_client *client, const char *name);
void        cwist_test_client_clear_cookies(cwist_test_client *client);
```

### Test assertion macros
```c
CWIST_ASSERT_STATUS(res, expected_status)
CWIST_ASSERT_HEADER(res, header_name, expected_value)
CWIST_ASSERT_BODY_CONTAINS(res, snippet)
```
Each macro prints a `[ASSERT FAIL]` message with file and line number and
calls `exit(1)` on failure, matching the style of the framework's own test
suite.
