/**
 * @file test_abi_layout.c
 * @brief Layout contract for the public structs a Rust binding mirrors.
 *
 * The checks are compile-time (see abi_layout.h): if this file builds, the
 * protected structs have the expected field order, types, padding, size and
 * alignment for the target. At run time it prints the resulting layout, so
 * the CI log records the concrete numbers per platform, and compares the
 * legacy-header view from abi_layout_legacy.c with the canonical one.
 */
#include <cwist/net/http/http.h>
#include <stdio.h>
#include <stdlib.h>

#include "abi_layout.h"

ABI_CHECK_SCALARS();
#ifndef USE_128BIT_ERRCODE
ABI_CHECK_ERROR();
#endif
ABI_CHECK_SSTRING();
ABI_CHECK_HEADER_NODE();
ABI_CHECK_REQUEST();
ABI_CHECK_RESPONSE();

#define PRINT_LAYOUT(T, last)                                                          \
    printf("  %-24s size %3zu  align %2zu  last field %-16s at %3zu\n", #T, sizeof(T), \
           (size_t)_Alignof(T), #last, offsetof(T, last))

int main(void) {
    printf("Checking the public struct layout contract...\n");
    printf("  pointers %zu bytes, size_t %zu, off_t %zu\n", sizeof(void *), sizeof(size_t),
           sizeof(off_t));
    PRINT_LAYOUT(cwist_error_t, error);
    PRINT_LAYOUT(cwist_sstring, append);
    PRINT_LAYOUT(cwist_http_header_node, arena_owned);
    PRINT_LAYOUT(cwist_http_request, h2_queue);
    PRINT_LAYOUT(cwist_http_response, async);

    abi_layout_report canonical, legacy;
    ABI_FILL_REPORT(&canonical);
    abi_layout_legacy_report(&legacy);
    int failures = 0;
#define SAME(field)                                                                      \
    do {                                                                                 \
        if (canonical.field != legacy.field) {                                           \
            fprintf(stderr, "FAIL: legacy headers disagree on " #field ": %zu vs %zu\n", \
                    (size_t)canonical.field, (size_t)legacy.field);                      \
            failures++;                                                                  \
        }                                                                                \
    } while (0)
    SAME(error_size);
    SAME(error_align);
    SAME(error_value_offset);
    SAME(sstring_size);
    SAME(sstring_size_offset);
    SAME(sstring_append_offset);
    SAME(errtype_string);
    SAME(errtype_json);
    SAME(errtype_double);
#undef SAME
    if (failures) return 1;
    printf("Passed legacy headers agree with the canonical layout\n");
    printf("All ABI layout checks passed!\n");
    return 0;
}
