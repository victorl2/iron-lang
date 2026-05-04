/* test_type_mismatch_literal.c — Phase 4 Plan 04-01 Task 02 (EDIT-07).
 *
 * LIVE — exercises the emit_type_mismatch_maybe_literal helper in
 * src/analyzer/typecheck.c. Asserts that a literal-position RHS
 * emits IRON_ERR_TYPE_MISMATCH_LITERAL (code 235, not 202) with a
 * retyped-literal .suggestion, and that a non-literal RHS continues
 * to emit IRON_ERR_TYPE_MISMATCH (code 202) with .suggestion NULL.
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

/* Int literal RHS bound to Float-typed var -> code 235 + "42.0" suggestion. */
static void test_int_to_float_literal_emits_235(void) {
    const char *src =
        "func main() {\n"
        "  val x: Float = 42\n"
        "}\n";
    analyze(src);
    const Iron_Diagnostic *d235 = find_diag(&g_diags, IRON_ERR_TYPE_MISMATCH_LITERAL);
    TEST_ASSERT_NOT_NULL(d235);
    TEST_ASSERT_NOT_NULL(d235->suggestion);
    TEST_ASSERT_EQUAL_STRING("42.0", d235->suggestion);

    /* And code 202 must NOT be emitted for the same fixture. */
    TEST_ASSERT_NULL(find_diag(&g_diags, IRON_ERR_TYPE_MISMATCH));
}

/* Non-literal RHS (a bool-typed variable) bound to Int annotation stays at
 * code 202 (general type-mismatch) with .suggestion==NULL. */
static void test_non_literal_mismatch_stays_202(void) {
    const char *src =
        "func main() {\n"
        "  val b: Bool = true\n"
        "  val x: Int = b\n"
        "}\n";
    analyze(src);
    const Iron_Diagnostic *d202 = find_diag(&g_diags, IRON_ERR_TYPE_MISMATCH);
    TEST_ASSERT_NOT_NULL(d202);
    TEST_ASSERT_NULL(d202->suggestion);
    TEST_ASSERT_NULL(find_diag(&g_diags, IRON_ERR_TYPE_MISMATCH_LITERAL));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_int_to_float_literal_emits_235);
    RUN_TEST(test_non_literal_mismatch_stays_202);
    return UNITY_END();
}
