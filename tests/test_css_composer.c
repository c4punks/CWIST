#include <cwist/core/html/css_composer.h>
#include <cwist/core/sstring/sstring.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>

void test_hex_parsing_6digit(void) {
    cwist_color_rgb white = cwist_color_hex_to_rgb("#FFFFFF");
    assert(white.r == 255 && white.g == 255 && white.b == 255);

    cwist_color_rgb black = cwist_color_hex_to_rgb("000000");
    assert(black.r == 0 && black.g == 0 && black.b == 0);

    cwist_color_rgb custom = cwist_color_hex_to_rgb("#3B82F6");
    assert(custom.r == 0x3B && custom.g == 0x82 && custom.b == 0xF6);

    char hex_out[8];
    cwist_color_rgb_to_hex(custom, hex_out);
    assert(strcasecmp(hex_out, "#3B82F6") == 0);
    printf("Passed test_hex_parsing_6digit\n");
}

void test_hex_parsing_3digit(void) {
    /* 3-digit hex shorthand: #fff -> #ffffff */
    cwist_color_rgb white = cwist_color_hex_to_rgb("#fff");
    assert(white.r == 255 && white.g == 255 && white.b == 255);

    /* 3-digit without '#' prefix */
    cwist_color_rgb red = cwist_color_hex_to_rgb("f00");
    assert(red.r == 255 && red.g == 0 && red.b == 0);

    cwist_color_rgb custom = cwist_color_hex_to_rgb("#123");
    assert(custom.r == 0x11 && custom.g == 0x22 && custom.b == 0x33);

    char hex_out[8];
    cwist_color_rgb_to_hex(custom, hex_out);
    assert(strcasecmp(hex_out, "#112233") == 0);
    printf("Passed test_hex_parsing_3digit\n");
}

void test_hex_parsing_invalid(void) {
    /* NULL pointer */
    cwist_color_rgb c = cwist_color_hex_to_rgb(NULL);
    assert(c.r == 0 && c.g == 0 && c.b == 0);

    /* Empty string */
    c = cwist_color_hex_to_rgb("");
    assert(c.r == 0 && c.g == 0 && c.b == 0);

    /* Invalid lengths */
    c = cwist_color_hex_to_rgb("#12");
    assert(c.r == 0 && c.g == 0 && c.b == 0);

    c = cwist_color_hex_to_rgb("#12345");
    assert(c.r == 0 && c.g == 0 && c.b == 0);

    /* Invalid hex characters */
    c = cwist_color_hex_to_rgb("#12zz45");
    assert(c.r == 0 && c.g == 0 && c.b == 0);

    c = cwist_color_hex_to_rgb("#xyz");
    assert(c.r == 0 && c.g == 0 && c.b == 0);
    printf("Passed test_hex_parsing_invalid\n");
}

void test_css_generation(void) {
    cwist_css_config cfg;
    cwist_css_config_init(&cfg);
    assert(cfg.primary_color.r == 0x3B && cfg.primary_color.g == 0x82 &&
           cfg.primary_color.b == 0xF6);

    cwist_sstring *stylesheet = cwist_css_generate_stylesheet(&cfg);
    assert(stylesheet != NULL && stylesheet->data != NULL);
    assert(strstr(stylesheet->data, ":root") != NULL);
    assert(strstr(stylesheet->data, "--color-primary:") != NULL);
    assert(strstr(stylesheet->data, ".btn") != NULL);

    cwist_sstring_destroy(stylesheet);
    printf("Passed test_css_generation\n");
}

