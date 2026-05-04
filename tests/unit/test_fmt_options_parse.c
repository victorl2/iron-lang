/* Phase 5 Plan 05-01 (FMT-05, D-02, D-16): Unity tests for the [fmt]
 * TOML section parser. Six scenarios:
 *   - missing [fmt] section -> defaults preserved
 *   - all keys set         -> struct matches
 *   - partial keys         -> other fields stay at defaults
 *   - invalid int          -> falls back to default + stderr warning
 *   - invalid bool         -> use_tabs falls back to false
 *   - unknown key          -> silently ignored; sibling valid keys still parse */

#include "unity.h"
#include "cli/toml.h"
#include "fmt/options.h"
#include "fmt/config_load.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void setUp(void) {}
void tearDown(void) {}

/* mkstemp wants a writable XXXXXX-suffixed buffer. */
static char *write_temp_toml(const char *content) {
    static char path_buf[64];
    strcpy(path_buf, "/tmp/iron_fmt_toml_XXXXXX");
    int fd = mkstemp(path_buf);
    TEST_ASSERT_TRUE_MESSAGE(fd >= 0, "mkstemp failed");
    size_t len = strlen(content);
    ssize_t w  = write(fd, content, len);
    TEST_ASSERT_EQUAL_INT((int)len, (int)w);
    close(fd);
    return path_buf;
}

void test_missing_fmt_section_uses_defaults(void) {
    char *p = write_temp_toml("[package]\nname = \"foo\"\n");
    IronProject *proj = iron_toml_parse(p);
    TEST_ASSERT_NOT_NULL(proj);
    IronFmtOptions o = iron_fmt_options_from_toml(proj);
    TEST_ASSERT_EQUAL_INT(100, o.line_width);
    TEST_ASSERT_EQUAL_INT(2,   o.indent_width);
    TEST_ASSERT_FALSE(o.use_tabs);
    iron_toml_free(proj);
    unlink(p);
}

void test_all_fmt_keys_set(void) {
    char *p = write_temp_toml(
        "[package]\nname = \"foo\"\n"
        "[fmt]\nline_width = 80\nindent_width = 4\nuse_tabs = true\n");
    IronProject *proj = iron_toml_parse(p);
    TEST_ASSERT_NOT_NULL(proj);
    IronFmtOptions o = iron_fmt_options_from_toml(proj);
    TEST_ASSERT_EQUAL_INT(80, o.line_width);
    TEST_ASSERT_EQUAL_INT(4,  o.indent_width);
    TEST_ASSERT_TRUE(o.use_tabs);
    iron_toml_free(proj);
    unlink(p);
}

void test_partial_fmt_keys(void) {
    char *p = write_temp_toml("[fmt]\nindent_width = 4\n");
    IronProject *proj = iron_toml_parse(p);
    TEST_ASSERT_NOT_NULL(proj);
    IronFmtOptions o = iron_fmt_options_from_toml(proj);
    TEST_ASSERT_EQUAL_INT(100, o.line_width);   /* default preserved */
    TEST_ASSERT_EQUAL_INT(4,   o.indent_width);
    TEST_ASSERT_FALSE(o.use_tabs);              /* default preserved */
    iron_toml_free(proj);
    unlink(p);
}

void test_invalid_int_falls_back(void) {
    /* "hello" is not a valid integer; toml.c emits a stderr warning and
     * leaves the field at its calloc-zeroed value, which the loader
     * then maps to the default via the > 0 guard. */
    char *p = write_temp_toml("[fmt]\nindent_width = \"hello\"\n");
    IronProject *proj = iron_toml_parse(p);
    TEST_ASSERT_NOT_NULL(proj);
    IronFmtOptions o = iron_fmt_options_from_toml(proj);
    TEST_ASSERT_EQUAL_INT(2, o.indent_width);   /* default preserved */
    iron_toml_free(proj);
    unlink(p);
}

void test_invalid_bool_falls_back(void) {
    /* "42" is not "true" or "false"; use_tabs stays at the default. */
    char *p = write_temp_toml("[fmt]\nuse_tabs = 42\n");
    IronProject *proj = iron_toml_parse(p);
    TEST_ASSERT_NOT_NULL(proj);
    IronFmtOptions o = iron_fmt_options_from_toml(proj);
    TEST_ASSERT_FALSE(o.use_tabs);              /* default preserved */
    iron_toml_free(proj);
    unlink(p);
}

void test_unknown_key_silently_ignored(void) {
    char *p = write_temp_toml("[fmt]\nwizard = 1\nindent_width = 4\n");
    IronProject *proj = iron_toml_parse(p);
    TEST_ASSERT_NOT_NULL(proj);
    IronFmtOptions o = iron_fmt_options_from_toml(proj);
    TEST_ASSERT_EQUAL_INT(4, o.indent_width);   /* valid sibling key still parsed */
    iron_toml_free(proj);
    unlink(p);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_missing_fmt_section_uses_defaults);
    RUN_TEST(test_all_fmt_keys_set);
    RUN_TEST(test_partial_fmt_keys);
    RUN_TEST(test_invalid_int_falls_back);
    RUN_TEST(test_invalid_bool_falls_back);
    RUN_TEST(test_unknown_key_silently_ignored);
    return UNITY_END();
}
