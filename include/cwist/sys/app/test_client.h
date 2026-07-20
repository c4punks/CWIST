/**
 * @file test_client.h
 * @brief In-process HTTP test client (ala Flask test_client).
 */

#ifndef __CWIST_TEST_CLIENT_H__
#define __CWIST_TEST_CLIENT_H__

#include <cwist/sys/app/app.h>
#include <stddef.h>

typedef struct cwist_test_client_cookie {
    char *name;
    char *value;
    char *path;
    struct cwist_test_client_cookie *next;
} cwist_test_client_cookie;

typedef struct cwist_test_client {
    cwist_app *app;
    cwist_test_client_cookie *cookies;
} cwist_test_client;

/** @brief A single key/value pair for headers or ad-hoc cookies. */
typedef struct cwist_test_client_kv {
    const char *key;
    const char *value;
} cwist_test_client_kv;

/** @brief Optional parameters for advanced test requests. */
typedef struct cwist_test_client_request_options {
    const char *body;               /**< Request body (may be NULL). */
    size_t body_len;                /**< Explicit body length; 0 means strlen(body). */
    const char *content_type;       /**< Content-Type header value (may be NULL). */
    const cwist_test_client_kv *headers; /**< Extra request headers. */
    size_t header_count;            /**< Number of extra headers. */
    const cwist_test_client_kv *cookies; /**< Ad-hoc request cookies (not jarred). */
    size_t cookie_count;            /**< Number of ad-hoc cookies. */
    const char *query_string;       /**< Raw query string, e.g. "foo=bar" (may be NULL). */
} cwist_test_client_request_options;

/** @name Lifecycle */
/** @{ */

/**
 * @brief Create a test client for an app.
 */
cwist_test_client *cwist_test_client_create(cwist_app *app);

/**
 * @brief Destroy a test client.
 */
void cwist_test_client_destroy(cwist_test_client *client);

/** @} */

/** @name HTTP Methods */
/** @{ */

/**
 * @brief Perform a GET request.
 */
cwist_http_response *cwist_test_client_get(cwist_test_client *client, const char *path);

/**
 * @brief Perform a POST request.
 */
cwist_http_response *cwist_test_client_post(cwist_test_client *client, const char *path, const char *body);

/**
 * @brief Perform a POST request with JSON content-type.
 */
cwist_http_response *cwist_test_client_post_json(cwist_test_client *client, const char *path, const char *json_body);

/** @} */

#endif
