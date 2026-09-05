#include <cwist/err/cwist_err.h>

cwist_error_t make_error(cwist_errtype_t type) {
    cwist_error_t err;
    err.errtype = type;
    return err;
}

void cwist_error_dispose(cwist_error_t *err) {
    if (!err) return;
    if (err->errtype == CWIST_ERR_JSON && err->error.err_json) {
        cJSON_Delete(err->error.err_json);
        err->error.err_json = NULL;
    }
}
