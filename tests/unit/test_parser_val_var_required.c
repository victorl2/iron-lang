/* Phase 17 Wave 0: TDD scaffold for VAL-01 (bare local) + VAL-02 (bare field).
 *
 * VAL-01 emits at resolver level (src/analyzer/resolve.c:emit_undefined) —
 * the parser lookahead originally proposed in CONTEXT.md was reverted after
 * a regression sweep proved it cannot distinguish a NEW binding from
 * REASSIGNMENT to an existing `var`. The resolver knows scope, so it can
 * fire IRON_ERR_MISSING_VAL_VAR exactly when the IDENT on an assign-LHS
 * is undefined.
 *
 * VAL-02 emits at parser level (src/parser/parser.c:3702-3708) — fields
 * are decl-position only, no scope ambiguity.
 *
 * Test driver therefore runs the FULL analyzer pipeline via
 * iron_analyze_buffer so both emission sites surface uniformly. */
#include "unity.h"
#include "analyzer/analyzer.h"
#include "diagnostics/diagnostics.h"
#include "util/arena.h"
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

/* Run the full analyzer pipeline on `src`; return diags.error_count. */
static int parse_and_count_errors(const char *src) {
    Iron_AnalyzeResult r = iron_analyze_buffer(
        src, strlen(src), "test.iron",
        IRON_ANALYSIS_MODE_CLI,
        &arena, &diags, NULL,
        0);
    (void)r;
    return diags.error_count;
}

/* True iff at least one diag in diags carries the requested code. */
static bool has_error_with_code(int target) {
    for (int i = 0; i < arrlen(diags.items); i++) {
        if (diags.items[i].code == target) return true;
    }
    return false;
}

/* Find first diag with given code; return its message (or NULL). */
static const char *diag_message_for_code(int target) {
    for (int i = 0; i < arrlen(diags.items); i++) {
        if (diags.items[i].code == target) return diags.items[i].message;
    }
    return NULL;
}

/* Find first diag with given code; return its suggestion (or NULL). */
static const char *diag_suggestion_for_code(int target) {
    for (int i = 0; i < arrlen(diags.items); i++) {
        if (diags.items[i].code == target) return diags.items[i].suggestion;
    }
    return NULL;
}

/* -- VAL-01: bare local binding rejection -- */

void test_val_01_bare_local_assign_rejected(void) {
    TEST_ASSERT_GREATER_THAN_INT(0, parse_and_count_errors(
        "func main() {\n    x = 30\n}\n"));
    TEST_ASSERT_TRUE(has_error_with_code(IRON_ERR_MISSING_VAL_VAR));
}

void test_val_01_message_substring(void) {
    (void)parse_and_count_errors("func main() {\n    x = 30\n}\n");
    const char *msg = diag_message_for_code(IRON_ERR_MISSING_VAL_VAR);
    TEST_ASSERT_NOT_NULL(msg);
    TEST_ASSERT_NOT_NULL(strstr(msg, "must specify val or var"));
}

void test_val_01_hint_present(void) {
    (void)parse_and_count_errors("func main() {\n    x = 30\n}\n");
    const char *hint = diag_suggestion_for_code(IRON_ERR_MISSING_VAL_VAR);
    TEST_ASSERT_NOT_NULL(hint);
    TEST_ASSERT_NOT_NULL(strstr(hint, "insert 'val'"));
}

void test_val_01_no_cascade(void) {
    /* RESEARCH Pitfall 1: lookahead must trigger BEFORE iron_parse_expr
     * consumes the IDENT, otherwise a spurious IRON_ERR_UNDEFINED_VAR
     * cascades on the same span. Exact-1 assertion guards this. */
    TEST_ASSERT_EQUAL_INT(1, parse_and_count_errors(
        "func main() {\n    x = 30\n}\n"));
}

/* -- VAL-02: bare field declaration rejection -- */

void test_val_02_bare_field_decl_rejected(void) {
    TEST_ASSERT_GREATER_THAN_INT(0, parse_and_count_errors(
        "object Counter {\n    hp: Int\n}\n"));
    TEST_ASSERT_TRUE(has_error_with_code(IRON_ERR_MISSING_VAL_VAR));
}

void test_val_02_message_substring(void) {
    (void)parse_and_count_errors("object Counter {\n    hp: Int\n}\n");
    const char *msg = diag_message_for_code(IRON_ERR_MISSING_VAL_VAR);
    TEST_ASSERT_NOT_NULL(msg);
    TEST_ASSERT_NOT_NULL(strstr(msg, "must specify val or var"));
}

void test_val_02_hint_present(void) {
    (void)parse_and_count_errors("object Counter {\n    hp: Int\n}\n");
    const char *hint = diag_suggestion_for_code(IRON_ERR_MISSING_VAL_VAR);
    TEST_ASSERT_NOT_NULL(hint);
    TEST_ASSERT_NOT_NULL(strstr(hint, "insert 'val'"));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_val_01_bare_local_assign_rejected);
    RUN_TEST(test_val_01_message_substring);
    RUN_TEST(test_val_01_hint_present);
    RUN_TEST(test_val_01_no_cascade);
    RUN_TEST(test_val_02_bare_field_decl_rejected);
    RUN_TEST(test_val_02_message_substring);
    RUN_TEST(test_val_02_hint_present);
    return UNITY_END();
}
