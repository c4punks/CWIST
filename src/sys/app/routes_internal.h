/**
 * @file routes_internal.h
 * @brief Read-only walk of an app's route table for in-tree consumers
 *        (the runtime OpenAPI generator in openapi.c).
 */

#ifndef CWIST_SYS_APP_ROUTES_INTERNAL_H
#define CWIST_SYS_APP_ROUTES_INTERNAL_H

#include <cwist/sys/app/app.h>

/** Called once per HTTP route; WebSocket routes are skipped. */
typedef void (*cwist_route_visit_fn)(void *ctx, cwist_http_method_t method, const char *path);

/** Visit every HTTP route of `app`: exact routes first, then parameterized ones. */
void cwist_app_foreach_route(cwist_app *app, cwist_route_visit_fn visit, void *ctx);

#endif
