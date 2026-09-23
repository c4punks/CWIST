/**
 * @file assets_internal.h
 * @brief Private wiring between app.c and the asset registry (assets.c).
 */

#ifndef CWIST_SYS_APP_ASSETS_INTERNAL_H
#define CWIST_SYS_APP_ASSETS_INTERNAL_H

#include <cwist/sys/app/app.h>
#include <stdbool.h>

struct cwist_asset;

/** A request resolved to an asset, and whether it used the hashed URL. */
typedef struct cwist_asset_match {
    const struct cwist_asset *asset;
    bool immutable;
} cwist_asset_match;

/** Resolve a GET/HEAD request path to a registered asset. */
bool cwist_assets_match(cwist_app *app, const cwist_http_request *req, cwist_asset_match *out);

/** Fill `res` for a matched asset (200, or 304 on a matching If-None-Match). */
void cwist_assets_respond(cwist_http_request *req, cwist_http_response *res,
                          const cwist_asset_match *match);

/** Deep-copy a registry for a detached sub-app. 0 on success (NULL copies to NULL). */
int cwist_assets_clone(void **dst, const void *src);

/** Release a registry and every asset in it. NULL is ignored. */
void cwist_assets_destroy(void *registry);

#endif
