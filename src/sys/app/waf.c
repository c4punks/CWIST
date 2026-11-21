/** @file waf.c @brief Bounded, case-insensitive WAF-lite checks. */
#include <cwist/sys/app/waf.h>
#include <cwist/net/http/http.h>
#include <cwist/net/http/multipart.h>
#include <cwist/core/mem/alloc.h>
#include <string.h>
#include <strings.h>
#include <pthread.h>
#include <stdint.h>
#include <stdbool.h>

/** @brief Lowercase an ASCII letter, leaving all other bytes unchanged.
 * @param c Input byte.
 * @return The lowercase equivalent of @p c for 'A'..'Z', otherwise @p c. */
static unsigned char ascii_lower(unsigned char c) {
    return c >= 'A' && c <= 'Z' ? (unsigned char)(c + ('a' - 'A')) : c;
}

/* Signature scanning runs a single Aho-Corasick pass over the input instead
 * of re-probing every rule at every byte: the automaton is built once from
 * the pattern set (dense 128-entry transitions cover the lowered ASCII
 * alphabet; failure edges are folded in at build time, so the hot loop is
 * one table index per byte) and matching cost stays O(input length)
 * regardless of how many signatures are registered. */
typedef struct {
    const char *text;
    size_t length;
} waf_signature;

static const waf_signature waf_signatures[] = {{"<script", 7},
                                               {"</script", 8},
                                               {"javascript:", 11},
                                               {"vbscript:", 9},
                                               {"union select", 12},
                                               {"drop table", 10},
                                               {"insert into", 11},
                                               {"delete from", 11},
                                               {" or 1=1", 7},
                                               {" and 1=1", 8}};

#define WAF_MAX_PATTERNS 64
#define WAF_MAX_TEXT 4096
#define WAF_ALPHABET 128

static uint8_t waf_goto[WAF_MAX_TEXT][WAF_ALPHABET];
static uint16_t waf_fail[WAF_MAX_TEXT];
static bool waf_out[WAF_MAX_TEXT];
static uint16_t waf_states;
static pthread_once_t waf_once = PTHREAD_ONCE_INIT;

/** @brief Build the Aho-Corasick automaton from the signature table.
 *
 * Runs once via pthread_once. Inserts every signature into the goto table,
 * then computes BFS failure links and folds them into the goto table so the
 * scan loop is a single table index per byte. Not thread-safe by itself;
 * callers must synchronize via pthread_once.
 *
 * Fails open: if the state space exceeds 256 states the build is abandoned
 * and waf_states is left incomplete, in which case cwist_waf_is_safe()
 * simply never reports a match for deeper patterns. */
static void waf_build(void) {
    uint16_t queue[WAF_MAX_TEXT];
    waf_states = 1; /* state 0: root */

    for (size_t rule = 0; rule < sizeof(waf_signatures) / sizeof(waf_signatures[0]); ++rule) {
        uint16_t s = 0;
        for (size_t i = 0; i < waf_signatures[rule].length; ++i) {
            unsigned char c = (unsigned char)waf_signatures[rule].text[i];
            if (waf_goto[s][c] == 0) {
                if (waf_states > 255) return; /* uint8_t state space exhausted: fail open */
                waf_goto[s][c] = (uint8_t)waf_states++;
            }
            s = waf_goto[s][c];
        }
        waf_out[s] = true;
    }

    /* BFS failure links, then fold them into the goto table so the scan
     * loop never walks the fail chain per byte. */
    size_t qh = 0, qt = 0;
    for (unsigned c = 0; c < WAF_ALPHABET; ++c) {
        if (waf_goto[0][c]) queue[qt++] = waf_goto[0][c];
    }
    while (qh < qt) {
        uint16_t r = queue[qh++];
        if (waf_out[waf_fail[r]]) waf_out[r] = true;
        for (unsigned c = 0; c < WAF_ALPHABET; ++c) {
            uint8_t s = waf_goto[r][c];
            if (s) {
                queue[qt++] = s;
                uint16_t f = waf_fail[r];
                while (f && waf_goto[f][c] == 0) f = waf_fail[f];
                waf_fail[s] = waf_goto[f][c];
            } else {
                waf_goto[r][c] = waf_goto[waf_fail[r]][c];
            }
        }
    }
}

/** @brief Check whether a byte string is free of hostile signatures.
 *
 * Scans @p input once (O(length)) through the case-insensitive Aho-Corasick
 * automaton, which is built lazily on first call. Matching is ASCII-only:
 * bytes at or above 0x80 reset the automaton state, and control bytes other
 * than tab, newline, and carriage return are rejected outright.
 *
 * @param input Byte string to scan; may be NULL.
 * @param length Number of bytes to scan.
 * @return true if the input is safe, false if a signature or disallowed
 *         control byte is found. NULL input is considered safe. */
