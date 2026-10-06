#define _POSIX_C_SOURCE 200809L

#include "curl_global.h"

#include <curl/curl.h>
#include <pthread.h>

static pthread_mutex_t g_curl_global_mtx = PTHREAD_MUTEX_INITIALIZER;
static int g_curl_global_ref = 0;

/**
 * @brief Acquire a reference on the global libcurl instance.
 *
 * Initializes libcurl via curl_global_init() on the first acquisition
 * (reference count transitions from 0 to 1). Thread-safe; subsequent
 * calls only increment the reference count. Each call must be paired
 * with exactly one cwist_curl_global_release().
 */
void cwist_curl_global_acquire(void) {
    pthread_mutex_lock(&g_curl_global_mtx);
    if (g_curl_global_ref == 0) {
        curl_global_init(CURL_GLOBAL_DEFAULT);
    }
    g_curl_global_ref++;
    pthread_mutex_unlock(&g_curl_global_mtx);
}

/**
 * @brief Release a reference on the global libcurl instance.
 *
 * Decrements the reference count and calls curl_global_cleanup() when the
 * last reference is dropped. Thread-safe. Calls made without a matching
 * cwist_curl_global_acquire() are ignored (the reference count never goes
 * below zero).
 */
void cwist_curl_global_release(void) {
    pthread_mutex_lock(&g_curl_global_mtx);
    if (g_curl_global_ref > 0) {
        g_curl_global_ref--;
        if (g_curl_global_ref == 0) {
            curl_global_cleanup();
        }
    }
    pthread_mutex_unlock(&g_curl_global_mtx);
}
