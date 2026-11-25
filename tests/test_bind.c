/**
 * @file test_bind.c
 * @brief Unit tests for declarative schema validation and binding.
 */

#include <cwist/core/validation/bind.h>
#include <cwist/net/http/http.h>
#include <cwist/core/sstring/sstring.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>

#ifdef CWIST_TEST_WRAP
static int fail_alloc;
void *__real_cwist_alloc(size_t size);
void *__wrap_cwist_alloc(size_t size) {
    if (fail_alloc > 0) {
        fail_alloc--;
        return NULL;
    }
    return __real_cwist_alloc(size);
}
#endif

typedef struct {
    char email[128];
    int age;
    char name[64];
    double score;
} user_t;

CWIST_BIND_RULES(email_rules, CWIST_RULE_REQUIRED(), CWIST_RULE_EMAIL());
CWIST_BIND_RULES(age_rules, CWIST_RULE_MIN_VAL(18), CWIST_RULE_MAX_VAL(120));
CWIST_BIND_RULES(name_rules, CWIST_RULE_REQUIRED(), CWIST_RULE_MIN_LEN(2), CWIST_RULE_MAX_LEN(50));
CWIST_BIND_RULES(score_rules, CWIST_RULE_MIN_VAL(0.0), CWIST_RULE_MAX_VAL(100.0));

static const cwist_bind_field_t user_fields[] = {
    CWIST_BIND_FIELD(user_t, email, "email", email_rules),
    CWIST_BIND_FIELD(user_t, age, "age", age_rules),
    CWIST_BIND_FIELD(user_t, name, "name", name_rules),
    CWIST_BIND_FIELD(user_t, score, "score", score_rules),
};

static const cwist_bind_schema_t user_schema = CWIST_BIND_SCHEMA(user_t, user_fields);

void test_bind_success(void) {
    printf("test_bind_success...\n");
    const char *json =
        "{\"email\":\"alice@example.com\",\"age\":30,\"name\":\"Alice\",\"score\":95.5}";
    cwist_http_request *req = cwist_http_request_create();
    cwist_sstring_assign(req->body, (char *)json);

    user_t out;
    memset(&out, 0, sizeof(out));
    cwist_bind_result_t result;

    bool ok = cwist_app_req_bind_json(req, &user_schema, &out, &result);
    assert(ok == true);
    assert(strcmp(out.email, "alice@example.com") == 0);
    assert(out.age == 30);
    assert(strcmp(out.name, "Alice") == 0);
    assert(out.score > 95.0 && out.score < 96.0);

    cwist_http_request_destroy(req);
    printf("  OK\n");
}

void test_bind_missing_required(void) {
    printf("test_bind_missing_required...\n");
    const char *json = "{\"age\":30,\"name\":\"Alice\",\"score\":95.5}";
    cwist_http_request *req = cwist_http_request_create();
    cwist_sstring_assign(req->body, (char *)json);

    user_t out;
    memset(&out, 0, sizeof(out));
    cwist_bind_result_t result;

    bool ok = cwist_app_req_bind_json(req, &user_schema, &out, &result);
    assert(ok == false);
    assert(result.error_count > 0);
    assert(strcmp(result.errors[0].key, "email") == 0);

    cwist_http_request_destroy(req);
    printf("  OK\n");
}

void test_bind_invalid_email(void) {
    printf("test_bind_invalid_email...\n");
    static const char *invalid[] = {
        "not-an-email", "a b@c.d", "x@@y.z",   "a@b..c",
        "a@-b.c",       ".a@b.c",  "a..b@c.d", "a@b.c\\r\\nBcc: victim@example.com",
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        char json[512];
        snprintf(json, sizeof(json),
                 "{\"email\":\"%s\",\"age\":30,\"name\":\"Alice\",\"score\":95.5}", invalid[i]);
        cwist_http_request *req = cwist_http_request_create();
        cwist_sstring_assign(req->body, json);
        user_t out = {0};
        cwist_bind_result_t result;
        bool ok = cwist_app_req_bind_json(req, &user_schema, &out, &result);
        assert(ok == false);
        assert(result.error_count > 0);
        cwist_http_request_destroy(req);
    }
    printf("  OK\n");
}

void test_bind_age_out_of_range(void) {
    printf("test_bind_age_out_of_range...\n");
    const char *json = "{\"email\":\"bob@example.com\",\"age\":5,\"name\":\"Bob\",\"score\":50}";
    cwist_http_request *req = cwist_http_request_create();
    cwist_sstring_assign(req->body, (char *)json);

    user_t out;
    memset(&out, 0, sizeof(out));
    cwist_bind_result_t result;

    bool ok = cwist_app_req_bind_json(req, &user_schema, &out, &result);
    assert(ok == false);
    assert(result.error_count > 0);

    cwist_http_request_destroy(req);
    printf("  OK\n");
}

