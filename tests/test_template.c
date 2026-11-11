#define _POSIX_C_SOURCE 200809L
#include <cwist/core/template/template.h>

#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void write_text(const char *path, const char *text) {
    FILE *file = fopen(path, "wb");
    assert(file != NULL);
    assert(fwrite(text, 1, strlen(text), file) == strlen(text));
    assert(fclose(file) == 0);
}

static void assert_error(const cwist_template_error_t *error, cwist_template_error_code_t code,
                         size_t offset, const char *path, const char *tag) {
    assert(error->code == code);
    assert(error->byte_offset == offset);
    assert(strcmp(error->file_path, path) == 0);
    if (tag) {
        assert(error->tag_type != NULL && strcmp(error->tag_type, tag) == 0);
    } else {
        assert(error->tag_type == NULL);
    }
}

static void test_string_diagnostics(const cJSON *context) {
    const struct {
        const char *text;
        cwist_template_error_code_t code;
        size_t offset;
        const char *tag;
    } cases[] = {
        {NULL, CWIST_TEMPLATE_ERROR_INVALID_ARGUMENT, 0, NULL},
        {"Hello {{ name", CWIST_TEMPLATE_ERROR_UNCLOSED_TAG, 6, "variable"},
        {"\xc3\xa9{{ name", CWIST_TEMPLATE_ERROR_UNCLOSED_TAG, 2, "variable"},
        {"Hello {% if name", CWIST_TEMPLATE_ERROR_UNCLOSED_TAG, 6, "if"},
        {"Hello {% for item", CWIST_TEMPLATE_ERROR_UNCLOSED_TAG, 6, "for"},
        {"Hello {% include", CWIST_TEMPLATE_ERROR_UNCLOSED_TAG, 6, "include"},
        {"Hello {% unknown", CWIST_TEMPLATE_ERROR_UNCLOSED_TAG, 6, "control"},
        {"Hello {% if name %}yes", CWIST_TEMPLATE_ERROR_UNCLOSED_BLOCK, 6, "if"},
        {"Hello {% for item in items %}yes", CWIST_TEMPLATE_ERROR_UNCLOSED_BLOCK, 6, "for"},
        {"{% if name %}{% endif", CWIST_TEMPLATE_ERROR_UNCLOSED_TAG, 13, "control"},
        {"{% for x in items %}{% endfor", CWIST_TEMPLATE_ERROR_UNCLOSED_TAG, 20, "control"},
        {"{% if name %}{{ bad{% endif %}", CWIST_TEMPLATE_ERROR_UNCLOSED_TAG, 13, "variable"},
        {"{% if absent %}{{ bad{% endif %}", CWIST_TEMPLATE_ERROR_UNCLOSED_TAG, 15, "variable"},
        {"{% for x in absent %}{{ bad{% endfor %}", CWIST_TEMPLATE_ERROR_UNCLOSED_TAG, 21,
         "variable"},
    };
    cwist_template_error_t error;
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        assert(cwist_template_render_ex(cases[i].text, context, &error) == NULL);
        assert_error(&error, cases[i].code, cases[i].offset, "", cases[i].tag);
        assert(cwist_template_render(cases[i].text, context) == NULL);
    }

    memset(&error, 0xff, sizeof(error));
    cwist_sstring *rendered = cwist_template_render_ex("Hello {{ name }}", context, &error);
    assert(rendered != NULL && strcmp(rendered->data, "Hello world") == 0);
    assert_error(&error, CWIST_TEMPLATE_ERROR_NONE, 0, "", NULL);
    cwist_sstring_destroy(rendered);
    rendered = cwist_template_render_ex("plain text", NULL, NULL);
    assert(rendered != NULL && strcmp(rendered->data, "plain text") == 0);
    cwist_sstring_destroy(rendered);

    const struct {
        const char *text;
        const char *expected;
    } blocks[] = {
        {"{% if name %}yes{# {% endif %} {% if missing %} #}{% else %}no{% endif %}", "yes"},
        {"{% if absent %}no{% else %}{% if name %}yes{% endif %}{% endif %}", "yes"},
        {"{% for x in items %}{{ x }}{# {% endfor %} #}{% endfor %}", "1"},
        {"{% for x in items %}{% for y in items %}{{ y }}{% endfor %}{% endfor %}", "1"},
    };
    for (size_t i = 0; i < sizeof(blocks) / sizeof(blocks[0]); i++) {
        rendered = cwist_template_render_ex(blocks[i].text, context, &error);
        assert(rendered != NULL && strcmp(rendered->data, blocks[i].expected) == 0);
        assert_error(&error, CWIST_TEMPLATE_ERROR_NONE, 0, "", NULL);
        cwist_sstring_destroy(rendered);
    }

    /* The 33rd nested block fails at its opening, not at the start of its body. */
    char nested[2048] = "";
    for (int i = 0; i < 32; i++) strcat(nested, "{% if name %}");
    size_t opening = strlen(nested);
    strcat(nested, "{{ name }}");
    for (int i = 0; i < 32; i++) strcat(nested, "{% endif %}");
    rendered = cwist_template_render_ex(nested, context, &error);
    assert(rendered != NULL && strcmp(rendered->data, "world") == 0);
    cwist_sstring_destroy(rendered);
    memmove(nested + opening + 13, nested + opening, strlen(nested + opening) + 1);
    memcpy(nested + opening, "{% if name %}", 13);
    strcat(nested, "{% endif %}");
    assert(cwist_template_render_ex(nested, context, &error) == NULL);
    assert_error(&error, CWIST_TEMPLATE_ERROR_DEPTH, opening, "", "if");
}

