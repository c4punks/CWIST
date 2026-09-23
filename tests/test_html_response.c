#define _POSIX_C_SOURCE 200809L
#include <cwist/sys/app/app.h>
#include <cwist/sys/app/test_client.h>
#include <cwist/net/http/html_response.h>
#include <cwist/core/html/component.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <assert.h>

#define TEST_HOST "127.0.0.1"
#define TEST_PORT 19983

#define PAGE_PREFIX \
    "<!DOCTYPE html><html><head><title>Items</title></head><body><main id=\"content\">"
#define PAGE_SUFFIX "</main></body></html>"
#define LIST_HTML "<ul id=\"items\"><li>alpha</li><li>beta</li></ul>"

static void destroy_children(cwist_html_element_t **children, size_t from, size_t count) {
    for (size_t i = from; i < count; i++) {
        cwist_html_element_destroy(children[i]);
    }
}

static cwist_html_element_t *text_el(const char *tag, const char *text) {
    cwist_html_element_t *el = cwist_html_element_create(tag);
    assert(el != NULL);
    cwist_html_element_set_text(el, text);
    return el;
}

/* <html><head><title>props</title></head><body><main id="content">child</main></body></html> */
static cwist_html_element_t *layout_render(const void *props, cwist_html_element_t **children,
                                           size_t child_count) {
    cwist_html_element_t *html = cwist_html_element_create("html");
    if (!html) {
        destroy_children(children, 0, child_count);
        return NULL;
    }
    cwist_html_element_t *head = cwist_html_element_create("head");
    cwist_html_element_add_child(head, text_el("title", props ? (const char *)props : ""));
    cwist_html_element_add_child(html, head);
    cwist_html_element_t *body = cwist_html_element_create("body");
    cwist_html_element_t *main_el = cwist_html_element_create("main");
    cwist_html_element_set_id(main_el, "content");
    for (size_t i = 0; i < child_count; i++) {
        cwist_html_element_add_child(main_el, children[i]);
    }
    cwist_html_element_add_child(body, main_el);
    cwist_html_element_add_child(html, body);
    return html;
}

static cwist_html_element_t *
failing_layout_render(const void *props, cwist_html_element_t **children, size_t child_count) {
    (void)props;
    destroy_children(children, 0, child_count);
    return NULL;
}

static cwist_html_component_t *g_layout;

static cwist_html_element_t *item_list(void) {
    cwist_html_element_t *ul = cwist_html_element_create("ul");
    assert(ul != NULL);
    cwist_html_element_set_id(ul, "items");
    cwist_html_element_add_child(ul, text_el("li", "alpha"));
    cwist_html_element_add_child(ul, text_el("li", "beta"));
    return ul;
}

static void items_handler(cwist_http_request *req, cwist_http_response *res) {
    if (cwist_http_response_set_view(req, res, item_list(), g_layout, "Items") != 0) {
        res->status_code = CWIST_HTTP_INTERNAL_ERROR;
    }
}

static void bare_handler(cwist_http_request *req, cwist_http_response *res) {
    if (cwist_http_response_set_view(req, res, text_el("p", "bare"), NULL, NULL) != 0) {
        res->status_code = CWIST_HTTP_INTERNAL_ERROR;
    }
}

static void oob_handler(cwist_http_request *req, cwist_http_response *res) {
    int rc = cwist_http_response_set_view(req, res, item_list(), g_layout, "Items");
    cwist_html_element_t *count = text_el("span", "2");
    cwist_html_element_set_id(count, "count");
    rc |= cwist_http_response_add_oob(req, res, count);
    if (rc != 0) res->status_code = CWIST_HTTP_INTERNAL_ERROR;
}

static void save_handler(cwist_http_request *req, cwist_http_response *res) {
    if (cwist_http_response_html_redirect(req, res, "/items") != 0) {
        res->status_code = CWIST_HTTP_INTERNAL_ERROR;
    }
}

static void bad_redirect_handler(cwist_http_request *req, cwist_http_response *res) {
    if (cwist_http_response_html_redirect(req, res, "/items\r\nSet-Cookie: x=1") != 0) {
        res->status_code = CWIST_HTTP_INTERNAL_ERROR;
    }
}

static cwist_http_response *request(cwist_test_client *client, cwist_http_method_t method,
                                    const char *path, const cwist_test_client_kv *headers,
                                    size_t header_count) {
    cwist_test_client_request_options opts = {0};
    opts.headers = headers;
    opts.header_count = header_count;
    cwist_http_response *res = cwist_test_client_request_ex(client, method, path, &opts);
    assert(res != NULL);
    return res;
}

