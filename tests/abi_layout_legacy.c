/**
 * @file abi_layout_legacy.c
 * @brief The same layout contract, seen through the legacy umbrella headers.
 *
 * <cwist/sstring.h> and <cwist/err/cwist_err.h> keep their own copies of
 * cwist_sstring and cwist_error_t under the canonical headers' include
 * guards, so a program sees whichever header it includes first. Both copies
 * must describe the same layout and enumerator values as the canonical
 * headers checked in test_abi_layout.c. (The legacy <cwist/http.h> request
 * and response copies are stale and not part of this contract; bindings are
 * generated from the canonical headers.)
 *
 * Linked into test_abi_layout, which compares the report below with the
 * canonical one.
 */
#include <cwist/sstring.h>

#include "abi_layout.h"

ABI_CHECK_SCALARS();
#ifndef USE_128BIT_ERRCODE
ABI_CHECK_ERROR();
#endif
ABI_CHECK_SSTRING();

void abi_layout_legacy_report(abi_layout_report *out) {
    ABI_FILL_REPORT(out);
}
