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

static cJSON *secret_user_resolver(const cJSON *args, const cJSON *variables, void *ctx) {
    (void)args;
    (void)variables;
    (void)ctx;
    cJSON *u = cJSON_CreateObject();
    cJSON_AddNumberToObject(u, "id", 7);
    cJSON_AddStringToObject(u, "password_hash", "SECRET");
    cJSON *friends = cJSON_CreateArray();
    for (int i = 0; i < 2; i++) {
        cJSON *f = cJSON_CreateObject();
        cJSON_AddNumberToObject(f, "id", 100 + i);
        cJSON_AddStringToObject(f, "password_hash", "FRIEND-SECRET");
        cJSON_AddItemToArray(friends, f);
    }
    cJSON_AddItemToArray(friends, cJSON_CreateNull());
    cJSON_AddItemToObject(u, "friends", friends);
    cJSON_AddNullToObject(u, "manager");
    return u;
}

static int root_id_calls;
static cJSON *root_id_resolver(const cJSON *args, const cJSON *variables, void *ctx) {
    (void)args;
    (void)variables;
    (void)ctx;
    root_id_calls++;
    return cJSON_CreateString("ROOT-ID");
}

static cJSON *echo_n_resolver(const cJSON *args, const cJSON *variables, void *ctx) {
    (void)variables;
    (void)ctx;
    cJSON *n = args ? cJSON_GetObjectItemCaseSensitive(args, "n") : NULL;
    return n ? cJSON_Duplicate(n, true) : cJSON_CreateNull();
}

/* Run @p query (a GraphQL document, JSON-escaped by the caller) and return
 * the response text; the caller frees it with cwist_sstring_destroy(). */
static cwist_sstring *run_query(cwist_graphql_schema_t *schema, const char *query) {
    cJSON *request = cJSON_CreateObject();
    assert(request != NULL);
    cJSON_AddStringToObject(request, "query", query);
    char *raw = cJSON_PrintUnformatted(request);
    assert(raw != NULL);
    cwist_sstring *out = NULL;
    assert(cwist_graphql_execute(schema, raw, &out).error.err_i16 == 0);
    free(raw);
    cJSON_Delete(request);
    assert(out != NULL);
    return out;
}

/* #953, #954: nested selections return only the selected fields, read from
 * the parent value, never from a same-named root resolver. */
static void test_nested_selection(void) {
    cwist_graphql_schema_t *schema = cwist_graphql_schema_create();
    assert(cwist_graphql_add_query(schema, "user", secret_user_resolver, NULL));
    assert(cwist_graphql_add_query(schema, "id", root_id_resolver, NULL));

    cwist_sstring *out = run_query(schema, "{ user { id } }");
    assert(strcmp(out->data, "{\"data\":{\"user\":{\"id\":7}}}") == 0);
    assert(root_id_calls == 0);
    cwist_sstring_destroy(out);

    out = run_query(schema, "{ user { uid: id friends { id } manager { id } } }");
    assert(strcmp(out->data, "{\"data\":{\"user\":{\"uid\":7,\"friends\":[{\"id\":100},"
                             "{\"id\":101},null],\"manager\":null}}}") == 0);
    assert(!strstr(out->data, "SECRET"));
    cwist_sstring_destroy(out);

    /* Root fields still resolve at the root. */
    out = run_query(schema, "{ id }");
    assert(strstr(out->data, "\"id\":\"ROOT-ID\""));
    assert(root_id_calls == 1);
    cwist_sstring_destroy(out);
    cwist_graphql_schema_destroy(schema);
}

/* #965: number arguments follow the GraphQL IntValue/FloatValue grammar. */
static void test_number_arguments(void) {
    cwist_graphql_schema_t *schema = cwist_graphql_schema_create();
    assert(cwist_graphql_add_query(schema, "echo", echo_n_resolver, NULL));
    static const struct {
        const char *arg;
        const char *expect;
    } ok[] = {{"0", "\"echo\":0"},         {"-12", "\"echo\":-12"},   {"1.5", "\"echo\":1.5"},
              {"-0.25e2", "\"echo\":-25"}, {"1E+3", "\"echo\":1000"}, {"2e-1", "\"echo\":0.2"}};
    for (size_t i = 0; i < sizeof(ok) / sizeof(ok[0]); i++) {
        char q[64];
        snprintf(q, sizeof(q), "{ echo(n: %s) }", ok[i].arg);
        cwist_sstring *out = run_query(schema, q);
        assert(strstr(out->data, ok[i].expect));
        assert(!strstr(out->data, "errors"));
        cwist_sstring_destroy(out);
    }
    static const char *const bad[] = {"0x1F", "012", "1e999", "-",     "1.",
                                      ".5",   "1e",  "-inf",  "1.2.3", "1abc"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        char q[64];
        snprintf(q, sizeof(q), "{ echo(n: %s) }", bad[i]);
        cwist_sstring *out = run_query(schema, q);
        assert(strstr(out->data, "\"errors\""));
        assert(!strstr(out->data, "\"echo\":"));
        cwist_sstring_destroy(out);
    }
    cwist_graphql_schema_destroy(schema);
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

    snprintf(query, sizeof(query), "{\"query\":\"{ greet(%s: \\\"long argument\\\") }\"}",
             long_name);
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

    out = NULL;
    assert(cwist_graphql_execute(schema, "{\"query\":\"{ greet greet }\"}", &out).error.err_i16 ==
           0);
    const char *first = strstr(out->data, "\"greet\":");
    assert(first != NULL && strstr(first + 1, "\"greet\":") == NULL);
    cwist_sstring_destroy(out);

    out = NULL;
    assert(cwist_graphql_execute(
               schema, "{\"query\":\"{ greet(name: \\\"}\\\") { child } other: greet }\"}", &out)
               .error.err_i16 == 0);
    assert(strstr(out->data, "Selection set cannot be applied to a scalar value"));
    assert(strstr(out->data, "\"other\":\"hello world\""));
    cwist_sstring_destroy(out);

    out = NULL;
    assert(
        cwist_graphql_execute(
            schema, "{\"query\":\"mutation { createUser(name: \\\"bob\\\") { id name } }\"}", &out)
            .error.err_i16 == 0);
    assert(strstr(out->data, "\"id\":42"));
    assert(strstr(out->data, "\"name\":\"bob\""));
    cwist_sstring_destroy(out);

    out = NULL;
    assert(
        cwist_graphql_execute(
            schema,
            "{\"query\":\"mutation { createUser(name: \\\"bob\\\") { id } createUser(name: \\\"bob\\\") { name } }\"}",
            &out)
            .error.err_i16 == 0);
    assert(strstr(out->data, "\"id\":42"));
    assert(strstr(out->data, "\"name\":\"bob\""));
    const char *create = strstr(out->data, "\"createUser\":");
    assert(create != NULL && strstr(create + 1, "\"createUser\":") == NULL);
    cwist_sstring_destroy(out);

    out = NULL;
    assert(cwist_graphql_execute(schema, "{\"query\":\"{ greet }\\u0000 trailing\"}", &out)
               .error.err_i16 != 0);
    assert(out == NULL);

    cwist_graphql_schema_destroy(schema);
    test_nested_selection();
    test_number_arguments();
    puts("All GraphQL advanced tests passed successfully.");
    return 0;
}
