#include <cwist/net/graphql/graphql.h>
#include <cwist/core/mem/alloc.h>
#include <cwist/core/sstring/sstring.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include <stdio.h>

typedef enum { GQL_OP_QUERY, GQL_OP_MUTATION } gql_op_type_t;

typedef struct graphql_field {
    char *name;
    cwist_graphql_resolver_fn fn;
    void *ctx;
    struct graphql_field *next;
} graphql_field_t;

struct cwist_graphql_schema {
    graphql_field_t *queries;
    graphql_field_t *mutations;
};

/**
 * @brief Wrap an integer status code in a cwist_error_t tagged as CWIST_ERR_INT16.
 * @param code Integer status to encode (0 on success, negative on failure).
 * @return Error value carrying @p code in its err_i16 member.
 */
static cwist_error_t gql_error(int code) {
    return (cwist_error_t){.errtype = CWIST_ERR_INT16, .error.err_i16 = code};
}

/**
 * @brief Test whether a character is valid in a GraphQL name at a given position.
 * @param c Character to test.
 * @param first True when testing the leading character of a name (only '_' or a
 *              letter are allowed there).
 * @retval true The character is allowed at that position.
 * @retval false The character is not allowed.
 */
static bool gql_name_char(char c, bool first) {
    if (first) return (c == '_' || isalpha((unsigned char)c));
    return (c == '_' || isalnum((unsigned char)c));
}

/**
 * @brief Validate that a C string conforms to the GraphQL Name production.
 * @param s String to validate (may be NULL).
 * @retval true The string is a non-empty, fully valid GraphQL name.
 * @retval false The string is NULL, empty, or contains invalid characters.
 */
static bool gql_name_valid(const char *s) {
    if (!s || !gql_name_char(*s, true)) return false;
    for (++s; *s; ++s) {
        if (!gql_name_char(*s, false)) return false;
    }
    return true;
}

/**
 * @brief Append an error object {"message": ...} to a cJSON errors array.
 * @details Silently does nothing if the error object cannot be allocated.
 * @param errors cJSON array receiving the error entry.
 * @param message Human-readable error message text (borrowed, copied into the array).
 */
static void gql_add_error(cJSON *errors, const char *message) {
    cJSON *e = cJSON_CreateObject();
    if (e) {
        cJSON_AddStringToObject(e, "message", message);
        cJSON_AddItemToArray(errors, e);
    }
}

/**
 * @brief Allocate an empty GraphQL schema with no query or mutation fields.
 * @return Newly allocated schema on success, NULL on allocation failure.
 *         The caller owns the result and must free it with
 *         cwist_graphql_schema_destroy().
 */
cwist_graphql_schema_t *cwist_graphql_schema_create(void) {
    cwist_graphql_schema_t *schema = (cwist_graphql_schema_t *)cwist_alloc(sizeof(*schema));
    if (schema) {
        schema->queries = NULL;
        schema->mutations = NULL;
    }
    return schema;
}

/**
 * @brief Free a linked list of graphql_field_t nodes and their name strings.
 * @param f Head of the list (may be NULL).
 */
static void free_fields(graphql_field_t *f) {
    while (f) {
        graphql_field_t *n = f->next;
        if (f->name) cwist_free(f->name);
        cwist_free(f);
        f = n;
    }
}

/**
 * @brief Destroy a schema and all registered query/mutation fields.
 * @param schema Schema to free (may be NULL).
 */
void cwist_graphql_schema_destroy(cwist_graphql_schema_t *schema) {
    if (!schema) return;
    free_fields(schema->queries);
    free_fields(schema->mutations);
    cwist_free(schema);
}

/**
 * @brief Insert or update a resolver field in a schema field list.
 * @details If a field with the same name already exists, its resolver and
 *          context are replaced in place; otherwise a new node is prepended.
 *          Field names are validated as GraphQL names.
 * @param head Pointer to the list head pointer.
 * @param field GraphQL field name.
 * @param resolver Resolver callback invoked when the field is queried.
 * @param ctx Opaque pointer passed to the resolver.
 * @retval true The field was registered (or updated).
 * @retval false Invalid argument, invalid field name, or allocation failure.
 */
static bool add_field(graphql_field_t **head, const char *field, cwist_graphql_resolver_fn resolver,
                      void *ctx) {
    if (!head || !gql_name_valid(field) || !resolver) return false;
    for (graphql_field_t *f = *head; f; f = f->next) {
        if (strcmp(f->name, field) == 0) {
            f->fn = resolver;
            f->ctx = ctx;
            return true;
        }
    }
    graphql_field_t *f = (graphql_field_t *)cwist_alloc(sizeof(*f));
    if (!f) return false;
    f->name = cwist_strdup(field);
    if (!f->name) {
        cwist_free(f);
        return false;
    }
    f->fn = resolver;
    f->ctx = ctx;
    f->next = *head;
    *head = f;
    return true;
}

