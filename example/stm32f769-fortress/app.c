#include "app.h"

#include <cwist/app.h>
#include <cwist/net/http/http.h>
#include <cwist/sys/port/lwip.h>

#include "dashboard.h"

#include <stdio.h>
#include <string.h>

#define PAYLOAD_ADDRESS 0xC0200000UL

static cwist_app *g_app;
static cwist_lwip_server *g_server;
static cwist_lwip_conn_info g_info[FORTRESS_MAX_CONNS] FORTRESS_DTCM;
static uint32_t g_requests;
static size_t g_payload_bytes = 16384;
static unsigned char *const g_payload = (unsigned char *)PAYLOAD_ADDRESS;

static const char PAGE[] =
    "<!doctype html><html lang=en><head><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>CWIST Connection Fortress</title><style>"
    ":root{color-scheme:dark;font-family:system-ui,sans-serif;background:#0d1117;color:#c9d1d9}"
    "body{max-width:1180px;margin:0 auto;padding:24px}h1{color:#58a6ff;margin:0 0 6px}"
    ".sub{color:#8b949e;margin-bottom:22px}.stats{display:grid;grid-template-columns:repeat(4,1fr);gap:12px}"
    ".card{background:#161b22;border:1px solid #30363d;border-radius:10px;padding:14px}"
    ".value{font-size:28px;font-weight:700;color:#f0f6fc}.label{font-size:12px;color:#8b949e;text-transform:uppercase}"
    ".toolbar{display:flex;align-items:center;gap:14px;margin:18px 0}select,a{background:#21262d;color:#c9d1d9;"
    "border:1px solid #30363d;border-radius:6px;padding:9px;text-decoration:none}a:hover{border-color:#58a6ff}"
    "#grid{display:grid;grid-template-columns:repeat(24,1fr);gap:4px;margin:16px 0}"
    "#grid i{aspect-ratio:1.35;border-radius:3px;background:#161b22;box-shadow:inset 0 0 0 1px #30363d}"
    ".legend{display:flex;gap:18px;flex-wrap:wrap;color:#8b949e;font-size:13px}.legend b{display:inline-block;"
    "width:12px;height:12px;border-radius:2px;margin-right:6px;vertical-align:-1px}"
    ".s0{background:#161b22!important}.s1{background:#1f6fbe!important}.s2{background:#d29922!important}"
    ".s3{background:#a371f7!important}.s4{background:#3fb950!important}#status{color:#8b949e;font-size:13px}"
    "@media(max-width:700px){.stats{grid-template-columns:repeat(2,1fr)}#grid{grid-template-columns:repeat(16,1fr)}}"
    "</style></head><body><h1>CWIST Connection Fortress</h1>"
    "<div class=sub>STM32F769I-DISCO / Cortex-M7 / 512 KB SRAM / no RTOS</div>"
    "<div class=stats>"
    "<div class=card><div class=value id=active>-</div><div class=label>Active connections</div></div>"
    "<div class=card><div class=value id=accepted>-</div><div class=label>Accepted</div></div>"
    "<div class=card><div class=value id=rejected>-</div><div class=label>Rejected at capacity</div></div>"
    "<div class=card><div class=value id=requests>-</div><div class=label>HTTP requests</div></div></div>"
    "<div class=toolbar><label>Streaming payload <select id=payload>"
    "<option value=64>64 B</option><option value=16384>16 KiB</option>"
    "<option value=262144>256 KiB</option><option value=1048576>1 MiB</option>"
    "</select></label><a href=/stream target=_blank>Open stream</a><span id=status>Connecting...</span></div>"
    "<div class=legend><span><b class=s0></b>Free</span><span><b class=s1></b>Idle</span>"
    "<span><b class=s2></b>Receiving</span><span><b class=s3></b>Sending</span>"
    "<span><b class=s4></b>Streaming</span></div><div id=grid></div>"
    "<div class=sub>Each cell is one real slot in the fixed lwIP connection pool. "
    "The LCD on the board renders the same state.</div><script>"
    "const grid=document.querySelector('#grid'),cells=[];let timer;"
    "const names=['active','accepted','rejected','requests'];"
    "function draw(s){for(const n of names)document.querySelector('#'+n).textContent=n==='active'?"
    "s.active+' / '+s.max:s[n];document.querySelector('#payload').value=s.payload;"
    "while(cells.length<s.slots.length){const c=document.createElement('i');grid.append(c);cells.push(c)}"
    "s.slots.forEach((v,i)=>cells[i].className='s'+v);"
    "document.querySelector('#status').textContent='Live / '+s.streaming+' streaming';}"
    "async function poll(){try{const r=await fetch('/api/stats',{cache:'no-store'});if(!r.ok)throw 0;draw(await r.json())}"
    "catch(e){document.querySelector('#status').textContent='Board unreachable';}timer=setTimeout(poll,500)}"
    "document.querySelector('#payload').onchange=async e=>{await fetch('/api/config',{method:'POST',"
    "headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'payload='+e.target.value})};poll();"
    "</script></body></html>";

