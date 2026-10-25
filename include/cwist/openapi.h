/**
 * @file openapi.h
 * @brief OpenAPI 3.1 generation — source annotations and runtime serving.
 *
 * The CWIST_OPENAPI_* macros register a route normally and leave a parseable
 * declaration in C source for off-line tooling (`cwist openapi`).
 *
 * cwist_app_enable_openapi() serves an OpenAPI 3.1 document built from the
 * app's live route table on every request: each HTTP route appears with its
 * path (":id" segments become "{id}" path parameters) and method, however it
 * was registered.  cwist_openapi_annotate() adds a summary and tags.
 *
 * Minimal example:
 * @code
 *   cwist_openapi_meta_t meta = {
 *       .title   = "My API",
 *       .version = "1.0.0",
 *   };
 *   cwist_app_enable_openapi(app, "/openapi.json", &meta);
 *
 *   CWIST_OPENAPI_GET(app, "/users", list_users);
 *   cwist_openapi_annotate(app, "GET", "/users", "List users", "users");
 * @endcode
 */
#ifndef CWIST_OPENAPI_H
#define CWIST_OPENAPI_H

#include <cwist/sys/app/app.h>

/* Source-level route macros: preserve normal runtime routing while leaving an
 * unambiguous, parseable declaration for documentation tooling. */
#define CWIST_OPENAPI_GET(app, path, handler) cwist_app_get((app), (path), (handler))
#define CWIST_OPENAPI_POST(app, path, handler) cwist_app_post((app), (path), (handler))
#define CWIST_OPENAPI_PUT(app, path, handler) cwist_app_put((app), (path), (handler))
#define CWIST_OPENAPI_DELETE(app, path, handler) cwist_app_delete((app), (path), (handler))
#define CWIST_OPENAPI_PATCH(app, path, handler) cwist_app_patch((app), (path), (handler))

/**
 * @brief Per-application metadata for the generated OpenAPI document.
 *
 * Every field is optional: NULL title/version fall back to "CWIST API" and
 * "1.0.0", NULL server_url to "/".  The strings are copied.
 */
typedef struct {
    const char *title;       /**< API title (info.title). */
    const char *version;     /**< API version string, e.g. "1.0.0". */
    const char *description; /**< Optional description (info.description). */
    const char *server_url;  /**< Optional server URL; defaults to "/". */
} cwist_openapi_meta_t;

/**
 * @brief Serve the app's OpenAPI 3.1 document.
 *
 * Registers GET @p json_path.  The document lists every HTTP route of @p app
 * at request time (the spec route itself and WebSocket routes excepted), so
 * routes added before or after this call are all included.  A second call on
 * the same @p app is ignored.
 *
 * @param app       Application to serve the document from.
 * @param json_path URL path of the document; NULL means "/openapi.json".
 * @param meta      Document metadata; NULL uses the defaults.
 */
void cwist_app_enable_openapi(cwist_app *app, const char *json_path,
                              const cwist_openapi_meta_t *meta);

/**
 * @brief Attach a summary and tags to one operation of the document.
 *
 * Takes effect only after cwist_app_enable_openapi() on the same @p app.
 * Annotating the same method and path again replaces the earlier values.
 *
 * @param app     Application owning the route.
 * @param method  HTTP method name, case-insensitive ("GET", "post", ...).
 * @param path    Route path exactly as registered (e.g. "/users/:id").
 * @param summary Operation summary, or NULL.
 * @param tags    Comma-separated tag list, or NULL.
 */
void cwist_openapi_annotate(cwist_app *app, const char *method, const char *path,
                            const char *summary, const char *tags);

#endif