/**
 * @brief Register a resolver for a GraphQL query field.
 * @param schema Schema to register the field on.
 * @param field Query field name (must be a valid GraphQL name).
 * @param resolver Resolver callback invoked when the field is queried.
 * @param ctx Opaque pointer passed to the resolver.
 * @retval true The field was registered (or its resolver updated).
 * @retval false NULL schema or invalid field/resolver.
 */
bool cwist_graphql_add_query(cwist_graphql_schema_t *schema, const char *field,
                             cwist_graphql_resolver_fn resolver, void *ctx) {
    if (!schema) return false;
    return add_field(&schema->queries, field, resolver, ctx);
}

/**
 * @brief Register a resolver for a GraphQL mutation field.
 * @param schema Schema to register the field on.
 * @param field Mutation field name (must be a valid GraphQL name).
 * @param resolver Resolver callback invoked when the field is mutated.
 * @param ctx Opaque pointer passed to the resolver.
 * @retval true The field was registered (or its resolver updated).
 * @retval false NULL schema or invalid field/resolver.
 */
bool cwist_graphql_add_mutation(cwist_graphql_schema_t *schema, const char *field,
                                cwist_graphql_resolver_fn resolver, void *ctx) {
    if (!schema) return false;
    return add_field(&schema->mutations, field, resolver, ctx);
}

/**
 * @brief Look up a field by name in a schema field list.
 * @param head Head of the field list.
 * @param name Field name to find.
 * @return Matching field node, or NULL if not present.
 */
static graphql_field_t *gql_lookup(graphql_field_t *head, const char *name) {
    for (graphql_field_t *f = head; f; f = f->next) {
        if (strcmp(f->name, name) == 0) return f;
    }
    return NULL;
}

/**
 * @brief Advance a parse cursor past whitespace, commas, and GraphQL '#' comments.
 * @param p Current cursor position in the query text.
 * @return Pointer to the first character that is not insignificant.
 */
static const char *skip_ws_comments(const char *p) {
    while (*p) {
        if (isspace((unsigned char)*p) || *p == ',') {
            p++;
        } else if (*p == '#') {
            while (*p && *p != '\n' && *p != '\r') p++;
        } else {
            break;
        }
    }
    return p;
}

/**
 * @brief Parse GraphQL arguments: ( arg1: "val", arg2: 123, arg3: $varName )
 * @details Strings, numbers, booleans, null, and variable references are
 *          supported. Parse failures are appended to @p errors and leave
 *          *out_args as NULL.
 * @param p Cursor positioned at or before the '('.
 * @param variables Optional cJSON object supplying variable values.
 * @param out_args Receives a newly allocated cJSON object with parsed
 *                 arguments (NULL if the field has no argument list).
 * @param errors cJSON array receiving parse error entries.
 * @return Advanced cursor past the closing ')'.
 */