void test_bind_auto_400_response(void) {
    printf("test_bind_auto_400_response...\n");
    const char *json = "{\"age\":5,\"name\":\"B\",\"score\":50}";
    cwist_http_request *req = cwist_http_request_create();
    cwist_http_response *res = cwist_http_response_create();
    cwist_sstring_assign(req->body, (char *)json);

    user_t out;
    memset(&out, 0, sizeof(out));

    bool ok = cwist_app_req_bind_json_or_400(req, res, &user_schema, &out);
    assert(ok == false);
    assert(res->status_code == CWIST_HTTP_BAD_REQUEST);
    assert(strstr(res->body->data, "success") != NULL);
    assert(strstr(res->body->data, "errors") != NULL);

    cwist_http_request_destroy(req);
    cwist_http_response_destroy(res);
    printf("  OK\n");
}

void test_bind_form_data(void) {
    printf("test_bind_form_data...\n");
    const char *form = "em%61il=carol%40example.com&age=25&name=Carol+Smith&score=88";
    cwist_http_request *req = cwist_http_request_create();
    cwist_sstring_assign(req->body, (char *)form);

    user_t out;
    memset(&out, 0, sizeof(out));
    cwist_bind_result_t result;

    bool ok = cwist_app_req_bind_form(req, &user_schema, &out, &result);
    assert(ok == true);
    assert(strcmp(out.email, "carol@example.com") == 0);
    assert(out.age == 25);
    assert(strcmp(out.name, "Carol Smith") == 0);

    cwist_http_request_destroy(req);
    printf("  OK\n");
}

void test_bind_regex(void) {
    printf("test_bind_regex...\n");
    typedef struct {
        char code[16];
    } code_t;
    CWIST_BIND_RULES(code_rules, CWIST_RULE_REQUIRED(), CWIST_RULE_REGEX("[A-Z]{3}[0-9]{4}"));
    static const cwist_bind_field_t code_fields[] = {
        CWIST_BIND_FIELD(code_t, code, "code", code_rules),
    };
    static const cwist_bind_schema_t code_schema = CWIST_BIND_SCHEMA(code_t, code_fields);

    const char *json_ok = "{\"code\":\"ABC1234\"}";
    cwist_http_request *req = cwist_http_request_create();
    cwist_sstring_assign(req->body, (char *)json_ok);
    code_t out;
    memset(&out, 0, sizeof(out));
    cwist_bind_result_t result;
    bool ok = cwist_app_req_bind_json(req, &code_schema, &out, &result);
    assert(ok == true);
    cwist_http_request_destroy(req);

    const char *json_bad = "{\"code\":\"xABC1234\"}";
    req = cwist_http_request_create();
    cwist_sstring_assign(req->body, (char *)json_bad);
    memset(&out, 0, sizeof(out));
    ok = cwist_app_req_bind_json(req, &code_schema, &out, &result);
    assert(ok == false);
    cwist_http_request_destroy(req);
    printf("  OK\n");
}

void test_bind_utf8_length(void) {
    printf("test_bind_utf8_length...\n");
    typedef struct {
        char name[64];
    } name_t;
    CWIST_BIND_RULES(utf8_rules, CWIST_RULE_MIN_LEN(2), CWIST_RULE_MAX_LEN(4));
    static const cwist_bind_field_t fields[] = {
        CWIST_BIND_FIELD(name_t, name, "name", utf8_rules),
    };
    static const cwist_bind_schema_t schema = CWIST_BIND_SCHEMA(name_t, fields);
    static const struct {
        const char *json;
        bool expected;
    } cases[] = {
        {"{\"name\":\"\uAC00\uB098\uB2E4\uB77C\"}", true},
        {"{\"name\":\"\uAC00\uB098\uB2E4\uB77C\uB9C8\"}", false},
        {"{\"name\":\"\uAC00\"}", false},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        cwist_http_request *req = cwist_http_request_create();
        cwist_sstring_assign(req->body, (char *)cases[i].json);
        name_t out = {0};
        cwist_bind_result_t result;
        bool ok = cwist_app_req_bind_json(req, &schema, &out, &result);
        assert(ok == cases[i].expected);
        cwist_http_request_destroy(req);
    }
    printf("  OK\n");
}