bool cwist_waf_is_safe(const char *input, size_t length) {
    if (!input) return true;
    pthread_once(&waf_once, waf_build);
    uint16_t s = 0;
    for (size_t i = 0; i < length; ++i) {
        unsigned char c = (unsigned char)input[i];
        if (c == 0 || (c < 0x20U && c != '\t' && c != '\n' && c != '\r')) return false;
        /* Bytes outside ASCII cannot be part of any signature; resetting
         * keeps matching identical to the old per-position compare. */
        if (c >= WAF_ALPHABET) {
            s = 0;
            continue;
        }
        s = waf_goto[s][ascii_lower(c)];
        if (waf_out[s]) return false;
    }
    return true;
}

/** @brief Escape HTML-special characters in a NUL-terminated string.
 *
 * Replaces '&', '<', '>', '"', and '\'' with their HTML entity equivalents
 * and returns a newly allocated buffer holding the escaped text.
 *
 * @param input Input string; may be NULL.
 * @return Newly allocated, NUL-terminated escaped string that the caller
 *         owns and must free with cwist_free, or NULL if @p input is NULL,
 *         on allocation failure, or on size overflow. */
char *cwist_sanitize_html(const char *input) {
    if (!input) return NULL;
    size_t length = strlen(input), extra = 0;
    for (size_t i = 0; i < length; ++i) {
        switch (input[i]) {
            case '&': extra += 4; break;
            case '<':
            case '>': extra += 3; break;
            case '"': extra += 5; break;
            case '\'': extra += 4; break;
            default: break;
        }
    }
    if (length > SIZE_MAX - extra - 1) return NULL;
    char *output = cwist_alloc(length + extra + 1);
    if (!output) return NULL;
    size_t out = 0;
    for (size_t i = 0; i < length; ++i) {
        const char *replacement = NULL;
        switch (input[i]) {
            case '&': replacement = "&amp;"; break;
            case '<': replacement = "&lt;"; break;
            case '>': replacement = "&gt;"; break;
            case '"': replacement = "&quot;"; break;
            case '\'': replacement = "&#39;"; break;
            default: break;
        }
        if (replacement) {
            size_t n = strlen(replacement);
            memcpy(output + out, replacement, n);
            out += n;
        } else
            output[out++] = input[i];
    }
    output[out] = '\0';
    return output;
}

