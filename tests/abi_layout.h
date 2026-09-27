/**
 * @file abi_layout.h
 * @brief Compile-time layout contract for the structs a binding mirrors.
 *
 * A binding generated from the public headers (bindgen for the Rust crate
 * planned in ROADMAP.md, v3.8 Phase 2) copies each struct field by field and
 * relies on the C compiler placing them the same way. These checks pin that
 * layout so a struct change fails the build instead of silently corrupting
 * memory on the other side of the FFI boundary.
 *
 * No sizes are hard-coded. Each field must sit exactly where the platform's
 * C layout rule puts it - at the end of the previous field, rounded up to the
 * field's own alignment - and the struct size and alignment must follow from
 * its last field and its most-aligned member. That is the rule
 * `#[repr(C)]` implements, and it holds on every target CWIST builds for
 * (x86_64 and aarch64 SysV/Darwin, wasm32), so the same checks stay valid
 * where sizes differ (8-byte vs 4-byte pointers). Listing the fields in order
 * with their exact types also catches:
 *
 *   - reordering: a field no longer directly follows its predecessor;
 *   - inserted or removed fields and explicit padding: the next offset or the
 *     struct size no longer matches;
 *   - width or type changes (int vs size_t, pointer vs integer): the _Generic
 *     type check fails, and the offsets move.
 *
 * Not flagged: a new field small enough to fit entirely in existing padding
 * (a bool after the three sstring bools). Every listed offset and the size
 * stay the same, so a binding generated before it still reads every field
 * it knows correctly; it simply does not see the new one.
 *
 * When one of these fails, the struct change is an ABI change: update the
 * list here and regenerate any binding in the same change.
 *
 * Assumption: the targets above align struct members to _Alignof(type).
 * 32-bit x86, where 8-byte members are only 4-aligned inside structs, is not
 * a CWIST target and would fail these checks.
 */
#ifndef CWIST_TESTS_ABI_LAYOUT_H
#define CWIST_TESTS_ABI_LAYOUT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define ABI_ALIGN_UP(n, a) ((((n) + (a) - 1) / (a)) * (a))
#define ABI_MAX(a, b) ((a) > (b) ? (a) : (b))