void test_bind_null_guards_and_numeric_validation(void) {
    printf("test_bind_null_guards_and_numeric_validation...\n");
    /* 1. Non-numeric value for numeric rule */
    const char *json_nan =
        "{\"email\":\"test@example.com\",\"age\":\"notanumber\",\"name\":\"Test\",\"score\":50.0}";
    cwist_http_request *req = cwist_http_request_create();
    cwist_sstring_assign(req->body, (char *)json_nan);
    user_t out;
    memset(&out, 0, sizeof(out));
    cwist_bind_result_t result;
    bool ok = cwist_app_req_bind_json(req, &user_schema, &out, &result);
    assert(ok == false);
    assert(result.error_count > 0);
    cwist_http_request_destroy(req);

    /* 2. NULL pattern regex rule safety */
    typedef struct {
        char val[16];
    } null_re_t;
    CWIST_BIND_RULES(null_re_rules, CWIST_RULE_REGEX(NULL));
    static const cwist_bind_field_t null_re_fields[] = {
        CWIST_BIND_FIELD(null_re_t, val, "val", null_re_rules),
    };
    static const cwist_bind_schema_t null_re_schema = CWIST_BIND_SCHEMA(null_re_t, null_re_fields);
    req = cwist_http_request_create();
    cwist_sstring_assign(req->body, "{\"val\":\"abc\"}");
    null_re_t out_re;
    memset(&out_re, 0, sizeof(out_re));
    ok = cwist_app_req_bind_json(req, &null_re_schema, &out_re, &result);
    assert(ok == false);
    cwist_http_request_destroy(req);

    /* 3. NULL custom callback rule safety */
    typedef struct {
        char val[16];
    } null_custom_t;
    CWIST_BIND_RULES(null_custom_rules, CWIST_RULE_CUSTOM(NULL, NULL));
    static const cwist_bind_field_t null_custom_fields[] = {
        CWIST_BIND_FIELD(null_custom_t, val, "val", null_custom_rules),
    };
    static const cwist_bind_schema_t null_custom_schema =
        CWIST_BIND_SCHEMA(null_custom_t, null_custom_fields);
    req = cwist_http_request_create();
    cwist_sstring_assign(req->body, "{\"val\":\"abc\"}");
    null_custom_t out_custom;
    memset(&out_custom, 0, sizeof(out_custom));
    ok = cwist_app_req_bind_json(req, &null_custom_schema, &out_custom, &result);
    assert(ok == false);
    cwist_http_request_destroy(req);

    printf("  OK\n");
}

typedef struct {
    unsigned char tiny;
    double score;
} numeric_target_t;

static void test_bind_rejects_invalid_numbers_and_shapes(void) {
    CWIST_BIND_RULES(score_rules, CWIST_RULE_MIN_VAL(0), CWIST_RULE_MAX_VAL(100));
    const cwist_bind_field_t fields[] = {
        {.json_key = "tiny",
         .target_type = CWIST_BIND_UINT,
         .target_offset = offsetof(numeric_target_t, tiny),
         .target_size = sizeof(unsigned char)},
        {.json_key = "score",
         .target_type = CWIST_BIND_DOUBLE,
         .target_offset = offsetof(numeric_target_t, score),
         .target_size = sizeof(double),
         .rules = score_rules},
    };
    const cwist_bind_schema_t schema = {fields, 2, sizeof(numeric_target_t)};
    const char *invalid[] = {
        "{\"tiny\":256,\"score\":50}",
        "{\"tiny\":-1,\"score\":50}",
        "{\"tiny\":1,\"score\":\"nan\"}",
        "{\"tiny\":1,\"score\":1e300}",
        "[1,2,3]",
        "{\"tiny\":1,\"score\":50} trailing",
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        cwist_http_request *req = cwist_http_request_create();
        cwist_sstring_assign(req->body, invalid[i]);
        numeric_target_t out = {0};
        cwist_bind_result_t result;
        assert(!cwist_app_req_bind_json(req, &schema, &out, &result));
        assert(result.error_count > 0);
        cwist_http_request_destroy(req);
    }

    cwist_http_request *req = cwist_http_request_create();
    char long_form[8192];
    memset(long_form, 'x', sizeof(long_form));
    memcpy(long_form, "score=", 6);
    memcpy(long_form + 4090, "&tiny=1", 8);
    cwist_sstring_assign_len(req->body, long_form, sizeof(long_form));
    numeric_target_t out = {0};
    cwist_bind_result_t result;
    assert(!cwist_app_req_bind_form(req, &schema, &out, &result));
    cwist_http_request_destroy(req);

    const cwist_bind_rule_t unsentinel[] = {CWIST_RULE_REQUIRED()};
    const cwist_bind_field_t sentinel_fields[] = {
        {.json_key = "tiny",
         .target_type = CWIST_BIND_UINT,
         .target_offset = offsetof(numeric_target_t, tiny),
         .target_size = sizeof(unsigned char),
         .rules = unsentinel,
         .rule_count = 1},
    };
    const cwist_bind_schema_t sentinel_schema = {sentinel_fields, 1, sizeof(numeric_target_t)};
    req = cwist_http_request_create();
    cwist_sstring_assign(req->body, "{\"tiny\":1}");
    assert(!cwist_app_req_bind_json(req, &sentinel_schema, &out, &result));
    cwist_http_request_destroy(req);
}

