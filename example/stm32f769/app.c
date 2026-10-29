#include "app.h"

#include <cwist/app.h>
#include <cwist/net/http/http.h>
#include <cwist/sys/port/lwip.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static cwist_app *g_app;
static cwist_lwip_server *g_server;
static unsigned long g_hits;

/* What the board last put on the wire, kept so the next page load can show
 * the outgoing bytes beside the incoming ones. */
static char g_last_response[1024];
static size_t g_last_response_len;

/* Render a classic offset|hex|ascii dump, HTML-escaped, into out. */
static void hexdump_html(cwist_sstring *out, const unsigned char *data, size_t len) {
    char row[32];
    for (size_t off = 0; off < len; off += 16) {
        size_t n = len - off < 16 ? len - off : 16;
        int w = snprintf(row, sizeof row, "%06x  ", (unsigned)off);
        cwist_sstring_append_len(out, row, (size_t)w);
        for (size_t i = 0; i < 16; i++) {
            if (i < n) {
                w = snprintf(row, sizeof row, "%02x ", data[off + i]);
            } else {
                w = snprintf(row, sizeof row, "   ");
            }
            cwist_sstring_append_len(out, row, (size_t)w);
            if (i == 7) cwist_sstring_append_len(out, " ", 1);
        }
        cwist_sstring_append_len(out, " |", 2);
        for (size_t i = 0; i < n; i++) {
            unsigned char c = data[off + i];
            if (c == '<') {
                cwist_sstring_append_len(out, "&lt;", 4);
            } else if (c == '>') {
                cwist_sstring_append_len(out, "&gt;", 4);
            } else if (c == '&') {
                cwist_sstring_append_len(out, "&amp;", 5);
            } else if (c < 0x20 || c > 0x7e) {
                cwist_sstring_append_len(out, ".", 1);
            } else {
                cwist_sstring_append_len(out, (const char *)&data[off + i], 1);
            }
        }
        cwist_sstring_append_len(out, "|\n", 2);
    }
}

/* Rebuild the request head from the parsed fields; the parser does not keep
 * the raw octets, so this is a reassembly, not a capture. */
static void reconstruct_request(cwist_http_request *req, cwist_sstring *out) {
    cwist_sstring_append(out, cwist_http_method_to_string(req->method));
    cwist_sstring_append(out, " ");
    if (req->path) cwist_sstring_append_sstring(out, req->path);
    if (req->query && cwist_sstring_get_size(req->query) > 0) {
        cwist_sstring_append(out, "?");
        cwist_sstring_append_sstring(out, req->query);
    }
    cwist_sstring_append(out, " HTTP/1.1\r\n");
    for (cwist_http_header_node *h = req->headers; h; h = h->next) {
        if (h->key) cwist_sstring_append_sstring(out, h->key);
        cwist_sstring_append(out, ": ");
        if (h->value) cwist_sstring_append_sstring(out, h->value);
        cwist_sstring_append(out, "\r\n");
    }
    cwist_sstring_append(out, "\r\n");
    if (req->body && cwist_sstring_get_size(req->body) > 0) {
        cwist_sstring_append_sstring(out, req->body);
    }
}

static const char PAGE_HEAD[] =
    "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
    "<title>cwist f769 wire inspector</title><style>"
    "body{background:#0d1117;color:#c9d1d9;font-family:system-ui,sans-serif;margin:2em}"
    "h1{color:#58a6ff;font-size:1.4em}"
    ".grid{display:grid;grid-template-columns:1fr 1fr;gap:1em}"
    ".panel{background:#161b22;border:1px solid #30363d;border-radius:8px;padding:1em}"
    ".panel h2{margin-top:0;font-size:1em;color:#8b949e}"
    ".in h2{color:#3fb950}.out h2{color:#d29922}"
    "pre{font-family:ui-monospace,monospace;font-size:12px;white-space:pre-wrap;"
    "word-break:break-all;color:#a5d6ff}"
    ".meta{color:#8b949e;font-size:0.85em;margin-bottom:1em}"
    "textarea{width:100%;background:#0d1117;color:#c9d1d9;border:1px solid #30363d}"
    "button{background:#238636;color:#fff;border:0;border-radius:6px;padding:.5em 1em;cursor:pointer}"
    "</style></head><body>"
    "<h1>CWIST on STM32F769I-DISCO &mdash; wire inspector</h1>";

