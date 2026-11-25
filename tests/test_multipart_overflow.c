#include <assert.h>
#include <stdint.h>

#define cwist_multipart_parse test_multipart_parse_public
#define cwist_multipart_result_destroy test_multipart_result_destroy_public
#define cwist_multipart_extract_boundary test_multipart_extract_boundary_public
#include "../src/net/http/multipart.c"

int main(void) {
    multipart_parser_settings settings = {
        .on_header_field = mp_on_header_field,
        .on_header_value = mp_on_header_value,
        .on_headers_complete = mp_on_headers_complete,
        .on_part_data_begin = mp_on_part_data_begin,
        .on_part_data = mp_on_part_data,
        .on_part_data_end = mp_on_part_data_end,
        .on_body_end = mp_on_body_end,
    };
    multipart_parser *parser = multipart_parser_init("--x", &settings);
    assert(parser != NULL);
    mp_parse_ctx ctx = {0};
    multipart_parser_set_data(parser, &ctx);

    const char byte = 'x';
    ctx.data_len = SIZE_MAX - 1;
    assert(mp_on_part_data(parser, &byte, 2) == 1);
    ctx.data_len = SIZE_MAX - 2;
    assert(mp_on_part_data(parser, &byte, 2) == 1);
    ctx.data_len = SIZE_MAX / 2;
    assert(mp_on_part_data(parser, &byte, SIZE_MAX / 2 + 1) == 1);

    multipart_parser_free(parser);
    puts("multipart growth overflow is rejected: ok");
    return 0;
}