static void test_bind_schema_and_bare_form_keys(void) {
    typedef struct {
        char flag[8];
    } flag_t;
    const cwist_bind_field_t null_rules_fields[] = {
        {.json_key = "flag",
         .target_type = CWIST_BIND_STRING,
         .target_offset = offsetof(flag_t, flag),
         .target_size = sizeof(((flag_t *)0)->flag),
         .rules = NULL,
         .rule_count = 1},
    };
    const cwist_bind_schema_t null_rules_schema = {null_rules_fields, 1, sizeof(flag_t)};
    cwist_http_request *req = cwist_http_request_create();
    cwist_sstring_assign(req->body, "{\"flag\":\"x\"}");
    flag_t out = {0};
    cwist_bind_result_t result;
    assert(!cwist_app_req_bind_json(req, &null_rules_schema, &out, &result));
    cwist_http_request_destroy(req);

    const cwist_bind_field_t fields[] = {
        {.json_key = "flag",
         .target_type = CWIST_BIND_STRING,
         .target_offset = offsetof(flag_t, flag),
         .target_size = sizeof(((flag_t *)0)->flag)},
    };
    const cwist_bind_schema_t schema = {fields, 1, sizeof(flag_t)};
    req = cwist_http_request_create();
    cwist_sstring_assign(req->body, "flag&unused=1");
    assert(cwist_app_req_bind_form(req, &schema, &out, &result));
    assert(strcmp(out.flag, "") == 0);
    cwist_http_request_destroy(req);
}

static void test_bind_form_insertion_oom(void) {
    typedef struct {
        char value[16];
    } form_t;
    const cwist_bind_field_t fields[] = {
        {.json_key = "value",
         .target_type = CWIST_BIND_STRING,
         .target_offset = offsetof(form_t, value),
         .target_size = sizeof(((form_t *)0)->value)},
    };
    const cwist_bind_schema_t schema = {fields, 1, sizeof(form_t)};
    cwist_http_request *req = cwist_http_request_create();
    cwist_sstring_assign(req->body, "value=ok");
    form_t out = {0};
    cwist_bind_result_t result;
#ifdef CWIST_TEST_WRAP
    fail_alloc = 4;
    assert(!cwist_app_req_bind_form(req, &schema, &out, &result));
    fail_alloc = 0;
#endif
    cwist_http_request_destroy(req);

    req = cwist_http_request_create();
    cwist_sstring_assign(req->body, "value=first&value=second");
    memset(&out, 0, sizeof(out));
    assert(cwist_app_req_bind_form(req, &schema, &out, &result));
    assert(strcmp(out.value, "first") == 0);
    cwist_http_request_destroy(req);
}

static void test_bind_form_decoding_limits(void) {
    typedef struct {
        char value[8192];
    } long_form_t;
    const cwist_bind_field_t fields[] = {
        {.json_key = "value",
         .target_type = CWIST_BIND_STRING,
         .target_offset = offsetof(long_form_t, value),
         .target_size = sizeof(((long_form_t *)0)->value)},
    };
    const cwist_bind_schema_t schema = {fields, 1, sizeof(long_form_t)};
    char form[5010];
    memcpy(form, "value=", 6);
    memset(form + 6, 'x', 5000);
    form[5006] = '\0';
    cwist_http_request *req = cwist_http_request_create();
    cwist_sstring_assign(req->body, form);
    long_form_t out = {0};
    cwist_bind_result_t result;
    assert(cwist_app_req_bind_form(req, &schema, &out, &result));
    assert(strlen(out.value) == 5000);
    cwist_http_request_destroy(req);

    const char *invalid[] = {"value=%", "value=%0", "value=%GG", "value=%00"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        req = cwist_http_request_create();
        cwist_sstring_assign(req->body, invalid[i]);
        assert(!cwist_app_req_bind_form(req, &schema, &out, &result));
        assert(result.error_count > 0);
        cwist_http_request_destroy(req);
    }
}

