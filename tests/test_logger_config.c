/**
 * @file test_logger_config.c
 * @brief Unit tests for cwist_logger (sys/app/logger.h) and cwist_config
 * (sys/app/config.h).
 *
 * Logger output is captured by pointing stdout/stderr at temp files, so the
 * test checks the exact stream, prefix and message each entry point writes.
 */
#define _POSIX_C_SOURCE 200809L
#include <cwist/sys/app/config.h>
#include <cwist/sys/app/logger.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* --- stream capture ------------------------------------------------------ */

typedef struct {
    int saved_out, saved_err;
    FILE *out, *err;
} capture_t;

static void capture_begin(capture_t *c) {
    fflush(stdout);
    fflush(stderr);
    c->out = tmpfile();
    c->err = tmpfile();
    assert(c->out && c->err);
    c->saved_out = dup(STDOUT_FILENO);
    c->saved_err = dup(STDERR_FILENO);
    assert(dup2(fileno(c->out), STDOUT_FILENO) >= 0);
    assert(dup2(fileno(c->err), STDERR_FILENO) >= 0);
}

static char *slurp(FILE *f) {
    fflush(f);
    long n = ftell(f);
    assert(n >= 0);
    char *buf = calloc((size_t)n + 1, 1);
    assert(buf);
    rewind(f);
    assert(fread(buf, 1, (size_t)n, f) == (size_t)n);
    fclose(f);
    return buf;
}

/* Restores the streams; returns what was written to stdout and stderr. */
static void capture_end(capture_t *c, char **out, char **err) {
    fflush(stdout);
    fflush(stderr);
    assert(dup2(c->saved_out, STDOUT_FILENO) >= 0);
    assert(dup2(c->saved_err, STDERR_FILENO) >= 0);
    close(c->saved_out);
    close(c->saved_err);
    *out = slurp(c->out);
    *err = slurp(c->err);
}

static int count_lines(const char *s) {
    int n = 0;
    for (; *s; s++) n += *s == '\n';
    return n;
}

/* --- logger -------------------------------------------------------------- */

static void test_logger_create_defaults(void) {
    cwist_logger *log = cwist_logger_create("svc");
    assert(log);
    assert(strcmp(log->name, "svc") == 0);
    assert(log->level == CWIST_LOG_INFO);
    assert(log->use_colors);
    cwist_logger_destroy(log);

    log = cwist_logger_create(NULL);
    assert(log && strcmp(log->name, "cwist") == 0);
    cwist_logger_destroy(log);
    cwist_logger_destroy(NULL);
}

static void test_logger_streams_and_format(void) {
    cwist_logger *log = cwist_logger_create("svc");
    cwist_logger_set_colors(log, false);
    cwist_logger_set_level(log, CWIST_LOG_DEBUG);

    capture_t c;
    char *out, *err;
    capture_begin(&c);
    cwist_logger_debug(log, "d=%d", 1);
    cwist_logger_info(log, "i=%s", "two");
    cwist_logger_warn(log, "w=%u", 3u);
    cwist_logger_error(log, "e=%c", '4');
    cwist_logger_log(log, CWIST_LOG_INFO, "l=%d", 5);
    cwist_logger_log(log, CWIST_LOG_ERROR, "L=%d", 6);
    capture_end(&c, &out, &err);

    /* DEBUG and INFO go to stdout, WARN and ERROR to stderr. */
    assert(count_lines(out) == 3);
    assert(count_lines(err) == 3);
    /* cwist_logger_debug() used to print an empty message. */
    assert(strstr(out, "[DEBUG] svc: d=1\n"));
    assert(strstr(out, "[INFO] svc: i=two\n"));
    assert(strstr(out, "[INFO] svc: l=5\n"));
    assert(strstr(err, "[WARN] svc: w=3\n"));
    assert(strstr(err, "[ERROR] svc: e=4\n"));
    assert(strstr(err, "[ERROR] svc: L=6\n"));
    /* Plain output: no escape codes, line starts with the timestamp. */
    assert(!strchr(out, '\033') && !strchr(err, '\033'));
    assert(out[0] == '[' && out[5] == '-' && out[8] == '-');
    free(out);
    free(err);
    cwist_logger_destroy(log);
}

