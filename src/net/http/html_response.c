/**
 * @file html_response.c
 * @brief Page, fragment, out-of-band and redirect answers for HTML handlers.
 */

#include <cwist/net/http/html_response.h>
#include <cjson/cJSON.h>
#include <string.h>
#include <strings.h>

/**
 * @brief Look up a request header value by name.
 * @param req Request to inspect; may be NULL.
 * @param name Header name to look up.
 * @return Pointer to the header value, or NULL if the request is NULL or the
 *         header is not present. The pointer is owned by the request.
 */
static const char *request_header(const cwist_http_request *req, const char *name) {
    if (!req) return NULL;
    return cwist_http_header_get(req->headers, name);
}

/**
 * @brief Check whether a request header is present with the value "true".
 * @param req Request to inspect; may be NULL.
 * @param name Header name to check.
 * @return true if the header exists and equals "true" (case-insensitive),
 *         false otherwise.
 */
static bool header_is_true(const cwist_http_request *req, const char *name) {
    const char *value = request_header(req, name);
    return value && strcasecmp(value, "true") == 0;
}

/**
 * @brief Determine whether the client requested an HTML fragment only.
 *
 * A fragment is requested when the htmx `HX-Request` header is "true" (and it
 * is not a history restore) or when a non-empty `Turbo-Frame` header is set.
 * @param req Request to inspect; may be NULL.
 * @return true if only a fragment of the page should be returned.
 */
bool cwist_http_request_wants_fragment(const cwist_http_request *req) {
    if (header_is_true(req, "HX-Request"))
        return !header_is_true(req, "HX-History-Restore-Request");
    const char *frame = request_header(req, "Turbo-Frame");
    return frame && *frame;
}

/**
 * @brief Get the identifier of the fragment region the client wants updated.
 * @param req Request to inspect; may be NULL.
 * @return The `Turbo-Frame` id when present, otherwise the `HX-Target` header
 *         when present, or NULL when the client did not request a fragment or
 *         no target was given. The pointer is owned by the request.
 */
const char *cwist_http_request_fragment_target(const cwist_http_request *req) {
    if (!cwist_http_request_wants_fragment(req)) return NULL;
    const char *frame = request_header(req, "Turbo-Frame");
    if (frame && *frame) return frame;
    const char *target = request_header(req, "HX-Target");
    return target && *target ? target : NULL;
}

/**
 * @brief Check whether the response body can be set to HTML directly.
 * @return true if the response has an in-memory body (not a pointer body, file
 *         stream, or stream mode).
 */
static bool response_accepts_html(const cwist_http_response *res) {
    return res && res->body && !res->is_ptr_body && !res->use_file_stream && !res->stream_mode;
}

/**
 * @brief Replace any existing header of the same name with a new value.
 * @return 0 on success, -1 if adding the header failed.
 */
static int set_header(cwist_http_response *res, const char *name, const char *value) {
    cwist_http_header_remove(&res->headers, name);
    cwist_error_t err = cwist_http_header_add(&res->headers, name, value);
    bool ok = cwist_error_is_ok(&err);
    cwist_error_dispose(&err);
    return ok ? 0 : -1;
}

/**
 * @brief Append a header without removing existing entries of the same name.
 * @return 0 on success, -1 if adding the header failed.
 */
static int add_header(cwist_http_response *res, const char *name, const char *value) {
    cwist_error_t err = cwist_http_header_add(&res->headers, name, value);
    bool ok = cwist_error_is_ok(&err);
    cwist_error_dispose(&err);
    return ok ? 0 : -1;
}

/**
 * @brief Render `root` (consumed) to a new string, optionally behind a doctype.
 * @param root Element to render; ownership is always transferred and the
 *             element is destroyed, even on failure.
 * @param document When true, prepend "<!DOCTYPE html>" to the rendered HTML.
 * @return Newly allocated string on success (caller owns it), or NULL on
 *         allocation or render failure.
 */
