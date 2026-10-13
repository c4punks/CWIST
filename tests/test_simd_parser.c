/**
 * @file test_simd_parser.c
 * @brief Focused tests for the SIMD/SWAR byte-search helpers used by the
 * HTTP parser: paired "\r\n\r\n" matching (including bare-'\r'-heavy input
 * that would trigger the old rescan path and matches straddling a vector
 * boundary) plus the plain CRLF and single-char matchers.
 */
#define _POSIX_C_SOURCE 200809L
#include "../src/net/http/simd_parser.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- crlfcrlf ------------------------------------------------------------ */

static void test_crlfcrlf_basic(void) {
    const char *req = "GET / HTTP/1.1\r\nHost: x\r\n\r\n";
    const char *hit = cwist_simd_find_crlfcrlf(req, strlen(req));
    assert(hit == req + strlen("GET / HTTP/1.1\r\nHost: x"));

    /* Match at offset 0. */
    const char *at0 = "\r\n\r\n";
    assert(cwist_simd_find_crlfcrlf(at0, 4) == at0);

    /* No match. */
    assert(cwist_simd_find_crlfcrlf("abcdef", 6) == NULL);
    assert(cwist_simd_find_crlfcrlf("\r\n\rX", 4) == NULL);
    assert(cwist_simd_find_crlfcrlf("\r\n", 2) == NULL);
    assert(cwist_simd_find_crlfcrlf(NULL, 10) == NULL);
    assert(cwist_simd_find_crlfcrlf("xyz", 3) == NULL);
}

static void test_crlfcrlf_bare_cr_heavy(void) {
    /* 4096 bare '\r' bytes followed by the real terminator: the old
     * find-'\r'-then-check loop rescanned O(n*k) here; the paired matcher
     * must still return the exact terminator position. */
    size_t pad = 4096;
    size_t len = pad + 4;
    char *buf = malloc(len);
    assert(buf);
    memset(buf, '\r', pad);
    buf[pad] = '\r';
    buf[pad + 1] = '\n';
    buf[pad + 2] = '\r';
    buf[pad + 3] = '\n';

    /* Bare-'\r' runs must not produce false hits anywhere before pad. */
    const char *hit = cwist_simd_find_crlfcrlf(buf, len);
    assert(hit == buf + pad);

    /* Bare-'\r' mixture: '\r' and 'a' alternating never forms the pair. */
    for (size_t i = 0; i < pad; i++)
        buf[i] = (i % 2) ? '\r' : 'a';
    hit = cwist_simd_find_crlfcrlf(buf, len);
    assert(hit == buf + pad);

    /* "\r\n" repeated matches at the very first pair. */
    for (size_t i = 0; i + 1 < pad; i += 2) {
        buf[i] = '\r';
        buf[i + 1] = '\n';
    }
    hit = cwist_simd_find_crlfcrlf(buf, len);
    assert(hit == buf);
    free(buf);
}

static void test_crlfcrlf_vector_boundary(void) {
    /* Terminator straddling every alignment from 12..40 bytes in, so it
     * crosses 16- and 32-byte vector boundaries at several offsets. */
    for (size_t off = 12; off <= 40; off++) {
        size_t len = off + 4 + 8;
        char *buf = malloc(len);
        assert(buf);
        memset(buf, 'a', len);
        buf[off] = '\r';
        buf[off + 1] = '\n';
        buf[off + 2] = '\r';
        buf[off + 3] = '\n';
        /* Earlier bytes are all 'a', so the first hit is exactly off. */
        assert(cwist_simd_find_crlfcrlf(buf, len) == buf + off);

        /* Drop the final '\n': straddling partial match must not hit. */
        buf[off + 3] = 'x';
        assert(cwist_simd_find_crlfcrlf(buf, len) == NULL);
        free(buf);
    }

    /* Whole match fully inside the vector-safety margin (scalar tail). */
    const char *tail = "aaaaaaaaaaaaaaa\r\n\r\n"; /* match at offset 15 */
    assert(cwist_simd_find_crlfcrlf(tail, strlen(tail)) == tail + 15);
}

/* --- crlf / find_char regression ----------------------------------------- */

static void test_crlf_and_find_char(void) {
    const char *s = "line one\r\nline two\r\n";
    assert(cwist_simd_find_crlf(s, strlen(s)) == s + 8);
    assert(cwist_simd_find_crlf("no crlf here", 12) == NULL);
    assert(cwist_simd_find_crlf("\rX", 2) == NULL);

    const char *big = "0123456789abcdefghijklmnopqrstuvwxyz";
    assert(cwist_simd_find_char(big, strlen(big), 'v') == big + 31);
    assert(cwist_simd_find_char(big, strlen(big), '!') == NULL);
}

int main(void) {
    test_crlfcrlf_basic();
    test_crlfcrlf_bare_cr_heavy();
    test_crlfcrlf_vector_boundary();
    test_crlf_and_find_char();
    printf("test_simd_parser: ok\n");
    return 0;
}
