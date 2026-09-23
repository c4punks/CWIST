#include <cwist/core/sstring/sstring.h>
#include <cwist/core/mem/alloc.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <stdlib.h>

void test_trim() {
    printf("Testing trim...\n");
    cwist_sstring *s = cwist_sstring_create();
    assert(s != NULL);

    cwist_sstring_assign(s, "   hello world   ");
    assert(strcmp(s->data, "   hello world   ") == 0);
    assert(cwist_sstring_get_size(s) == 17);

    cwist_sstring_trim(s);
    printf("Trimmed: '%s'\n", s->data);
    assert(strcmp(s->data, "hello world") == 0);
    assert(s->size == 11);
    assert(cwist_sstring_get_size(s) == 11);

    /* Test rtrim alone updates size */
    cwist_sstring_assign(s, "abc   ");
    assert(cwist_sstring_get_size(s) == 6);
    cwist_sstring_rtrim(s);
    assert(strcmp(s->data, "abc") == 0);
    assert(s->size == 3);
    assert(cwist_sstring_get_size(s) == 3);

    /* Test whitespace-only string */
    cwist_sstring_assign(s, "    ");
    cwist_sstring_trim(s);
    assert(strcmp(s->data, "") == 0);
    assert(s->size == 0);
    assert(cwist_sstring_get_size(s) == 0);

    /* Test trimming borrowed buffer safely detaches without mutating borrowed memory */
    const char *orig = "   borrowed text   ";
    char borrowed_copy[32];
    strcpy(borrowed_copy, orig);
    cwist_sstring_borrow(s, borrowed_copy, strlen(borrowed_copy));
    assert(s->borrows_buffer == true);

    cwist_sstring_trim(s);
    assert(strcmp(s->data, "borrowed text") == 0);
    assert(s->size == 13);
    assert(cwist_sstring_get_size(s) == 13);
    assert(s->borrows_buffer == false); /* safely detached */
    assert(strcmp(borrowed_copy, orig) == 0); /* original borrowed source remains intact */

    cwist_sstring_destroy(s);
    printf("Passed trim.\n");
}

void test_resize() {
    printf("Testing resize...\n");
    cwist_sstring *s = cwist_sstring_create();
    cwist_sstring_assign(s, "12345");
    assert(s->size == 5);

    // Grow
    cwist_error_t err = cwist_sstring_change_size(s, 10, false);
    assert(err.errtype == CWIST_ERR_INT8); // Success
    assert(s->size == 5);
    assert(strcmp(s->data, "12345") == 0);

    // Shrink safely
    err = cwist_sstring_change_size(s, 5, false); // "12345" fits in 5
    assert(err.errtype == CWIST_ERR_INT8);

    // Shrink with data loss warning
    err = cwist_sstring_change_size(s, 2, false); // "12345" -> 2 bytes?
    assert(err.errtype == CWIST_ERR_JSON); // Should fail
    cwist_error_dispose(&err);

    // Shrink with blow_data
    err = cwist_sstring_change_size(s, 2, true);
    assert(err.errtype == CWIST_ERR_INT8);

    assert(strcmp(s->data, "12") == 0);

    cwist_sstring_destroy(s);
    printf("Passed resize.\n");
}

void test_seek() {
    printf("Testing seek...\n");
    cwist_sstring *s = cwist_sstring_create();
    cwist_sstring_assign(s, "abcdef");

    char buffer[10];
    cwist_sstring_seek(s, buffer, 2);
    assert(strcmp(buffer, "cdef") == 0);

    cwist_sstring_destroy(s);
    printf("Passed seek.\n");
}

void test_compare() {
    printf("Testing compare...\n");
    cwist_sstring *s = cwist_sstring_create();
    cwist_sstring_assign(s, "hello");

    assert(cwist_sstring_compare(s, "hello") == 0);
    assert(cwist_sstring_compare(s, "world") != 0);
    assert(cwist_sstring_compare(s, "he") > 0);
    assert(cwist_sstring_compare(s, "hello world") < 0);

    cwist_sstring_destroy(s);
    printf("Passed compare.\n");
}

