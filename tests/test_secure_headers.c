#include <cwist/net/http/http.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>

void test_security_headers_present(void) {
    printf("Testing security headers...\n");

    cwist_http_response *res = cwist_http_response_create();
    assert(res != NULL);
    cwist_http_response_add_security_headers(res);

    // Verify headers added by cwist_http_response_add_security_headers()
    assert(cwist_http_header_get(res->headers, "X-Frame-Options") != NULL);
    assert(strcmp(cwist_http_header_get(res->headers, "X-Frame-Options"), "DENY") == 0);

    assert(cwist_http_header_get(res->headers, "X-Content-Type-Options") != NULL);
    assert(strcmp(cwist_http_header_get(res->headers, "X-Content-Type-Options"), "nosniff") == 0);

    assert(cwist_http_header_get(res->headers, "Referrer-Policy") != NULL);

    assert(cwist_http_header_get(res->headers, "Content-Security-Policy") != NULL);

    assert(cwist_http_header_get(res->headers, "Cross-Origin-Resource-Policy") != NULL);
    assert(strcmp(cwist_http_header_get(res->headers, "Cross-Origin-Resource-Policy"),
                  "same-origin") == 0);

    assert(cwist_http_header_get(res->headers, "Permissions-Policy") != NULL);

    assert(cwist_http_header_get(res->headers, "Cross-Origin-Opener-Policy") != NULL);
    assert(strcmp(cwist_http_header_get(res->headers, "Cross-Origin-Opener-Policy"),
                  "same-origin") == 0);

    // RFC 6797 section 7.2: HSTS must not be sent over plain HTTP. This function is
    // transport-agnostic (safe for both HTTP and HTTPS responses), so it
    // must NOT set HSTS itself -- that's cwist_http_response_add_hsts()'s
    // job, called only from an HTTPS-specific path (see below).
    assert(cwist_http_header_get(res->headers, "Strict-Transport-Security") == NULL);

    cwist_http_response_destroy(res);
    printf("Passed security headers.\n");
}

void test_hsts_added_explicitly(void) {
    printf("Testing HSTS opt-in for TLS responses...\n");

    cwist_http_response *res = cwist_http_response_create();
    assert(res != NULL);

    // Generic headers first (as a real HTTPS handler would), then the
    // explicit HSTS opt-in.
    cwist_http_response_add_security_headers(res);
    assert(cwist_http_header_get(res->headers, "Strict-Transport-Security") == NULL);

    cwist_http_response_add_hsts(res);
    assert(cwist_http_header_get(res->headers, "Strict-Transport-Security") != NULL);
    assert(strcmp(cwist_http_header_get(res->headers, "Strict-Transport-Security"),
                  "max-age=31536000; includeSubDomains") == 0);

    // Calling it again must not duplicate the header.
    cwist_http_response_add_hsts(res);
    size_t count = 0;
    for (cwist_http_header_node *n = res->headers; n; n = n->next) {
        if (strcasecmp(n->key->data, "Strict-Transport-Security") == 0) count++;
    }
    assert(count == 1);

    cwist_http_response_destroy(res);
    printf("Passed HSTS opt-in.\n");
}

void test_security_headers_not_duplicated(void) {
    printf("Testing security headers deduplication...\n");

    cwist_http_response *res = cwist_http_response_create();
    assert(res != NULL);

    // Manually add existing headers with different values
    cwist_http_header_add(&res->headers, "X-Frame-Options", "SAMEORIGIN");
    cwist_http_header_add(&res->headers, "Cross-Origin-Opener-Policy", "same-origin-allow-popups");

    // Re-run security header injection
    cwist_http_response_add_security_headers(res);

    // The manually-added values should remain (first one wins)
    assert(strcmp(cwist_http_header_get(res->headers, "X-Frame-Options"), "SAMEORIGIN") == 0);
    assert(strcmp(cwist_http_header_get(res->headers, "Cross-Origin-Opener-Policy"),
                  "same-origin-allow-popups") == 0);

    cwist_http_response_destroy(res);
    printf("Passed deduplication.\n");
}

int main(void) {
    test_security_headers_present();
    test_security_headers_not_duplicated();
    test_hsts_added_explicitly();
    printf("All secure-headers tests passed.\n");
    return 0;
}