static const char *header(cwist_http_response *res, const char *name) {
    return cwist_http_header_get(res->headers, name);
}

static void assert_html_headers(cwist_http_response *res) {
    assert(res->status_code == CWIST_HTTP_OK);
    assert(strcmp(header(res, "Content-Type"), "text/html; charset=utf-8") == 0);
    assert(strcmp(header(res, "Vary"), CWIST_HTML_FRAGMENT_VARY) == 0);
}

static void test_dispatch_page_and_fragment(cwist_test_client *client) {
    cwist_http_response *res = request(client, CWIST_HTTP_GET, "/items", NULL, 0);
    assert_html_headers(res);
    assert(strcmp(res->body->data, PAGE_PREFIX LIST_HTML PAGE_SUFFIX) == 0);
    cwist_http_response_destroy(res);

    const cwist_test_client_kv htmx[] = {{"HX-Request", "true"}, {"HX-Target", "items"}};
    res = request(client, CWIST_HTTP_GET, "/items", htmx, 2);
    assert_html_headers(res);
    assert(strcmp(res->body->data, LIST_HTML) == 0);
    cwist_http_response_destroy(res);

    const cwist_test_client_kv turbo[] = {{"Turbo-Frame", "items"}};
    res = request(client, CWIST_HTTP_GET, "/items", turbo, 1);
    assert_html_headers(res);
    assert(strcmp(res->body->data, LIST_HTML) == 0);
    cwist_http_response_destroy(res);

    /* htmx asks for the whole page again when restoring history. */
    const cwist_test_client_kv restore[] = {{"HX-Request", "true"},
                                            {"HX-History-Restore-Request", "true"}};
    res = request(client, CWIST_HTTP_GET, "/items", restore, 2);
    assert_html_headers(res);
    assert(strcmp(res->body->data, PAGE_PREFIX LIST_HTML PAGE_SUFFIX) == 0);
    cwist_http_response_destroy(res);

    /* Anything but "true" is not an htmx request. */
    const cwist_test_client_kv not_htmx[] = {{"HX-Request", "false"}};
    res = request(client, CWIST_HTTP_GET, "/items", not_htmx, 1);
    assert(strncmp(res->body->data, "<!DOCTYPE html>", 15) == 0);
    cwist_http_response_destroy(res);

    /* Without a layout the content is the document root. */
    res = request(client, CWIST_HTTP_GET, "/bare", NULL, 0);
    assert_html_headers(res);
    assert(strcmp(res->body->data, "<!DOCTYPE html><p>bare</p>") == 0);
    cwist_http_response_destroy(res);
    printf("Passed test_dispatch_page_and_fragment\n");
}

static void test_dispatch_oob(cwist_test_client *client) {
    const cwist_test_client_kv htmx[] = {{"HX-Request", "true"}};
    cwist_http_response *res = request(client, CWIST_HTTP_GET, "/oob", htmx, 1);
    assert_html_headers(res);
    assert(strcmp(res->body->data, LIST_HTML "<span id=\"count\" hx-swap-oob=\"true\">2</span>") ==
           0);
    cwist_http_response_destroy(res);

    /* A full page already holds the region, so nothing is appended. */
    res = request(client, CWIST_HTTP_GET, "/oob", NULL, 0);
    assert_html_headers(res);
    assert(strcmp(res->body->data, PAGE_PREFIX LIST_HTML PAGE_SUFFIX) == 0);
    cwist_http_response_destroy(res);
    printf("Passed test_dispatch_oob\n");
}

static void test_dispatch_redirect(cwist_test_client *client) {
    cwist_http_response *res = request(client, CWIST_HTTP_POST, "/save", NULL, 0);
    assert(res->status_code == CWIST_HTTP_SEE_OTHER);
    assert(strcmp(header(res, "Location"), "/items") == 0);
    assert(header(res, "HX-Redirect") == NULL);
    assert(strcmp(header(res, "Vary"), "HX-Request") == 0);
    assert(strcmp(res->body->data, "") == 0);
    cwist_http_response_destroy(res);

    const cwist_test_client_kv htmx[] = {{"HX-Request", "true"}};
    res = request(client, CWIST_HTTP_POST, "/save", htmx, 1);
    assert(res->status_code == CWIST_HTTP_OK);
    assert(strcmp(header(res, "HX-Redirect"), "/items") == 0);
    assert(header(res, "Location") == NULL);
    cwist_http_response_destroy(res);

    /* Turbo frames follow a plain 303. */
    const cwist_test_client_kv turbo[] = {{"Turbo-Frame", "items"}};
    res = request(client, CWIST_HTTP_POST, "/save", turbo, 1);
    assert(res->status_code == CWIST_HTTP_SEE_OTHER);
    assert(strcmp(header(res, "Location"), "/items") == 0);
    cwist_http_response_destroy(res);

    /* CR/LF in the target would split the response; it is refused. */
    res = request(client, CWIST_HTTP_GET, "/bad-redirect", NULL, 0);
    assert(res->status_code == CWIST_HTTP_INTERNAL_ERROR);
    assert(header(res, "Location") == NULL);
    assert(header(res, "Set-Cookie") == NULL);
    cwist_http_response_destroy(res);
    printf("Passed test_dispatch_redirect\n");
}