static const char *parse_arguments(const char *p, const cJSON *variables, cJSON **out_args,
                                   cJSON *errors) {
    *out_args = NULL;
    p = skip_ws_comments(p);
    if (*p != '(') return p;
    p++; /* skip '(' */

    cJSON *args = cJSON_CreateObject();
    if (!args) {
        gql_add_error(errors, "Internal memory error parsing arguments");
        return p;
    }

    while (*p) {
        p = skip_ws_comments(p);
        if (*p == ')' || *p == '\0') {
            if (*p == ')') p++;
            break;
        }

        /* Read argument name */
        const char *name_start = p;
        if (!gql_name_char(*p, true)) {
            gql_add_error(errors, "Invalid argument name");
            cJSON_Delete(args);
            return p;
        }
        while (*p && gql_name_char(*p, false)) p++;
        size_t name_len = (size_t)(p - name_start);
        char arg_name[128];
        if (name_len >= sizeof(arg_name)) name_len = sizeof(arg_name) - 1;
        memcpy(arg_name, name_start, name_len);
        arg_name[name_len] = '\0';

        p = skip_ws_comments(p);
        if (*p != ':') {
            gql_add_error(errors, "Expected ':' after argument name");
            cJSON_Delete(args);
            return p;
        }
        p++; /* skip ':' */
        p = skip_ws_comments(p);

        /* Parse argument value */
        cJSON *val = NULL;
        if (*p == '"') {
            p++;
            const char *str_start = p;
            while (*p && *p != '"') {
                if (*p == '\\' && *(p + 1))
                    p += 2;
                else
                    p++;
            }
            if (*p != '"') {
                gql_add_error(errors, "Unterminated string in argument");
                cJSON_Delete(args);
                return p;
            }
            size_t str_len = (size_t)(p - str_start);
            cwist_scratch_t str_val_s CWIST_SCRATCH_DEFER = {0};
            char *str_val = cwist_scratch_alloc(&str_val_s, str_len + 1);
            if (str_val) {
                memcpy(str_val, str_start, str_len);
                str_val[str_len] = '\0';
                val = cJSON_CreateString(str_val);
            }
            p++; /* skip closing '"' */
        } else if (*p == '$') { /* Variable reference */
            p++;
            const char *var_start = p;
            while (*p && gql_name_char(*p, false)) p++;
            size_t var_len = (size_t)(p - var_start);
            char var_name[128];
            if (var_len >= sizeof(var_name)) var_len = sizeof(var_name) - 1;
            memcpy(var_name, var_start, var_len);
            var_name[var_len] = '\0';

            if (variables && cJSON_IsObject(variables)) {
                cJSON *v = cJSON_GetObjectItemCaseSensitive(variables, var_name);
                if (v) val = cJSON_Duplicate(v, true);
            }
            if (!val) val = cJSON_CreateNull();
        } else if (isdigit((unsigned char)*p) || *p == '-') {
            char *endptr = NULL;
            double num = strtod(p, &endptr);
            val = cJSON_CreateNumber(num);
            p = endptr;
        } else if (strncmp(p, "true", 4) == 0 && !gql_name_char(p[4], false)) {
            val = cJSON_CreateBool(true);
            p += 4;
        } else if (strncmp(p, "false", 5) == 0 && !gql_name_char(p[5], false)) {
            val = cJSON_CreateBool(false);
            p += 5;
        } else if (strncmp(p, "null", 4) == 0 && !gql_name_char(p[4], false)) {
            val = cJSON_CreateNull();
            p += 4;
        } else {
            gql_add_error(errors, "Unsupported argument value format");
            cJSON_Delete(args);
            return p;
        }

        if (val) {
            cJSON_AddItemToObject(args, arg_name, val);
        }
    }

    *out_args = args;
    return p;
}

/**
 * @brief Parse and execute field selections recursively.
 * @details Each selected field is resolved through @p schema_fields and its
 *          value added to @p parent_data. Recursion depth and field count are
 *          bounded by CWIST_GRAPHQL_MAX_DEPTH / CWIST_GRAPHQL_MAX_FIELDS, and
 *          unknown fields are reported in @p errors.
 * @param p Cursor positioned at '{'.
 * @param schema_fields Field list used to resolve selections.
 * @param variables Optional cJSON object of variable values.
 * @param parent_data cJSON object receiving resolved field values.
 * @param errors cJSON array receiving error entries.
 * @param depth Current recursion depth (1 for the root selection set).
 * @return Advanced cursor past the closing '}'.
 */