static void join_path(char *out, size_t out_len, const char *dir, const char *name) {
    int n = snprintf(out, out_len, "%s/%s", dir, name);
    assert(n > 0 && (size_t)n < out_len);
}

static void assert_rendered(cwist_sstring *rendered, const char *expected) {
    assert(rendered != NULL && strcmp(rendered->data, expected) == 0);
    cwist_sstring_destroy(rendered);
}

static void test_include_root(const cJSON *context) {
    char base[] = "/tmp/cwist-template-root-XXXXXX";
    assert(mkdtemp(base) != NULL);

    char root[PATH_MAX], partials[PATH_MAX], outside[PATH_MAX], evil[PATH_MAX];
    join_path(root, sizeof(root), base, "root");
    join_path(partials, sizeof(partials), root, "partials");
    join_path(outside, sizeof(outside), base, "outside");
    join_path(evil, sizeof(evil), base, "root-evil");
    assert(mkdir(root, 0700) == 0);
    assert(mkdir(partials, 0700) == 0);
    assert(mkdir(outside, 0700) == 0);
    assert(mkdir(evil, 0700) == 0);

    char nav[PATH_MAX], up[PATH_MAX], page[PATH_MAX], escape[PATH_MAX], secret[PATH_MAX];
    char evil_file[PATH_MAX], abs_in[PATH_MAX], abs_out[PATH_MAX], link[PATH_MAX];
    char via_link[PATH_MAX], nested[PATH_MAX], sibling[PATH_MAX], missing[PATH_MAX];
    join_path(nav, sizeof(nav), partials, "nav.html");
    join_path(up, sizeof(up), partials, "up.html");
    join_path(page, sizeof(page), root, "page.html");
    join_path(escape, sizeof(escape), root, "escape.html");
    join_path(secret, sizeof(secret), outside, "secret.html");
    join_path(evil_file, sizeof(evil_file), evil, "x.html");
    join_path(abs_in, sizeof(abs_in), root, "abs_in.html");
    join_path(abs_out, sizeof(abs_out), root, "abs_out.html");
    join_path(link, sizeof(link), root, "link.html");
    join_path(via_link, sizeof(via_link), root, "via_link.html");
    join_path(nested, sizeof(nested), root, "nested.html");
    join_path(sibling, sizeof(sibling), root, "sibling.html");
    join_path(missing, sizeof(missing), root, "missing.html");

    char text[PATH_MAX + 64];
    write_text(nav, "N{{ name }}");
    write_text(secret, "SECRET");
    write_text(evil_file, "EVIL");
    write_text(page, "P[{% include \"partials/nav.html\" %}]");
    write_text(escape, "x{% include \"../outside/secret.html\" %}");
    write_text(up, "u{% include \"../../outside/secret.html\" %}");
    write_text(nested, "{% include \"partials/up.html\" %}");
    write_text(sibling, "{% include \"../root-evil/x.html\" %}");
    write_text(missing, "{% include \"partials/none.html\" %}");
    assert(snprintf(text, sizeof(text), "{%% include \"%s\" %%}", nav) > 0);
    write_text(abs_in, text);
    assert(snprintf(text, sizeof(text), "{%% include \"%s\" %%}", secret) > 0);
    write_text(abs_out, text);
    assert(symlink(secret, link) == 0);
    write_text(via_link, "{% include \"link.html\" %}");

    cwist_template_error_t error;

    /* Includes inside the root render as usual. */
    assert_rendered(cwist_template_render_file_rooted(page, context, root, &error), "P[Nworld]");
    assert_error(&error, CWIST_TEMPLATE_ERROR_NONE, 0, "", NULL);
    assert_rendered(cwist_template_render_file_rooted(abs_in, context, root, &error), "Nworld");

    /* "..", absolute paths, symlinks and prefix-sharing siblings cannot leave it. */
    assert(cwist_template_render_file_rooted(escape, context, root, &error) == NULL);
    assert_error(&error, CWIST_TEMPLATE_ERROR_FORBIDDEN, 1, escape, "include");
    assert(cwist_template_render_file_rooted(abs_out, context, root, &error) == NULL);
    assert_error(&error, CWIST_TEMPLATE_ERROR_FORBIDDEN, 0, abs_out, "include");
    assert(cwist_template_render_file_rooted(via_link, context, root, &error) == NULL);
    assert_error(&error, CWIST_TEMPLATE_ERROR_FORBIDDEN, 0, via_link, "include");
    assert(cwist_template_render_file_rooted(sibling, context, root, &error) == NULL);
    assert_error(&error, CWIST_TEMPLATE_ERROR_FORBIDDEN, 0, sibling, "include");

    /* An escape deep in the include chain names the file that tried it. */
    char up_as_included[PATH_MAX];
    join_path(up_as_included, sizeof(up_as_included), root, "partials/up.html");
    assert(cwist_template_render_file_rooted(nested, context, root, &error) == NULL);
    assert_error(&error, CWIST_TEMPLATE_ERROR_FORBIDDEN, 1, up_as_included, "include");

    /* A missing file inside the root is still an I/O error. */
    assert(cwist_template_render_file_rooted(missing, context, root, &error) == NULL);
    assert_error(&error, CWIST_TEMPLATE_ERROR_IO, 0, missing, "include");

    /* In-memory templates resolve relative includes against the root. */
    assert_rendered(
        cwist_template_render_rooted("{% include \"partials/nav.html\" %}", context, root, &error),
        "Nworld");
    assert(cwist_template_render_rooted("ab{% include \"../outside/secret.html\" %}", context, root,
                                        &error) == NULL);
    assert_error(&error, CWIST_TEMPLATE_ERROR_FORBIDDEN, 2, "", "include");

    /* The root must exist. */
    char no_root[PATH_MAX];
    join_path(no_root, sizeof(no_root), base, "absent");
    assert(cwist_template_render_rooted("x", context, no_root, &error) == NULL);
    assert_error(&error, CWIST_TEMPLATE_ERROR_INVALID_ARGUMENT, 0, "", NULL);
    assert(cwist_template_render_file_rooted(page, context, NULL, &error) == NULL);
    assert_error(&error, CWIST_TEMPLATE_ERROR_INVALID_ARGUMENT, 0, "", NULL);

    /* Without a root, behavior is unchanged: the escape is followed. */
    assert_rendered(cwist_template_render_file_ex(escape, context, &error), "xSECRET");
    assert_rendered(cwist_template_render_file(via_link, context), "SECRET");

    const char *files[] = {nav,     up,   page,     escape, secret,  evil_file, abs_in,
                           abs_out, link, via_link, nested, sibling, missing};
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) assert(unlink(files[i]) == 0);
    assert(rmdir(partials) == 0);
    assert(rmdir(root) == 0);
    assert(rmdir(outside) == 0);
    assert(rmdir(evil) == 0);
    assert(rmdir(base) == 0);
}

