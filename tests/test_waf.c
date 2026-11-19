#include <cwist/sys/app/waf.h>
#include <cwist/net/http/http.h>
#include <cwist/core/mem/alloc.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

static int called;
static void next(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    (void)res;
    ++called;
}

static void test_signatures_are_rejected(void) {
    const char *bad[] = {"<ScRiPt>alert(1)</script>", "UNION SELECT password", "x' OR 1=1 --",
                         "javascript:alert(1)"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i)
        assert(!cwist_waf_is_safe(bad[i], strlen(bad[i])));
    assert(cwist_waf_is_safe("search=flowers&sort=price", strlen("search=flowers&sort=price")));
    assert(cwist_waf_is_safe("*/*", 3));
    assert(cwist_waf_is_safe("2026--report", 12));
}

static void check_request(cwist_http_request *req, bool expected_safe) {
    cwist_middleware_func waf = cwist_mw_waf_lite();
    cwist_http_response *res = cwist_http_response_create();
    assert(res != NULL);
    called = 0;
    waf(req, res, next);
    assert((called == 1) == expected_safe);
    if (!expected_safe) assert(res->status_code == CWIST_HTTP_BAD_REQUEST);
    cwist_http_response_destroy(res);
    cwist_http_request_destroy(req);
}

static void test_middleware_rejects_and_allows(void) {
    cwist_http_request *req = cwist_http_request_create();
    cwist_sstring_assign(req->body, "q=<script>alert(1)</script>");
    check_request(req, false);

    req = cwist_http_request_create();
    cwist_sstring_assign(req->body, "name=Jane+Doe");
    check_request(req, true);

    req = cwist_http_request_create();
    cwist_sstring_assign(req->path, "/%3Cscript%3Ealert(1)%3C/script%3E");
    check_request(req, false);

    req = cwist_http_request_create();
    cwist_sstring_assign(req->query, "q=1+union+select+password");
    check_request(req, false);

    req = cwist_http_request_create();
    cwist_sstring_assign(req->query, "q=1%20union%20select%20password");
    check_request(req, false);

    req = cwist_http_request_create();
    cwist_sstring_assign(req->body, "q=1%20union%20select%20password");
    cwist_http_header_add(&req->headers, "Content-Type", "application/x-www-form-urlencoded");
    check_request(req, false);

    req = cwist_http_request_create();
    cwist_sstring_assign(req->path, "/2026--report");
    cwist_sstring_assign(req->query, "q=ordinary");
    cwist_sstring_assign(req->body, "body { --foreground: #fff; }");
    cwist_http_header_add(&req->headers, "Accept", "text/html,application/xhtml+xml,*/*;q=0.8");
    check_request(req, true);
}

static void test_html_escape(void) {
    char *escaped = cwist_sanitize_html("<a href='x'>&\"");
    assert(escaped && strcmp(escaped, "&lt;a href=&#39;x&#39;&gt;&amp;&quot;") == 0);
    cwist_free(escaped);
}

int main(void) {
    test_signatures_are_rejected();
    test_middleware_rejects_and_allows();
    test_html_escape();
    puts("All WAF tests passed.");
}