static const char *parse_selection_set(const char *p, graphql_field_t *schema_fields,
                                       const cJSON *variables, cJSON *parent_data, cJSON *errors,
                                       unsigned int depth) {
    if (depth > CWIST_GRAPHQL_MAX_DEPTH) {
        gql_add_error(errors, "Query selection depth exceeds maximum allowed");
        return p;
    }

    p = skip_ws_comments(p);
    if (*p != '{') return p;
    p++; /* skip '{' */

    size_t field_count = 0;
    while (*p) {
        p = skip_ws_comments(p);
        if (*p == '}' || *p == '\0') {
            if (*p == '}') p++;
            break;
        }

        if (++field_count > CWIST_GRAPHQL_MAX_FIELDS) {
            gql_add_error(errors, "Field count limit exceeded");
            break;
        }

        /* Read Alias or Field Name */
        const char *name_start = p;
        if (!gql_name_char(*p, true)) {
            gql_add_error(errors, "Invalid field name syntax");
            break;
        }
        while (*p && gql_name_char(*p, false)) p++;
        size_t name_len = (size_t)(p - name_start);
        char token1[128];
        if (name_len >= sizeof(token1)) name_len = sizeof(token1) - 1;
        memcpy(token1, name_start, name_len);
        token1[name_len] = '\0';

        char field_name[128];
        char output_key[128];
        strcpy(output_key, token1);
        strcpy(field_name, token1);

        p = skip_ws_comments(p);
        if (*p == ':') { /* Alias: output_key: field_name */
            p++;
            p = skip_ws_comments(p);
            const char *field_start = p;
            if (!gql_name_char(*p, true)) {
                gql_add_error(errors, "Invalid field name after alias");
                break;
            }
            while (*p && gql_name_char(*p, false)) p++;
            size_t f_len = (size_t)(p - field_start);
            if (f_len >= sizeof(field_name)) f_len = sizeof(field_name) - 1;
            memcpy(field_name, field_start, f_len);
            field_name[f_len] = '\0';
        }

        /* Parse Field Arguments if present */
        cJSON *args = NULL;
        p = parse_arguments(p, variables, &args, errors);

        /* Resolve Field Value */
        cJSON *val = NULL;
        graphql_field_t *f = gql_lookup(schema_fields, field_name);
        if (!f) {
            char err_buf[256];
            snprintf(err_buf, sizeof(err_buf), "Cannot query field '%s' on schema", field_name);
            gql_add_error(errors, err_buf);
            val = cJSON_CreateNull();
        } else {
            val = f->fn(args, variables, f->ctx);
            if (!val) {
                val = cJSON_CreateNull();
            }
        }
        if (args) cJSON_Delete(args);

        /* Parse nested selection set if present and value is object */
        p = skip_ws_comments(p);
        if (*p == '{') {
            if (cJSON_IsObject(val)) {
                p = parse_selection_set(p, schema_fields, variables, val, errors, depth + 1);
            } else {
                /* Skip sub-selection if non-object or null */
                int brace_count = 0;
                while (*p) {
                    if (*p == '{')
                        brace_count++;
                    else if (*p == '}') {
                        brace_count--;
                        if (brace_count == 0) {
                            p++;
                            break;
                        }
                    }
                    p++;
                }
            }
        }

        cJSON_AddItemToObject(parent_data, output_key, val);
    }

    return p;
}

/**
 * @brief Execute a GraphQL query or mutation from a JSON request body.
 * @details The request must be a JSON object with a string "query" member and
 *          an optional "variables" object. On success *out_json receives a
 *          compact JSON response of the form {"data": {...}} with an "errors"
 *          array added when resolution produced errors. Ownership of
 *          *out_json transfers to the caller.
 * @param schema Schema providing the query/mutation fields.
 * @param request_json JSON-encoded GraphQL request text.
 * @param out_json Receives the response body string on success.
 * @return 0 on success, non-zero on invalid arguments or internal failure.
 */
cwist_error_t cwist_graphql_execute(cwist_graphql_schema_t *schema, const char *request_json,
                                    cwist_sstring **out_json) {
    if (!schema || !request_json || !out_json) return gql_error(-1);
    *out_json = NULL;

    cJSON *request = cJSON_Parse(request_json);
    cJSON *root = cJSON_CreateObject();
    cJSON *data = cJSON_CreateObject();
    cJSON *errors = cJSON_CreateArray();

    if (!request || !root || !data || !errors) {
        cJSON_Delete(request);
        cJSON_Delete(root);
        cJSON_Delete(data);
        cJSON_Delete(errors);
        return gql_error(-1);
    }
    cJSON_AddItemToObject(root, "data", data);
    cJSON_AddItemToObject(root, "errors", errors);

    cJSON *query = cJSON_GetObjectItemCaseSensitive(request, "query");
    cJSON *variables = cJSON_GetObjectItemCaseSensitive(request, "variables");

    if (!cJSON_IsString(query) || !query->valuestring) {
        gql_add_error(errors, "GraphQL request must contain a string 'query'");
    } else if (strlen(query->valuestring) > CWIST_GRAPHQL_MAX_QUERY_SIZE) {
        gql_add_error(errors, "Query string exceeds maximum allowed size");
    } else {
        const char *p = skip_ws_comments(query->valuestring);
        gql_op_type_t op_type = GQL_OP_QUERY;

        if (strncmp(p, "mutation", 8) == 0 && !gql_name_char(p[8], false)) {
            op_type = GQL_OP_MUTATION;
            p += 8;
            p = skip_ws_comments(p);
            /* Skip optional mutation name / variable defs until '{' */
            while (*p && *p != '{') p++;
        } else if (strncmp(p, "query", 5) == 0 && !gql_name_char(p[5], false)) {
            op_type = GQL_OP_QUERY;
            p += 5;
            p = skip_ws_comments(p);
            while (*p && *p != '{') p++;
        }

        if (*p != '{') {
            gql_add_error(errors, "Expected '{' to begin selection set");
        } else {
            graphql_field_t *target_schema =
                (op_type == GQL_OP_MUTATION) ? schema->mutations : schema->queries;
            parse_selection_set(p, target_schema, variables, data, errors, 1);
        }
    }

    if (cJSON_GetArraySize(errors) == 0) {
        cJSON_DeleteItemFromObjectCaseSensitive(root, "errors");
    }

    char *printed = cJSON_PrintUnformatted(root);
    cJSON_Delete(request);
    cJSON_Delete(root);

    if (!printed) return gql_error(-1);

    *out_json = cwist_sstring_create();
    if (!*out_json) {
        free(printed);
        return gql_error(-1);
    }
    cwist_sstring_assign(*out_json, printed);
    free(printed);

    return gql_error(0);
}