int main(void) {
    cJSON *context = cJSON_CreateObject();
    assert(context != NULL);
    assert(cJSON_AddStringToObject(context, "name", "world") != NULL);
    assert(cJSON_AddItemToObject(context, "items", cJSON_CreateArray()));
    assert(cJSON_AddItemToArray(cJSON_GetObjectItem(context, "items"), cJSON_CreateNumber(1)));
    test_string_diagnostics(context);
    test_include_root(context);

    cwist_sstring *rendered = cwist_template_render("Hello {{ name }}", context);
    assert(rendered != NULL && strcmp(rendered->data, "Hello world") == 0);
    cwist_sstring_destroy(rendered);

    assert(cwist_template_render("Hello {{ name", context) == NULL);
    assert(cwist_template_render("Hello {% if name", context) == NULL);

    rendered = cwist_template_render("{{ }}", context);
    assert(rendered != NULL && (!rendered->data || strcmp(rendered->data, "") == 0));
    cwist_sstring_destroy(rendered);

    rendered = cwist_template_render("{{ name| }}", context);
    assert(rendered != NULL && strcmp(rendered->data, "world") == 0);
    cwist_sstring_destroy(rendered);

    rendered = cwist_template_render("{{ |upper }}", context);
    assert(rendered != NULL && (!rendered->data || strcmp(rendered->data, "") == 0));
    cwist_sstring_destroy(rendered);

    char directory[] = "/tmp/cwist-template-XXXXXX";
    assert(mkdtemp(directory) != NULL);
    char partial[PATH_MAX];
    char first[PATH_MAX];
    char second[PATH_MAX];
    char missing[PATH_MAX];
    assert(snprintf(partial, sizeof(partial), "%s/partial.html", directory) > 0);
    assert(snprintf(first, sizeof(first), "%s/first.html", directory) > 0);
    assert(snprintf(second, sizeof(second), "%s/second.html", directory) > 0);
    assert(snprintf(missing, sizeof(missing), "%s/missing.html", directory) > 0);

    write_text(partial, "Included {{ name }}");
    rendered = cwist_template_render_file(partial, context);
    assert(rendered != NULL && strcmp(rendered->data, "Included world") == 0);
    cwist_sstring_destroy(rendered);

    char include_partial[PATH_MAX + 32];
    assert(snprintf(include_partial, sizeof(include_partial), "{%% include \"%s\" %%}", partial) >
           0);
    char include_first[PATH_MAX + 32];
    char include_second[PATH_MAX + 32];
    assert(snprintf(include_first, sizeof(include_first), "{%% include \"%s\" %%}", first) > 0);
    assert(snprintf(include_second, sizeof(include_second), "{%% include \"%s\" %%}", second) > 0);
    rendered = cwist_template_render(include_partial, context);
    assert(rendered != NULL && strcmp(rendered->data, "Included world") == 0);
    cwist_sstring_destroy(rendered);

    char recursive[PATH_MAX + 32];
    assert(snprintf(recursive, sizeof(recursive), "{%% include \"%s\" %%}", first) > 0);
    write_text(first, recursive);
    rendered = cwist_template_render_file(first, context);
    assert(rendered == NULL);

    cwist_template_error_t error;
    assert(cwist_template_render_file_ex(first, context, &error) == NULL);
    assert_error(&error, CWIST_TEMPLATE_ERROR_DEPTH, 0, first, "include");

    /* The innermost path is copied before its include buffers are freed. */
    write_text(first, include_second);
    write_text(second, include_partial);
    write_text(partial, "child: {{ name");
    assert(cwist_template_render_file_ex(first, context, &error) == NULL);
    assert_error(&error, CWIST_TEMPLATE_ERROR_UNCLOSED_TAG, 7, partial, "variable");
    assert(cwist_template_render_ex(include_first, context, &error) == NULL);
    assert_error(&error, CWIST_TEMPLATE_ERROR_UNCLOSED_TAG, 7, partial, "variable");
    write_text(partial, "child: {% if name %}missing terminator");
    assert(cwist_template_render_file_ex(first, context, &error) == NULL);
    assert_error(&error, CWIST_TEMPLATE_ERROR_UNCLOSED_BLOCK, 7, partial, "if");

    /* Include read failures identify the including source and its tag offset. */
    char include_missing[PATH_MAX + 64];
    assert(snprintf(include_missing, sizeof(include_missing), "child: {%% include \"%s\" %%}",
                    missing) > 0);
    write_text(second, include_missing);
    assert(cwist_template_render_file_ex(first, context, &error) == NULL);
    assert_error(&error, CWIST_TEMPLATE_ERROR_IO, 7, second, "include");
    assert(cwist_template_render_ex(include_missing, context, &error) == NULL);
    assert_error(&error, CWIST_TEMPLATE_ERROR_IO, 7, "", "include");
    assert(cwist_template_render_file_ex(missing, context, &error) == NULL);
    assert_error(&error, CWIST_TEMPLATE_ERROR_IO, 0, missing, NULL);
    assert(cwist_template_render_file_ex(NULL, context, &error) == NULL);
    assert_error(&error, CWIST_TEMPLATE_ERROR_INVALID_ARGUMENT, 0, "", NULL);

    /* Every selected control branch must propagate a failed include. */
    const char *blocks[] = {
        "{%% if name %%}%s{%% endif %%}",
        "{%% if absent %%}unused{%% else %%}%s{%% endif %%}",
        "{%% for x in items %%}%s{%% endfor %%}",
    };
    for (size_t i = 0; i < sizeof(blocks) / sizeof(blocks[0]); i++) {
        char block[PATH_MAX + 256];
        assert(snprintf(block, sizeof(block), blocks[i], include_missing) > 0);
        const char *tag = strstr(block, "{% include");
        assert(tag != NULL);
        assert(cwist_template_render_ex(block, context, &error) == NULL);
        assert_error(&error, CWIST_TEMPLATE_ERROR_IO, (size_t)(tag - block), "", "include");
        assert(cwist_template_render(block, context) == NULL);
    }

    write_text(partial, "Included {{ name }}");
    rendered = cwist_template_render_file_ex(partial, context, &error);
    assert(rendered != NULL && strcmp(rendered->data, "Included world") == 0);
    assert_error(&error, CWIST_TEMPLATE_ERROR_NONE, 0, "", NULL);
    cwist_sstring_destroy(rendered);
    rendered = cwist_template_render_file_ex(partial, context, NULL);
    assert(rendered != NULL && strcmp(rendered->data, "Included world") == 0);
    cwist_sstring_destroy(rendered);

    write_text(first, include_second);
    write_text(second, include_first);
    rendered = cwist_template_render_file(first, context);
    assert(rendered == NULL);

    cJSON_Delete(context);
    assert(unlink(partial) == 0);
    assert(unlink(first) == 0);
    assert(unlink(second) == 0);
    assert(rmdir(directory) == 0);
    puts("test_template: all passed");
    return 0;
}
