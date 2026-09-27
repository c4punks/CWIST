#include <cwist/err/cwist_err.h>

cwist_error_t make_error(cwist_errtype_t type) {
    cwist_error_t err;
    err.errtype = type;
    return err;
}

/* Forwards to the inline helper so the two can never disagree. */
bool cwist_error_is_ok_extern(const cwist_error_t *err) {
    return cwist_error_is_ok(err);
}

void cwist_error_dispose(cwist_error_t *err) {
    if (!err) return;
    if (err->errtype == CWIST_ERR_JSON && err->error.err_json) {
        cJSON_Delete(err->error.err_json);
        err->error.err_json = NULL;
    }
}
