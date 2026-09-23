/* Native half of the server/WASM rendering parity check: the same views and
 * the same expected bytes run under Emscripten in tests/wasm_smoke.c. */
#include "html_views_shared.h"
#include <stdio.h>

int main(void) {
    printf("Testing HTML view rendering (native half of the WASM parity check)...\n");
    cwist_app *app = cwist_app_create();
    if (!app || html_views_install(app) != 0) {
        fprintf(stderr, "FAIL: could not install the shared views\n");
        return 1;
    }
    int failed = html_views_check(app);
    html_views_teardown();
    cwist_app_destroy(app);
    if (failed) {
        fprintf(stderr, "FAIL: shared view check %d\n", failed);
        return 1;
    }
    printf("All HTML parity tests passed!\n");
    return 0;
}
