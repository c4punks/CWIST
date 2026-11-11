#ifndef CWIST_APP_ROUTE_INTERNAL_H
#define CWIST_APP_ROUTE_INTERNAL_H

#include <stdbool.h>
#include <cwist/sys/app/app.h>

bool cwist_app_post_checked(cwist_app *app, const char *path, cwist_handler_func handler);

/**
 * In-tree fallback dispatch (used by the reverse proxy). When installed,
 * requests that match no route are offered to @p match before assets, static
 * directories and the 404 handler; on a true return @p handler runs through
 * the app's middleware chain. Install once before serving; pass NULLs to
 * remove. Not a public API: the hook is process-global.
 */
typedef bool (*cwist_app_fallback_match_fn)(cwist_app *app, cwist_http_request *req);
void cwist_app_internal_set_fallback(cwist_app_fallback_match_fn match,
                                     cwist_handler_func handler);

#endif
