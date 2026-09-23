/* Streaming dispatch guest for the component pipeline (issue #203, stage 3).
 *
 * Same routes as tests/wasm_component_guest.c plus an SSE-style route,
 * exporting the cwist-guest-stream world from wit/cwist.wit: the plain
 * guest interface (dispatch, use-session) plus guest-stream's
 * dispatch-stream, whose response chunks travel through the async host
 * import host.send-chunk. Built for wasm32-wasip3 only; driven from node
 * by tests/wasm_component_stream_test.js.
 *
 * The chunk sink blocks on the waitable set until the host resolves each
 * send-chunk call: the sync cwist_app_dispatch_stream() pump needs no
 * continuation rewrite, the export task simply suspends mid-pump. */

#include <cwist/sys/app/app.h>
#include <cwist/net/http/session.h>
#include <cwist/wasm/wasm_component.h>
#include <string.h>

#include "cwist_guest_stream.h"

static cwist_app *g_app;

static void hello_handler(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    cwist_sstring_assign(res->body, "hello-from-wrapper-test");
    cwist_http_header_add(&res->headers, "Content-Type", "text/plain");
    cwist_http_header_add(&res->headers, "X-Wrapper-Test", "yes");
}

static void echo_handler(cwist_http_request *req, cwist_http_response *res) {
    if (req->body && req->body->data) {
        cwist_sstring_assign(res->body, req->body->data);
    }
    cwist_http_header_add(&res->headers, "Content-Type", "application/octet-stream");
}

static void session_set_handler(cwist_http_request *req, cwist_http_response *res) {
    cwist_session_t *s = cwist_session_start(g_app, req, res);
    if (!s) {
        res->status_code = CWIST_HTTP_INTERNAL_ERROR;
        return;
    }
    cwist_session_set(s, "user", "alice");
    cwist_session_commit(s, res);
    cwist_sstring_assign(res->body, "session-set");
}

static void session_get_handler(cwist_http_request *req, cwist_http_response *res) {
    cwist_session_t *s = cwist_session_start(g_app, req, res);
    const char *user = s ? cwist_session_get(s, "user") : NULL;
    cwist_sstring_assign(res->body, user ? user : "anonymous");
}

static void events_handler(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    cwist_http_header_add(&res->headers, "Content-Type", "text/event-stream");
    cwist_sstring_assign(res->body, "data: one\n\ndata: two\n\n");
}

/* --- chunk sink over the async host import -------------------------------- */

static cwist_guest_stream_waitable_set_t g_set;
static cwist_guest_stream_subtask_t g_subtask;

/* Send one chunk and block until the host resolves the call. Returns 0 on
 * success, nonzero to abort the dispatch (cwist_app_dispatch_stream()
 * then returns -2). */
static int send_chunk_blocking(const uint8_t *data, size_t len, void *ctx) {
    (void)ctx;
    cwist_guest_stream_list_u8_t chunk = { (uint8_t *)data, len };
    c4punks_cwist_host_result_void_string_t result;
    cwist_guest_stream_subtask_status_t status =
        c4punks_cwist_host_send_chunk(chunk, &result);

    if (CWIST_GUEST_STREAM_SUBTASK_STATE(status) == CWIST_GUEST_STREAM_SUBTASK_RETURNED) {
        if (result.is_err) {
            c4punks_cwist_host_result_void_string_free(&result);
            return 1;
        }
        return 0;
    }

    g_subtask = CWIST_GUEST_STREAM_SUBTASK_HANDLE(status);
    if (!g_set) g_set = cwist_guest_stream_waitable_set_new();
    cwist_guest_stream_waitable_join(g_subtask, g_set);
    for (;;) {
        cwist_guest_stream_event_t event;
        cwist_guest_stream_waitable_set_wait(g_set, &event);
        if (event.event == CWIST_GUEST_STREAM_EVENT_SUBTASK) break;
    }
    cwist_guest_stream_subtask_drop(g_subtask);
    if (result.is_err) {
        c4punks_cwist_host_result_void_string_free(&result);
        return 1;
    }
    return 0;
}

/* cwist_stream_write_fn passes ctx first; adapt to the sender's
 * (data, len, ctx) shape. */
static int stream_sink_adapter(void *ctx, const char *data, size_t len) {
    return send_chunk_blocking((const uint8_t *)data, len, ctx);
}

/* --- wit-bindgen guest exports -------------------------------------------- */

bool exports_c4punks_cwist_guest_dispatch(cwist_guest_stream_list_u8_t *request,
                                          cwist_guest_stream_list_u8_t *ret,
                                          exports_c4punks_cwist_guest_dispatch_error_t *err) {
    uint8_t *res_buf = NULL;
    size_t res_len = 0;
    if (cwist_wasm_component_dispatch(g_app, request->ptr, request->len, &res_buf, &res_len) != 0) {
        err->tag = EXPORTS_C4PUNKS_CWIST_GUEST_DISPATCH_ERROR_INVALID_REQUEST;
        return false;
    }
    ret->ptr = res_buf;
    ret->len = res_len;
    return true;
}

bool exports_c4punks_cwist_guest_use_session(cwist_guest_stream_string_t *maybe_secret,
                                             exports_c4punks_cwist_guest_session_error_t *err) {
    char buf[256];
    const char *secret = NULL;
    if (maybe_secret) {
        if (maybe_secret->len >= sizeof(buf)) {
            err->tag = EXPORTS_C4PUNKS_CWIST_GUEST_SESSION_ERROR_REJECTED;
            return false;
        }
        memcpy(buf, maybe_secret->ptr, maybe_secret->len);
        buf[maybe_secret->len] = '\0';
        secret = buf;
    }
    if (cwist_app_use_session(g_app, secret) != 0) {
        err->tag = EXPORTS_C4PUNKS_CWIST_GUEST_SESSION_ERROR_REJECTED;
        return false;
    }
    return true;
}

bool exports_c4punks_cwist_guest_stream_dispatch_stream(
    cwist_guest_stream_list_u8_t *request,
    exports_c4punks_cwist_guest_stream_dispatch_error_t *err) {
    if (cwist_wasm_component_dispatch_stream(g_app, request->ptr, request->len,
                                             stream_sink_adapter, NULL) != 0) {
        err->tag = EXPORTS_C4PUNKS_CWIST_GUEST_DISPATCH_ERROR_INVALID_REQUEST;
        return false;
    }
    return true;
}

int main(void) {
    g_app = cwist_app_create();
    if (!g_app) return 1;
    cwist_app_get(g_app, "/hello", hello_handler);
    cwist_app_post(g_app, "/echo", echo_handler);
    cwist_app_get(g_app, "/session/set", session_set_handler);
    cwist_app_get(g_app, "/session/get", session_get_handler);
    cwist_app_get(g_app, "/events", events_handler);
    return 0;
}
