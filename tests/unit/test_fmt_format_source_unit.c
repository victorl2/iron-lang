/* Phase 5 Plan 05-01 (FMT-01, D-01, D-03): unit tests for the
 * iron_format_source library entry. Covers:
 *   - empty source (closes RESEARCH Open Q3 / Plan 05-01 Step 12)
 *   - clean single-decl input round-trips
 *   - parse error -> ok=false, error_count > 0 (refusal D-03)
 *   - lex error  -> ok=false, error_count > 0 (refusal D-03)
 *   - opts == NULL == explicit defaults */

#include "unity.h"
#include "fmt/format.h"
#include "fmt/options.h"
#include "util/arena.h"
#include "diagnostics/diagnostics.h"

#include <string.h>

static Iron_Arena    arena;
static Iron_DiagList diags;

void setUp(void) {
    arena = iron_arena_create(64 * 1024);
    diags = iron_diaglist_create();
}

void tearDown(void) {
    iron_arena_free(&arena);
    iron_diaglist_free(&diags);
}

/* ── Closes RESEARCH Open Q3: empty source -> ok=true, formatted_len=0 ── */
void test_empty_source_returns_ok_with_zero_length(void) {
    IronFmtResult r = iron_format_source("", "<test>", NULL, &arena, &diags);
    TEST_ASSERT_TRUE(r.ok);
    TEST_ASSERT_EQUAL_INT(0, r.error_count);
    TEST_ASSERT_EQUAL_size_t(0, r.formatted_len);
    TEST_ASSERT_NOT_NULL(r.formatted);                  /* Empty C string, NEVER NULL */
    TEST_ASSERT_EQUAL_STRING("", r.formatted);
}

void test_clean_single_decl_returns_ok(void) {
    const char *src = "func main() {}\n";
    IronFmtResult r = iron_format_source(src, "<test>", NULL, &arena, &diags);
    TEST_ASSERT_TRUE(r.ok);
    TEST_ASSERT_EQUAL_INT(0, r.error_count);
    TEST_ASSERT_TRUE(r.formatted_len > 0);
    TEST_ASSERT_NOT_NULL(r.formatted);
    /* Sanity: formatted source mentions the func name. */
    TEST_ASSERT_NOT_NULL(strstr(r.formatted, "main"));
}

void test_parse_error_returns_not_ok(void) {
    /* "func main(" is unterminated; parser will error. */
    const char *src = "func main(";
    IronFmtResult r = iron_format_source(src, "<test>", NULL, &arena, &diags);
    TEST_ASSERT_FALSE(r.ok);
    TEST_ASSERT_TRUE(r.error_count > 0);
    /* Refusal contract: formatted is "" (empty C string, NEVER NULL). */
    TEST_ASSERT_NOT_NULL(r.formatted);
    TEST_ASSERT_EQUAL_size_t(0, r.formatted_len);
}

void test_lex_error_returns_not_ok(void) {
    /* Unterminated string literal triggers IRON_ERR_UNTERMINATED_STRING. */
    const char *src = "val x = \"unterminated";
    IronFmtResult r = iron_format_source(src, "<test>", NULL, &arena, &diags);
    TEST_ASSERT_FALSE(r.ok);
    TEST_ASSERT_TRUE(r.error_count > 0);
    TEST_ASSERT_NOT_NULL(r.formatted);
    TEST_ASSERT_EQUAL_size_t(0, r.formatted_len);
}

void test_null_opts_equivalent_to_defaults(void) {
    const char *src = "func f() { val x = 1 }\n";

    /* Explicit defaults */
    IronFmtOptions defaults = iron_fmt_options_default();
    IronFmtResult  r1 = iron_format_source(src, "<test>", &defaults, &arena, &diags);
    TEST_ASSERT_TRUE(r1.ok);

    /* Reset diags (the second pass needs a clean slate). */
    iron_diaglist_free(&diags);
    diags = iron_diaglist_create();

    /* NULL opts */
    IronFmtResult  r2 = iron_format_source(src, "<test>", NULL, &arena, &diags);
    TEST_ASSERT_TRUE(r2.ok);

    /* Byte-identical output. */
    TEST_ASSERT_EQUAL_size_t(r1.formatted_len, r2.formatted_len);
    TEST_ASSERT_EQUAL_STRING(r1.formatted, r2.formatted);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_empty_source_returns_ok_with_zero_length);
    RUN_TEST(test_clean_single_decl_returns_ok);
    RUN_TEST(test_parse_error_returns_not_ok);
    RUN_TEST(test_lex_error_returns_not_ok);
    RUN_TEST(test_null_opts_equivalent_to_defaults);
    return UNITY_END();
}