void test_scope_class_names(void) {
    cwist_css_scope a, b, other, empty;
    cwist_css_scope_init(&a, "card");
    cwist_css_scope_init(&b, "card");
    cwist_css_scope_init(&other, "nav");
    cwist_css_scope_init(&empty, NULL);

    /* Unseeded FNV-1a: fixed across runs and processes. */
    assert(strcmp(a.suffix, "8827595f") == 0);
    assert(strcmp(other.suffix, "178eedf2") == 0);
    assert(strcmp(empty.suffix, "811c9dc5") == 0);

    const char *btn = cwist_css_scope_class(&a, "btn");
    assert(btn != NULL && strcmp(btn, "btn-8827595f") == 0);
    assert(cwist_css_scope_class(&a, "btn") == btn);
    assert(strcmp(cwist_css_scope_class(&b, "btn"), btn) == 0);
    assert(strcmp(cwist_css_scope_class(&other, "btn"), "btn-178eedf2") == 0);
    assert(strcmp(cwist_css_scope_class(&a, "_x-1"), "_x-1-8827595f") == 0);
    assert(strcmp(cwist_css_scope_class(&a, "-y"), "-y-8827595f") == 0);

    /* Enough classes to force the entry array to grow; earlier pointers stay valid. */
    char name[16];
    for (int i = 0; i < 40; i++) {
        snprintf(name, sizeof(name), "c%d", i);
        assert(cwist_css_scope_class(&a, name) != NULL);
    }
    assert(strcmp(btn, "btn-8827595f") == 0);
    assert(cwist_css_scope_class(&a, "btn") == btn);

    const char *bad[] = {"", "1abc", "-1", "--", "a b", "a{", "a.b", "a:hover", "\xc3\xa9"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        assert(cwist_css_scope_class(&a, bad[i]) == NULL);
    }
    assert(cwist_css_scope_class(&a, NULL) == NULL);
    assert(cwist_css_scope_class(NULL, "btn") == NULL);

    cwist_css_scope_destroy(&a);
    cwist_css_scope_destroy(&b);
    cwist_css_scope_destroy(&other);
    cwist_css_scope_destroy(&empty);
    printf("Passed test_scope_class_names\n");
}

void test_scope_stylesheet(void) {
    cwist_css_scope scope;
    cwist_css_scope_init(&scope, "card");

    /* Rules alone are not emitted until their class is used. */
    assert(cwist_css_scope_add_rule(&scope, "title", "font-weight: 700;") == 0);
    assert(cwist_css_scope_add_rule(&scope, "unused", "color: red;") == 0);
    assert(cwist_css_scope_add_rule(&scope, "body", "margin: 0;") == 0);
    cwist_sstring *css = cwist_css_scope_generate_stylesheet(&scope);
    assert(css != NULL && strcmp(css->data, "") == 0);
    cwist_sstring_destroy(css);

    /* A used class with no rule emits nothing either. */
    assert(cwist_css_scope_class(&scope, "plain") != NULL);
    assert(cwist_css_scope_class(&scope, "body") != NULL);
    assert(cwist_css_scope_class(&scope, "title") != NULL);

    /* Later declarations replace earlier ones; order follows first sighting. */
    assert(cwist_css_scope_add_rule(&scope, "body", "margin: 0 auto;") == 0);
    css = cwist_css_scope_generate_stylesheet(&scope);
    assert(strcmp(css->data, ".title-8827595f { font-weight: 700; }\n"
                             ".body-8827595f { margin: 0 auto; }\n") == 0);
    cwist_sstring_destroy(css);

    /* Declarations that could close the rule or the <style> element are rejected. */
    assert(cwist_css_scope_add_rule(&scope, "title", "x: 1; } body { y: 2") == -1);
    assert(cwist_css_scope_add_rule(&scope, "title", "x: 1; { y: 2") == -1);
    assert(cwist_css_scope_add_rule(&scope, "title", "x: 1;</style><script>") == -1);
    assert(cwist_css_scope_add_rule(&scope, "bad name", "x: 1;") == -1);
    assert(cwist_css_scope_add_rule(&scope, "title", NULL) == -1);
    assert(cwist_css_scope_add_rule(NULL, "title", "x: 1;") == -1);
    css = cwist_css_scope_generate_stylesheet(&scope);
    assert(strstr(css->data, ".title-8827595f { font-weight: 700; }\n") != NULL);
    cwist_sstring_destroy(css);

    assert(cwist_css_scope_generate_stylesheet(NULL) == NULL);

    /* Destroy resets the scope; a destroyed or zeroed scope refuses new classes. */
    cwist_css_scope_destroy(&scope);
    cwist_css_scope_destroy(&scope);
    assert(scope.count == 0 && scope.entries == NULL);
    assert(cwist_css_scope_class(&scope, "btn") == NULL);
    assert(cwist_css_scope_add_rule(&scope, "btn", "x: 1;") == -1);
    cwist_css_scope_destroy(NULL);
    printf("Passed test_scope_stylesheet\n");
}

int main(void) {
    test_hex_parsing_6digit();
    test_hex_parsing_3digit();
    test_hex_parsing_invalid();
    test_css_generation();
    test_scope_class_names();
    test_scope_stylesheet();
    printf("All CSS composer tests passed!\n");
    return 0;
}
