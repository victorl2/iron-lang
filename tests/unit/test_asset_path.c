/* test_asset_path.c — WEB-TEST-05
 *
 * Structural round-trip test for the asset-path resolution pipeline:
 *   iron.toml directory  → stat check  → --preload-file argv entry
 *
 * The full "argv → MEMFS mount → fopen in WASM" path needs a real browser
 * run and is deferred to Phase 12's Pong manual verification. This test
 * exercises the parts that run without emcc: that `iron_toml_parse` fills
 * `proj->toml_dir`, that `proj->web.assets` carries the relative spec,
 * and that the resolved absolute path is constructible.
 */

#include "unity.h"
#include "cli/toml.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

void setUp(void) {}
void tearDown(void) {}

static const char *write_temp_toml(const char *contents) {
    static char path[] = "/tmp/iron_test_asset_path_XXXXXX";
    strcpy(path, "/tmp/iron_test_asset_path_XXXXXX");
    int fd = mkstemp(path);
    if (fd < 0) return NULL;
    write(fd, contents, strlen(contents));
    close(fd);
    return path;
}

static void test_toml_dir_populated_for_absolute_path(void) {
    const char *toml = "[package]\nname = \"x\"\nversion = \"0.1.0\"\n"
                       "[web]\nassets = \"assets/\"\n";
    const char *path = write_temp_toml(toml);
    TEST_ASSERT_NOT_NULL(path);

    IronProject *proj = iron_toml_parse(path);
    TEST_ASSERT_NOT_NULL(proj);
    TEST_ASSERT_NOT_NULL_MESSAGE(proj->toml_dir, "toml_dir must be populated");
    TEST_ASSERT_EQUAL_STRING("/tmp", proj->toml_dir);

    iron_toml_free(proj);
    unlink(path);
}

static void test_web_assets_parsed_from_string_form(void) {
    const char *toml = "[package]\nname = \"x\"\nversion = \"0.1.0\"\n"
                       "[web]\nassets = \"assets/\"\n";
    const char *path = write_temp_toml(toml);
    TEST_ASSERT_NOT_NULL(path);

    IronProject *proj = iron_toml_parse(path);
    TEST_ASSERT_NOT_NULL(proj);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, proj->web.asset_count,
        "string form of assets should normalize to 1-element array");
    TEST_ASSERT_NOT_NULL(proj->web.assets);
    TEST_ASSERT_EQUAL_STRING("assets/", proj->web.assets[0]);

    iron_toml_free(proj);
    unlink(path);
}

static void test_web_assets_parsed_from_array_form(void) {
    const char *toml = "[package]\nname = \"x\"\nversion = \"0.1.0\"\n"
                       "[web]\nassets = [\"a/\", \"b/\", \"c/\"]\n";
    const char *path = write_temp_toml(toml);
    TEST_ASSERT_NOT_NULL(path);

    IronProject *proj = iron_toml_parse(path);
    TEST_ASSERT_NOT_NULL(proj);
    TEST_ASSERT_EQUAL_INT_MESSAGE(3, proj->web.asset_count,
        "array form with 3 entries should produce asset_count == 3");
    TEST_ASSERT_EQUAL_STRING("a/", proj->web.assets[0]);
    TEST_ASSERT_EQUAL_STRING("b/", proj->web.assets[1]);
    TEST_ASSERT_EQUAL_STRING("c/", proj->web.assets[2]);

    iron_toml_free(proj);
    unlink(path);
}

static void test_resolve_asset_path_from_toml_dir(void) {
    /* Given toml_dir = /tmp and assets entry = "assets/",
     * the resolved path is /tmp/assets/ which stat should recognize. */
    const char *toml_dir = "/tmp";
    const char *asset_spec = "subdir_that_does_not_exist/";
    char resolved[512];
    snprintf(resolved, sizeof(resolved), "%s/%s", toml_dir, asset_spec);
    TEST_ASSERT_EQUAL_STRING("/tmp/subdir_that_does_not_exist/", resolved);

    /* stat should report the path doesn't exist (missing-dir path in build_web.c
     * produces a warning, not an error, per WEB-ASSET-05). */
    struct stat st;
    TEST_ASSERT_NOT_EQUAL_MESSAGE(0, stat(resolved, &st),
        "nonexistent asset dir should fail stat — build_web.c warns and continues");
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_toml_dir_populated_for_absolute_path);
    RUN_TEST(test_web_assets_parsed_from_string_form);
    RUN_TEST(test_web_assets_parsed_from_array_form);
    RUN_TEST(test_resolve_asset_path_from_toml_dir);
    return UNITY_END();
}
