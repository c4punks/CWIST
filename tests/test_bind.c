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
    const char *json = "{\"email\":\"not-an-email\",\"age\":30,\"name\":\"Alice\",\"score\":95.5}";
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
    const char *form = "email=carol@example.com&age=25&name=Carol&score=88";
    cwist_http_request *req = cwist_http_request_create();
    cwist_sstring_assign(req->body, (char *)form);

    user_t out;
    memset(&out, 0, sizeof(out));
    cwist_bind_result_t result;

    bool ok = cwist_app_req_bind_form(req, &user_schema, &out, &result);
    assert(ok == true);
    assert(strcmp(out.email, "carol@example.com") == 0);
    assert(out.age == 25);
    assert(strcmp(out.name, "Carol") == 0);

    cwist_http_request_destroy(req);
    printf("  OK\n");
}

void test_bind_regex(void) {
    printf("test_bind_regex...\n");
    typedef struct {
        char code[16];
    } code_t;
    CWIST_BIND_RULES(code_rules, CWIST_RULE_REQUIRED(), CWIST_RULE_REGEX("^[A-Z]{3}[0-9]{4}$"));
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

    const char *json_bad = "{\"code\":\"abc1234\"}";
    req = cwist_http_request_create();
    cwist_sstring_assign(req->body, (char *)json_bad);
    memset(&out, 0, sizeof(out));
    ok = cwist_app_req_bind_json(req, &code_schema, &out, &result);
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

int main(void) {
    test_bind_rejects_invalid_numbers_and_shapes();
    test_bind_success();
    test_bind_missing_required();
    test_bind_invalid_email();
    test_bind_age_out_of_range();
    test_bind_auto_400_response();
    test_bind_form_data();
    test_bind_regex();
    printf("All bind tests passed.\n");
    return 0;
}
