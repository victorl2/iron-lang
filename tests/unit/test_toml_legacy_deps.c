/* test_toml_legacy_deps.c — Unity tests for the legacy [dependencies]
 * detection in iron.toml.
 *
 * Iron has no package manager. Third-party code is vendored under vendor/,
 * so the manifest has no dependency table:
 *   - A legacy [dependencies] header is recorded (legacy_deps_section) so
 *     the iron CLI can print a migration message.
 *   - The first real legacy entry is recorded in legacy_dep_name;
 *     `raylib = true` alone is not a real entry (raylib is enabled by
 *     `import raylib`).
 */

#include "unity.h"
#include "cli/toml.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char g_tmp_path[128];

static const char *write_fixture(const char *contents) {
    snprintf(g_tmp_path, sizeof(g_tmp_path), "/tmp/iron_test_toml_legacy_deps_XXXXXX");
    int fd = mkstemp(g_tmp_path);
    TEST_ASSERT_NOT_EQUAL_MESSAGE(-1, fd, "mkstemp failed");
    ssize_t written = write(fd, contents, strlen(contents));
    (void)written;
    close(fd);
    return g_tmp_path;
}

void setUp(void)    { g_tmp_path[0] = '\0'; }
void tearDown(void) { if (g_tmp_path[0]) unlink(g_tmp_path); }

/* No [dependencies]: nothing recorded. */
void test_plain_manifest_has_no_legacy_deps(void) {
    const char *path = write_fixture(
        "[package]\n"
        "name = \"app\"\n"
        "version = \"0.1.0\"\n");
    IronProject *proj = iron_toml_parse(path);
    TEST_ASSERT_NOT_NULL(proj);
    TEST_ASSERT_FALSE(proj->legacy_deps_section);
    TEST_ASSERT_NULL(proj->legacy_dep_name);
    iron_toml_free(proj);
}

/* An empty legacy [dependencies] table (the old `iron init` template) is
 * detected but carries no entry. */
void test_empty_legacy_dependencies_detected(void) {
    const char *path = write_fixture(
        "[package]\n"
        "name = \"app\"\n"
        "version = \"0.1.0\"\n"
        "\n"
        "[dependencies]\n");
    IronProject *proj = iron_toml_parse(path);
    TEST_ASSERT_NOT_NULL(proj);
    TEST_ASSERT_TRUE(proj->legacy_deps_section);
    TEST_ASSERT_NULL(proj->legacy_dep_name);
    iron_toml_free(proj);
}

/* `raylib = true` is not a real dependency entry. */
void test_legacy_raylib_flag_is_not_an_entry(void) {
    const char *path = write_fixture(
        "[package]\n"
        "name = \"game\"\n"
        "version = \"0.1.0\"\n"
        "[dependencies]\n"
        "raylib = true\n");
    IronProject *proj = iron_toml_parse(path);
    TEST_ASSERT_NOT_NULL(proj);
    TEST_ASSERT_TRUE(proj->legacy_deps_section);
    TEST_ASSERT_NULL(proj->legacy_dep_name);
    iron_toml_free(proj);
}

/* Git and path entries are recorded by name so the CLI can reject them. */
void test_legacy_entries_record_first_name(void) {
    const char *path = write_fixture(
        "[package]\n"
        "name = \"app\"\n"
        "version = \"0.1.0\"\n"
        "[dependencies]\n"
        "raylib = true\n"
        "iron-ecs = { git = \"owner/iron-ecs\", version = \"0.2.0\" }\n"
        "mylib = { path = \"../mylib\" }\n");
    IronProject *proj = iron_toml_parse(path);
    TEST_ASSERT_NOT_NULL(proj);
    TEST_ASSERT_TRUE(proj->legacy_deps_section);
    TEST_ASSERT_EQUAL_STRING("iron-ecs", proj->legacy_dep_name);
    iron_toml_free(proj);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_plain_manifest_has_no_legacy_deps);
    RUN_TEST(test_empty_legacy_dependencies_detected);
    RUN_TEST(test_legacy_raylib_flag_is_not_an_entry);
    RUN_TEST(test_legacy_entries_record_first_name);
    return UNITY_END();
}
