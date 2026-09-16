/* Independent public contract matrix, active under NDEBUG. */
#include "../src/sys/app/public_fixed_cache.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#define CHECK(x)                                                    \
    do {                                                            \
        if (!(x)) {                                                 \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); \
            exit(1);                                                \
        }                                                           \
    } while (0)
static cwist_sstring s(char *p) {
    return (cwist_sstring){.data = p, .size = strlen(p)};
}
static uint64_t now = 100;
static uint64_t clock_now(void) {
    return now;
}
static int fail;
static void *allocate(size_t n) {
    return fail ? NULL : malloc(n);
}
struct race {
    cwist_pfc *cache;
    cwist_pfc_key key;
    const cwist_http_request *q;
    const cwist_http_response *r;
};
static void *race_worker(void *arg) {
    struct race *v = arg;
    for (int i = 0; i < 1000; i++) {
        CHECK(cwist_pfc_put(v->cache, &v->key, v->q, v->r));
        cwist_pfc_snapshot *s = NULL;
        if (cwist_pfc_get(v->cache, &v->key, &s)) {
            CHECK(s->body_len == 3 && !memcmp(s->body, "a\0b", 3));
            cwist_pfc_clear(v->cache);
            CHECK(!memcmp(s->body, "a\0b", 3));
            cwist_pfc_snapshot_free(s);
        }
    }
    return NULL;
}
int main(void) {
    cwist_sstring path = s("/public"), ver = s("HTTP/1.1"), hn = s("Host"), hv = s("one.test");
    cwist_http_header_node h = {.key = &hn, .value = &hv};
    cwist_http_request q = {
        .method = CWIST_HTTP_GET, .path = &path, .version = &ver, .headers = &h};
    cwist_pfc_key key;
    int route = 0;
#define ELIG(qp) cwist_pfc_request(qp, &route, CWIST_ENDPOINT_PUBLIC_FIXED, false, &key)
    CHECK(ELIG(&q));
    const cwist_endpoint_opt_t badopts[] = {0,
                                            CWIST_ENDPOINT_FIXED,
                                            CWIST_DYNAMIC,
                                            CWIST_ENDPOINT_FILE,
                                            CWIST_ENDPOINT_PUBLIC_FIXED | CWIST_DYNAMIC,
                                            CWIST_ENDPOINT_PUBLIC_FIXED | CWIST_ENDPOINT_FILE,
                                            CWIST_ENDPOINT_PUBLIC_FIXED | 128};
    for (size_t i = 0; i < sizeof(badopts) / sizeof(*badopts); i++)
        CHECK(!cwist_pfc_request(&q, &route, badopts[i], false, &key));
    CHECK(!cwist_pfc_request(&q, &route, CWIST_ENDPOINT_PUBLIC_FIXED, true, &key));
    CHECK(!cwist_pfc_request(&q, NULL, CWIST_ENDPOINT_PUBLIC_FIXED, false, &key));
#define BADQ(field, value)          \
    do {                            \
        cwist_http_request bad = q; \
        bad.field = value;          \
        CHECK(!ELIG(&bad));         \
    } while (0)
    BADQ(method, CWIST_HTTP_HEAD);
    BADQ(method, CWIST_HTTP_POST);
    BADQ(https_conn, &route);
    BADQ(h2_queue, &route);
    BADQ(stream_id, 1);
    BADQ(private_data, &route);
    BADQ(session, &route);
    BADQ(csrf_token, "x");
    BADQ(flash, (void *)&route);
    BADQ(path_params, (void *)&route);
    BADQ(route_middleware_state, &route);
    BADQ(content_length, 1);
    BADQ(te_chunked_seen, true);
    BADQ(expect_100_seen, true);
    BADQ(upgraded, true);
    BADQ(headers, NULL);
    cwist_sstring badstr = s("x");
    BADQ(query, &badstr);
    BADQ(body, &badstr);
    badstr = s("HTTP/1.0");
    BADQ(version, &badstr);
    cwist_sstring name, value = s("x");
    cwist_http_header_node extra = {.key = &name, .value = &value};
    const char *deny[] = {"Cookie",          "Authorization",  "Proxy-Authorization",
                          "Range",           "If-None-Match",  "Cache-Control",
                          "Accept-Encoding", "Content-Length", "Transfer-Encoding",
                          "Expect",          "Upgrade",        "X-Unknown"};
    h.next = &extra;
    for (size_t i = 0; i < sizeof(deny) / sizeof(*deny); i++) {
        name = s((char *)deny[i]);
        CHECK(!ELIG(&q));
    }
    name = s("hOsT");
    value = hv;
    CHECK(!ELIG(&q));
    h.next = NULL;
    const char *invalid[] = {"", "a b", "a/b", "a@b", "a:xyz", "[xyz]", "a\r\nb", "a:99999"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(*invalid); i++) {
        hv = s((char *)invalid[i]);
        CHECK(!ELIG(&q));
    }
    hv = s("one.test");
    const char *allow[] = {"User-Agent", "Accept", "Connection"};
    const char *val[] = {"agent", "*/*", "close"};
    h.next = &extra;
    for (int i = 0; i < 3; i++) {
        name = s((char *)allow[i]);
        value = s((char *)val[i]);
        CHECK(ELIG(&q));
        cwist_http_header_node dup = extra;
        extra.next = &dup;
        CHECK(!ELIG(&q));
        extra.next = NULL;
    }
    name = s("Accept");
    value = s("text/html");
    CHECK(!ELIG(&q));
    name = s("Connection");
    value = s("close, upgrade");
    CHECK(!ELIG(&q));
    h.next = NULL;
    badstr = s("");
    q.query = &badstr;
    CHECK(ELIG(&q));
    q.query = NULL;
    CHECK(ELIG(&q));
    cwist_sstring reason = s("OK"), body = {.data = "a\0b", .size = 3};
    cwist_http_response r = {.version = &ver,
                             .status_code = 200,
                             .status_text = &reason,
                             .body = &body,
                             .keep_alive = true,
                             .endpoint_opts = CWIST_ENDPOINT_PUBLIC_FIXED};
    CHECK(cwist_pfc_response(&q, &r));
#define BADR(field, value)                    \
    do {                                      \
        cwist_http_response bad = r;          \
        bad.field = value;                    \
        CHECK(!cwist_pfc_response(&q, &bad)); \
    } while (0)
    BADR(status_code, 201);
    BADR(status_code, 500);
    BADR(keep_alive, false);
    BADR(deferred, true);
    BADR(async, &route);
    BADR(use_file_stream, true);
    BADR(alt_svc, "h3");
    BADR(endpoint_opts, CWIST_ENDPOINT_FILE);
    BADR(ptr_body_len, 1);
    badstr = s("CUSTOM");
    BADR(status_text, &badstr);
    BADR(version, &badstr);
    body.size = 65537;
    CHECK(!cwist_pfc_response(&q, &r));
    body.size = 3;
    const char *rd[] = {"Set-Cookie", "Vary",           "Cache-Control",    "Date",     "Alt-Svc",
                        "Connection", "Content-Length", "Content-Encoding", "X-Unknown"};
    for (size_t i = 0; i < sizeof(rd) / sizeof(*rd); i++) {
        name = s((char *)rd[i]);
        BADR(headers, &extra);
    }
    name = s("Content-Type");
    value = s("application/octet-stream");
    r.headers = &extra;
    CHECK(cwist_pfc_response(&q, &r));
    cwist_http_header_node dup = extra;
    extra.next = &dup;
    CHECK(!cwist_pfc_response(&q, &r));
    extra.next = NULL;
    value = s("x\r\nInjected: yes");
    CHECK(!cwist_pfc_response(&q, &r));
    char ct[1026];
    memset(ct, 'x', 1025);
    ct[1025] = 0;
    value = s(ct);
    CHECK(!cwist_pfc_response(&q, &r));
    value = s("application/octet-stream");
    cwist_pfc *c = cwist_pfc_create_with(clock_now, allocate);
    CHECK(c);
    cwist_pfc_snapshot *hit = NULL;
    CHECK(!cwist_pfc_get(c, &key, &hit));
    CHECK(cwist_pfc_put(c, &key, &q, &r));
    CHECK(cwist_pfc_get(c, &key, &hit));
    CHECK(hit->body_len == 3 && !memcmp(hit->body, "a\0b", 3));
    CHECK(!strcmp(hit->content_type, "application/octet-stream"));
    cwist_pfc_clear(c);
    CHECK(!cwist_pfc_get(c, &key, &hit));
    CHECK(hit && !memcmp(hit->body, "a\0b", 3));
    cwist_pfc_snapshot_free(hit);
    hit = NULL;
    CHECK(cwist_pfc_put(c, &key, &q, &r));
    now += 59;
    CHECK(cwist_pfc_get(c, &key, &hit));
    cwist_pfc_snapshot_free(hit);
    hit = NULL;
    now++;
    CHECK(!cwist_pfc_get(c, &key, &hit));
    CHECK(cwist_pfc_put(c, &key, &q, &r));
    fail = 1;
    CHECK(!cwist_pfc_get(c, &key, &hit));
    CHECK(!cwist_pfc_put(c, &key, &q, &r));
    fail = 0;
    CHECK(cwist_pfc_get(c, &key, &hit));
    cwist_pfc_snapshot_free(hit);
    hit = NULL;
    struct race shared = {c, key, &q, &r};
    pthread_t workers[4];
    for (int i = 0; i < 4; i++) CHECK(!pthread_create(&workers[i], NULL, race_worker, &shared));
    for (int i = 0; i < 4; i++) CHECK(!pthread_join(workers[i], NULL));
    hv = s("two.test");
    CHECK(ELIG(&q));
    CHECK(!cwist_pfc_get(c, &key, &hit));
    char big[65536];
    memset(big, 42, sizeof(big));
    r.is_ptr_body = true;
    r.ptr_body = big;
    r.ptr_body_len = sizeof(big);
    char hosts[300][32];
    for (int i = 0; i < 300; i++) {
        snprintf(hosts[i], 32, "host%d.test", i);
        hv = s(hosts[i]);
        CHECK(ELIG(&q));
        CHECK(cwist_pfc_put(c, &key, &q, &r));
    }
    CHECK(cwist_pfc_count(c) <= 256);
    CHECK(cwist_pfc_count(c) < 256);
    CHECK(cwist_pfc_bytes(c) <= 16u * 1024 * 1024);
    big[0] = 99;
    CHECK(cwist_pfc_get(c, &key, &hit));
    CHECK(hit->body[0] == 42);
    big[0] = 42;
    cwist_pfc_destroy(c);
    CHECK(hit->body_len == sizeof(big) && !memcmp(hit->body, big, sizeof(big)));
    cwist_pfc_snapshot_free(hit);
    /* Put must not publish a response under a key unrelated to its request. */
    cwist_pfc *guard = cwist_pfc_create_with(clock_now, allocate);
    CHECK(guard);
    cwist_pfc_key forged = key;
    forged.path = "/other";
    forged.path_len = 6;
    CHECK(!cwist_pfc_put(guard, &forged, &q, &r));
    r.ptr_body = NULL;
    r.ptr_body_len = 0;
    for (int i = 0; i < 300; i++) {
        snprintf(hosts[i], 32, "small%d.test", i);
        hv = s(hosts[i]);
        CHECK(ELIG(&q));
        CHECK(cwist_pfc_put(guard, &key, &q, &r));
    }
    CHECK(cwist_pfc_count(guard) == 256);
    now = UINT64_MAX;
    CHECK(!cwist_pfc_get(guard, &key, &hit));
    CHECK(cwist_pfc_count(guard) == 0);
    CHECK(!cwist_pfc_put(guard, &key, &q, &r));
    cwist_pfc_destroy(guard);
    /* Two independent application caches must share the process budget. */
    now = 200;
    cwist_pfc *apps[2] = {cwist_pfc_create_with(clock_now, allocate),
                          cwist_pfc_create_with(clock_now, allocate)};
    CHECK(apps[0] && apps[1]);
    for (int phase = 0; phase < 2; phase++) {
        r.ptr_body = phase ? big : NULL;
        r.ptr_body_len = phase ? sizeof(big) : 0;
        for (int i = 0; i < 600; i++) {
            snprintf(hosts[i % 300], 32, "aggregate%d.test", i);
            hv = s(hosts[i % 300]);
            CHECK(ELIG(&q));
            (void)cwist_pfc_put(apps[i % 2], &key, &q, &r);
            CHECK(cwist_pfc_count(apps[0]) + cwist_pfc_count(apps[1]) <= CWIST_PFC_ENTRIES_MAX);
            CHECK(cwist_pfc_bytes(apps[0]) + cwist_pfc_bytes(apps[1]) <= CWIST_PFC_BYTES_MAX);
        }
        CHECK(cwist_pfc_count(apps[0]) + cwist_pfc_count(apps[1]) > 0);
        cwist_pfc_clear(apps[0]);
        cwist_pfc_clear(apps[1]);
    }
    /* Destroy releases the shared reservation; another app can use all slots. */
    cwist_pfc_destroy(apps[0]);
    r.ptr_body = NULL;
    r.ptr_body_len = 0;
    for (int i = 0; i < 256; i++) {
        snprintf(hosts[i], 32, "reclaim%d.test", i);
        hv = s(hosts[i]);
        CHECK(ELIG(&q));
        CHECK(cwist_pfc_put(apps[1], &key, &q, &r));
    }
    CHECK(cwist_pfc_count(apps[1]) == 256);
    cwist_pfc_destroy(apps[1]);
    fail = 1;
    CHECK(!cwist_pfc_create_with(clock_now, allocate));
    puts("public FIXED guard/storage contract: PASS");
    return 0;
}
