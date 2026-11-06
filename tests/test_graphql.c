#include <cwist/net/graphql/graphql.h>
#include <cwist/core/sstring/sstring.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int query_count;
static int mutation_count;
static char last_name[128];

static cJSON *hello_resolver(const cJSON *args, const cJSON *variables, void *ctx) {
    (void)variables;
    (void)ctx;
    query_count++;
    const char *name = "world";
    if (args) {
        cJSON *name_arg = cJSON_GetObjectItemCaseSensitive(args, "name");
        if (!name_arg) name_arg = args->child;
        if (name_arg && cJSON_IsString(name_arg)) {
            name = name_arg->valuestring;
        }
    }
    snprintf(last_name, sizeof(last_name), "%s", name);
    char buf[128];
    snprintf(buf, sizeof(buf), "hello %s", name);
    return cJSON_CreateString(buf);
}

static cJSON *create_user_mutation(const cJSON *args, const cJSON *variables, void *ctx) {
    (void)variables;
    (void)ctx;
    mutation_count++;
    cJSON *res = cJSON_CreateObject();
    cJSON_AddNumberToObject(res, "id", 42);
    if (args) {
        cJSON *name = cJSON_GetObjectItemCaseSensitive(args, "name");
        if (name && cJSON_IsString(name)) {
            cJSON_AddStringToObject(res, "name", name->valuestring);
        }
    }
    return res;
}

