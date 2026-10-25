/**
 * @file wasm_component.h
 * @brief WASI component-model helpers for CWIST guest apps (issue #203).
 *
 * Counterpart of wasm_entry.h for the component pipeline: the same
 * cwist_app_dispatch_memory() core, reached through the canonical ABI
 * exports of wit/cwist.wit (world cwist-guest) instead of Emscripten's
 * pointer ABI.
 *
 * The guest app implements the two exports wit-bindgen generates from the
 * world (dispatch, use-session) and forwards them to the helpers below; see
 * tests/wasm_component_guest.c for the full pattern. Guest-side buffer
 * ownership is implicit in the canonical ABI: the host copies the returned
 * list<u8> out of guest linear memory, so there is no dispose entry point.
 *
 * WASI-only: everything is guarded by __wasi__ and this header is a no-op
 * elsewhere, like typedarray.h on native toolchains.
 */

#ifndef __CWIST_WASM_COMPONENT_H__
#define __CWIST_WASM_COMPONENT_H__

#if defined(__wasi__)

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <cwist/sys/app/app.h>
#include <cwist/core/mem/alloc.h>

/**
 * Run the in-memory dispatcher for one serialized HTTP/1.1 request and hand
 * back the serialized response buffer. The caller (the wit-bindgen export
 * shim) copies the bytes into the canonical ABI result and then frees the
 * buffer with cwist_wasm_component_dispose().
 *
 * Returns 0 on success; the WIT error variant for dispatch failures maps to
 * invalid-request here because cwist_app_dispatch_memory() does not
 * distinguish parse failures from unregistered routes.
 */
static inline int cwist_wasm_component_dispatch(cwist_app *app, const uint8_t *req_buf,
                                                size_t req_len, uint8_t **res_buf,
                                                size_t *res_len) {
    if (!res_buf || !res_len) return -1;
    *res_buf = NULL;
    *res_len = 0;
    return cwist_app_dispatch_memory(app, (const char *)req_buf, req_len, (char **)res_buf,
                                     res_len);
}

/** Free a buffer returned by cwist_wasm_component_dispatch(). */
static inline void cwist_wasm_component_dispose(void *ptr) {
    cwist_free(ptr);
}

/**
 * Run the streaming dispatcher for one serialized HTTP/1.1 request:
 * cwist_app_dispatch_stream() pumps the head first and then the body in
 * slices through @p write_fn. On the cwist-guest-stream world the sink
 * wraps the async host import host.send-chunk and blocks on the waitable
 * set until the host resolves it (the export task suspends mid-pump, so
 * the sync cwist_app_dispatch_stream() pump needs no continuation
 * rewrite). Reference sink: tests/wasm_component_stream_guest.c (issue
 * #203, stage 3).
 *
 * Returns 0 on full delivery, nonzero on dispatch failure or sink abort.
 */
static inline int cwist_wasm_component_dispatch_stream(cwist_app *app, const uint8_t *req_buf,
                                                       size_t req_len,
                                                       cwist_stream_write_fn write_fn,
                                                       void *write_ctx) {
    return cwist_app_dispatch_stream(app, (const char *)req_buf, req_len,
                                     write_fn, write_ctx);
}

/**
 * Define the two cwist-guest world exports (dispatch, use-session) for
 * @p app_ptr, a `cwist_app *` populated in main() before the first dispatch:
 * the component counterpart of CWIST_WASM_DEFINE_ENTRY. Expand once at file
 * scope in a translation unit that includes the wit-bindgen header
 * cwist_guest.h. The response buffer handed to the canonical ABI is a libc
 * allocation (cwist_alloc under __wasi__), which the ABI frees itself.
 */
#define CWIST_WASM_COMPONENT_DEFINE_GUEST(app_ptr)                                           \
    bool exports_c4punks_cwist_guest_dispatch(                                               \
        cwist_guest_list_u8_t *request, cwist_guest_list_u8_t *ret,                          \
        exports_c4punks_cwist_guest_dispatch_error_t *err) {                                 \
        uint8_t *res_buf = NULL;                                                             \
        size_t res_len = 0;                                                                  \
        if (cwist_wasm_component_dispatch((app_ptr), request->ptr, request->len, &res_buf,   \
                                          &res_len) != 0) {                                  \
            err->tag = EXPORTS_C4PUNKS_CWIST_GUEST_DISPATCH_ERROR_INVALID_REQUEST;           \
            return false;                                                                    \
        }                                                                                    \
        ret->ptr = res_buf;                                                                  \
        ret->len = res_len;                                                                  \
        return true;                                                                         \
    }                                                                                        \
    bool exports_c4punks_cwist_guest_use_session(                                            \
        cwist_guest_string_t *maybe_secret, exports_c4punks_cwist_guest_session_error_t *err) { \
        char secret_buf[256];                                                                \
        const char *secret = NULL;                                                           \
        if (maybe_secret) {                                                                  \
            if (maybe_secret->len >= sizeof(secret_buf)) {                                   \
                err->tag = EXPORTS_C4PUNKS_CWIST_GUEST_SESSION_ERROR_REJECTED;               \
                return false;                                                                \
            }                                                                                \
            memcpy(secret_buf, maybe_secret->ptr, maybe_secret->len);                        \
            secret_buf[maybe_secret->len] = '\0';                                            \
            secret = secret_buf;                                                             \
        }                                                                                    \
        if (cwist_app_use_session((app_ptr), secret) != 0) {                                 \
            err->tag = EXPORTS_C4PUNKS_CWIST_GUEST_SESSION_ERROR_REJECTED;                   \
            return false;                                                                    \
        }                                                                                    \
        return true;                                                                         \
    }

#else /* !__wasi__ */

/* Non-WASI builds: this header intentionally provides nothing. */

#endif /* __wasi__ */

#endif /* __CWIST_WASM_COMPONENT_H__ */
