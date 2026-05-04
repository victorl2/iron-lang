/* test_toml_path_dep.c — Unity tests for the [dependencies] inline-table
 * `path = "..."` extraction (Phase 94 LIB-03).
 *
 * Locks Plan 94-02 Task 1 acceptance criteria:
 *   - `mylib = { path = "../mylib" }` populates IronDep.path with the path
 *     string, leaves git/version NULL.
 *   - `mylib = { git = "user/repo", version = "1.0.0" }` keeps existing
 *     git-form behavior (path == NULL).
 *   - `mylib = { path = "./local", version = "0.1.0" }` populates both
 *     fields (mixed form is acceptable).
 *   - iron_toml_free releases dep->path without leaks (smoke).
 */

#include "unity.h"
#include "cli/toml.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char g_tmp_path[128];

static const char *write_fixture(const char *contents) {
    snprintf(g_tmp_path, sizeof(g_tmp_path), "/tmp/iron_test_toml_path_dep_XXXXXX");
    int fd = mkstemp(g_tmp_path);
    TEST_ASSERT_NOT_EQUAL_MESSAGE(-1, fd, "mkstemp failed");
    ssize_t written = write(fd, contents, strlen(contents));
    (void)written;
    close(fd);
    return g_tmp_path;
}

static void cleanup_fixture(void) {
    unlink(g_tmp_path);
}

void setUp(void)    { g_tmp_path[0] = '\0'; }
void tearDown(void) { if (g_tmp_path[0]) cleanup_fixture(); }

/* Test 1: Pure path-form populates path; git/version stay NULL. */
void test_v94_path_form_only_populates_path(void) {
    const char *fixture =
        "[package]\n"
        "name = \"consumer\"\n"
        "version = \"0.1.0\"\n"
        "type = \"bin\"\n"
        "[dependencies]\n"
        "mylib = { path = \"../mylib\" }\n";

    const char *path = write_fixture(fixture);
    IronProject *proj = iron_toml_parse(path);
    TEST_ASSERT_NOT_NULL(proj);
    TEST_ASSERT_EQUAL_INT(1, proj->dep_count);
    TEST_ASSERT_EQUAL_STRING("mylib", proj->deps[0].name);
    TEST_ASSERT_NOT_NULL_MESSAGE(proj->deps[0].path,
        "expected dep->path populated for `path = \"../mylib\"` form");
    TEST_ASSERT_EQUAL_STRING("../mylib", proj->deps[0].path);
    TEST_ASSERT_NULL(proj->deps[0].git);
    TEST_ASSERT_NULL(proj->deps[0].version);

    iron_toml_free(proj);
}

/* Test 2: Pure git-form keeps path == NULL (existing behavior preserved). */
void test_v94_git_form_keeps_path_null(void) {
    const char *fixture =
        "[package]\n"
        "name = \"consumer\"\n"
        "version = \"0.1.0\"\n"
        "[dependencies]\n"
        "mylib = { git = \"user/repo\", version = \"1.0.0\" }\n";

    const char *path = write_fixture(fixture);
    IronProject *proj = iron_toml_parse(path);
    TEST_ASSERT_NOT_NULL(proj);
    TEST_ASSERT_EQUAL_INT(1, proj->dep_count);
    TEST_ASSERT_EQUAL_STRING("user/repo", proj->deps[0].git);
    TEST_ASSERT_EQUAL_STRING("1.0.0", proj->deps[0].version);
    TEST_ASSERT_NULL_MESSAGE(proj->deps[0].path,
        "expected dep->path NULL for git-form deps");

    iron_toml_free(proj);
}

/* Test 3: Mixed form populates both path and version. */
void test_v94_mixed_path_and_version(void) {
    const char *fixture =
        "[package]\n"
        "name = \"consumer\"\n"
        "version = \"0.1.0\"\n"
        "[dependencies]\n"
        "mylib = { path = \"./local\", version = \"0.1.0\" }\n";

    const char *path = write_fixture(fixture);
    IronProject *proj = iron_toml_parse(path);
    TEST_ASSERT_NOT_NULL(proj);
    TEST_ASSERT_EQUAL_INT(1, proj->dep_count);
    TEST_ASSERT_EQUAL_STRING("./local", proj->deps[0].path);
    TEST_ASSERT_EQUAL_STRING("0.1.0", proj->deps[0].version);
    TEST_ASSERT_NULL(proj->deps[0].git);

    iron_toml_free(proj);
}

/* Test 4: No deps -> dep_count == 0; iron_toml_free does not crash. */
void test_v94_no_deps_section_no_crash(void) {
    const char *fixture =
        "[package]\n"
        "name = \"consumer\"\n"
        "version = \"0.1.0\"\n";

    const char *path = write_fixture(fixture);
    IronProject *proj = iron_toml_parse(path);
    TEST_ASSERT_NOT_NULL(proj);
    TEST_ASSERT_EQUAL_INT(0, proj->dep_count);

    iron_toml_free(proj);
}

/* Test 5: Two path-deps in one [dependencies] block both populate path. */
void test_v94_two_path_deps(void) {
    const char *fixture =
        "[package]\n"
        "name = \"consumer\"\n"
        "version = \"0.1.0\"\n"
        "[dependencies]\n"
        "mylib1 = { path = \"../mylib1\" }\n"
        "mylib2 = { path = \"../mylib2\" }\n";

    const char *path = write_fixture(fixture);
    IronProject *proj = iron_toml_parse(path);
    TEST_ASSERT_NOT_NULL(proj);
    TEST_ASSERT_EQUAL_INT(2, proj->dep_count);
    TEST_ASSERT_EQUAL_STRING("mylib1", proj->deps[0].name);
    TEST_ASSERT_EQUAL_STRING("../mylib1", proj->deps[0].path);
    TEST_ASSERT_EQUAL_STRING("mylib2", proj->deps[1].name);
    TEST_ASSERT_EQUAL_STRING("../mylib2", proj->deps[1].path);

    iron_toml_free(proj);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_v94_path_form_only_populates_path);
    RUN_TEST(test_v94_git_form_keeps_path_null);
    RUN_TEST(test_v94_mixed_path_and_version);
    RUN_TEST(test_v94_no_deps_section_no_crash);
    RUN_TEST(test_v94_two_path_deps);
    return UNITY_END();
}
