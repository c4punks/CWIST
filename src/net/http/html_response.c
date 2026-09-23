/**
 * @file html_response.c
 * @brief Page, fragment, out-of-band and redirect answers for HTML handlers.
 */

#include <cwist/net/http/html_response.h>
#include <cjson/cJSON.h>
#include <string.h>
#include <strings.h>

static const char *request_header(const cwist_http_request *req, const char *name) {
    if (!req) return NULL;
    return cwist_http_header_get(req->headers, name);
}

static bool header_is_true(const cwist_http_request *req, const char *name) {
    const char *value = request_header(req, name);
    return value && strcasecmp(value, "true") == 0;
}

bool cwist_http_request_wants_fragment(const cwist_http_request *req) {
    if (header_is_true(req, "HX-Request"))
        return !header_is_true(req, "HX-History-Restore-Request");
    const char *frame = request_header(req, "Turbo-Frame");
    return frame && *frame;
}

const char *cwist_http_request_fragment_target(const cwist_http_request *req) {
    if (!cwist_http_request_wants_fragment(req)) return NULL;
    const char *frame = request_header(req, "Turbo-Frame");
    if (frame && *frame) return frame;
    const char *target = request_header(req, "HX-Target");
    return target && *target ? target : NULL;
}

static bool response_accepts_html(const cwist_http_response *res) {
    return res && res->body && !res->is_ptr_body && !res->use_file_stream && !res->stream_mode;
}

static int set_header(cwist_http_response *res, const char *name, const char *value) {
    cwist_http_header_remove(&res->headers, name);
    cwist_error_t err = cwist_http_header_add(&res->headers, name, value);
    bool ok = cwist_error_is_ok(&err);
    cwist_error_dispose(&err);
    return ok ? 0 : -1;
}

static int add_header(cwist_http_response *res, const char *name, const char *value) {
    cwist_error_t err = cwist_http_header_add(&res->headers, name, value);
    bool ok = cwist_error_is_ok(&err);
    cwist_error_dispose(&err);
    return ok ? 0 : -1;
}

/**
 * @brief Render `root` (consumed) to a new string, optionally behind a doctype.
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

static bool element_has_id(const cwist_html_element_t *el) {
    const cJSON *id = el->attributes ? cJSON_GetObjectItem(el->attributes, "id") : NULL;
    return cJSON_IsString(id) && id->valuestring && *id->valuestring;
}

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

static bool location_is_valid(const char *location) {
    if (!location || !*location) return false;
    for (const unsigned char *p = (const unsigned char *)location; *p; p++) {
        if (*p < 0x20 || *p == 0x7f) return false;
    }
    return true;
}

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