int main(void) {
    cwist_graphql_schema_t *schema = cwist_graphql_schema_create();
    assert(schema);

    assert(cwist_graphql_add_query(schema, "greet", hello_resolver, NULL));
    assert(cwist_graphql_add_mutation(schema, "createUser", create_user_mutation, NULL));

    /* Test 1: Query with field argument */
    cwist_sstring *out = NULL;
    assert(cwist_graphql_execute(schema, "{\"query\":\"{ greet(name: \\\"alice\\\") }\"}", &out)
               .error.err_i16 == 0);
    assert(strstr(out->data, "\"greet\":\"hello alice\""));
    cwist_sstring_destroy(out);

    /* Test 2: Query with Field Alias */
    out = NULL;
    assert(
        cwist_graphql_execute(schema, "{\"query\":\"{ myAlias: greet }\"}", &out).error.err_i16 ==
        0);
    assert(strstr(out->data, "\"myAlias\":\"hello world\""));
    cwist_sstring_destroy(out);

    /* Test 3: Mutation execution */
    out = NULL;
    assert(cwist_graphql_execute(schema,
                                 "{\"query\":\"mutation { createUser(name: \\\"bob\\\") }\"}", &out)
               .error.err_i16 == 0);
    assert(strstr(out->data, "\"createUser\":{\"id\":42,\"name\":\"bob\"}"));
    cwist_sstring_destroy(out);

    /* Test 4: Variable replacement in arguments */
    out = NULL;
    assert(
        cwist_graphql_execute(
            schema,
            "{\"query\":\"{ greet(name: $userName) }\", \"variables\": {\"userName\": \"charlie\"}}",
            &out)
            .error.err_i16 == 0);
    assert(strstr(out->data, "\"greet\":\"hello charlie\""));
    cwist_sstring_destroy(out);

    /* Test 5: Unterminated string argument */
    out = NULL;
    assert(cwist_graphql_execute(schema, "{\"query\":\"{ greet(name: \\\"unterminated) }\"}", &out)
               .error.err_i16 == 0);
    assert(strstr(out->data, "Unterminated string in argument"));
    cwist_sstring_destroy(out);

    /* Test 6: Missing selection set brace */
    out = NULL;
    assert(cwist_graphql_execute(schema, "{\"query\":\"query Foo\"}", &out).error.err_i16 == 0);
    assert(strstr(out->data, "Expected '{' to begin selection set"));
    cwist_sstring_destroy(out);

    /* Invalid operations never invoke resolvers. */
    out = NULL;
    assert(cwist_graphql_execute(schema, "{\"query\":\"mutation { createUser(name: ) }\"}", &out)
               .error.err_i16 == 0);
    assert(strstr(out->data, "Unsupported argument value format"));
    assert(mutation_count == 1);
    cwist_sstring_destroy(out);

    out = NULL;
    assert(cwist_graphql_execute(schema, "{\"query\":\"mutation { missing createUser }\"}", &out)
               .error.err_i16 == 0);
    assert(strstr(out->data, "Cannot query field"));
    assert(mutation_count == 1);
    cwist_sstring_destroy(out);

    out = NULL;
    assert(cwist_graphql_execute(schema, "{\"query\":\"mutation { createUser } trailing\"}", &out)
               .error.err_i16 == 0);
    assert(strstr(out->data, "Unexpected trailing content"));
    assert(mutation_count == 1);
    cwist_sstring_destroy(out);

    out = NULL;
    assert(cwist_graphql_execute(schema, "{\"query\":\"mutation { createUser\"}", &out)
               .error.err_i16 == 0);
    assert(strstr(out->data, "Unterminated selection set"));
    assert(mutation_count == 1);
    cwist_sstring_destroy(out);

    out = NULL;
    assert(cwist_graphql_execute(
               schema, "{\"query\":\"mutation { createUser(name: \\\"bad\\\\q\\\") }\"}", &out)
               .error.err_i16 == 0);
    assert(strstr(out->data, "Invalid string escape sequence"));
    assert(mutation_count == 1);
    cwist_sstring_destroy(out);

    /* Names and argument names are not truncated. */
    char long_name[129];
    char long_name_b[130];
    memset(long_name, 'a', 128);
    memset(long_name_b, 'a', 128);
    long_name[128] = '\0';
    long_name_b[128] = 'b';
    long_name_b[129] = '\0';
    assert(cwist_graphql_add_query(schema, long_name, hello_resolver, NULL));
    assert(cwist_graphql_add_query(schema, long_name_b, hello_resolver, NULL));
    char query[1024];
    snprintf(query, sizeof(query), "{\"query\":\"{ %s %s }\"}", long_name, long_name_b);
    out = NULL;
    assert(cwist_graphql_execute(schema, query, &out).error.err_i16 == 0);
    assert(strstr(out->data, long_name));
    assert(strstr(out->data, long_name_b));
    cwist_sstring_destroy(out);

    snprintf(query, sizeof(query), "{\"query\":\"{ greet(%s: \\\"long argument\\\") }\"}", long_name);
    out = NULL;
    assert(cwist_graphql_execute(schema, query, &out).error.err_i16 == 0);
    assert(strcmp(last_name, "long argument") == 0);
    cwist_sstring_destroy(out);

    snprintf(query, sizeof(query), "subscription { %s }", long_name);
    char subscription_field[512];
    cJSON *subscription_args = NULL;
    assert(cwist_graphql_parse_subscription(query, NULL, subscription_field,
                                            sizeof(subscription_field), &subscription_args));
    assert(strcmp(subscription_field, long_name) == 0);
    assert(subscription_args == NULL);

    /* GraphQL string escapes produce their represented characters. */
    cJSON *request = cJSON_CreateObject();
    assert(request != NULL);
    cJSON_AddStringToObject(request, "query",
                            "{ greet(name: \"line\\nquote\\\" slash\\\\ snowman\\u2603 "
                            "music\\uD834\\uDD1E\") }");
    char *raw_request = cJSON_PrintUnformatted(request);
    assert(raw_request != NULL);
    out = NULL;
    assert(cwist_graphql_execute(schema, raw_request, &out).error.err_i16 == 0);
    assert(strcmp(last_name, "line\nquote\" slash\\ snowman\342\230\203 music\360\235\204\236") ==
           0);
    cwist_sstring_destroy(out);
    free(raw_request);
    cJSON_Delete(request);

    out = NULL;
    assert(
        cwist_graphql_execute(schema, "{\"query\":\"{ greet } trailing\"}", &out).error.err_i16 ==
        0);
    assert(strstr(out->data, "Unexpected trailing content"));
    assert(query_count == 7);
    cwist_sstring_destroy(out);

    cwist_graphql_schema_destroy(schema);
    puts("All GraphQL advanced tests passed successfully.");
    return 0;
}
