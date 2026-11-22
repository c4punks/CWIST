/**
 * @file json_builder.h
 * @brief Simple JSON string builder utility.
 */

#ifndef __CWIST_JSON_BUILDER_H__
#define __CWIST_JSON_BUILDER_H__

#include <cwist/core/sstring/sstring.h>
#include <stdbool.h>

/**
 * @brief Simple JSON string builder utility.
 *
 * Usage:
 * @code
 * cwist_json_builder *jb = cwist_json_builder_create();
 * cwist_json_begin_object(jb);
 * cwist_json_add_string(jb, "message", "Hello");
 * cwist_json_add_int(jb, "code", 200);
 * cwist_json_begin_object_key(jb, "user");
 * cwist_json_add_string(jb, "name", "alice");
 * cwist_json_end_object(jb);
 * cwist_json_end_object(jb);
 * const char *json = cwist_json_get_raw(jb); // NULL if an append failed
 * // {"message":"Hello","code":200,"user":{"name":"alice"}}
 * cwist_json_builder_destroy(jb);
 * @endcode
 */

typedef struct cwist_json_builder {
    cwist_sstring *buffer;
    bool needs_comma;
} cwist_json_builder;

/**
 * @brief Create a new JSON builder.
 */
cwist_json_builder *cwist_json_builder_create(void);

/**
 * @brief Destroy a JSON builder.
 */
void cwist_json_builder_destroy(cwist_json_builder *b);

/**
 * @brief Begin a JSON object.
 */
void cwist_json_begin_object(cwist_json_builder *b);

/**
 * @brief Begin a JSON object as the value of member @p key (inside an
 * object). A NULL key begins a bare object, like cwist_json_begin_object().
 */
void cwist_json_begin_object_key(cwist_json_builder *b, const char *key);

/**
 * @brief End a JSON object.
 */
void cwist_json_end_object(cwist_json_builder *b);

/**
 * @brief Begin a JSON array.
 */
void cwist_json_begin_array(cwist_json_builder *b, const char *key);

/**
 * @brief End a JSON array.
 */
void cwist_json_end_array(cwist_json_builder *b);

/**
 * @brief Add a string value.
 */
void cwist_json_add_string(cwist_json_builder *b, const char *key, const char *value);

/**
 * @brief Add an integer value.
 */
void cwist_json_add_int(cwist_json_builder *b, const char *key, int value);

/**
 * @brief Add a boolean value.
 */
void cwist_json_add_bool(cwist_json_builder *b, const char *key, bool value);

/**
 * @brief Add a null value.
 */
void cwist_json_add_null(cwist_json_builder *b, const char *key);

/**
 * @brief Returns the raw string buffer. Invalidated on destroy.
 * Returns NULL if any append failed (for example on allocation failure), so
 * truncated JSON is never returned.
 */
const char *cwist_json_get_raw(cwist_json_builder *b);

#endif
