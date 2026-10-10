/** @file csrf.c @brief Double-submit CSRF protection. */
#define _POSIX_C_SOURCE 200809L
#include <cwist/sys/app/csrf.h>
#include <cwist/net/http/cookie.h>
#include <cwist/core/mem/alloc.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#define CWIST_CSRF_COOKIE_NAME "csrf_token"
#define CWIST_CSRF_TOKEN_BYTES 32
#define CWIST_CSRF_TOKEN_CHARS (CWIST_CSRF_TOKEN_BYTES * 2)

/**
 * @brief Check whether an HTTP method is exempt from CSRF validation.
 * @param method HTTP method to test.
 * @return true for GET, HEAD, and OPTIONS; false otherwise.
 */
static bool csrf_safe_method(cwist_http_method_t method) {
    return method == CWIST_HTTP_GET || method == CWIST_HTTP_HEAD || method == CWIST_HTTP_OPTIONS;
}

/**
 * @brief Generate a new random CSRF token.
 * Reads 32 bytes from /dev/urandom and hex-encodes them into a
 * 64-character NUL-terminated string.
 * @return Newly allocated token string on success; NULL on I/O failure or
 *         allocation failure. Caller owns the returned buffer and must free
 *         it with cwist_free().
 */
static char *csrf_generate_token(void) {
    unsigned char bytes[CWIST_CSRF_TOKEN_BYTES];
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return NULL;
    size_t got = 0;
    while (got < sizeof(bytes)) {
        ssize_t n = read(fd, bytes + got, sizeof(bytes) - got);
        if (n > 0) {
            got += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        close(fd);
        return NULL;
    }
    close(fd);
    static const char hex[] = "0123456789abcdef";
    char *token = cwist_alloc(CWIST_CSRF_TOKEN_CHARS + 1);
    if (!token) return NULL;
    for (size_t i = 0; i < sizeof(bytes); ++i) {
        token[2 * i] = hex[bytes[i] >> 4];
        token[2 * i + 1] = hex[bytes[i] & 15U];
    }
    token[CWIST_CSRF_TOKEN_CHARS] = '\0';
    return token;
}

/**
 * @brief Compare two strings for equality in constant time.
 * @param a First string; may be NULL.
 * @param b Second string; may be NULL.
 * @return true if both strings are non-NULL, equal in length and content;
 *         false otherwise.
 */
static bool csrf_equal(const char *a, const char *b) {
    if (!a || !b) return false;
    size_t alen = strlen(a), blen = strlen(b);
    size_t diff = alen ^ blen;
    size_t limit = alen > blen ? alen : blen;
    for (size_t i = 0; i < limit; ++i) {
        unsigned char ac = i < alen ? (unsigned char)a[i] : 0;
        unsigned char bc = i < blen ? (unsigned char)b[i] : 0;
        diff |= (size_t)(ac ^ bc);
    }
    return diff == 0;
}

/**
 * @brief Decode a single hexadecimal digit character to its numeric value.
 * @param c Character to decode.
 * @return Numeric value 0-15 on success; -1 if @p c is not a hex digit.
 */
static int hex_value(unsigned char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/**
 * @brief Extract and URL-decode the CSRF token from a form-encoded body.
 * Scans the request body for a "_csrf" or "csrf_token" field and decodes
 * its value ('+' becomes a space, '%XX' becomes the byte). Stops at the
 * first matching field.
 * @param req Request whose body is parsed.
 * @return Newly allocated decoded value on success; NULL if no matching
 *         field exists, on malformed percent-encoding, or on allocation
 *         failure. Caller owns the returned buffer and must free it with
 *         cwist_free().
 */
static char *csrf_form_value(const cwist_http_request *req) {
    if (!req || !req->body || !req->body->data) return NULL;
    const char *body = req->body->data;
    size_t len = req->body->size;
    size_t pos = 0;
    while (pos < len) {
        size_t field_start = pos;
        while (pos < len && body[pos] != '&') ++pos;
        size_t field_end = pos++;
        const char *eq = memchr(body + field_start, '=', field_end - field_start);
        size_t key_len = eq ? (size_t)(eq - (body + field_start)) : field_end - field_start;
        if (!((key_len == 5 && memcmp(body + field_start, "_csrf", 5) == 0) ||
              (key_len == 10 && memcmp(body + field_start, "csrf_token", 10) == 0)))
            continue;
        const char *value = eq ? eq + 1 : "";
        size_t value_len = eq ? field_end - (size_t)(value - body) : 0;
        char *decoded = cwist_alloc(value_len + 1);
        if (!decoded) return NULL;
        size_t out = 0;
        for (size_t i = 0; i < value_len; ++i) {
            unsigned char c = (unsigned char)value[i];
            if (c == '+')
                decoded[out++] = ' ';
            else if (c == '%' && i + 2 < value_len) {
                int hi = hex_value((unsigned char)value[i + 1]);
                int lo = hex_value((unsigned char)value[i + 2]);
                if (hi < 0 || lo < 0) {
                    cwist_free(decoded);
                    return NULL;
                }
                decoded[out++] = (char)((hi << 4) | lo);
                i += 2;
            } else
                decoded[out++] = (char)c;
        }
        decoded[out] = '\0';
        return decoded;
    }
    return NULL;
}

/**
 * @brief Set the CSRF cookie on the response.
 * Cookie is scoped to path "/", expires after 30 days, is marked Secure
 * when the app uses SSL, and uses SameSite=Strict. Not HttpOnly so the
 * client-side code can read it for form submission.
 * @param req Current request (used to determine SSL usage).
 * @param res Response to attach the cookie to.
 * @param token Token value to store in the cookie.
 */
static void csrf_issue_cookie(cwist_http_request *req, cwist_http_response *res,
                              const char *token) {
    cwist_cookie_options options = {0};
    options.path = "/";
    options.max_age_seconds = 30 * 24 * 60 * 60;
    options.http_only = false;
    options.secure = req && req->app && req->app->use_ssl;
    options.same_site = "Strict";
    cwist_cookie_set(res, CWIST_CSRF_COOKIE_NAME, token, &options);
}

/**
 * @brief CSRF protection middleware handler.
 * Ensures every request carries a CSRF token cookie, refreshing it when
 * absent. For unsafe methods (not GET/HEAD/OPTIONS) it validates the token
 * from the X-CSRF-Token header or the parsed form body against the cookie
 * and rejects mismatches with 403 Forbidden. Stores the current token in
 * req->csrf_token (freeing any previous value). Safe methods are passed
 * through to @p next.
 * @param req Current request; receives the token in req->csrf_token.
 * @param res Response object; status and cookie may be modified.
 * @param next Handler to invoke when the request passes validation.
 */
static void csrf_handler(cwist_http_request *req, cwist_http_response *res,
                         cwist_handler_func next) {
    const char *cookie_header = cwist_http_header_get(req->headers, "Cookie");
    cwist_query_map *cookies = cwist_query_map_create();
    const char *cookie = cookies ? cwist_cookie_get(cookies, CWIST_CSRF_COOKIE_NAME) : NULL;
    if (cookies) {
        cwist_cookie_parse(cookies, cookie_header);
        cookie = cwist_cookie_get(cookies, CWIST_CSRF_COOKIE_NAME);
    }
    if (req->csrf_token) {
        cwist_free(req->csrf_token);
        req->csrf_token = NULL;
    }
    if (!cookie) {
        char *token = csrf_generate_token();
        if (!token) {
            if (cookies) cwist_query_map_destroy(cookies);
            res->status_code = CWIST_HTTP_INTERNAL_ERROR;
            return;
        }
        req->csrf_token = token;
        csrf_issue_cookie(req, res, token);
        if (cookies) cwist_query_map_destroy(cookies);
        if (csrf_safe_method(req->method))
            next(req, res);
        else {
            res->status_code = CWIST_HTTP_FORBIDDEN;
            cwist_sstring_assign(res->body, "CSRF token missing");
        }
        return;
    }
    req->csrf_token = cwist_strdup(cookie);
    if (csrf_safe_method(req->method)) {
        cwist_query_map_destroy(cookies);
        next(req, res);
        return;
    }
    const char *header = cwist_http_header_get(req->headers, "X-CSRF-Token");
    char *form = header ? NULL : csrf_form_value(req);
    bool valid = csrf_equal(header ? header : form, cookie);
    if (form) cwist_free(form);
    cwist_query_map_destroy(cookies);
    if (!valid) {
        res->status_code = CWIST_HTTP_FORBIDDEN;
        cwist_sstring_assign(res->body, "CSRF token invalid");
        return;
    }
    next(req, res);
}

/**
 * @brief Create the CSRF protection middleware.
 * @param app Application instance; unused.
 * @return The CSRF middleware handler function.
 */
cwist_middleware_func cwist_mw_csrf(cwist_app *app) {
    (void)app;
    return csrf_handler;
}
/**
 * @brief Get the CSRF token associated with a request.
 * @param req Request to query.
 * @return The request's CSRF token, or NULL if @p req is NULL or no token
 *         has been set. The returned pointer is owned by the request.
 */
const char *cwist_csrf_token(cwist_http_request *req) {
    return req ? req->csrf_token : NULL;
}
