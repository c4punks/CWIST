#define _POSIX_C_SOURCE 200809L
#include <cwist/core/template/template.h>

#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void write_text(const char *path, const char *text) {
    FILE *file = fopen(path, "wb");
    assert(file != NULL);
    assert(fwrite(text, 1, strlen(text), file) == strlen(text));
    assert(fclose(file) == 0);
}

int main(void) {
    cJSON *context = cJSON_CreateObject();
    assert(context != NULL);
    assert(cJSON_AddStringToObject(context, "name", "world") != NULL);

    cwist_sstring *rendered = cwist_template_render("Hello {{ name }}", context);
    assert(rendered != NULL && strcmp(rendered->data, "Hello world") == 0);
    cwist_sstring_destroy(rendered);

    char directory[] = "/tmp/cwist-template-XXXXXX";
    assert(mkdtemp(directory) != NULL);
    char partial[PATH_MAX];
    char first[PATH_MAX];
    char second[PATH_MAX];
    assert(snprintf(partial, sizeof(partial), "%s/partial.html", directory) > 0);
    assert(snprintf(first, sizeof(first), "%s/first.html", directory) > 0);
    assert(snprintf(second, sizeof(second), "%s/second.html", directory) > 0);

    write_text(partial, "Included {{ name }}");
    rendered = cwist_template_render_file(partial, context);
    assert(rendered != NULL && strcmp(rendered->data, "Included world") == 0);
    cwist_sstring_destroy(rendered);

    char include_partial[PATH_MAX + 32];
    assert(snprintf(include_partial, sizeof(include_partial), "{%% include \"%s\" %%}", partial) >
           0);
    rendered = cwist_template_render(include_partial, context);
    assert(rendered != NULL && strcmp(rendered->data, "Included world") == 0);
    cwist_sstring_destroy(rendered);

    char recursive[PATH_MAX + 32];
    assert(snprintf(recursive, sizeof(recursive), "{%% include \"%s\" %%}", first) > 0);
    write_text(first, recursive);
    rendered = cwist_template_render_file(first, context);
    assert(rendered == NULL);

    char include_first[PATH_MAX + 32];
    char include_second[PATH_MAX + 32];
    assert(snprintf(include_first, sizeof(include_first), "{%% include \"%s\" %%}", first) > 0);
    assert(snprintf(include_second, sizeof(include_second), "{%% include \"%s\" %%}", second) > 0);
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
