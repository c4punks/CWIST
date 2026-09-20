/**
 * @file wasm_entry.h
 * @brief Standard WASM entry points the cwist-wasm JS wrapper calls.
 *
 * A consumer app compiles against libcwist_wasm.a and expands
 * CWIST_WASM_DEFINE_ENTRY(app_symbol) exactly once, after its routes are
 * registered and before the module is used. It exports:
 *
 *   - _cwist_wasm_dispatch(req_ptr, req_len, out_len_ptr) -> res_ptr
 *   - _cwist_wasm_dispose(res_ptr)
 *
 * JS side (via the npm package `cwist-wasm`):
 *   const { createCwist } = require('cwist-wasm');
 *   const handle = createCwist(Module);
 *   const res = handle({ method: 'GET', path: '/hello' });
 *   // res.status, res.headers, res.body (Uint8Array)
 *
 * Build the consumer module with the entry exported and the runtime
 * methods the wrapper needs, e.g.:
 *
 *   emcc -O2 -std=c17 -I./include -I./lib -o app.js my_app.c \
 *       libcwist_wasm.a \
 *       -sEXPORTED_FUNCTIONS=_cwist_wasm_dispatch,_cwist_wasm_free,_malloc,_free \
 *       -sEXPORTED_RUNTIME_METHODS=ccall
 *
 * Emscripten-only, like typedarray.h: everything is guarded by
 * __EMSCRIPTEN__ and this header is a no-op on native toolchains.
 */

#ifndef __CWIST_WASM_ENTRY_H__
#define __CWIST_WASM_ENTRY_H__

#ifdef __EMSCRIPTEN__

#include <emscripten.h>
#include <stddef.h>
#include <cwist/sys/app/app.h>
#include <cwist/core/mem/alloc.h>
#include <cwist/wasm/typedarray.h>

/**
 * Define the two entry points the JS wrapper calls, bound to one app
 * instance. The app pointer must outlive the module (register routes first,
 * then expand this macro).
 */
// clang-format off
/* Named cwist_wasm_dispose, not cwist_wasm_free: typedarray.h already
 * defines a static inline cwist_wasm_free(), and a second external
 * definition with the same name collides at file scope. */
#define CWIST_WASM_DEFINE_ENTRY(app_ptr)                                       \
    CWIST_WASM_EXPORT const char *cwist_wasm_dispatch(const char *req_buf,     \
                                                      size_t req_len,          \
                                                      size_t *res_len) {       \
        return cwist_wasm_dispatch_memory((app_ptr), req_buf, req_len,         \
                                          res_len);                            \
    }                                                                          \
    CWIST_WASM_EXPORT void cwist_wasm_dispose(const void *ptr) {               \
        cwist_free((void *)ptr);                                               \
    }                                                                          \
    EM_JS(int, cwist_wasm_stream_chunk_js, (const char *chunk, size_t len), {  \
        /* clang-format off - JS body, not C */                                 \
        if (typeof Module !== "undefined" && typeof Module.cwistStreamChunk === \
            "function") {                                                      \
            return Module.cwistStreamChunk(chunk, len) ? 1 : 0;                \
        }                                                                      \
        return 0;                                                              \
        /* clang-format on */                                                   \
    });                                                                        \
    static int cwist_wasm_stream_sink(void *ctx, const char *data, size_t len) { \
        (void)ctx;                                                             \
        return cwist_wasm_stream_chunk_js(data, len);                          \
    }                                                                          \
    CWIST_WASM_EXPORT int cwist_wasm_dispatch_stream(const char *req_buf,      \
                                                     size_t req_len) {         \
        return cwist_app_dispatch_stream((app_ptr), req_buf, req_len,          \
                                         cwist_wasm_stream_sink, NULL);        \
    }
// clang-format on

#else /* !__EMSCRIPTEN__ */

/* Native builds: nothing to define. */

#endif /* __EMSCRIPTEN__ */

#endif /* __CWIST_WASM_ENTRY_H__ */
