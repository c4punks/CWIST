#ifndef CWIST_APP_ROUTE_INTERNAL_H
#define CWIST_APP_ROUTE_INTERNAL_H

#include <stdbool.h>
#include <cwist/sys/app/app.h>

/* cwist_app_post() that reports whether the route was stored. In-tree only
 * (gRPC route registration); not a public API. */
bool cwist_app_post_checked(cwist_app *app, const char *path, cwist_handler_func handler);

#endif