static void text_response(cwist_http_response *res, const char *type, const char *body,
                          size_t len) {
    cwist_http_response_header_add(res, "Content-Type", type);
    cwist_http_response_set_body_ptr(res, body, len);
}

static void page_handler(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    g_requests++;
    text_response(res, "text/html; charset=utf-8", PAGE, sizeof(PAGE) - 1);
}

static void stream_handler(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    g_requests++;
    text_response(res, "application/octet-stream", (const char *)g_payload, g_payload_bytes);
}

static void stats_handler(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    g_requests++;
    cwist_lwip_server_stats stats;
    cwist_lwip_get_server_stats(g_server, &stats);
    cwist_lwip_get_connection_info(g_server, g_info, FORTRESS_MAX_CONNS);
    size_t streaming = 0;
    cwist_sstring *json = cwist_sstring_create();
    if (!json) {
        res->status_code = CWIST_HTTP_INTERNAL_ERROR;
        return;
    }
    char field[192];
    int n = snprintf(field, sizeof field,
                     "{\"max\":%u,\"active\":%u,\"accepted\":%lu,\"rejected\":%lu,"
                     "\"requests\":%lu,\"payload\":%u,",
                     (unsigned)stats.max_connections, (unsigned)stats.active_connections,
                     (unsigned long)stats.accepted_connections,
                     (unsigned long)stats.rejected_connections, (unsigned long)g_requests,
                     (unsigned)g_payload_bytes);
    cwist_sstring_append_len(json, field, (size_t)n);
    for (size_t i = 0; i < FORTRESS_MAX_CONNS; i++) {
        if (g_info[i].state == CWIST_LWIP_CONN_STREAMING) streaming++;
    }
    n = snprintf(field, sizeof field, "\"streaming\":%u,\"slots\":[", (unsigned)streaming);
    cwist_sstring_append_len(json, field, (size_t)n);
    for (size_t i = 0; i < FORTRESS_MAX_CONNS; i++) {
        n = snprintf(field, sizeof field, i ? ",%d" : "%d", (int)g_info[i].state);
        cwist_sstring_append_len(json, field, (size_t)n);
    }
    cwist_sstring_append(json, "]}");
    cwist_http_response_header_add(res, "Content-Type", "application/json");
    cwist_http_response_header_add(res, "Cache-Control", "no-store");
    cwist_sstring_assign(res->body, "");
    cwist_sstring_append_sstring(res->body, json);
    cwist_sstring_destroy(json);
}

static void config_handler(cwist_http_request *req, cwist_http_response *res) {
    g_requests++;
    size_t value = 0;
    if (req->body && req->body->size > 8 && req->body->size < 32 &&
        memcmp(req->body->data, "payload=", 8) == 0) {
        for (size_t i = 8; i < req->body->size; i++) {
            char c = req->body->data[i];
            if (c < '0' || c > '9') {
                value = 0;
                break;
            }
            value = value * 10 + (size_t)(c - '0');
        }
    }
    if (value != 64 && value != 16384 && value != 262144 && value != FORTRESS_MAX_PAYLOAD) {
        res->status_code = CWIST_HTTP_BAD_REQUEST;
        text_response(res, "text/plain", "invalid payload size", 20);
        return;
    }
    g_payload_bytes = value;
    char body[48];
    int n = snprintf(body, sizeof body, "{\"payload\":%u}", (unsigned)g_payload_bytes);
    cwist_http_response_header_add(res, "Content-Type", "application/json");
    cwist_sstring_assign_len(res->body, body, (size_t)n);
}

int cwist_stm32_app_init(void) {
    if (g_server) return 0;
    for (size_t i = 0; i < FORTRESS_MAX_PAYLOAD; i++) g_payload[i] = (unsigned char)('A' + i % 26);

    g_app = cwist_app_create();
    if (!g_app) return -1;
    cwist_app_get(g_app, "/", page_handler);
    cwist_app_get(g_app, "/stream", stream_handler);
    cwist_app_get(g_app, "/api/stats", stats_handler);
    cwist_app_post(g_app, "/api/config", config_handler);

    cwist_lwip_config cfg;
    cwist_lwip_config_init(&cfg);
    cfg.max_conns = FORTRESS_MAX_CONNS;
    cfg.max_header_bytes = 512;
    cfg.max_body_bytes = 256;
    cfg.rx_buffer_bytes = 512;
    cfg.tx_buffer_bytes = 192;
    cfg.idle_timeout_ms = 30000;
    g_server = cwist_lwip_listen(g_app, &cfg);
    if (!g_server) {
        cwist_app_destroy(g_app);
        g_app = NULL;
        return -1;
    }
    return 0;
}

void cwist_stm32_app_update(uint32_t ipv4) {
    dashboard_render(g_server, ipv4, g_requests, g_payload_bytes);
}
