#include <cwist/core/html/builder.h>
#include <cwist/core/sstring/sstring.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>

void test_basic_element(void) {
    cwist_html_element_t *div = cwist_html_element_create("div");
    assert(div != NULL);
    cwist_html_element_add_attr(div, "id", "container");
    cwist_html_element_add_class(div, "main-box");
    cwist_html_element_set_text(div, "Hello World");

    cwist_sstring *rendered = cwist_html_render(div);
    assert(rendered != NULL && rendered->data != NULL);
    assert(strstr(rendered->data, "<div") != NULL);
    assert(strstr(rendered->data, "id=\"container\"") != NULL);
    assert(strstr(rendered->data, "class=\"main-box\"") != NULL);
    assert(strstr(rendered->data, ">Hello World</div>") != NULL);

    cwist_sstring_destroy(rendered);
    cwist_html_element_destroy(div);
    printf("Passed test_basic_element\n");
}

void test_attribute_escaping(void) {
    cwist_html_element_t *input = cwist_html_element_create("input");
    assert(input != NULL);
    /* Attribute value containing quotes and special chars */
    cwist_html_element_add_attr(input, "value", "attack\" onclick=\"evil()\" & 'more'");
    cwist_html_element_add_attr(input, "name", "safe_name");

    cwist_sstring *rendered = cwist_html_render(input);
    assert(rendered != NULL && rendered->data != NULL);
    /* Should NOT contain raw unescaped quotes inside value */
    assert(strstr(rendered->data, "attack&quot; onclick=&quot;evil()&quot; &amp; &#39;more&#39;") !=
           NULL);

    cwist_sstring_destroy(rendered);
    cwist_html_element_destroy(input);
    printf("Passed test_attribute_escaping\n");
}

void test_boolean_and_numeric_attributes(void) {
    cwist_html_element_t *btn = cwist_html_element_create("button");
    assert(btn != NULL);
    cJSON_AddTrueToObject(btn->attributes, "disabled");
    cJSON_AddNumberToObject(btn->attributes, "tabindex", 2);

    cwist_sstring *rendered = cwist_html_render(btn);
    assert(rendered != NULL && rendered->data != NULL);
    assert(strstr(rendered->data, " disabled") != NULL);
    assert(strstr(rendered->data, "tabindex=\"2\"") != NULL);

    cwist_sstring_destroy(rendered);
    cwist_html_element_destroy(btn);
    printf("Passed test_boolean_and_numeric_attributes\n");
}

void test_nested_children_and_null_safety(void) {
    /* Null safety */
    assert(cwist_html_render(NULL) == NULL);
    cwist_html_element_destroy(NULL);
    cwist_html_element_add_attr(NULL, "k", "v");
    cwist_html_element_set_id(NULL, "id");
    cwist_html_element_add_class(NULL, "cls");
    cwist_html_element_set_text(NULL, "txt");
    cwist_html_element_add_child(NULL, NULL);

    /* Nested tree */
    cwist_html_element_t *parent = cwist_html_element_create("ul");
    cwist_html_element_t *li1 = cwist_html_element_create("li");
    cwist_html_element_set_text(li1, "Item 1 & 2");
    cwist_html_element_t *li2 = cwist_html_element_create("li");
    cwist_html_element_set_text(li2, "Item 3");

    cwist_html_element_add_child(parent, li1);
    cwist_html_element_add_child(parent, li2);

    cwist_sstring *rendered = cwist_html_render(parent);
    assert(rendered != NULL && rendered->data != NULL);
    assert(strstr(rendered->data, "<li>Item 1 &amp; 2</li>") != NULL);
    assert(strstr(rendered->data, "<li>Item 3</li>") != NULL);
    assert(strstr(rendered->data, "</ul>") != NULL);

    cwist_sstring_destroy(rendered);
    cwist_html_element_destroy(parent);
    printf("Passed test_nested_children_and_null_safety\n");
}

void test_void_elements(void) {
    cwist_html_element_t *head = cwist_html_element_create("head");
    cwist_html_element_t *meta = cwist_html_element_create("meta");
    cwist_html_element_add_attr(meta, "charset", "utf-8");
    cwist_html_element_add_child(head, meta);
    cwist_html_element_t *link = cwist_html_element_create("LINK");
    cwist_html_element_add_attr(link, "rel", "stylesheet");
    /* Content given to a void element is not rendered (but still freed). */
    cwist_html_element_set_text(link, "ignored");
    cwist_html_element_add_child(link, cwist_html_element_create("span"));
    cwist_html_element_add_child(head, link);
    cwist_html_element_add_child(head, cwist_html_element_create("br"));
    /* Names that merely start or end like a void element are normal. */
    cwist_html_element_add_child(head, cwist_html_element_create("brx"));
    cwist_html_element_add_child(head, cwist_html_element_create("imgs"));
    cwist_html_element_add_child(head, cwist_html_element_create("b"));

    cwist_sstring *rendered = cwist_html_render(head);
    assert(rendered != NULL && rendered->data != NULL);
    assert(strcmp(rendered->data, "<head><meta charset=\"utf-8\"><LINK rel=\"stylesheet\"><br>"
                                  "<brx></brx><imgs></imgs><b></b></head>") == 0);
    cwist_sstring_destroy(rendered);
    cwist_html_element_destroy(head);
    printf("Passed test_void_elements\n");
}

int main(void) {
    test_basic_element();
    test_attribute_escaping();
    test_boolean_and_numeric_attributes();
    test_nested_children_and_null_safety();
    test_void_elements();
    printf("All HTML builder tests passed!\n");
    return 0;
}
