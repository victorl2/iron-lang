/* test_toml_iron_field.c — Unity tests for the [package].iron field
 * (Phase 95 PIN-01).
 *
 * Locks Plan 95-02 Task 1 acceptance criteria:
 *   - `[package]\niron = ">= 3.2.0"` populates IronProject.iron_constraint.
 *   - Missing iron field leaves iron_constraint NULL (backward compat).
 *   - iron_toml_free releases iron_constraint without leaks.
 *   - Comma-separated AND ranges round-trip as the literal string.
 *   - An empty iron value parses as an empty string (not NULL); the
 *     check_iron_version helper (Plan 02 Task 2) treats this as a parse
 *     error and emits the malformed-constraint diagnostic.
 */

#include "unity.h"
#include "cli/toml.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char g_tmp_path[128];

static const char *write_fixture(const char *contents) {
    snprintf(g_tmp_path, sizeof(g_tmp_path), "/tmp/iron_test_toml_iron_field_XXXXXX");
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

/* Test 1: [package].iron present populates iron_constraint. */
void test_v95_iron_field_populates_constraint(void) {
    const char *fixture =
        "[package]\n"
        "name = \"x\"\n"
        "version = \"0.1.0\"\n"
        "iron = \">= 3.2.0\"\n";

    const char *path = write_fixture(fixture);
    IronProject *proj = iron_toml_parse(path);
    TEST_ASSERT_NOT_NULL(proj);
    TEST_ASSERT_NOT_NULL_MESSAGE(proj->iron_constraint,
        "expected iron_constraint populated for `iron = \">= 3.2.0\"` form");
    TEST_ASSERT_EQUAL_STRING(">= 3.2.0", proj->iron_constraint);

    iron_toml_free(proj);
}

/* Test 2: Missing [package].iron leaves iron_constraint NULL. */
void test_v95_no_iron_field_keeps_constraint_null(void) {
    const char *fixture =
        "[package]\n"
        "name = \"x\"\n"
        "version = \"0.1.0\"\n";

    const char *path = write_fixture(fixture);
    IronProject *proj = iron_toml_parse(path);
    TEST_ASSERT_NOT_NULL(proj);
    TEST_ASSERT_NULL_MESSAGE(proj->iron_constraint,
        "expected iron_constraint NULL when [package].iron is absent");

    iron_toml_free(proj);
}

/* Test 3: iron_toml_free releases iron_constraint without leak/crash. */
void test_v95_iron_field_free_no_leak(void) {
    const char *fixture =
        "[package]\n"
        "name = \"x\"\n"
        "version = \"0.1.0\"\n"
        "iron = \"^3.2\"\n";

    const char *path = write_fixture(fixture);
    IronProject *proj = iron_toml_parse(path);
    TEST_ASSERT_NOT_NULL(proj);
    TEST_ASSERT_NOT_NULL(proj->iron_constraint);
    /* No explicit assertion here beyond no-crash: ASAN/valgrind-clean run is
     * the lock. The test exists so the free path is exercised in isolation. */
    iron_toml_free(proj);
}

/* Test 4: Comma-separated AND range round-trips as the literal string. */
void test_v95_iron_field_comma_range_round_trip(void) {
    const char *fixture =
        "[package]\n"
        "name = \"x\"\n"
        "version = \"0.1.0\"\n"
        "iron = \">= 3.0.0, < 4.0.0\"\n";

    const char *path = write_fixture(fixture);
    IronProject *proj = iron_toml_parse(path);
    TEST_ASSERT_NOT_NULL(proj);
    TEST_ASSERT_NOT_NULL(proj->iron_constraint);
    TEST_ASSERT_EQUAL_STRING(">= 3.0.0, < 4.0.0", proj->iron_constraint);

    iron_toml_free(proj);
}

/* Test 5: Empty iron value parses as empty string, not NULL.
 * (Plan 02 Task 2's check_iron_version treats this as a parse error.) */
void test_v95_iron_field_empty_string_is_not_null(void) {
    const char *fixture =
        "[package]\n"
        "name = \"x\"\n"
        "version = \"0.1.0\"\n"
        "iron = \"\"\n";

    const char *path = write_fixture(fixture);
    IronProject *proj = iron_toml_parse(path);
    TEST_ASSERT_NOT_NULL(proj);
    TEST_ASSERT_NOT_NULL_MESSAGE(proj->iron_constraint,
        "expected iron_constraint to be empty string, not NULL");
    TEST_ASSERT_EQUAL_INT(0, (int)strlen(proj->iron_constraint));

    iron_toml_free(proj);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_v95_iron_field_populates_constraint);
    RUN_TEST(test_v95_no_iron_field_keeps_constraint_null);
    RUN_TEST(test_v95_iron_field_free_no_leak);
    RUN_TEST(test_v95_iron_field_comma_range_round_trip);
    RUN_TEST(test_v95_iron_field_empty_string_is_not_null);
    return UNITY_END();
}
