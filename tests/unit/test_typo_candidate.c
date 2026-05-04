/* test_typo_candidate.c — Phase 4 Plan 04-01 Task 01 (EDIT-07).
 *
 * LIVE tests for iron_levenshtein + iron_best_typo_candidate. The helper
 * is ready in Task 01 (no stub needed); the 5 emit-site enrichments land
 * in Task 02 and flip the other four new unit-test TUs to live.
 *
 * Covers:
 *   1. `iron_levenshtein("println", "prinln", 2)` returns 1
 *   2. `iron_levenshtein("foo", "bar", 2)` returns > 2 (exceeds threshold)
 *   3. `iron_best_typo_candidate` with no match in scope returns NULL
 *   4. `iron_best_typo_candidate` with a visible near-miss returns that name
 */

#include "unity.h"
#include "analyzer/typo_candidate.h"
#include "analyzer/scope.h"
#include "analyzer/types.h"
#include "util/arena.h"
#include "diagnostics/diagnostics.h"

#include <string.h>
#include <limits.h>

static Iron_Arena g_arena;

void setUp(void) {
    g_arena = iron_arena_create(4096);
}

void tearDown(void) {
    iron_arena_free(&g_arena);
}

/* Test 1: one-edit distance between real identifiers. */
static void test_levenshtein_one_edit(void) {
    int d = iron_levenshtein("println", "prinln", 2);
    TEST_ASSERT_EQUAL_INT(1, d);
}

/* Test 2: over-threshold inputs trigger early-exit to max_dist+1. */
static void test_levenshtein_over_threshold(void) {
    int d = iron_levenshtein("foo", "bar", 2);
    TEST_ASSERT_GREATER_THAN_INT(2, d);
}

/* Test 3: empty scope => NULL suggestion (no candidate within distance). */
static void test_best_candidate_empty_scope(void) {
    Iron_Scope *s = iron_scope_create(&g_arena, NULL, IRON_SCOPE_GLOBAL);
    TEST_ASSERT_NOT_NULL(s);
    const char *sug = iron_best_typo_candidate(s, &g_arena, "foo");
    TEST_ASSERT_NULL(sug);
}

/* Test 4: a visible symbol within distance 2 is returned (arena-strdup'd). */
static void test_best_candidate_finds_near_miss(void) {
    Iron_Scope *s = iron_scope_create(&g_arena, NULL, IRON_SCOPE_GLOBAL);
    TEST_ASSERT_NOT_NULL(s);

    /* Define a symbol "println" in the scope. */
    Iron_Span sp = iron_span_make("test.iron", 1, 1, 1, 8);
    Iron_Symbol *sym = iron_symbol_create(&g_arena, "println",
                                           IRON_SYM_FUNCTION, NULL, sp);
    TEST_ASSERT_NOT_NULL(sym);
    TEST_ASSERT_TRUE(iron_scope_define(s, &g_arena, sym));

    /* Typo "prinln" should return "println" (distance 1). */
    const char *sug = iron_best_typo_candidate(s, &g_arena, "prinln");
    TEST_ASSERT_NOT_NULL(sug);
    TEST_ASSERT_EQUAL_STRING("println", sug);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_levenshtein_one_edit);
    RUN_TEST(test_levenshtein_over_threshold);
    RUN_TEST(test_best_candidate_empty_scope);
    RUN_TEST(test_best_candidate_finds_near_miss);
    return UNITY_END();
}
