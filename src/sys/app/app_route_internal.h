#ifndef CWIST_APP_ROUTE_INTERNAL_H
#define CWIST_APP_ROUTE_INTERNAL_H

#include <stdbool.h>
#include <cwist/sys/app/app.h>
#include "../../net/websocket/ws_async_internal.h"

bool cwist_app_post_checked(cwist_app *app, const char *path, cwist_handler_func handler);

/** Register an in-tree WebSocket endpoint with full lifecycle and limits. */
bool cwist_app_internal_ws_async(cwist_app *app, const char *path, cwist_ws_on_message_t on_message,
                                 cwist_ws_on_open_t on_open, cwist_ws_on_close_t on_close,
                                 void *user_data, size_t max_message_bytes);

/**
 * In-tree fallback dispatch (used by the reverse proxy). When installed,
 * requests that match no route are offered to @p match before assets, static
 * directories and the 404 handler; on a true return @p handler runs through
 * the app's middleware chain. Install once before serving; pass NULLs to
 * remove. Not a public API: the hook is process-global.
 */
typedef bool (*cwist_app_fallback_match_fn)(cwist_app *app, cwist_http_request *req);
void cwist_app_internal_set_fallback(cwist_app_fallback_match_fn match, cwist_handler_func handler);

#endif