static int waf_hex(unsigned char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static bool waf_url_safe(const cwist_sstring *input, bool plus_is_space) {
    if (!input || !input->data) return true;
    const char *data = input->data;
    size_t len = input->size;
    if (!memchr(data, '%', len) && !(plus_is_space && memchr(data, '+', len)))
        return cwist_waf_is_safe(data, len);

    char *decoded = (char *)cwist_alloc(len + 1);
    if (!decoded) return false;
    size_t out = 0;
    for (size_t i = 0; i < len;) {
        if (data[i] == '%') {
            if (i + 2 >= len) goto invalid;
            int hi = waf_hex((unsigned char)data[i + 1]);
            int lo = waf_hex((unsigned char)data[i + 2]);
            if (hi < 0 || lo < 0) goto invalid;
            decoded[out++] = (char)((hi << 4) | lo);
            i += 3;
        } else {
            decoded[out++] = plus_is_space && data[i] == '+' ? ' ' : data[i];
            i++;
        }
    }
    bool safe = cwist_waf_is_safe(decoded, out);
    cwist_free(decoded);
    return safe;

invalid:
    cwist_free(decoded);
    return false;
}

static bool waf_headers_safe(const cwist_http_header_node *header) {
    for (; header; header = header->next) {
        if (header->key && !cwist_waf_is_safe(header->key->data, header->key->size)) return false;
    }
    return true;
}

/** @brief Compare the media type of a Content-Type value (before ';').
 * @param ct Content-Type header value.
 * @param type Lowercase media type to compare with.
 * @return true when @p ct names @p type, ignoring case and parameters. */
static bool waf_media_is(const char *ct, const char *type) {
    size_t n = strlen(type);
    if (strncasecmp(ct, type, n) != 0) return false;
    char c = ct[n];
    return c == '\0' || c == ';' || c == ' ' || c == '\t';
}

/** @brief Decide whether a body of this Content-Type is text the WAF scans.
 *
 * Text media (text/x, JSON, XML, GraphQL, JavaScript, and +json / +xml
 * suffixes) and bodies without a Content-Type are scanned. Other media types
 * are opaque bytes (images, archives, protobuf, octet-stream) that the
 * framework does not interpret, so they are handed to the handler unchanged,
 * like Drogon does; their size is bounded by the body limit, not by the WAF.
 *
 * @param ct Content-Type header value, or NULL.
 * @return true when the body should be scanned as text. */
static bool waf_body_is_text(const char *ct) {
    if (!ct) return true;
    while (*ct == ' ' || *ct == '\t') ct++;
    if (*ct == '\0') return true;
    if (strncasecmp(ct, "text/", 5) == 0) return true;
    static const char *const text_types[] = {"application/json", "application/xml",
                                             "application/graphql", "application/javascript",
                                             "application/ecmascript"};
    for (size_t i = 0; i < sizeof(text_types) / sizeof(text_types[0]); ++i)
        if (waf_media_is(ct, text_types[i])) return true;
    const char *end = strchr(ct, ';');
    size_t len = end ? (size_t)(end - ct) : strlen(ct);
    while (len > 0 && (ct[len - 1] == ' ' || ct[len - 1] == '\t')) len--;
    return (len > 5 && strncasecmp(ct + len - 5, "+json", 5) == 0) ||
           (len > 4 && strncasecmp(ct + len - 4, "+xml", 4) == 0);
}

/** @brief Screen a multipart/form-data body part by part.
 *
 * Part names and file names are always scanned. Part contents are scanned
 * only for form fields (no filename) whose own Content-Type is text, so file
 * uploads pass unchanged. A body that does not parse as multipart is rejected.
 *
 * @param body Request body.
 * @param ct Content-Type header value carrying the boundary.
 * @return true when every scanned piece is safe. */
static bool waf_multipart_safe(const cwist_sstring *body, const char *ct) {
    if (!body || !body->data || body->size == 0) return true;
    char *boundary = cwist_multipart_extract_boundary(ct);
    if (!boundary) return false;
    cwist_multipart_result *mp = cwist_multipart_parse(body->data, body->size, boundary);
    cwist_free(boundary);
    if (!mp) return false;
    bool safe = true;
    for (cwist_multipart_field *f = mp->fields; f && safe; f = f->next) {
        if (f->name && !cwist_waf_is_safe(f->name, strlen(f->name))) safe = false;
        if (safe && f->filename && !cwist_waf_is_safe(f->filename, strlen(f->filename)))
            safe = false;
        if (safe && !f->filename && waf_body_is_text(f->content_type) &&
            !cwist_waf_is_safe(f->data, f->data_len))
            safe = false;
    }
    cwist_multipart_result_destroy(mp);
    return safe;
}

/** @brief Screen the request body according to its Content-Type.
 * @param req Request whose body is screened.
 * @return true when the body is safe or is opaque binary content. */
static bool waf_body_safe(const cwist_http_request *req) {
    if (!req->body || !req->body->data || req->body->size == 0) return true;
    const char *ct = cwist_http_header_get(req->headers, "Content-Type");
    if (ct && waf_media_is(ct, "application/x-www-form-urlencoded"))
        return waf_url_safe(req->body, true);
    if (ct && waf_media_is(ct, "multipart/form-data")) return waf_multipart_safe(req->body, ct);
    if (waf_body_is_text(ct)) return cwist_waf_is_safe(req->body->data, req->body->size);
    return true;
}

/** @brief Middleware handler that screens a request with the WAF checks.
 *
 * Decodes path, query, and form-body URL encoding before scanning. Header
 * names are screened without rejecting generic header values such as Accept.
 * Text bodies are scanned, multipart bodies are scanned per part (names, file
 * names, text fields), and opaque binary bodies are not scanned.
 * If any part fails, the response is set to HTTP 400 with a fixed body and
 * the next handler is not invoked; otherwise the request is passed through.
 *
 * @param req Request to screen; NULL is treated as safe for its fields.
 * @param res Response to fill when the request is rejected.
 * @param next Next handler in the chain, invoked when the request passes. */
static void waf_handler(cwist_http_request *req, cwist_http_response *res,
                        cwist_handler_func next) {
    bool safe = req && waf_url_safe(req->path, false) && waf_url_safe(req->query, true) &&
                waf_body_safe(req) && waf_headers_safe(req->headers);
    if (!safe) {
        res->status_code = CWIST_HTTP_BAD_REQUEST;
        cwist_sstring_assign(res->body, "Request rejected by WAF");
        return;
    }
    next(req, res);
}

/** @brief Construct the WAF-lite middleware.
 * @return A middleware function that screens each request with the
 *         signature-based WAF checks before invoking the next handler. */
cwist_middleware_func cwist_mw_waf_lite(void) {
    return waf_handler;
}