static cwist_http_request *make_request(const char *name, const char *value) {
    cwist_http_request *req = cwist_http_request_create();
    assert(req != NULL);
    if (name) {
        cwist_error_t err = cwist_http_header_add(&req->headers, name, value);
        assert(cwist_error_is_ok(&err));
        cwist_error_dispose(&err);
    }
    return req;
}

/* Every element handed to these helpers is consumed on every path; the
 * sanitizer build proves nothing leaks or is freed twice. */
static void test_helper_contracts(void) {
    cwist_http_request *plain = make_request(NULL, NULL);
    cwist_http_request *htmx = make_request("HX-Request", "TRUE");
    cwist_http_request *frame = make_request("Turbo-Frame", "cart");

    assert(!cwist_http_request_wants_fragment(NULL));
    assert(!cwist_http_request_wants_fragment(plain));
    assert(cwist_http_request_wants_fragment(htmx));
    assert(cwist_http_request_fragment_target(plain) == NULL);
    assert(cwist_http_request_fragment_target(htmx) == NULL);
    assert(strcmp(cwist_http_request_fragment_target(frame), "cart") == 0);

    cwist_http_response *res = cwist_http_response_create();
    assert(res != NULL);

    /* NULL arguments. */
    assert(cwist_http_response_set_html(res, NULL, true) == -1);
    assert(cwist_http_response_set_html(NULL, text_el("p", "x"), true) == -1);
    assert(cwist_http_response_set_view(plain, NULL, text_el("p", "x"), NULL, NULL) == -1);
    assert(cwist_http_response_set_view(plain, res, NULL, NULL, NULL) == -1);
    assert(cwist_http_response_add_oob(htmx, res, NULL) == -1);
    assert(cwist_http_response_html_redirect(plain, res, NULL) == -1);
    assert(cwist_http_response_html_redirect(plain, res, "") == -1);
    assert(cwist_http_response_html_redirect(plain, NULL, "/x") == -1);

    /* A layout that fails consumes the content itself. */
    cwist_html_component_t *failing = cwist_html_component_create("failing", failing_layout_render);
    assert(cwist_http_response_set_view(plain, res, text_el("p", "x"), failing, NULL) == -1);
    cwist_html_component_destroy(failing);

    /* An out-of-band element needs an id, for fragments and pages alike. */
    assert(cwist_http_response_add_oob(htmx, res, text_el("span", "x")) == -1);
    assert(cwist_http_response_add_oob(plain, res, text_el("span", "x")) == -1);

    /* A caller-chosen swap strategy is kept. */
    assert(cwist_http_response_set_html(res, text_el("p", "main"), false) == 0);
    cwist_html_element_t *toast = text_el("div", "saved");
    cwist_html_element_set_id(toast, "toast");
    cwist_html_element_add_attr(toast, "hx-swap-oob", "beforeend");
    assert(cwist_http_response_add_oob(htmx, res, toast) == 0);
    assert(strcmp(res->body->data,
                  "<p>main</p><div id=\"toast\" hx-swap-oob=\"beforeend\">saved</div>") == 0);

    /* Content-Type is replaced, not duplicated. */
    assert(cwist_http_response_set_html(res, text_el("p", "again"), true) == 0);
    size_t content_types = 0;
    for (cwist_http_header_node *h = res->headers; h; h = h->next) {
        if (h->key && h->key->data && strcasecmp(h->key->data, "Content-Type") == 0) {
            content_types++;
        }
    }
    assert(content_types == 1);
    assert(strcmp(res->body->data, "<!DOCTYPE html><p>again</p>") == 0);

    /* Control characters anywhere in the location are refused. */
    assert(cwist_http_response_html_redirect(plain, res, "/a\tb") == -1);
    assert(cwist_http_response_html_redirect(plain, res, "/a\x7f") == -1);

    cwist_http_response_destroy(res);
    cwist_http_request_destroy(plain);
    cwist_http_request_destroy(htmx);
    cwist_http_request_destroy(frame);
    printf("Passed test_helper_contracts\n");
}

