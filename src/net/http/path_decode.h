#ifndef CWIST_HTTP_PATH_DECODE_H
#define CWIST_HTTP_PATH_DECODE_H

#include <cwist/core/mem/alloc.h>
#include <stddef.h>

static inline int cwist_http_origin_target_is_valid(const char *path, size_t len) {
    if (!path || len == 0) return 0;
    if (path[0] != '/' && !(len == 1 && path[0] == '*')) return 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)path[i];
        if (c <= 0x20 || c == 0x7f || c == '#') return 0;
        if (c == '%') {
            if (i + 2 >= len) return 0;
            unsigned char hi = (unsigned char)path[i + 1];
            unsigned char lo = (unsigned char)path[i + 2];
            int hi_hex = (hi >= '0' && hi <= '9') || (hi >= 'a' && hi <= 'f') ||
                         (hi >= 'A' && hi <= 'F');
            int lo_hex = (lo >= '0' && lo <= '9') || (lo >= 'a' && lo <= 'f') ||
                         (lo >= 'A' && lo <= 'F');
            if (!hi_hex || !lo_hex) return 0;
            i += 2;
        }
    }
    return 1;
}

static inline int cwist_http_path_segment_decode(const char *src, size_t len, char **decoded) {
    if (decoded) *decoded = NULL;
    char *out = decoded ? (char *)cwist_alloc(len + 1) : NULL;
    if (decoded && !out) return -1;

    size_t i = 0;
    size_t j = 0;
    while (i < len) {
        unsigned char c = (unsigned char)src[i];
        if (c == '%') {
            if (i + 2 >= len) goto invalid;
            unsigned char hi = (unsigned char)src[i + 1];
            unsigned char lo = (unsigned char)src[i + 2];
            if (hi >= '0' && hi <= '9') hi = (unsigned char)(hi - '0');
            else if (hi >= 'A' && hi <= 'F') hi = (unsigned char)(hi - 'A' + 10);
            else if (hi >= 'a' && hi <= 'f') hi = (unsigned char)(hi - 'a' + 10);
            else goto invalid;
            if (lo >= '0' && lo <= '9') lo = (unsigned char)(lo - '0');
            else if (lo >= 'A' && lo <= 'F') lo = (unsigned char)(lo - 'A' + 10);
            else if (lo >= 'a' && lo <= 'f') lo = (unsigned char)(lo - 'a' + 10);
            else goto invalid;
            c = (unsigned char)((hi << 4) | lo);
            i += 3;
        } else {
            i++;
        }
        if (c < 0x20 || c == 0x7f) goto invalid;
        if (out) out[j] = (char)c;
        j++;
    }
    if (out) {
        out[j] = '\0';
        *decoded = out;
    }
    return 0;

invalid:
    cwist_free(out);
    return -1;
}

#endif