/* Field f of T has exactly type `type` (after the usual lvalue conversion). */
#define ABI_TYPE(T, f, type)                                                                   \
    _Static_assert(_Generic(((T *)0)->f, type: 1, default: 0), "ABI: " #T "." #f " must have " \
                                                               "type " #type)

/* First field: at offset 0. */
#define ABI_FIRST(T, f, type) \
    ABI_TYPE(T, f, type);     \
    _Static_assert(offsetof(T, f) == 0, "ABI: " #T "." #f " must be the first field")

/* Next field: directly after prev, with only the alignment padding the
 * platform ABI inserts for `type`. */
#define ABI_NEXT(T, prev, f, type)                                                               \
    ABI_TYPE(T, f, type);                                                                        \
    _Static_assert(offsetof(T, f) ==                                                             \
                       ABI_ALIGN_UP(offsetof(T, prev) + sizeof(((T *)0)->prev), _Alignof(type)), \
                   "ABI: " #T "." #f " must directly follow " #prev)

/* End of struct: `last` is the final field, the struct is aligned to its most
 * aligned member, and the size is the end of `last` rounded to that. */
#define ABI_END(T, last, max_align)                                                           \
    _Static_assert(_Alignof(T) == (max_align), "ABI: alignment of " #T " changed");           \
    _Static_assert(sizeof(T) ==                                                               \
                       ABI_ALIGN_UP(offsetof(T, last) + sizeof(((T *)0)->last), _Alignof(T)), \
                   "ABI: " #T " must end with " #last " (field added, removed, or padded)")

/* Type facts a generated binding relies on: size_t maps to usize and C bool
 * to a one-byte bool, and enums are int-sized. */
#define ABI_CHECK_SCALARS()                                                                \
    _Static_assert(sizeof(size_t) == sizeof(void *), "ABI: size_t must be pointer-sized"); \
    _Static_assert(sizeof(bool) == 1, "ABI: bool must be one byte");                       \
    _Static_assert(sizeof(int) == 4, "ABI: int must be 32 bits")

/* --- cwist_errtype_t and cwist_error_t ----------------------------------------
 * Returned by value from nearly every public function, so a binding holds it
 * by value and reads errtype to pick the active value field. The enumerator
 * values are part of the contract. Builds with USE_128BIT_ERRCODE add fields
 * and enumerators and are a different ABI; this contract is the default one. */
#ifndef USE_128BIT_ERRCODE
#define ABI_CHECK_ERROR()                                                                    \
    _Static_assert(sizeof(cwist_errtype_t) == sizeof(int), "ABI: cwist_errtype_t size");     \
    _Static_assert(CWIST_ERR_INT8 == 0 && CWIST_ERR_INT16 == 1 && CWIST_ERR_INT32 == 2 &&    \
                       CWIST_ERR_INT64 == 3,                                                 \
                   "ABI: signed cwist_errtype_t values");                                    \
    _Static_assert(CWIST_ERR_UINT8 == 4 && CWIST_ERR_UINT16 == 5 && CWIST_ERR_UINT32 == 6 && \
                       CWIST_ERR_UINT64 == 7,                                                \
                   "ABI: unsigned cwist_errtype_t values");                                  \
    _Static_assert(CWIST_ERR_STRING == 8 && CWIST_ERR_JSON == 9 && CWIST_ERR_FLOAT == 10 &&  \
                       CWIST_ERR_DOUBLE == 11,                                               \
                   "ABI: complex cwist_errtype_t values");                                   \
    ABI_FIRST(__prim_cwist_error_t, err_i8, int8_t);                                         \
    ABI_NEXT(__prim_cwist_error_t, err_i8, err_i16, int16_t);                                \
    ABI_NEXT(__prim_cwist_error_t, err_i16, err_i32, int32_t);                               \
    ABI_NEXT(__prim_cwist_error_t, err_i32, err_i64, int64_t);                               \
    ABI_NEXT(__prim_cwist_error_t, err_i64, err_u8, uint8_t);                                \
    ABI_NEXT(__prim_cwist_error_t, err_u8, err_u16, uint16_t);                               \
    ABI_NEXT(__prim_cwist_error_t, err_u16, err_u32, uint32_t);                              \
    ABI_NEXT(__prim_cwist_error_t, err_u32, err_u64, uint64_t);                              \
    ABI_NEXT(__prim_cwist_error_t, err_u64, err_string, struct cwist_sstring *);             \
    ABI_NEXT(__prim_cwist_error_t, err_string, err_json, cJSON *);                           \
    ABI_END(__prim_cwist_error_t, err_json, ABI_MAX(_Alignof(int64_t), _Alignof(void *)));   \
    ABI_FIRST(cwist_error_t, errtype, cwist_errtype_t);                                      \
    ABI_NEXT(cwist_error_t, errtype, error, __prim_cwist_error_t);                           \
    ABI_END(cwist_error_t, error, _Alignof(__prim_cwist_error_t))
#endif

/* --- cwist_sstring -------------------------------------------------------------
 * Every request/response string (path, body, header key/value) is a
 * cwist_sstring; a binding reads data and size directly. */
#define ABI_CHECK_SSTRING()                                                            \
    ABI_FIRST(cwist_sstring, data, char *);                                            \
    ABI_NEXT(cwist_sstring, data, is_fixed, bool);                                     \
    ABI_NEXT(cwist_sstring, is_fixed, owns_storage, bool);                             \
    ABI_NEXT(cwist_sstring, owns_storage, borrows_buffer, bool);                       \
    ABI_NEXT(cwist_sstring, borrows_buffer, size, size_t);                             \
    ABI_NEXT(cwist_sstring, size, capacity, size_t);                                   \
    ABI_NEXT(cwist_sstring, capacity, base, char *);                                   \
    ABI_NEXT(cwist_sstring, base, get_size, size_t (*)(struct cwist_sstring *));       \
    ABI_NEXT(cwist_sstring, get_size, compare,                                         \
             int (*)(struct cwist_sstring *, const struct cwist_sstring *));           \
    ABI_NEXT(cwist_sstring, compare, copy,                                             \
             cwist_error_t (*)(struct cwist_sstring *, const struct cwist_sstring *)); \
    ABI_NEXT(cwist_sstring, copy, append,                                              \
             cwist_error_t (*)(struct cwist_sstring *, const struct cwist_sstring *)); \
    ABI_END(cwist_sstring, append, ABI_MAX(_Alignof(void *), _Alignof(size_t)))

/* --- cwist_http_header_node ------------------------------------------------------
 * Request and response headers are a singly linked list a binding walks. */
#define ABI_CHECK_HEADER_NODE()                                                     \
    ABI_FIRST(cwist_http_header_node, key, cwist_sstring *);                        \
    ABI_NEXT(cwist_http_header_node, key, value, cwist_sstring *);                  \
    ABI_NEXT(cwist_http_header_node, value, next, struct cwist_http_header_node *); \
    ABI_NEXT(cwist_http_header_node, next, arena_owned, bool);                      \
    ABI_END(cwist_http_header_node, arena_owned, _Alignof(void *))

/* --- cwist_http_request --------------------------------------------------------
 * The handler argument. Internal fields are included: a generated struct
 * mirrors every field, so moving any of them shifts the ones a binding reads. */
#define ABI_CHECK_REQUEST()                                                            \
    ABI_FIRST(cwist_http_request, method, cwist_http_method_t);                        \
    ABI_NEXT(cwist_http_request, method, path, cwist_sstring *);                       \
    ABI_NEXT(cwist_http_request, path, query, cwist_sstring *);                        \
    ABI_NEXT(cwist_http_request, query, query_params, cwist_query_map *);              \
    ABI_NEXT(cwist_http_request, query_params, path_params, cwist_query_map *);        \
    ABI_NEXT(cwist_http_request, path_params, version, cwist_sstring *);               \
    ABI_NEXT(cwist_http_request, version, headers, cwist_http_header_node *);          \
    ABI_NEXT(cwist_http_request, headers, body, cwist_sstring *);                      \
    ABI_NEXT(cwist_http_request, body, keep_alive, bool);                              \
    ABI_NEXT(cwist_http_request, keep_alive, te_chunked_seen, bool);                   \
    ABI_NEXT(cwist_http_request, te_chunked_seen, expect_100_seen, bool);              \
    ABI_NEXT(cwist_http_request, expect_100_seen, client_fd, int);                     \
    ABI_NEXT(cwist_http_request, client_fd, app, struct cwist_app *);                  \
    ABI_NEXT(cwist_http_request, app, db, cwist_db *);                                 \
    ABI_NEXT(cwist_http_request, db, upgraded, bool);                                  \
    ABI_NEXT(cwist_http_request, upgraded, stream_id, uint32_t);                       \
    ABI_NEXT(cwist_http_request, stream_id, private_data, void *);                     \
    ABI_NEXT(cwist_http_request, private_data, route_middleware_state, void *);        \
    ABI_NEXT(cwist_http_request, route_middleware_state, content_length, size_t);      \
    ABI_NEXT(cwist_http_request, content_length, endpoint_opts, cwist_endpoint_opt_t); \
    ABI_NEXT(cwist_http_request, endpoint_opts, flash, cwist_query_map *);             \
    ABI_NEXT(cwist_http_request, flash, session, void *);                              \
    ABI_NEXT(cwist_http_request, session, csrf_token, char *);                         \
    ABI_NEXT(cwist_http_request, csrf_token, arena, void *);                           \
    ABI_NEXT(cwist_http_request, arena, async_conn, void *);                           \
    ABI_NEXT(cwist_http_request, async_conn, ws_async_handoff, bool);                  \
    ABI_NEXT(cwist_http_request, ws_async_handoff, https_conn, void *);                \
    ABI_NEXT(cwist_http_request, https_conn, h2_queue, void *);                        \
    ABI_END(cwist_http_request, h2_queue, ABI_MAX(_Alignof(void *), _Alignof(size_t)))

/* --- cwist_http_response -------------------------------------------------------
 * The handler's output. file_stream_offset is off_t, which is 64-bit on the
 * CWIST targets even where pointers are 32-bit, so it can raise the
 * struct's alignment above a pointer's. */
#define ABI_CHECK_RESPONSE()                                                                   \
    ABI_FIRST(cwist_http_response, version, cwist_sstring *);                                  \
    ABI_NEXT(cwist_http_response, version, status_code, cwist_http_status_t);                  \
    ABI_NEXT(cwist_http_response, status_code, status_text, cwist_sstring *);                  \
    ABI_NEXT(cwist_http_response, status_text, headers, cwist_http_header_node *);             \
    ABI_NEXT(cwist_http_response, headers, body, cwist_sstring *);                             \
    ABI_NEXT(cwist_http_response, body, endpoint_opts, cwist_endpoint_opt_t);                  \
    ABI_NEXT(cwist_http_response, endpoint_opts, is_ptr_body, bool);                           \
    ABI_NEXT(cwist_http_response, is_ptr_body, ptr_body, const void *);                        \
    ABI_NEXT(cwist_http_response, ptr_body, ptr_body_len, size_t);                             \
    ABI_NEXT(cwist_http_response, ptr_body_len, ptr_body_cleanup, cwist_http_body_cleanup_fn); \
    ABI_NEXT(cwist_http_response, ptr_body_cleanup, ptr_body_cleanup_ctx, void *);             \
    ABI_NEXT(cwist_http_response, ptr_body_cleanup_ctx, use_file_stream, bool);                \
    ABI_NEXT(cwist_http_response, use_file_stream, file_stream_fd, int);                       \
    ABI_NEXT(cwist_http_response, file_stream_fd, file_stream_len, size_t);                    \
    ABI_NEXT(cwist_http_response, file_stream_len, file_stream_offset, off_t);                 \
    ABI_NEXT(cwist_http_response, file_stream_offset, file_stream_auto_close, bool);           \
    ABI_NEXT(cwist_http_response, file_stream_auto_close, stream_mode, bool);                  \
    ABI_NEXT(cwist_http_response, stream_mode, stream_ended, bool);                            \
    ABI_NEXT(cwist_http_response, stream_ended, stream_failed, bool);                          \
    ABI_NEXT(cwist_http_response, stream_failed, stream_buf, cwist_sstring *);                 \
    ABI_NEXT(cwist_http_response, stream_buf, stream_flushed, size_t);                         \
    ABI_NEXT(cwist_http_response, stream_flushed, stream_sink,                                 \
             int (*)(void *, const char *, size_t));                                           \
    ABI_NEXT(cwist_http_response, stream_sink, stream_sink_ctx, void *);                       \
    ABI_NEXT(cwist_http_response, stream_sink_ctx, stream_head_sent, bool);                    \
    ABI_NEXT(cwist_http_response, stream_head_sent, keep_alive, bool);                         \
    ABI_NEXT(cwist_http_response, keep_alive, alt_svc, char *);                                \
    ABI_NEXT(cwist_http_response, alt_svc, arena, void *);                                     \
    ABI_NEXT(cwist_http_response, arena, arena_borrowed, bool);                                \
    ABI_NEXT(cwist_http_response, arena_borrowed, deferred, bool);                             \
    ABI_NEXT(cwist_http_response, deferred, async, void *);                                    \
    ABI_END(cwist_http_response, async,                                                        \
            ABI_MAX(ABI_MAX(_Alignof(void *), _Alignof(size_t)), _Alignof(off_t)))

/* Layout facts one translation unit reports so another can compare them:
 * catches two headers that each pass on their own but disagree. */
typedef struct {
    size_t error_size, error_align, error_value_offset;
    size_t sstring_size, sstring_size_offset, sstring_append_offset;
    int errtype_string, errtype_json, errtype_double;
} abi_layout_report;

/** Filled from the legacy umbrella headers (tests/abi_layout_legacy.c). */
void abi_layout_legacy_report(abi_layout_report *out);

#define ABI_FILL_REPORT(out)                                            \
    do {                                                                \
        (out)->error_size = sizeof(cwist_error_t);                      \
        (out)->error_align = _Alignof(cwist_error_t);                   \
        (out)->error_value_offset = offsetof(cwist_error_t, error);     \
        (out)->sstring_size = sizeof(cwist_sstring);                    \
        (out)->sstring_size_offset = offsetof(cwist_sstring, size);     \
        (out)->sstring_append_offset = offsetof(cwist_sstring, append); \
        (out)->errtype_string = CWIST_ERR_STRING;                       \
        (out)->errtype_json = CWIST_ERR_JSON;                           \
        (out)->errtype_double = CWIST_ERR_DOUBLE;                       \
    } while (0)

#endif
