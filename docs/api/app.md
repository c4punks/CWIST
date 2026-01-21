# Framework & App API

*Header:* `<cwist/app.h>` (Proposed)

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

### `cwist_app_get`, `cwist_app_post`
```c
void cwist_app_get(cwist_app *app, const char *path, cwist_handler_func handler);
void cwist_app_post(cwist_app *app, const char *path, cwist_handler_func handler);
```
Registers a route handler for a specific method and path.

### `cwist_app_listen`
```c
int cwist_app_listen(cwist_app *app, int port);
```
Starts the server loop on the specified port.