static void test_bind_defensive_bounds(void) {
    printf("test_bind_defensive_bounds...\n");
    typedef struct {
        int x;
    } small_t;

    /* 1. NULL key field */
    cwist_bind_field_t bad_fields[] = {
        {.json_key = NULL,
         .target_type = CWIST_BIND_INT,
         .target_offset = 0,
         .target_size = sizeof(int)},
    };
    cwist_bind_schema_t bad_schema = {bad_fields, 1, sizeof(small_t)};
    small_t out = {0};
    cwist_bind_result_t result;
    cwist_http_request *req = cwist_http_request_create();
    cwist_sstring_assign(req->body, "{\"x\":1}");

    bool ok = cwist_app_req_bind_json(req, &bad_schema, &out, &result);
    assert(!ok);
    assert(!result.ok);

    /* 2. OOB offset */
    cwist_bind_field_t oob_fields[] = {
        {.json_key = "x",
         .target_type = CWIST_BIND_INT,
         .target_offset = 100,
         .target_size = sizeof(int)},
    };
    cwist_bind_schema_t oob_schema = {oob_fields, 1, sizeof(small_t)};
    ok = cwist_app_req_bind_json(req, &oob_schema, &out, &result);
    assert(!ok);
    assert(!result.ok);

    cwist_http_request_destroy(req);
    printf("  OK\n");
}

static void test_bind_query(void) {
    printf("test_bind_query...\n");
    cwist_http_request *req = cwist_http_request_create();
    cwist_sstring_assign(req->query, "email=alice%40example.com&age=30&name=Alice&score=95.5");

    user_t out = {0};
    cwist_bind_result_t result;
    bool ok = cwist_app_req_bind_query(req, &user_schema, &out, &result);
    assert(ok);
    assert(strcmp(out.email, "alice@example.com") == 0);
    assert(out.age == 30);
    assert(strcmp(out.name, "Alice") == 0);
    assert(out.score > 95.0 && out.score < 96.0);

    /* Test query_or_400 */
    cwist_http_response *res = cwist_http_response_create();
    cwist_sstring_assign(req->query, "email=invalid-email&age=5");
    ok = cwist_app_req_bind_query_or_400(req, res, &user_schema, &out);
    assert(!ok);
    assert(res->status_code == CWIST_HTTP_BAD_REQUEST);
    assert(strstr(res->body->data, "\"success\":false") != NULL);

    cwist_http_response_destroy(res);
    cwist_http_request_destroy(req);
    printf("  OK\n");
}

static void test_bind_nul_and_precision(void) {
    const char *nul_json =
        "{\"email\":\"alice@example.com\",\"age\":30,\"name\":\"Al\\u0000ice\",\"score\":95.5}";
    cwist_http_request *req = cwist_http_request_create();
    cwist_sstring_assign(req->body, (char *)nul_json);
    user_t out = {0};
    cwist_bind_result_t result;
    assert(!cwist_app_req_bind_json(req, &user_schema, &out, &result));
    cwist_http_request_destroy(req);

    const char *literal_json =
        "{\"email\":\"alice@example.com\",\"age\":30,\"name\":\"Al\\\\u0000\",\"score\":95.5}";
    req = cwist_http_request_create();
    cwist_sstring_assign(req->body, (char *)literal_json);
    assert(cwist_app_req_bind_json(req, &user_schema, &out, &result));
    cwist_http_request_destroy(req);

    const char *rounded_json =
        "{\"email\":\"alice@example.com\",\"age\":9007199254740992,\"name\":\"Alice\",\"score\":95.5}";
    req = cwist_http_request_create();
    cwist_sstring_assign(req->body, (char *)rounded_json);
    assert(!cwist_app_req_bind_json(req, &user_schema, &out, &result));
    cwist_http_request_destroy(req);
}

int main(void) {
    test_bind_nul_and_precision();
    test_bind_rejects_invalid_numbers_and_shapes();
    test_bind_success();
    test_bind_missing_required();
    test_bind_invalid_email();
    test_bind_age_out_of_range();
    test_bind_auto_400_response();
    test_bind_form_data();
    test_bind_schema_and_bare_form_keys();
    test_bind_form_insertion_oom();
    test_bind_form_decoding_limits();
    test_bind_regex();
    test_bind_utf8_length();
    test_bind_null_guards_and_numeric_validation();
    test_bind_defensive_bounds();
    test_bind_query();
    printf("All bind tests passed.\n");
    return 0;
}
