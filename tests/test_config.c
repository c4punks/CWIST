#define _POSIX_C_SOURCE 200809L
#include <cwist/sys/app/config.h>

#include <assert.h>
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

    cwist_config_destroy(cfg);
    assert(unlink(path) == 0);
    assert(rmdir(directory) == 0);
    puts("test_config: all passed");
    return 0;
}