static cwist_sstring *render_consumed(cwist_html_element_t *root, bool document) {
    cwist_sstring *html = cwist_html_render(root);
    cwist_html_element_destroy(root);
    if (!html || !document) return html;

    cwist_sstring *out = cwist_sstring_create();
    bool ok = out != NULL;
    if (ok) {
        cwist_error_t err = cwist_sstring_assign(out, "<!DOCTYPE html>");
        ok = cwist_error_is_ok(&err);
        cwist_error_dispose(&err);
    }
    if (ok && html->data) {
        cwist_error_t err = cwist_sstring_append(out, html->data);
        ok = cwist_error_is_ok(&err);
        cwist_error_dispose(&err);
    }
    cwist_sstring_destroy(html);
    if (!ok) {
        if (out) cwist_sstring_destroy(out);
        return NULL;
    }
    return out;
}

/**
 * @brief Set the response body to the rendered HTML of `root`.
 *
 * Always takes ownership of `root` and destroys it. On success the
 * `Content-Type` header is set to `text/html; charset=utf-8`.
 * @param res Response whose body is replaced.
 * @param root Element tree to render; consumed regardless of the result.
 * @param document When true, render as a full document with a doctype.
 * @return 0 on success, -1 if the response cannot hold an HTML body, the
 *         element is NULL, or rendering/assignment fails.
 */
int cwist_http_response_set_html(cwist_http_response *res, cwist_html_element_t *root,
                                 bool document) {
    if (!root) return -1;
    if (!response_accepts_html(res)) {
        cwist_html_element_destroy(root);
        return -1;
    }

    cwist_sstring *html = render_consumed(root, document);
    if (!html) return -1;

    const char *data = html->data ? html->data : "";
    cwist_error_t err = cwist_sstring_assign_len(res->body, data, strlen(data));
    bool ok = cwist_error_is_ok(&err);
    cwist_error_dispose(&err);
    cwist_sstring_destroy(html);
    if (!ok) return -1;

    return set_header(res, "Content-Type", "text/html; charset=utf-8");
}

/**
 * @brief Set the response body from `content`, wrapping it in `layout` for full pages.
 *
 * When the client requested a fragment, `content` is rendered alone; otherwise
 * it is embedded into the layout component (or rendered as a full document
 * when no layout is given). Takes ownership of `content` in all cases. On
 * success a `Vary` header for fragment-negotiation is appended.
 * @param req Request used to decide between fragment and full-page rendering.
 * @param res Response whose body is replaced.
 * @param content Element tree for the page content; consumed regardless of
 *                the result.
 * @param layout Optional layout component that wraps `content`; may be NULL.
 * @param layout_props Props passed to the layout component.
 * @return 0 on success, -1 if the response cannot hold HTML, `content` is
 *         NULL, or rendering fails.
 */
int cwist_http_response_set_view(const cwist_http_request *req, cwist_http_response *res,
                                 cwist_html_element_t *content, cwist_html_component_t *layout,
                                 const void *layout_props) {
    if (!content) return -1;
    if (!response_accepts_html(res)) {
        cwist_html_element_destroy(content);
        return -1;
    }

    int rc;
    if (cwist_http_request_wants_fragment(req)) {
        rc = cwist_http_response_set_html(res, content, false);
    } else if (layout) {
        /* The layout's render function now owns `content`. */
        cwist_html_element_t *page =
            cwist_html_component_instantiate(layout, layout_props, &content, 1);
        rc = page ? cwist_http_response_set_html(res, page, true) : -1;
    } else {
        rc = cwist_http_response_set_html(res, content, true);
    }
    if (rc != 0) return -1;
    return add_header(res, "Vary", CWIST_HTML_FRAGMENT_VARY);
}

/**
 * @brief Check whether the element has a non-empty string "id" attribute.
 * @return true if the element carries a non-empty "id" attribute.
 */
