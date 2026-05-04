#include "unity.h"
#include "analyzer/analyzer.h"
#include "lexer/lexer.h"
#include "parser/parser.h"
#include "util/arena.h"
#include "diagnostics/diagnostics.h"
#include "stb_ds.h"

#include <stdatomic.h>
#include <string.h>

static Iron_Arena    arena;
static Iron_DiagList diags;

void setUp(void) {
    arena = iron_arena_create(131072);
    diags = iron_diaglist_create();
}

void tearDown(void) {
    iron_diaglist_free(&diags);
    iron_arena_free(&arena);
}

/* HARD-01: iron_analyze_buffer accepts the locked 7-param signature and
 * returns the same Iron_AnalyzeResult shape as iron_analyze. */
void test_iron_analyze_buffer_well_formed_input(void) {
    const char *src = "func main() -> Int { return 0 }\n";
    Iron_AnalyzeResult r = iron_analyze_buffer(
        src, strlen(src), "basic.iron",
        IRON_ANALYSIS_MODE_CLI,
        &arena, &diags, NULL,
        0);
    TEST_ASSERT_EQUAL_INT(0, diags.error_count);
    TEST_ASSERT_NOT_NULL(r.global_scope);
    TEST_ASSERT_FALSE(r.has_errors);
}

/* HARD-05 skeleton: passing a pre-signaled cancel flag causes early return. */
void test_iron_analyze_buffer_pre_cancelled_returns_early(void) {
    const char *src = "func main() -> Int { return 0 }\n";
    _Atomic bool cancel = true;
    Iron_AnalyzeResult r = iron_analyze_buffer(
        src, strlen(src), "cancel.iron",
        IRON_ANALYSIS_MODE_CLI,
        &arena, &diags, &cancel,
        0);
    /* Before Plan 03 wires poll sites, the pre-cancel check at entry is
     * the only observed cancellation. Partial result is NULL global scope. */
    TEST_ASSERT_NULL(r.global_scope);
}

/* HARD-01: NULL cancel flag behaves as "never cancel". */
void test_iron_analyze_buffer_null_cancel_flag(void) {
    const char *src = "func main() -> Int { return 0 }\n";
    Iron_AnalyzeResult r = iron_analyze_buffer(
        src, strlen(src), "nullcancel.iron",
        IRON_ANALYSIS_MODE_CLI,
        &arena, &diags, NULL,
        0);
    TEST_ASSERT_NOT_NULL(r.global_scope);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_iron_analyze_buffer_well_formed_input);
    RUN_TEST(test_iron_analyze_buffer_pre_cancelled_returns_early);
    RUN_TEST(test_iron_analyze_buffer_null_cancel_flag);
    return UNITY_END();
}