void test_substr() {
    printf("Testing substr...\n");
    cwist_sstring *s = cwist_sstring_create();
    cwist_sstring_assign(s, "0123456789");

    cwist_sstring *sub = cwist_sstring_substr(s, 2, 3); // "234"
    assert(sub != NULL);
    assert(strcmp(sub->data, "234") == 0);
    cwist_sstring_destroy(sub);

    sub = cwist_sstring_substr(s, 8, 5); // "89" (capped)
    assert(sub != NULL);
    assert(strcmp(sub->data, "89") == 0);
    cwist_sstring_destroy(sub);

    sub = cwist_sstring_substr(s, 10, 1); // Out of bounds
    assert(sub == NULL);

    cwist_sstring_destroy(s);
    printf("Passed substr.\n");
}

void test_sstring_ops() {
    printf("Testing sstring-to-sstring ops...\n");
    cwist_sstring left;
    cwist_sstring right;

    cwist_sstring_init(&left);
    cwist_sstring_init(&right);

    cwist_sstring_assign(&left, "hello");
    cwist_sstring_assign(&right, " world");

    cwist_error_t err = left.append(&left, &right);
    assert(err.error.err_i8 == ERR_SSTRING_OKAY);
    assert(strcmp(left.data, "hello world") == 0);

    err = right.copy(&right, &left);
    assert(err.error.err_i8 == ERR_SSTRING_OKAY);
    assert(strcmp(right.data, "hello world") == 0);

    assert(left.compare(&left, &right) == 0);

    cwist_free(left.data);
    cwist_free(right.data);
    printf("Passed sstring-to-sstring ops.\n");
}

void test_html_escape() {
    printf("Testing html escape...\n");
    cwist_sstring *s = cwist_sstring_create();
    assert(s != NULL);

    cwist_error_t err =
        cwist_sstring_append_escaped(s, "<div class=\"alert\">Bob & Alice's test > 0</div>");
    assert(err.errtype == CWIST_ERR_INT8 && err.error.err_i8 == ERR_SSTRING_OKAY);
    assert(
        strcmp(
            s->data,
            "&lt;div class=&quot;alert&quot;&gt;Bob &amp; Alice&#39;s test &gt; 0&lt;/div&gt;") ==
        0);
    assert(
        cwist_sstring_get_size(s) ==
        strlen("&lt;div class=&quot;alert&quot;&gt;Bob &amp; Alice&#39;s test &gt; 0&lt;/div&gt;"));

    /* Test NULL string and NULL data safety */
    err = cwist_sstring_append_escaped(NULL, "test");
    assert(err.errtype == CWIST_ERR_INT8 && err.error.err_i8 == ERR_SSTRING_NULL_STRING);

    err = cwist_sstring_append_escaped(s, NULL);
    assert(err.errtype == CWIST_ERR_INT8 && err.error.err_i8 == ERR_SSTRING_OKAY);

    /* Test standalone > character */
    cwist_sstring *s2 = cwist_sstring_create();
    cwist_sstring_append_escaped(s2, ">");
    assert(strcmp(s2->data, "&gt;") == 0);
    assert(cwist_sstring_get_size(s2) == 4);

    cwist_sstring_destroy(s);
    cwist_sstring_destroy(s2);
    printf("Passed html escape.\n");
}