static bool element_has_id(const cwist_html_element_t *el) {
    const cJSON *id = el->attributes ? cJSON_GetObjectItem(el->attributes, "id") : NULL;
    return cJSON_IsString(id) && id->valuestring && *id->valuestring;
}

/**
 * @brief Append an out-of-band swapped element to the response body.
 *
 * The element must have an "id" attribute. When the client requested a full
 * page the element is dropped (a full page already contains the region) and
 * success is reported. Otherwise an `hx-swap-oob="true"` attribute is added if
 * missing and the element is rendered and appended to the body. Takes
 * ownership of `el` in all cases.
 * @param req Request used to decide whether an OOB update applies.
 * @param res Response whose body the rendered element is appended to.
 * @param el Element to append; consumed regardless of the result.
 * @return 0 on success (including the dropped full-page case), -1 if the
 *         response cannot hold HTML, the element has no id, or rendering or
 *         appending fails.
 */
int cwist_http_response_add_oob(const cwist_http_request *req, cwist_http_response *res,
                                cwist_html_element_t *el) {
    if (!el) return -1;
    if (!response_accepts_html(res) || !element_has_id(el)) {
        cwist_html_element_destroy(el);
        return -1;
    }
    if (!cwist_http_request_wants_fragment(req)) {
        /* A full page already contains this region. */
        cwist_html_element_destroy(el);
        return 0;
    }

    if (!cJSON_HasObjectItem(el->attributes, "hx-swap-oob")) {
        cwist_html_element_add_attr(el, "hx-swap-oob", "true");
        if (!cJSON_HasObjectItem(el->attributes, "hx-swap-oob")) {
            cwist_html_element_destroy(el);
            return -1;
        }
    }

    cwist_sstring *html = render_consumed(el, false);
    if (!html) return -1;
    cwist_error_t err = cwist_sstring_append(res->body, html->data);
    bool ok = cwist_error_is_ok(&err);
    cwist_error_dispose(&err);
    cwist_sstring_destroy(html);
    return ok ? 0 : -1;
}

/**
 * @brief Check whether a redirect location is safe to send in a header.
 * @return true if the location is non-empty and contains no control bytes
 *         (values below 0x20 or 0x7f).
 */
static bool location_is_valid(const char *location) {
    if (!location || !*location) return false;
    for (const unsigned char *p = (const unsigned char *)location; *p; p++) {
        if (*p < 0x20 || *p == 0x7f) return false;
    }
    return true;
}

/**
 * @brief Redirect an HTML request to `location`, using htmx headers when applicable.
 *
 * For htmx requests (`HX-Request: true`) the status stays 200 and the target
 * is sent via the `HX-Redirect` header; otherwise a 303 See Other response
 * with a `Location` header is produced. Any previous `Location` and
 * `HX-Redirect` headers are removed and the body is cleared.
 * @param req Request used to decide between htmx and regular redirects.
 * @param res Response to modify.
 * @param location Redirect target; must be non-empty and free of control bytes.
 * @return 0 on success, -1 if the response cannot hold HTML, the location is
 *         invalid, or header/body updates fail.
 */
int cwist_http_response_html_redirect(const cwist_http_request *req, cwist_http_response *res,
                                      const char *location) {
    if (!response_accepts_html(res) || !location_is_valid(location)) return -1;

    cwist_http_header_remove(&res->headers, "Location");
    cwist_http_header_remove(&res->headers, "HX-Redirect");

    int rc;
    if (header_is_true(req, "HX-Request")) {
        res->status_code = CWIST_HTTP_OK;
        rc = add_header(res, "HX-Redirect", location);
    } else {
        res->status_code = CWIST_HTTP_SEE_OTHER;
        rc = add_header(res, "Location", location);
    }
    if (rc != 0) return -1;

    cwist_error_t err = cwist_sstring_assign(res->body, "");
    bool ok = cwist_error_is_ok(&err);
    cwist_error_dispose(&err);
    if (!ok) return -1;
    return add_header(res, "Vary", "HX-Request");
}
