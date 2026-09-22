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

#include <stddef.h>
#include <stdint.h>
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

#else /* !__wasi__ */

/* Non-WASI builds: this header intentionally provides nothing. */

#endif /* __wasi__ */

#endif /* __CWIST_WASM_COMPONENT_H__ */