/* --- Classic-mode server: the path-keyed reply cache must not replay a
 * fragment for a full-page navigation of the same URL. ------------------- */

static void slow_items_handler(cwist_http_request *req, cwist_http_response *res) {
    /* Slower than the cache's 10 ms learning threshold. */
    struct timespec delay = {0, 30 * 1000 * 1000};
    nanosleep(&delay, NULL);
    items_handler(req, res);
}

static int fetch(const char *extra_headers, char *buf, size_t cap) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    /* A reply replayed from the cache keeps the connection headers of the
     * reply it was learned from and may never close the socket; give up
     * after a few seconds and let the caller inspect what arrived. */
    struct timeval timeout = {5, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(TEST_PORT)};
    inet_pton(AF_INET, TEST_HOST, &addr.sin_addr);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    char req[512];
    int len = snprintf(req, sizeof(req),
                       "GET /slow HTTP/1.1\r\nHost: localhost\r\n%sConnection: close\r\n\r\n",
                       extra_headers);
    if (send(fd, req, (size_t)len, 0) != len) {
        close(fd);
        return -1;
    }
    size_t used = 0;
    ssize_t n;
    while (used + 1 < cap && (n = recv(fd, buf + used, cap - 1 - used, 0)) > 0) {
        used += (size_t)n;
    }
    buf[used] = '\0';
    close(fd);
    return (int)used;
}

static int test_reply_cache_keeps_variants_apart(void) {
    pid_t pid = fork();
    if (pid < 0) return 1;
    if (pid == 0) {
        setenv("CWIST_C1M_MODE", "0", 1);
        setenv("CWIST_WORKERS", "1", 1);
        cwist_app *app = cwist_app_create();
        if (!app) _exit(1);
        g_layout = cwist_html_component_create("layout", layout_render);
        cwist_app_get(app, "/slow", slow_items_handler);
        int rc = cwist_app_listen(app, TEST_PORT);
        cwist_app_destroy(app);
        cwist_html_component_destroy(g_layout);
        _exit(rc == 0 ? 0 : 1);
    }

    int failures = 0;
    char buf[8192];
    int up = -1;
    for (int i = 0; i < 50 && up < 0; i++) {
        usleep(100000);
        up = fetch("", buf, sizeof(buf));
    }
    if (up <= 0) {
        fprintf(stderr, "FAIL: server did not come up\n");
        failures++;
    }
    /* Enough identical fragment replies for the cache to consider the URL
     * stable if it were allowed to learn it. */
    for (int i = 0; i < 6 && !failures; i++) {
        if (fetch("HX-Request: true\r\n", buf, sizeof(buf)) <= 0 ||
            strstr(buf, "<!DOCTYPE html>") != NULL || strstr(buf, LIST_HTML) == NULL) {
            fprintf(stderr, "FAIL: fragment request %d did not get the fragment\n", i);
            failures++;
        }
    }
    for (int i = 0; i < 3 && !failures; i++) {
        if (fetch("", buf, sizeof(buf)) <= 0 ||
            strstr(buf, PAGE_PREFIX LIST_HTML PAGE_SUFFIX) == NULL) {
            fprintf(stderr, "FAIL: page request %d was answered with:\n%s\n", i, buf);
            failures++;
        }
    }

    kill(pid, SIGTERM);
    int status = 0;
    waitpid(pid, &status, 0);
    if (!failures) printf("Passed test_reply_cache_keeps_variants_apart\n");
    return failures;
}

int main(void) {
    printf("Testing HTML page/fragment responses...\n");
    test_helper_contracts();

    cwist_app *app = cwist_app_create();
    assert(app != NULL);
    g_layout = cwist_html_component_create("layout", layout_render);
    assert(g_layout != NULL);
    cwist_app_get(app, "/items", items_handler);
    cwist_app_get(app, "/bare", bare_handler);
    cwist_app_get(app, "/oob", oob_handler);
    cwist_app_post(app, "/save", save_handler);
    cwist_app_get(app, "/bad-redirect", bad_redirect_handler);

    cwist_test_client *client = cwist_test_client_create(app);
    assert(client != NULL);
    test_dispatch_page_and_fragment(client);
    test_dispatch_oob(client);
    test_dispatch_redirect(client);
    cwist_test_client_destroy(client);
    cwist_app_destroy(app);
    cwist_html_component_destroy(g_layout);
    g_layout = NULL;

    int failures = test_reply_cache_keeps_variants_apart();
    if (failures) return 1;
    printf("All HTML response tests passed!\n");
    return 0;
}
