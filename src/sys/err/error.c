#include <cwist/sys/err/cwist_err.h>

/**
 * @file error.c
 * @brief Minimal constructors for the framework's tagged error container.
 */

/**
 * @brief Initialize a cwist_error_t with the requested storage variant.
 * @param type Error payload discriminator to store in the result.
 * @return Error value with @p errtype set and the payload left for the caller to fill.
 */
cwist_error_t make_error(cwist_errtype_t type) {
    cwist_error_t err;
    err.errtype = type;
    return err;
}

/**
 * @brief Out-of-line variant of the inline cwist_error_is_ok() helper.
 *
 * Forwards to the inline helper so the two can never disagree. Success means
 * the active channel holds its zero/empty value; NULL counts as success and
 * an unknown errtype counts as failure.
 * @param err Error object to test (may be NULL).
 * @return true when @p err represents success.
 */
bool cwist_error_is_ok_extern(const cwist_error_t *err) {
    return cwist_error_is_ok(err);
}

/**
 * @brief Release resources owned by an error value.
 *
 * Currently only CWIST_ERR_JSON owns memory (the cJSON payload); all other
 * variants are no-ops. Safe on success values and on NULL. The released
 * cJSON pointer is reset to NULL, but the rest of the error value is left
 * as-is.
 * @param err Error object to release (may be NULL).
 */
void cwist_error_dispose(cwist_error_t *err) {
    if (!err) return;
    if (err->errtype == CWIST_ERR_JSON && err->error.err_json) {
        cJSON_Delete(err->error.err_json);
        err->error.err_json = NULL;
    }
}
