/**
 * @file async_tls.c
 * @brief Dispatch-thread record of a deferred-response handoff.
 *
 * cwist_async_defer() records the handle it created here, on the dispatch
 * thread. Dispatch decides the handoff from this record instead of
 * res->deferred/res->async, which a producer on another thread can reach and
 * overwrite before the ack (#344). Kept apart from async.c so that http.c and
 * the middleware can use it without linking the completion machinery.
 */

#include "async_internal.h"
#include <cwist/net/http/async.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

static __thread const cwist_async *t_pending_async = NULL;
static __thread const cwist_http_response *t_pending_res = NULL;

static _Atomic bool g_misuse_reported = false;

/* Report an exchange whose public deferred/async fields disagree with what
 * cwist_async_defer() recorded. Prints once per process; with
 * CWIST_ASYNC_STRICT=1 it aborts so the misuse leaves a core. */
static void cwist_async_report_misuse(const char *what) {
    const char *strict = getenv("CWIST_ASYNC_STRICT");
    bool abort_now = strict && strict[0] == '1';
    if (!atomic_exchange(&g_misuse_reported, true) || abort_now) {
        fprintf(stderr,
                "cwist: async misuse: %s; producers must not write res->deferred or "
                "res->async (see cwist_async_wait_dispatched())\n",
                what);
    }
    if (abort_now) abort();
}

void cwist_async_record_pending(cwist_async *a, const cwist_http_response *res) {
    t_pending_async = a;
    t_pending_res = res;
}

void cwist_async_forget_handle(const cwist_async *a) {
    if (a && t_pending_async == a) {
        t_pending_async = NULL;
        t_pending_res = NULL;
    }
}

void cwist_async_forget_response(const cwist_http_response *res) {
    if (res && t_pending_res == res) {
        t_pending_async = NULL;
        t_pending_res = NULL;
    }
}

cwist_async *cwist_async_dispatch_take(cwist_http_response *res) {
    cwist_async *a = t_pending_res == res ? (cwist_async *)t_pending_async : NULL;
    t_pending_async = NULL;
    t_pending_res = NULL;
    if (!res) return NULL;
    if (a) {
        if (!res->deferred || res->async != a)
            cwist_async_report_misuse("res->deferred/res->async changed before the dispatch ack");
    } else if (res->deferred) {
        cwist_async_report_misuse("res->deferred set without cwist_async_defer()");
    }
    return a;
}

bool cwist_async_res_deferred(const cwist_http_response *res) {
    return res && (res->deferred || (t_pending_async && t_pending_res == res));
}
