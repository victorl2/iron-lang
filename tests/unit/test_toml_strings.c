/* test_toml_strings.c - iron.toml strings, long lines and package names
 * (#565): escapes are decoded, malformed strings and text after a value are
 * reported with their line, a '#' inside a string is not a comment, long
 * lines stay whole, and the package name must be a plain file name. */

#include "unity.h"
#include "cli/toml.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "support/posix_test.h"

static char g_tmp_path[128];

static const char *write_fixture(const char *contents) {
    snprintf(g_tmp_path, sizeof(g_tmp_path), IRON_TEST_TMP "/iron_test_toml_strings_XXXXXX");
    int fd = mkstemp(g_tmp_path);
    TEST_ASSERT_NOT_EQUAL_MESSAGE(-1, fd, "mkstemp failed");
    ssize_t written = write(fd, contents, strlen(contents));
    (void)written;
    close(fd);
    return g_tmp_path;
}

void setUp(void)    { g_tmp_path[0] = '\0'; }
void tearDown(void) { if (g_tmp_path[0]) unlink(g_tmp_path); }

static IronProject *parse(const char *text) {
    IronProject *proj = iron_toml_parse(write_fixture(text));
    TEST_ASSERT_NOT_NULL(proj);
    return proj;
}

void test_escapes_are_decoded(void) {
    IronProject *p = parse("[package]\nname = \"app\"\nversion = \"0.1.0\"\n"
                           "description = \"a \\\"quoted\\\" word # not a comment\\tand \\u00e9\" # comment\n");
    TEST_ASSERT_NULL(p->error);
    TEST_ASSERT_EQUAL_STRING("a \"quoted\" word # not a comment\tand \xc3\xa9", p->description);
    iron_toml_free(p);
}

void test_text_after_string_is_an_error(void) {
    IronProject *p = parse("[package]\nname = \"a\"b\"\nversion = \"0.1.0\"\n");
    TEST_ASSERT_NOT_NULL(p->error);
    TEST_ASSERT_NOT_NULL(strstr(p->error, "iron.toml:2:"));
    iron_toml_free(p);
}

void test_unterminated_string_is_an_error(void) {
    IronProject *p = parse("[package]\nname = \"app\nversion = \"0.1.0\"\n");
    TEST_ASSERT_NOT_NULL(p->error);
    TEST_ASSERT_NOT_NULL(strstr(p->error, "unterminated"));
    iron_toml_free(p);
}

void test_backslash_quote_does_not_end_the_string(void) {
    IronProject *p = parse("[package]\nname = \"app\"\nversion = \"0.1.0\"\ndescription = \"a\\\\\"\n");
    TEST_ASSERT_NULL(p->error);
    TEST_ASSERT_EQUAL_STRING("a\\", p->description);
    iron_toml_free(p);
}

void test_long_line_stays_whole(void) {
    char text[4096];
    char desc[3000];
    memset(desc, 'x', sizeof(desc) - 1);
    desc[sizeof(desc) - 1] = '\0';
    snprintf(text, sizeof(text), "[package]\nname = \"app\"\ndescription = \"%s\"\nversion = \"0.1.0\"\n", desc);
    IronProject *p = parse(text);
    TEST_ASSERT_NULL(p->error);
    TEST_ASSERT_EQUAL_size_t(strlen(desc), strlen(p->description));
    TEST_ASSERT_EQUAL_STRING("0.1.0", p->version);
    iron_toml_free(p);
}

void test_package_name_must_be_a_file_name(void) {
    const char *bad[] = { "../../x", "a/b", "-x", "", "a b" };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        char text[256];
        snprintf(text, sizeof(text), "[package]\nname = \"%s\"\nversion = \"0.1.0\"\n", bad[i]);
        IronProject *p = parse(text);
        TEST_ASSERT_NOT_NULL_MESSAGE(p->error, bad[i]);
        iron_toml_free(p);
    }
    TEST_ASSERT_TRUE(iron_toml_valid_package_name("my-app_2"));
}

void test_type_must_be_bin_or_lib(void) {
    IronProject *p = parse("[package]\nname = \"app\"\nversion = \"0.1.0\"\ntype = \"binary\"\n");
    TEST_ASSERT_NOT_NULL(p->error);
    iron_toml_free(p);
    p = parse("[package]\nname = \"app\"\nversion = \"0.1.0\"\ntype = \"lib\"\n");
    TEST_ASSERT_NULL(p->error);
    iron_toml_free(p);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_escapes_are_decoded);
    RUN_TEST(test_text_after_string_is_an_error);
    RUN_TEST(test_unterminated_string_is_an_error);
    RUN_TEST(test_backslash_quote_does_not_end_the_string);
    RUN_TEST(test_long_line_stays_whole);
    RUN_TEST(test_package_name_must_be_a_file_name);
    RUN_TEST(test_type_must_be_bin_or_lib);
    return UNITY_END();
}