/**
 * @brief Extract the root field name and parsed arguments of a `subscription`
 *        operation.
 * @details Used by the experimental WS subscription layer
 *          (cwist/net/graphql/graphql_ws.h); not part of the query/mutation
 *          execute path. Only the first root field is reported, matching the
 *          single-stream model of the graphql-ws broker.
 * @param query GraphQL subscription query text.
 * @param variables Optional cJSON object of variable values.
 * @param field_out Buffer receiving the resolved root field name.
 * @param field_cap Capacity of @p field_out in bytes.
 * @param args_out Receives a newly allocated cJSON argument object (NULL when
 *                 the field carries no arguments); caller owns it.
 * @retval true The subscription field and arguments were extracted.
 * @retval false Invalid arguments or malformed subscription query.
 */
bool cwist_graphql_parse_subscription(const char *query, const cJSON *variables, char *field_out,
                                      size_t field_cap, cJSON **args_out) {
    if (!query || !field_out || field_cap == 0 || !args_out) return false;
    *args_out = NULL;
    field_out[0] = '\0';

    const char *p = skip_ws_comments(query);
    if (strncmp(p, "subscription", 12) != 0 || gql_name_char(p[12], false)) return false;
    p += 12;
    p = skip_ws_comments(p);
    /* Skip optional operation name / variable definitions until '{' (same
     * treatment as the query/mutation keyword in cwist_graphql_execute). */
    while (*p && *p != '{') p++;
    if (*p != '{') return false;
    p++;
    p = skip_ws_comments(p);
    if (*p == '}' || !gql_name_char(*p, true)) return false;

    char token[128];
    const char *name_start = p;
    while (*p && gql_name_char(*p, false)) p++;
    size_t name_len = (size_t)(p - name_start);
    if (name_len >= sizeof(token)) name_len = sizeof(token) - 1;
    memcpy(token, name_start, name_len);
    token[name_len] = '\0';

    p = skip_ws_comments(p);
    if (*p == ':') { /* Alias: the resolvable name follows the alias. */
        p++;
        p = skip_ws_comments(p);
        name_start = p;
        if (!gql_name_char(*p, true)) return false;
        while (*p && gql_name_char(*p, false)) p++;
        name_len = (size_t)(p - name_start);
        if (name_len >= sizeof(token)) name_len = sizeof(token) - 1;
        memcpy(token, name_start, name_len);
        token[name_len] = '\0';
    }

    size_t out_len = strlen(token);
    if (out_len >= field_cap) return false;
    memcpy(field_out, token, out_len + 1);

    cJSON *errors = cJSON_CreateArray();
    if (!errors) return false;
    parse_arguments(p, variables, args_out, errors);
    cJSON_Delete(errors);
    /* *args_out stays NULL when the field carries no arguments. */
    return true;
}

/**
 * @brief HTTP handler serving GraphQL requests over POST.
 * @details Executes the request body via cwist_graphql_execute() and writes
 *          the JSON response. Non-POST requests or execution failures yield a
 *          400 status with an error body. The Content-Type header is always
 *          set to application/json.
 * @param schema Schema used to resolve the request.
 * @param req Incoming HTTP request (must use POST).
 * @param res Response object receiving status, body, and headers.
 */
void cwist_graphql_serve(cwist_graphql_schema_t *schema, cwist_http_request *req,
                         cwist_http_response *res) {
    if (!schema || !req || !res || req->method != CWIST_HTTP_POST) {
        if (res) res->status_code = CWIST_HTTP_BAD_REQUEST;
        return;
    }

    cwist_sstring *json = NULL;
    const char *body = req->body ? req->body->data : NULL;

    if (cwist_graphql_execute(schema, body, &json).error.err_i16 != 0) {
        res->status_code = CWIST_HTTP_BAD_REQUEST;
        cwist_sstring_assign(res->body, "{\"errors\":[{\"message\":\"invalid GraphQL request\"}]}");
    } else {
        res->status_code = CWIST_HTTP_OK;
        cwist_sstring_assign_len(res->body, json->data, json->size);
        cwist_sstring_destroy(json);
    }
    cwist_http_header_add(&res->headers, "Content-Type", "application/json");
}
