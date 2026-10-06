/** @file waf.c @brief Bounded, case-insensitive WAF-lite checks. */
#include <cwist/sys/app/waf.h>
#include <cwist/net/http/http.h>
#include <cwist/core/mem/alloc.h>
#include <string.h>
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
                                               {" and 1=1", 8},
                                               {"--", 2},
                                               {"/*", 2},
                                               {"*/", 2}};

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

/** @brief Check every header key/value pair in a linked list for hostile
 * signatures.
 * @param header First node of the header list; may be NULL (treated as safe).
 * @return true if all present keys and values pass cwist_waf_is_safe(). */
static bool waf_headers_safe(const cwist_http_header_node *header) {
    for (; header; header = header->next) {
        if ((header->key && !cwist_waf_is_safe(header->key->data, header->key->size)) ||
            (header->value && !cwist_waf_is_safe(header->value->data, header->value->size)))
            return false;
    }
    return true;
}

/** @brief Middleware handler that screens a request with the WAF checks.
 *
 * Runs cwist_waf_is_safe() over the request path, query, body, and headers.
 * If any part fails, the response is set to HTTP 400 with a fixed body and
 * the next handler is not invoked; otherwise the request is passed through.
 *
 * @param req Request to screen; NULL is treated as safe for its fields.
 * @param res Response to fill when the request is rejected.
 * @param next Next handler in the chain, invoked when the request passes. */
static void waf_handler(cwist_http_request *req, cwist_http_response *res,
                        cwist_handler_func next) {
    bool safe = req && (!req->path || cwist_waf_is_safe(req->path->data, req->path->size)) &&
                (!req->query || cwist_waf_is_safe(req->query->data, req->query->size)) &&
                (!req->body || cwist_waf_is_safe(req->body->data, req->body->size)) &&
                waf_headers_safe(req ? req->headers : NULL);
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
