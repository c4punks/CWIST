/**
 * @file assets.h
 * @brief Content-hashed, in-memory static assets served by a cwist_app.
 *
 * An asset is registered under a logical name ("css/app.css") and served at a
 * URL that embeds a hash of its content ("/assets/css/app.3f2a9c1d0b7e6a54.css"),
 * with a one-year `immutable` cache lifetime. Changing the content changes the
 * URL, so browsers and proxies never serve a stale copy, and unchanged content
 * keeps its URL across restarts and across prefork workers.
 *
 * Typical use: bundle and minify the stylesheets produced by the CSS composer
 * and component scopes (cwist_css_bundle()), register the result, and emit
 * cwist_app_asset_url() in the page layout.
 *
 * Assets are served on a route miss, before static directories, through the
 * app's middleware chain, and also work in the WASM builds (no filesystem is
 * needed unless cwist_app_asset_add_file() is used).
 */

#ifndef CWIST_SYS_APP_ASSETS_H
#define CWIST_SYS_APP_ASSETS_H

#include <cwist/sys/err/cwist_err.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct cwist_app;

/** Default URL prefix for assets. */
#define CWIST_ASSET_DEFAULT_PREFIX "/assets"

/** Cache-Control sent with content-hashed asset URLs. */
#define CWIST_ASSET_IMMUTABLE_CACHE "public, max-age=31536000, immutable"

/**
 * @brief Choose the URL prefix assets are served under.
 *
 * Must be called before the first asset is added, since URLs already handed
 * out would otherwise stop resolving.
 *
 * @param app App to configure.
 * @param url_prefix Absolute path such as "/static/build"; "/" serves assets
 *                   at the root. Trailing slashes are ignored.
 * @return 0 on success; -1 on invalid arguments, when assets were already
 *         added, or on allocation failure (err_i16 channel).
 */
cwist_error_t cwist_app_asset_prefix(struct cwist_app *app, const char *url_prefix);

/**
 * @brief Register an asset from memory. The data is copied.
 *
 * The asset is served at `<prefix>/<hashed name>`, where the hashed name puts
 * the first 16 hex digits of the SHA-256 of `data` before the last extension
 * ("css/app.css" -> "css/app.<hash>.css"), with `ETag`, `Cache-Control:
 * CWIST_ASSET_IMMUTABLE_CACHE` and 304 answers to a matching `If-None-Match`.
 * The logical name is also served, at `<prefix>/<name>`, with `Cache-Control:
 * no-cache`, for tools that cannot know the hash.
 *
 * Adding a name again with different content publishes a new hashed URL; the
 * previous one keeps working until the app is destroyed, so pages cached with
 * the old URL still load. Adding identical content again changes nothing.
 * Register assets before cwist_app_listen(): prefork workers only see what
 * existed when they were forked.
 *
 * @param app App that serves the asset.
 * @param name Logical name: ASCII letters, digits, '.', '-', '_', '~' and '/'
 *             separated segments, no leading or trailing '/', no empty, "."
 *             or ".." segment.
 * @param data Content (may be NULL only when `len` is 0).
 * @param len Content length in bytes.
 * @param content_type Content-Type to send; NULL picks one from the extension.
 * @return 0 on success, -1 on invalid arguments or allocation failure
 *         (err_i16 channel).
 */
cwist_error_t cwist_app_asset_add(struct cwist_app *app, const char *name, const void *data,
                                  size_t len, const char *content_type);

/**
 * @brief Register an asset from a file, read once now.
 * @param app App that serves the asset.
 * @param name Logical name, same rules as cwist_app_asset_add().
 * @param path File to read.
 * @param content_type Content-Type to send; NULL picks one from `name`.
 * @return 0 on success, -1 on invalid arguments, a read error or allocation
 *         failure (err_i16 channel).
 */
cwist_error_t cwist_app_asset_add_file(struct cwist_app *app, const char *name, const char *path,
                                       const char *content_type);

/**
 * @brief Content-hashed URL of the newest asset registered under `name`.
 * @return App-owned string valid until the app is destroyed, or NULL when no
 *         asset has that name.
 */
const char *cwist_app_asset_url(struct cwist_app *app, const char *name);

#ifdef __cplusplus
}
#endif

#endif