static void inspector_handler(cwist_http_request *req, cwist_http_response *res) {
    g_hits++;

    cwist_sstring *incoming = cwist_sstring_create();
    reconstruct_request(req, incoming);
    size_t in_len = cwist_sstring_get_size(incoming);
    char *in_buf = malloc(in_len + 1);

    cwist_sstring *page = cwist_sstring_create();
    cwist_sstring_append(page, PAGE_HEAD);

    char meta[160];
    int w = snprintf(meta, sizeof meta,
                     "<div class=\"meta\">request #%lu &middot; hit Refresh or F5 to swap the panels</div>",
                     g_hits);
    cwist_sstring_append_len(page, meta, (size_t)w);

    cwist_sstring_append(page, "<div class=\"grid\"><div class=\"panel in\">"
                               "<h2>INCOMING &mdash; browser &rarr; board (this request)</h2><pre>");
    if (in_buf) {
        cwist_sstring_copy(incoming, in_buf);
        hexdump_html(page, (const unsigned char *)in_buf, in_len);
        free(in_buf);
    }
    cwist_sstring_destroy(incoming);

    cwist_sstring_append(page, "</pre></div><div class=\"panel out\">"
                               "<h2>OUTGOING &mdash; board &rarr; browser (previous response)</h2><pre>");
    if (g_last_response_len > 0) {
        hexdump_html(page, (const unsigned char *)g_last_response, g_last_response_len);
    } else {
        cwist_sstring_append(page, "(nothing sent yet &mdash; reload once)");
    }
    cwist_sstring_append(page, "</pre></div></div>");

    cwist_sstring_append(page,
        "<h2 style=\"margin-top:1em;color:#8b949e;font-size:1em\">POST something</h2>"
        "<form method=\"post\" action=\"/\">"
        "<textarea name=\"payload\" rows=\"3\" placeholder=\"type bytes to POST\">hello stm32</textarea>"
        "<br><button type=\"submit\">POST /</button></form>"
        "</body></html>");

    cwist_http_response_header_add(res, "Content-Type", "text/html; charset=utf-8");
    cwist_sstring_assign(res->body, "");
    cwist_sstring_append_sstring(res->body, page);
    cwist_sstring_destroy(page);

    /* Keep the exact bytes we are about to send for the next page load. */
    char *wire = NULL;
    size_t wire_len = 0;
    if (cwist_http_response_serialize(res, &wire, &wire_len) == 0 && wire) {
        g_last_response_len = wire_len < sizeof g_last_response ? wire_len : sizeof g_last_response;
        memcpy(g_last_response, wire, g_last_response_len);
        free(wire);
    }
}

static void hello_handler(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    cwist_http_response_set_body_ptr(res, "hello from stm32f769", 20);
    cwist_http_response_header_add(res, "Content-Type", "text/plain");
}

static void json_handler(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    static const char body[] = "{\"board\":\"stm32f769i-disco\",\"rtos\":false}";
    cwist_http_response_set_body_ptr(res, body, sizeof(body) - 1);
    cwist_http_response_header_add(res, "Content-Type", "application/json");
}

int cwist_stm32_app_init(void) {
    if (g_server) return 0;
    g_app = cwist_app_create();
    if (!g_app) return -1;
    cwist_app_get(g_app, "/", inspector_handler);
    cwist_app_post(g_app, "/", inspector_handler);
    cwist_app_get(g_app, "/hello", hello_handler);
    cwist_app_get(g_app, "/json", json_handler);

    cwist_lwip_config cfg;
    cwist_lwip_config_init(&cfg);
    cfg.max_conns = 4;
    cfg.max_header_bytes = 4096;
    cfg.max_body_bytes = 4096;
    cfg.idle_timeout_ms = 30000;
    g_server = cwist_lwip_listen(g_app, &cfg);
    if (!g_server) {
        cwist_app_destroy(g_app);
        g_app = NULL;
        return -1;
    }
    return 0;
}