static void test_logger_level_filter(void) {
    cwist_logger *log = cwist_logger_create("f");
    cwist_logger_set_colors(log, false);
    cwist_logger_set_level(log, CWIST_LOG_WARN);

    capture_t c;
    char *out, *err;
    capture_begin(&c);
    cwist_logger_debug(log, "no");
    cwist_logger_info(log, "no");
    cwist_logger_log(log, CWIST_LOG_INFO, "no");
    cwist_logger_warn(log, "yes1");
    cwist_logger_log(log, CWIST_LOG_ERROR, "yes2");
    /* NULL logger is ignored by every entry point. */
    cwist_logger_debug(NULL, "x");
    cwist_logger_info(NULL, "x");
    cwist_logger_warn(NULL, "x");
    cwist_logger_error(NULL, "x");
    cwist_logger_log(NULL, CWIST_LOG_ERROR, "x");
    cwist_logger_set_level(NULL, CWIST_LOG_DEBUG);
    cwist_logger_set_colors(NULL, true);
    capture_end(&c, &out, &err);

    assert(out[0] == '\0');
    assert(count_lines(err) == 2);
    assert(strstr(err, "yes1") && strstr(err, "yes2"));
    free(out);
    free(err);
    cwist_logger_destroy(log);
}

static void test_logger_colors(void) {
    cwist_logger *log = cwist_logger_create("c");
    capture_t c;
    char *out, *err;
    capture_begin(&c);
    cwist_logger_info(log, "colored");
    cwist_logger_error(log, "red");
    capture_end(&c, &out, &err);
    assert(strncmp(out, "\033[32m[", 6) == 0);
    assert(strstr(out, "\033[0m [INFO] c: colored\n"));
    assert(strncmp(err, "\033[31m[", 6) == 0);
    free(out);
    free(err);
    cwist_logger_destroy(log);
}

/* --- config -------------------------------------------------------------- */

static void test_config_set_get(void) {
    cwist_config *cfg = cwist_config_create();
    assert(cfg);
    assert(cwist_config_get(cfg, "missing") == NULL);
    cwist_config_set(cfg, "a", "1");
    cwist_config_set(cfg, "b", NULL); /* NULL value is stored as "" */
    assert(strcmp(cwist_config_get(cfg, "a"), "1") == 0);
    assert(strcmp(cwist_config_get(cfg, "b"), "") == 0);
    cwist_config_set(cfg, "a", "overwritten");
    assert(strcmp(cwist_config_get(cfg, "a"), "overwritten") == 0);

    /* Many keys share the 64 buckets: chains must keep every entry. */
    char key[32], val[32];
    for (int i = 0; i < 500; i++) {
        snprintf(key, sizeof(key), "k%d", i);
        snprintf(val, sizeof(val), "v%d", i);
        cwist_config_set(cfg, key, val);
    }
    for (int i = 0; i < 500; i++) {
        snprintf(key, sizeof(key), "k%d", i);
        snprintf(val, sizeof(val), "v%d", i);
        assert(strcmp(cwist_config_get(cfg, key), val) == 0);
    }

    /* NULL arguments are ignored. */
    cwist_config_set(NULL, "a", "b");
    cwist_config_set(cfg, NULL, "b");
    assert(cwist_config_get(NULL, "a") == NULL);
    assert(cwist_config_get(cfg, NULL) == NULL);
    cwist_config_destroy(cfg);
    cwist_config_destroy(NULL);
}