void test_growth() {
    printf("Testing geometric growth...\n");
    cwist_sstring *s = cwist_sstring_create();
    assert(s != NULL);

    /* Small appends keep amortized O(n): capacity must double, not exact-fit. */
    cwist_error_t err = cwist_sstring_append(s, "abc");
    assert(err.error.err_i8 == ERR_SSTRING_OKAY);
    assert(s->size == 3);
    size_t first_cap = s->capacity;
    assert(first_cap >= 3);

    err = cwist_sstring_append(s, "def");
    assert(err.error.err_i8 == ERR_SSTRING_OKAY);
    assert(strcmp(s->data, "abcdef") == 0);
    assert(s->capacity >= first_cap); /* no shrink on append */

    /* Chained appends must stay within the doubled buffer until it fills. */
    char *before = s->data;
    err = cwist_sstring_append(s, "ghi");
    assert(err.error.err_i8 == ERR_SSTRING_OKAY);
    assert(s->data == before); /* 9 bytes fit the initial capacity */
    assert(strcmp(s->data, "abcdefghi") == 0);

    /* Many small appends produce the exact concatenation. */
    cwist_sstring *acc = cwist_sstring_create();
    for (int i = 0; i < 1000; i++) {
        err = cwist_sstring_append_len(acc, "x", 1);
        assert(err.error.err_i8 == ERR_SSTRING_OKAY);
    }
    assert(acc->size == 1000);
    assert(acc->capacity >= 1000);
    assert(acc->data[999] == 'x' && acc->data[1000] == '\0');

    /* Large single append grows past the current capacity in one realloc. */
    char *big = malloc(5000);
    memset(big, 'y', 5000);
    err = cwist_sstring_append_len(acc, big, 5000);
    assert(err.error.err_i8 == ERR_SSTRING_OKAY);
    assert(acc->size == 6000);
    assert(acc->data[999] == 'x' && acc->data[1000] == 'y' && acc->data[5999] == 'y');
    free(big);

    /* Assign replaces contents and the NUL stays in bounds. */
    err = cwist_sstring_assign(acc, "short");
    assert(err.error.err_i8 == ERR_SSTRING_OKAY);
    assert(strcmp(acc->data, "short") == 0);

    /* Borrowed buffers detach with geometric headroom on first mutation. */
    cwist_sstring_borrow(acc, "borrowed", 8);
    assert(acc->borrows_buffer == true);
    assert(acc->capacity == 0);
    err = cwist_sstring_append(acc, "!");
    assert(err.error.err_i8 == ERR_SSTRING_OKAY);
    assert(strcmp(acc->data, "borrowed!") == 0);
    assert(acc->borrows_buffer == false);
    assert(acc->capacity >= 9);

    cwist_sstring_destroy(s);
    cwist_sstring_destroy(acc);
    printf("Passed geometric growth.\n");
}

void test_adopt_region() {
    printf("Testing adopt_region...\n");
    char *buf = cwist_alloc(64);
    assert(buf != NULL);
    memcpy(buf, "HEADERpayload", 13);
    buf[13] = '\0';

    cwist_sstring *s = cwist_sstring_create();
    assert(s != NULL);
    cwist_sstring_adopt_region(s, buf, 6, 7);
    assert(strcmp(s->data, "payload") == 0);
    assert(s->size == 7);

    /* Growth must realloc the base and keep viewing the same region. */
    for (int i = 0; i < 100; i++) {
        cwist_error_t err = cwist_sstring_append_len(s, "0123456789", 10);
        assert(err.error.err_i8 == ERR_SSTRING_OKAY);
    }
    assert(s->size == 1007);
    assert(strncmp(s->data, "payload", 7) == 0);
    assert(s->data[1006] == '9' && s->data[1007] == '\0');

    /* Reassign releases the base exactly once. */
    cwist_error_t err = cwist_sstring_assign(s, "done");
    assert(err.error.err_i8 == ERR_SSTRING_OKAY);
    assert(strcmp(s->data, "done") == 0);

    cwist_sstring_destroy(s);
    printf("Passed adopt_region.\n");
}

int main() {
    test_trim();
    test_resize();
    test_seek();
    test_compare();
    test_substr();
    test_sstring_ops();
    test_html_escape();
    test_growth();
    test_adopt_region();
    printf("All tests passed!\n");
    return 0;
}
