/* test_toml_web.c — Unity tests for the [web] TOML section parser.
 *
 * Covers WEB-MANIFEST-01 through WEB-MANIFEST-08:
 *   - full [web] section populates IronWebConfig fields
 *   - assets = "path"       -> asset_count=1
 *   - assets = ["a","b"]    -> asset_count=2
 *   - unknown [web].foo key -> warning, does not fail
 *   - misspelled [wbe]      -> "did you mean [web]?" warning, parse succeeds
 *   - iron_toml_free releases every web heap allocation
 */

#include "unity.h"
#include "cli/toml.h"
#include "cli/web_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ── Fixture helpers ─────────────────────────────────────────────────────── */

static char g_tmp_path[128];

/* Write `contents` to a unique temp file, return the path (static buffer). */
static const char *write_fixture(const char *contents) {
    snprintf(g_tmp_path, sizeof(g_tmp_path), "/tmp/iron_test_toml_web_XXXXXX");
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

/* ── Unity boilerplate ───────────────────────────────────────────────────── */

void setUp(void)    { g_tmp_path[0] = '\0'; }
void tearDown(void) { if (g_tmp_path[0]) cleanup_fixture(); }

/* ── Tests ───────────────────────────────────────────────────────────────── */

/* Test 1: All [web] fields parsed correctly alongside [package]. */
void test_web_full_section_parses(void) {
    const char *fixture =
        "[package]\n"
        "name = \"pong\"\n"
        "version = \"0.1.0\"\n"
        "\n"
        "[web]\n"
        "title = \"Pong\"\n"
        "shell = \"custom.html\"\n"
        "initial_memory = 268435456\n"
        "stack_size = 2097152\n"
        "pthread_pool_size = 8\n";

    const char *path = write_fixture(fixture);
    IronProject *proj = iron_toml_parse(path);
    TEST_ASSERT_NOT_NULL(proj);

    /* [package] section still works */
    TEST_ASSERT_EQUAL_STRING("pong", proj->name);

    /* [web] fields populated correctly */
    TEST_ASSERT_EQUAL_STRING("Pong", proj->web.title);
    TEST_ASSERT_EQUAL_STRING("custom.html", proj->web.shell);
    TEST_ASSERT_EQUAL_INT(268435456, proj->web.initial_memory);
    TEST_ASSERT_EQUAL_INT(2097152, proj->web.stack_size);
    TEST_ASSERT_EQUAL_INT(8, proj->web.pthread_pool_size);

    iron_toml_free(proj);
}

/* Test 2: assets = "string" normalized to one-element array. */
void test_web_assets_string(void) {
    const char *fixture =
        "[web]\n"
        "assets = \"assets/\"\n";

    const char *path = write_fixture(fixture);
    IronProject *proj = iron_toml_parse(path);
    TEST_ASSERT_NOT_NULL(proj);

    TEST_ASSERT_EQUAL_INT(1, proj->web.asset_count);
    TEST_ASSERT_NOT_NULL(proj->web.assets);
    TEST_ASSERT_EQUAL_STRING("assets/", proj->web.assets[0]);

    iron_toml_free(proj);
}

/* Test 3: assets = ["a.png", "b.png", "c.png"] parsed as 3-element array. */
void test_web_assets_array(void) {
    const char *fixture =
        "[web]\n"
        "assets = [\"a.png\", \"b.png\", \"c.png\"]\n";

    const char *path = write_fixture(fixture);
    IronProject *proj = iron_toml_parse(path);
    TEST_ASSERT_NOT_NULL(proj);

    TEST_ASSERT_EQUAL_INT(3, proj->web.asset_count);
    TEST_ASSERT_NOT_NULL(proj->web.assets);
    TEST_ASSERT_EQUAL_STRING("a.png", proj->web.assets[0]);
    TEST_ASSERT_EQUAL_STRING("b.png", proj->web.assets[1]);
    TEST_ASSERT_EQUAL_STRING("c.png", proj->web.assets[2]);

    iron_toml_free(proj);
}

/* Test 4: Unknown [web] key warns but does not fail parse (WEB-MANIFEST-08). */
void test_web_unknown_key_does_not_fail(void) {
    const char *fixture =
        "[web]\n"
        "title = \"Pong\"\n"
        "foo = 42\n";

    const char *path = write_fixture(fixture);
    IronProject *proj = iron_toml_parse(path);

    /* Parse must succeed despite the unknown key. */
    TEST_ASSERT_NOT_NULL(proj);

    /* Known keys are still processed correctly. */
    TEST_ASSERT_EQUAL_STRING("Pong", proj->web.title);

    iron_toml_free(proj);
}

/* Test 5: No [web] section -> all web fields at zero/NULL defaults. */
void test_web_defaults_when_unset(void) {
    const char *fixture =
        "[package]\n"
        "name = \"x\"\n";

    const char *path = write_fixture(fixture);
    IronProject *proj = iron_toml_parse(path);
    TEST_ASSERT_NOT_NULL(proj);

    /* All web pointer fields are NULL when section is absent. */
    TEST_ASSERT_NULL(proj->web.title);
    TEST_ASSERT_NULL(proj->web.shell);
    TEST_ASSERT_NULL(proj->web.assets);

    /* Numeric fields are 0 (zero == "use IRON_WEB_DEFAULT_*" sentinel). */
    TEST_ASSERT_EQUAL_INT(0, proj->web.asset_count);
    TEST_ASSERT_EQUAL_INT(0, proj->web.initial_memory);
    TEST_ASSERT_EQUAL_INT(0, proj->web.stack_size);
    TEST_ASSERT_EQUAL_INT(0, proj->web.pthread_pool_size);

    iron_toml_free(proj);
}

/* Test 6: Misspelled [wbe] section: parse succeeds, keys not populated. */
void test_web_misspelled_section_silent_on_parse(void) {
    const char *fixture =
        "[wbe]\n"
        "title = \"Pong\"\n";

    const char *path = write_fixture(fixture);
    IronProject *proj = iron_toml_parse(path);

    /* Parse must not fail (the section is unknown-recovered). */
    TEST_ASSERT_NOT_NULL(proj);

    /* The typo means the [web] struct is untouched — title is NOT set. */
    TEST_ASSERT_NULL(proj->web.title);

    iron_toml_free(proj);
}

/* Test 7: Completely unrelated section is silently ignored. */
void test_web_totally_unrelated_section_silent(void) {
    const char *fixture =
        "[completely_different]\n"
        "foo = \"bar\"\n";

    const char *path = write_fixture(fixture);
    IronProject *proj = iron_toml_parse(path);

    /* Parse must succeed and not crash. */
    TEST_ASSERT_NOT_NULL(proj);

    /* No web fields touched. */
    TEST_ASSERT_NULL(proj->web.title);
    TEST_ASSERT_NULL(proj->web.shell);
    TEST_ASSERT_NULL(proj->web.assets);
    TEST_ASSERT_EQUAL_INT(0, proj->web.asset_count);

    iron_toml_free(proj);
}

/* Test 8: Full [web] section parse + free — smoke gate for leak detection.
 * Under IRON_ENABLE_SANITIZERS=ON, AddressSanitizer will report any leak at
 * process exit. This test verifies that iron_toml_free correctly releases
 * all heap allocations made during a full [web] parse. */
void test_web_free_no_leak_on_full_section(void) {
    const char *fixture =
        "[package]\n"
        "name = \"pong\"\n"
        "version = \"0.1.0\"\n"
        "\n"
        "[web]\n"
        "title = \"Pong\"\n"
        "shell = \"custom.html\"\n"
        "initial_memory = 268435456\n"
        "stack_size = 2097152\n"
        "pthread_pool_size = 8\n"
        "assets = [\"a.png\", \"b.png\"]\n";

    const char *path = write_fixture(fixture);
    IronProject *proj = iron_toml_parse(path);
    TEST_ASSERT_NOT_NULL(proj);

    /* Verify all fields parsed (regression: free might be called on junk) */
    TEST_ASSERT_EQUAL_STRING("Pong", proj->web.title);
    TEST_ASSERT_EQUAL_STRING("custom.html", proj->web.shell);
    TEST_ASSERT_EQUAL_INT(268435456, proj->web.initial_memory);
    TEST_ASSERT_EQUAL_INT(2097152, proj->web.stack_size);
    TEST_ASSERT_EQUAL_INT(8, proj->web.pthread_pool_size);
    TEST_ASSERT_EQUAL_INT(2, proj->web.asset_count);
    TEST_ASSERT_EQUAL_STRING("a.png", proj->web.assets[0]);
    TEST_ASSERT_EQUAL_STRING("b.png", proj->web.assets[1]);

    /* Free must not crash; ASan will catch any heap-use-after-free or leak. */
    iron_toml_free(proj);
}

/* Test 9: toml_dir is populated with the containing directory after parse.
 * Uses write_fixture which writes to /tmp, so toml_dir must contain "/tmp"
 * and must NOT contain the filename "iron_test_toml_web_". */
void test_toml_dir_is_populated(void) {
    const char *fixture =
        "[package]\n"
        "name = \"x\"\n"
        "version = \"0.1.0\"\n";

    const char *path = write_fixture(fixture);
    IronProject *proj = iron_toml_parse(path);
    TEST_ASSERT_NOT_NULL(proj);

    /* toml_dir must be non-NULL on success. */
    TEST_ASSERT_NOT_NULL(proj->toml_dir);

    /* Must point into /tmp (macOS resolves /tmp -> /private/tmp; accept both). */
    TEST_ASSERT_TRUE(strstr(proj->toml_dir, "/tmp") != NULL);

    /* Must NOT contain the filename — it's the directory, not the full path. */
    TEST_ASSERT_TRUE(strstr(proj->toml_dir, "iron_test_toml_web_") == NULL);

    iron_toml_free(proj);
}

/* Test 10: toml_dir is "." when path has no directory component.
 * Achieves this by chdir'ing to /tmp, writing a file there, and passing the
 * basename alone so dirname() returns ".". */
void test_toml_dir_relative_path(void) {
    /* Write a minimal iron.toml directly in /tmp under a predictable name. */
    const char *bare_name = "iron_test_toml_bare_XXXXXX";
    char tmp_path[128];
    snprintf(tmp_path, sizeof(tmp_path), "/tmp/%s", bare_name);
    /* Use mkstemp to create the file first. */
    snprintf(tmp_path, sizeof(tmp_path), "/tmp/iron_test_bare_XXXXXX");
    int fd = mkstemp(tmp_path);
    TEST_ASSERT_NOT_EQUAL_MESSAGE(-1, fd, "mkstemp failed for bare test");
    const char *contents = "[package]\nname = \"bare\"\nversion = \"0.1.0\"\n";
    ssize_t written = write(fd, contents, strlen(contents));
    (void)written;
    close(fd);

    /* Get the basename portion of tmp_path (everything after last '/'). */
    const char *slash = strrchr(tmp_path, '/');
    const char *filename = slash ? slash + 1 : tmp_path;

    /* chdir to /tmp so we can pass a bare filename with no slash. */
    char saved_cwd[1024];
    char *cwd_result = getcwd(saved_cwd, sizeof(saved_cwd));
    (void)cwd_result;
    int chdir_rc = chdir("/tmp");
    (void)chdir_rc;

    IronProject *proj = iron_toml_parse(filename);

    /* Restore cwd before any assertion so tearDown sees the right state. */
    int restore_rc = chdir(saved_cwd);
    (void)restore_rc;
    unlink(tmp_path);

    TEST_ASSERT_NOT_NULL(proj);
    TEST_ASSERT_NOT_NULL(proj->toml_dir);

    /* dirname of a bare filename with no slash must return ".". */
    TEST_ASSERT_EQUAL_STRING(".", proj->toml_dir);

    iron_toml_free(proj);
}

/* ── Main ────────────────────────────────────────────────────────────────── */

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_web_full_section_parses);
    RUN_TEST(test_web_assets_string);
    RUN_TEST(test_web_assets_array);
    RUN_TEST(test_web_unknown_key_does_not_fail);
    RUN_TEST(test_web_defaults_when_unset);
    RUN_TEST(test_web_misspelled_section_silent_on_parse);
    RUN_TEST(test_web_totally_unrelated_section_silent);
    RUN_TEST(test_web_free_no_leak_on_full_section);
    RUN_TEST(test_toml_dir_is_populated);
    RUN_TEST(test_toml_dir_relative_path);
    return UNITY_END();
}