static void test_config_typed_getters(void) {
    cwist_config *cfg = cwist_config_create();
    cwist_config_set(cfg, "int", "42");
    cwist_config_set(cfg, "neg", "-7");
    cwist_config_set(cfg, "junk", "12abc");
    cwist_config_set(cfg, "empty", "");
    assert(cwist_config_get_int(cfg, "int", 0) == 42);
    assert(cwist_config_get_int(cfg, "neg", 0) == -7);
    assert(cwist_config_get_int(cfg, "junk", 9) == 9);
    assert(cwist_config_get_int(cfg, "empty", 8) == 8);
    assert(cwist_config_get_int(cfg, "missing", 5) == 5);

    const char *truthy[] = {"true", "TRUE", "1", "yes", "Yes"};
    const char *falsy[] = {"false", "False", "0", "no", "NO"};
    for (size_t i = 0; i < sizeof(truthy) / sizeof(*truthy); i++) {
        cwist_config_set(cfg, "b", truthy[i]);
        assert(cwist_config_get_bool(cfg, "b", false));
        cwist_config_set(cfg, "b", falsy[i]);
        assert(!cwist_config_get_bool(cfg, "b", true));
    }
    cwist_config_set(cfg, "b", "maybe");
    assert(cwist_config_get_bool(cfg, "b", true));
    assert(!cwist_config_get_bool(cfg, "b", false));
    assert(cwist_config_get_bool(cfg, "missing", true));
    cwist_config_destroy(cfg);
}

static void test_config_load_env(void) {
    setenv("CWISTTEST_HOST", "example.org", 1);
    setenv("CWISTTEST_PORT", "8080", 1);
    setenv("OTHERTEST_SKIP", "x", 1);

    cwist_config *cfg = cwist_config_create();
    cwist_config_load_env(cfg, "CWISTTEST_");
    /* Keys keep the prefix. */
    assert(strcmp(cwist_config_get(cfg, "CWISTTEST_HOST"), "example.org") == 0);
    assert(cwist_config_get_int(cfg, "CWISTTEST_PORT", 0) == 8080);
    assert(cwist_config_get(cfg, "OTHERTEST_SKIP") == NULL);
    cwist_config_destroy(cfg);

    /* No prefix loads the whole environment. */
    cfg = cwist_config_create();
    cwist_config_load_env(cfg, NULL);
    assert(strcmp(cwist_config_get(cfg, "OTHERTEST_SKIP"), "x") == 0);
    cwist_config_load_env(NULL, NULL);
    cwist_config_destroy(cfg);
}

static void test_config_load_file(void) {
    char path[] = "/tmp/cwist_cfg_XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    FILE *f = fdopen(fd, "w");
    assert(f);
    fputs("# comment line\n"
          "\n"
          "   \n"
          "name=cwist\n"
          "  spaced key  =   spaced value  \n"
          "quoted=\"with quotes\"\n"
          "half=\"open\n"
          "eq=a=b\n"
          "no_equals_line\n"
          "empty=\n"
          "\t# indented comment\n",
          f);
    fclose(f);

    cwist_config *cfg = cwist_config_create();
    cwist_config_set(cfg, "name", "before");
    cwist_config_load_file(cfg, path);
    assert(strcmp(cwist_config_get(cfg, "name"), "cwist") == 0); /* file overrides */
    assert(strcmp(cwist_config_get(cfg, "spaced key"), "spaced value") == 0);
    assert(strcmp(cwist_config_get(cfg, "quoted"), "with quotes") == 0);
    assert(strcmp(cwist_config_get(cfg, "half"), "\"open") == 0);
    assert(strcmp(cwist_config_get(cfg, "eq"), "a=b") == 0); /* split at the first '=' */
    assert(strcmp(cwist_config_get(cfg, "empty"), "") == 0);
    assert(cwist_config_get(cfg, "no_equals_line") == NULL);
    assert(cwist_config_get(cfg, "# comment line") == NULL);

    /* Missing file and NULL arguments leave the config unchanged. */
    cwist_config_load_file(cfg, "/nonexistent/cwist.conf");
    cwist_config_load_file(cfg, NULL);
    cwist_config_load_file(NULL, path);
    assert(strcmp(cwist_config_get(cfg, "name"), "cwist") == 0);
    cwist_config_destroy(cfg);
    unlink(path);
}

int main(void) {
    test_logger_create_defaults();
    test_logger_streams_and_format();
    test_logger_level_filter();
    test_logger_colors();
    test_config_set_get();
    test_config_typed_getters();
    test_config_load_env();
    test_config_load_file();
    printf("test_logger_config: ok\n");
    return 0;
}
