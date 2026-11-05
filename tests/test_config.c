#define _POSIX_C_SOURCE 200809L
#include <cwist/sys/app/config.h>

#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(void) {
    char directory[] = "/tmp/cwist-config-XXXXXX";
    assert(mkdtemp(directory) != NULL);
    char path[256];
    assert(snprintf(path, sizeof(path), "%s/app.properties", directory) > 0);

    FILE *file = fopen(path, "wb");
    assert(file != NULL);
    const char contents[] =
        "=invalid\n"
        "   =invalid-too\n"
        "key=\n"
        "   \n"
        "valid = \"quoted\" \n"
        "empty =   \n";
    assert(fwrite(contents, 1, sizeof(contents) - 1, file) == sizeof(contents) - 1);
    assert(fclose(file) == 0);

    cwist_config *cfg = cwist_config_create();
    assert(cfg != NULL);
    cwist_config_load_file(cfg, path);

    assert(cwist_config_get(cfg, "") == NULL);
    assert(cwist_config_get(cfg, "   ") == NULL);
    assert(strcmp(cwist_config_get(cfg, "key"), "") == 0);
    assert(strcmp(cwist_config_get(cfg, "valid"), "quoted") == 0);
    assert(strcmp(cwist_config_get(cfg, "empty"), "") == 0);

    char int_max[32];
    char int_min[32];
    char int_above_max[32];
    char int_below_min[32];
    assert(snprintf(int_max, sizeof(int_max), "%d", INT_MAX) > 0);
    assert(snprintf(int_min, sizeof(int_min), "%d", INT_MIN) > 0);
    assert(snprintf(int_above_max, sizeof(int_above_max), "%lld", (long long)INT_MAX + 1) > 0);
    assert(snprintf(int_below_min, sizeof(int_below_min), "%lld", (long long)INT_MIN - 1) > 0);

    cwist_config_set(cfg, "int.max", int_max);
    assert(cwist_config_get_int(cfg, "int.max", 0) == INT_MAX);
    cwist_config_set(cfg, "int.min", int_min);
    assert(cwist_config_get_int(cfg, "int.min", 0) == INT_MIN);
    cwist_config_set(cfg, "int.above-max", int_above_max);
    assert(cwist_config_get_int(cfg, "int.above-max", 7) == 7);
    cwist_config_set(cfg, "int.below-min", int_below_min);
    assert(cwist_config_get_int(cfg, "int.below-min", 7) == 7);
    cwist_config_set(cfg, "int.huge", "999999999999999999999999999999");
    assert(cwist_config_get_int(cfg, "int.huge", 7) == 7);
    cwist_config_set(cfg, "int.junk", "12x");
    assert(cwist_config_get_int(cfg, "int.junk", 7) == 7);
    cwist_config_set(cfg, "int.empty", "");
    assert(cwist_config_get_int(cfg, "int.empty", 7) == 7);

    char long_path[256];
    assert(snprintf(long_path, sizeof(long_path), "%s/long.properties", directory) > 0);
    file = fopen(long_path, "wb");
    assert(file != NULL);
    assert(fputs("exact=", file) >= 0);
    const size_t exact_value_len = 1023 - strlen("exact=");
    for (size_t i = 0; i < exact_value_len; i++) assert(fputc('x', file) != EOF);
    assert(fputs("\nlong=", file) >= 0);
    for (size_t i = 0; i < 2048; i++) assert(fputc('y', file) != EOF);
    assert(fputs("\nafter=loaded\n", file) >= 0);
    assert(fclose(file) == 0);

    cwist_config_load_file(cfg, long_path);
    const char *exact_value = cwist_config_get(cfg, "exact");
    assert(exact_value != NULL && strlen(exact_value) == exact_value_len);
    for (size_t i = 0; i < exact_value_len; i++) assert(exact_value[i] == 'x');
    const char *long_value = cwist_config_get(cfg, "long");
    assert(long_value != NULL && strlen(long_value) == 2048);
    for (size_t i = 0; i < 2048; i++) assert(long_value[i] == 'y');
    assert(strcmp(cwist_config_get(cfg, "after"), "loaded") == 0);

    cwist_config_destroy(cfg);
    assert(unlink(path) == 0);
    assert(unlink(long_path) == 0);
    assert(rmdir(directory) == 0);
    puts("test_config: all passed");
    return 0;
}
