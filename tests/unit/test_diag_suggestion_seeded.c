/* test_diag_suggestion_seeded.c — Phase 4 Plan 04-01 Task 02 (EDIT-07).
 *
 * LIVE — asserts `.suggestion != NULL` for every P1 diagnostic code emitted
 * by the compiler frontend after Task 02:
 *   IRON_ERR_UNDEFINED_VAR         (200)  — resolver typo candidate
 *   IRON_ERR_TYPE_MISMATCH_LITERAL (235)  — literal-position RHS mismatch
 *   IRON_ERR_MISSING_RETURN        (236)  — missing-return walker
 *   IRON_WARN_UNUSED_IMPORT        (611)  — unused-import walker
 *   IRON_WARN_REDUNDANT_CAST       (612)  — redundant-cast check
 *
 * Each test drives `iron_analyze_buffer` with a CLI-mode fixture that
 * triggers exactly one of the five codes, then scans the resulting
 * diag list for a matching entry with .suggestion non-NULL.
 */

#include "unity.h"
#include "analyzer/analyzer.h"
#include "diagnostics/diagnostics.h"
#include "util/arena.h"

#include <stddef.h>
#include <string.h>

static Iron_Arena    g_arena;
static Iron_DiagList g_diags;

void setUp(void) {
    g_arena = iron_arena_create(1024 * 256);
    g_diags = iron_diaglist_create();
}

void tearDown(void) {
    iron_arena_free(&g_arena);
    iron_diaglist_free(&g_diags);
}

/* Helper: find a diagnostic by code; return pointer or NULL. */
static const Iron_Diagnostic *find_diag(const Iron_DiagList *dl, int code) {
    for (int i = 0; i < dl->count; i++) {
        if (dl->items[i].code == code) return &dl->items[i];
    }
    return NULL;
}

static void analyze(const char *src) {
    (void)iron_analyze_buffer(src, strlen(src), "test.iron",
                               IRON_ANALYSIS_MODE_CLI,
                               &g_arena, &g_diags, NULL,
        0);
}

static void test_undefined_var_has_suggestion(void) {
    /* "println" is a builtin; "prinln" is a typo within distance 1. */
    const char *src =
        "func main() {\n"
        "  prinln(\"hi\")\n"
        "}\n";
    analyze(src);
    const Iron_Diagnostic *d = find_diag(&g_diags, IRON_ERR_UNDEFINED_VAR);
    TEST_ASSERT_NOT_NULL_MESSAGE(d, "expected IRON_ERR_UNDEFINED_VAR to fire");
    TEST_ASSERT_NOT_NULL_MESSAGE(d->suggestion,
        "undefined-var diagnostic must carry a typo-candidate suggestion");
}

static void test_type_mismatch_literal_has_suggestion(void) {
    /* val x: Float = 42 — literal RHS of Int bound to Float annotation. */
    const char *src =
        "func main() {\n"
        "  val x: Float = 42\n"
        "}\n";
    analyze(src);
    const Iron_Diagnostic *d = find_diag(&g_diags, IRON_ERR_TYPE_MISMATCH_LITERAL);
    TEST_ASSERT_NOT_NULL_MESSAGE(d, "expected code 235 to fire for literal RHS");
    TEST_ASSERT_NOT_NULL_MESSAGE(d->suggestion,
        "literal-position mismatch must carry retyped-literal suggestion");
}

static void test_missing_return_has_suggestion(void) {
    /* Non-void return type without a return in the body. */
    const char *src =
        "func f() -> Int {\n"
        "  val x = 1\n"
        "}\n";
    analyze(src);
    const Iron_Diagnostic *d = find_diag(&g_diags, IRON_ERR_MISSING_RETURN);
    TEST_ASSERT_NOT_NULL_MESSAGE(d, "expected code 236 to fire");
    TEST_ASSERT_NOT_NULL_MESSAGE(d->suggestion,
        "missing-return must carry a 'return 0;' suggestion");
}

static void test_unused_import_has_suggestion(void) {
    /* Aliased import never referenced in the module. */
    const char *src =
        "import std.math as m\n"
        "func main() {}\n";
    analyze(src);
    const Iron_Diagnostic *d = find_diag(&g_diags, IRON_WARN_UNUSED_IMPORT);
    TEST_ASSERT_NOT_NULL_MESSAGE(d, "expected code 611 to fire");
    TEST_ASSERT_NOT_NULL_MESSAGE(d->suggestion,
        "unused-import must carry a non-NULL suggestion (empty-string sentinel)");
}

static void test_redundant_cast_has_suggestion(void) {
    /* Float(1.0) — cast of a Float literal to Float. */
    const char *src =
        "func main() {\n"
        "  val x = Float(1.0)\n"
        "}\n";
    analyze(src);
    const Iron_Diagnostic *d = find_diag(&g_diags, IRON_WARN_REDUNDANT_CAST);
    TEST_ASSERT_NOT_NULL_MESSAGE(d, "expected code 612 to fire");
    TEST_ASSERT_NOT_NULL_MESSAGE(d->suggestion,
        "redundant-cast must carry the inner-expression text as suggestion");
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_undefined_var_has_suggestion);
    RUN_TEST(test_type_mismatch_literal_has_suggestion);
    RUN_TEST(test_missing_return_has_suggestion);
    RUN_TEST(test_unused_import_has_suggestion);
    RUN_TEST(test_redundant_cast_has_suggestion);
    return UNITY_END();
}
