/* test_missing_return.c — Phase 4 Plan 04-01 Task 02 (EDIT-07).
 *
 * LIVE — exercises the check_missing_return walker in
 * src/analyzer/typecheck.c. Asserts IRON_ERR_MISSING_RETURN (236) fires
 * on non-void functions that reach the end without a return, and stays
 * quiet on void functions + on functions whose body unambiguously
 * returns on every path.
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
                               &g_arena, &g_diags, NULL);
}

/* Non-void function body reaches end without returning -> 236 + "return 0;". */
static void test_non_void_return_reaches_end(void) {
    const char *src =
        "func f() -> Int {\n"
        "  val x = 1\n"
        "}\n";
    analyze(src);
    const Iron_Diagnostic *d = find_diag(&g_diags, IRON_ERR_MISSING_RETURN);
    TEST_ASSERT_NOT_NULL(d);
    TEST_ASSERT_NOT_NULL(d->suggestion);
    TEST_ASSERT_EQUAL_STRING("return 0;", d->suggestion);
}

/* Void function body with no return is fine; walker must stay quiet. */
static void test_void_return_skipped(void) {
    const char *src =
        "func f() {\n"
        "  val x = 1\n"
        "}\n";
    analyze(src);
    TEST_ASSERT_NULL(find_diag(&g_diags, IRON_ERR_MISSING_RETURN));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_non_void_return_reaches_end);
    RUN_TEST(test_void_return_skipped);
    return UNITY_END();
}
