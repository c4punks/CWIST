/**
 * @file html_response.h
 * @brief HTML page and fragment responses for HTML-over-the-wire handlers.
 *
 * A handler builds the content it owns (usually with cwist_html_component_t)
 * and lets these helpers decide how to answer: a full document on a normal
 * navigation, only the fragment when the client asked for one. Out-of-band
 * elements and redirects follow the same request-driven switch, so the same
 * route works with and without client-side script (progressive enhancement).
 *
 * Fragment requests are recognised by the request headers that fragment-
 * swapping clients already send: `HX-Request: true` (htmx) and `Turbo-Frame`
 * (Hotwire Turbo frames). Nothing here requires either library.
 */

#ifndef CWIST_NET_HTTP_HTML_RESPONSE_H
#define CWIST_NET_HTTP_HTML_RESPONSE_H

#include <cwist/net/http/http.h>
#include <cwist/core/html/builder.h>
#include <cwist/core/html/component.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Request headers that select between a page and a fragment.
 *
 * cwist_http_response_set_view() sends this as `Vary` so shared caches keep
 * the two representations of one URL apart.
 */
#define CWIST_HTML_FRAGMENT_VARY "HX-Request, HX-History-Restore-Request, HX-Target, Turbo-Frame"

/**
 * @brief Whether the client asked for a fragment instead of a full page.
 *
 * True for `HX-Request: true` (unless `HX-History-Restore-Request: true`, which
 * asks for the whole page again) and for any non-empty `Turbo-Frame` header.
 *
 * @param req Parsed request. NULL gives false.
 */
bool cwist_http_request_wants_fragment(const cwist_http_request *req);

/**
 * @brief The element id the client will swap the fragment into, if it said.
 *
 * `Turbo-Frame` first, then `HX-Target`. Only meaningful when
 * cwist_http_request_wants_fragment() is true.
 *
 * @return Request-owned string, or NULL when not a fragment request or no
 *         target was sent.
 */
const char *cwist_http_request_fragment_target(const cwist_http_request *req);

/**
 * @brief Render an element tree into the response body as HTML.
 *
 * Replaces the body, sets `Content-Type: text/html; charset=utf-8` (replacing
 * any earlier Content-Type) and leaves the status code alone. With `document`
 * set, the body starts with `<!DOCTYPE html>`.
 *
 * @param res Response to fill.
 * @param root Element tree to render; always consumed (destroyed), including
 *             on failure.
 * @param document True for a full document, false for a fragment.
 * @return 0 on success, -1 on invalid arguments or allocation failure (the
 *         body is then left unchanged).
 */
int cwist_http_response_set_html(cwist_http_response *res, cwist_html_element_t *root,
                                 bool document);

/**
 * @brief Answer with a fragment or a full page, depending on the request.
 *
 * A fragment request gets `content` alone. Any other request gets a document:
 * `content` wrapped by `layout` when one is given (the layout component
 * receives `content` as its single child, see cwist_html_component_instantiate()),
 * or `content` itself as the document root otherwise. Both answers carry
 * `Vary: CWIST_HTML_FRAGMENT_VARY`.
 *
 * @param req Request being answered.
 * @param res Response to fill.
 * @param content Page content; always consumed, including on failure.
 * @param layout Optional layout component for full-page answers (not consumed).
 * @param layout_props Props passed to `layout` (may be NULL).
 * @return 0 on success, -1 on invalid arguments, a failed layout render, or
 *         allocation failure.
 */
int cwist_http_response_set_view(const cwist_http_request *req, cwist_http_response *res,
                                 cwist_html_element_t *content, cwist_html_component_t *layout,
                                 const void *layout_props);

/**
 * @brief Append an out-of-band element to a fragment response.
 *
 * For a fragment request the element is rendered after the current body with
 * `hx-swap-oob="true"` (unless the caller already set `hx-swap-oob`), so the
 * client replaces the element with the same id elsewhere on the page. For a
 * full-page request it is dropped: the page being sent already contains that
 * region. Clients that do not implement out-of-band swaps ignore the extra
 * element.
 *
 * Call it after the main content has been set.
 *
 * @param req Request being answered.
 * @param res Response whose body receives the element.
 * @param el Element to append; must carry a non-empty `id` attribute. Always
 *           consumed, including on failure.
 * @return 0 when appended or deliberately dropped, -1 on invalid arguments (a
 *         missing id included) or allocation failure.
 */
int cwist_http_response_add_oob(const cwist_http_request *req, cwist_http_response *res,
                                cwist_html_element_t *el);

/**
 * @brief Redirect in a way both plain navigation and fragment clients follow.
 *
 * An htmx request (`HX-Request: true`) gets `200` with `HX-Redirect: <location>`,
 * because a script-driven request follows a 3xx itself and would swap the
 * target page into the fragment slot. Every other request, Turbo frames
 * included, gets `303 See Other` with `Location`, which also turns a form POST
 * into a GET. The body is emptied in both cases.
 *
 * @param req Request being answered.
 * @param res Response to fill.
 * @param location Target URL; must be non-empty and free of control
 *                 characters (CR/LF would split the response).
 * @return 0 on success, -1 on invalid arguments or allocation failure.
 */
int cwist_http_response_html_redirect(const cwist_http_request *req, cwist_http_response *res,
                                      const char *location);

#ifdef __cplusplus
}
#endif

#endif
