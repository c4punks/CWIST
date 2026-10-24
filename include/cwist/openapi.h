/**
 * @file openapi.h
 * @brief OpenAPI 3.1 generation — source annotations and runtime serving.
 *
 * Two complementary mechanisms:
 *
 * 1. Source-level macros (CWIST_OPENAPI_GET etc.) leave an unambiguous,
 *    parseable declaration in C source for off-line tooling (e.g. `cwist
 *    openapi`). They also register the route normally, so every route
 *    declared with these macros appears in the runtime-generated spec.
 *
 * 2. Runtime generation: cwist_app_enable_openapi() registers a GET
 *    handler that serves an OpenAPI 3.1 JSON document at the given path,
 *    built from routes annotated via cwist_openapi_annotate() before
 *    cwist_app_listen() is called.
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
 * All fields are optional (NULL falls back to a sensible default) except
 * @p title and @p version, which appear in the OpenAPI `info` object.
 */
typedef struct {
    const char *title;       /**< API title (info.title). */
    const char *version;     /**< API version string, e.g. "1.0.0". */
    const char *description; /**< Optional description (info.description). */
    const char *server_url;  /**< Optional server URL; defaults to "/". */
} cwist_openapi_meta_t;

/**
 * @brief Enable the runtime OpenAPI 3.1 endpoint.
 *
 * Registers a GET handler at @p json_path that serves an OpenAPI 3.1
 * JSON document collected from all cwist_openapi_annotate() calls made
 * after this call and before cwist_app_listen().
 *
 * Must be called before the first cwist_openapi_annotate() call.
 * Calling this more than once on the same @p app is a no-op after the
 * first call.
 *
 * @param app       Application to enable the endpoint on.
 * @param json_path URL path to serve the spec (e.g. "/openapi.json").
 *                  NULL defaults to "/openapi.json".
 * @param meta      Document metadata; NULL uses built-in defaults.
 */
void cwist_app_enable_openapi(cwist_app *app, const char *json_path,
                              const cwist_openapi_meta_t *meta);

/**
 * @brief Annotate a route for inclusion in the generated OpenAPI spec.
 *
 * Must be called after cwist_app_enable_openapi() on the same @p app.
 * Routes registered without a matching annotation still appear in the
 * spec (path and method only) when they are declared with the
 * CWIST_OPENAPI_* macros; this call adds richer metadata.
 *
 * @param app     Application owning the route.
 * @param method  HTTP method string: "GET", "POST", "PUT", etc.
 * @param path    Route path as registered (e.g. "/users/:id").
 * @param summary Short human-readable summary for the operation.
 * @param tags    Comma-separated tag list, or NULL.
 */
void cwist_openapi_annotate(cwist_app *app, const char *method, const char *path,
                            const char *summary, const char *tags);

#endif
