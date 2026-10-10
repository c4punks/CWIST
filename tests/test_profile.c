/* test_profile.c - unit tests for the CWIST_PROFILE preset env var system
 * (issue #171).
 *
 * cwist_apply_profile() uses setenv(overwrite=0), so a variable that is
 * already set before the call keeps its value.  Each test unsets the
 * relevant variables first, calls cwist_apply_profile(), then checks the
 * expected state.
 *
 * Build (from repo root, after make):
 *   cc -O0 -std=c17 -I include -o test_profile \
 *       tests/test_profile.c libcwist.a $(LIBS)
 * Or via make:
 *   make test_profile
 */
#include <cwist/sys/app/app.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void clear_profile_vars(void) {
    unsetenv("CWIST_C1M_MODE");
    unsetenv("CWIST_MALLOC_ARENA_MAX");
    unsetenv("CWIST_REACTOR_DRAIN_CHUNK");
    unsetenv("CWIST_HTTP_BATCH");
    unsetenv("CWIST_WORKERS");
    unsetenv("CWIST_PROFILE");
}

static const char *eget(const char *name) {
    const char *v = getenv(name);
    return v ? v : "";
}

void test_performance_profile(void) {
    printf("Testing CWIST_PROFILE=performance...\n");
    clear_profile_vars();
    setenv("CWIST_PROFILE", "performance", 1);
    cwist_apply_profile();
    assert(strcmp(eget("CWIST_C1M_MODE"), "1") == 0);
    assert(strcmp(eget("CWIST_MALLOC_ARENA_MAX"), "1") == 0);
    assert(strcmp(eget("CWIST_REACTOR_DRAIN_CHUNK"), "8") == 0);
    assert(strcmp(eget("CWIST_HTTP_BATCH"), "64") == 0);
    clear_profile_vars();
    printf("Passed.\n");
}

void test_lowmem_profile(void) {
    printf("Testing CWIST_PROFILE=lowmem...\n");
    clear_profile_vars();
    setenv("CWIST_PROFILE", "lowmem", 1);
    cwist_apply_profile();
    assert(strcmp(eget("CWIST_C1M_MODE"), "1") == 0);
    assert(strcmp(eget("CWIST_MALLOC_ARENA_MAX"), "1") == 0);
    assert(strcmp(eget("CWIST_WORKERS"), "2") == 0);
    clear_profile_vars();
    printf("Passed.\n");
}

void test_lowlat_profile(void) {
    printf("Testing CWIST_PROFILE=lowlat...\n");
    clear_profile_vars();
    setenv("CWIST_PROFILE", "lowlat", 1);
    cwist_apply_profile();
    assert(strcmp(eget("CWIST_C1M_MODE"), "0") == 0);
    clear_profile_vars();
    printf("Passed.\n");
}

void test_default_profile_sets_baseline(void) {
    printf("Testing CWIST_PROFILE=default applies the C1M + drain-chunk baseline...\n");
    clear_profile_vars();
    setenv("CWIST_PROFILE", "default", 1);
    cwist_apply_profile();
    assert(strcmp(eget("CWIST_C1M_MODE"), "1") == 0);
    assert(strcmp(eget("CWIST_REACTOR_DRAIN_CHUNK"), "8") == 0);
    /* variables outside the default baseline stay untouched */
    assert(getenv("CWIST_MALLOC_ARENA_MAX") == NULL);
    clear_profile_vars();
    printf("Passed.\n");
}

void test_unset_profile_matches_default(void) {
    printf("Testing unset CWIST_PROFILE applies the same baseline as default...\n");
    clear_profile_vars();
    cwist_apply_profile();
    assert(strcmp(eget("CWIST_C1M_MODE"), "1") == 0);
    assert(strcmp(eget("CWIST_REACTOR_DRAIN_CHUNK"), "8") == 0);
    clear_profile_vars();
    printf("Passed.\n");
}

void test_explicit_var_wins_over_profile(void) {
    printf("Testing explicit env var is not overridden by profile...\n");
    clear_profile_vars();
    /* User wants classic pool even under the performance profile. */
    setenv("CWIST_C1M_MODE", "0", 1);
    setenv("CWIST_PROFILE", "performance", 1);
    cwist_apply_profile();
    /* profile would set C1M_MODE=1, but user's 0 must survive */
    assert(strcmp(eget("CWIST_C1M_MODE"), "0") == 0);
    /* other vars not pre-set should still pick up profile defaults */
    assert(strcmp(eget("CWIST_MALLOC_ARENA_MAX"), "1") == 0);
    clear_profile_vars();
    printf("Passed.\n");
}

void test_explicit_var_wins_over_default(void) {
    printf("Testing explicit env var is not overridden by the default baseline...\n");
    clear_profile_vars();
    /* User wants the legacy 64-event drain chunk with default profile. */
    setenv("CWIST_REACTOR_DRAIN_CHUNK", "64", 1);
    setenv("CWIST_PROFILE", "default", 1);
    cwist_apply_profile();
    assert(strcmp(eget("CWIST_REACTOR_DRAIN_CHUNK"), "64") == 0);
    assert(strcmp(eget("CWIST_C1M_MODE"), "1") == 0);
    clear_profile_vars();
    printf("Passed.\n");
}

int main(void) {
    test_performance_profile();
    test_lowmem_profile();
    test_lowlat_profile();
    test_default_profile_sets_baseline();
    test_unset_profile_matches_default();
    test_explicit_var_wins_over_profile();
    test_explicit_var_wins_over_default();
    printf("All profile tests passed.\n");
    return 0;
}
