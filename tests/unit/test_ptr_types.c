/* Phase 25 Wave 0 (Plan 25-01): TDD scaffold for PTR-02 — the is_unchecked
 * flag makes *T and *unchecked T distinct types, and iron_type_equals must
 * return false for cross-regime pairs.
 *
 * Tests the 4-way matrix: *T, *var T, *unchecked T, *var unchecked T.
 * Six unique unordered pairs:
 *   (T1,T2), (T1,T3), (T1,T4), (T2,T3), (T2,T4), (T3,T4) — all must be false.
 *   Each type must equal itself — four true assertions.
 *
 * RESEARCH Pitfall 2: without is_unchecked in iron_type_equals, T1==T3 and
 * T2==T4 would silently pass, breaking regime isolation. */
#include "unity.h"
#include "analyzer/types.h"
#include "util/arena.h"

static Iron_Arena arena;

void setUp(void) {
    arena = iron_arena_create(131072);
    iron_types_init(&arena);
}

void tearDown(void) {
    iron_arena_free(&arena);
}

/* PTR-02: four distinct pointer types from the (is_var, is_unchecked) matrix.
 *
 * T1 = *Int          (is_var=false, is_unchecked=false)
 * T2 = *var Int      (is_var=true,  is_unchecked=false)
 * T3 = *unchecked Int(is_var=false, is_unchecked=true)
 * T4 = *var unchecked Int (is_var=true, is_unchecked=true)
 */
void test_ptr_unchecked_is_distinct_from_checked(void) {
    Iron_Type *int_t = iron_type_make_primitive(IRON_TYPE_INT);
    TEST_ASSERT_NOT_NULL(int_t);

    Iron_Type *t1 = iron_type_make_ptr(&arena, int_t, false, false); /* *Int */
    Iron_Type *t2 = iron_type_make_ptr(&arena, int_t, true,  false); /* *var Int */
    Iron_Type *t3 = iron_type_make_ptr(&arena, int_t, false, true);  /* *unchecked Int */
    Iron_Type *t4 = iron_type_make_ptr(&arena, int_t, true,  true);  /* *var unchecked Int */

    TEST_ASSERT_NOT_NULL(t1);
    TEST_ASSERT_NOT_NULL(t2);
    TEST_ASSERT_NOT_NULL(t3);
    TEST_ASSERT_NOT_NULL(t4);

    /* Self-equality */
    TEST_ASSERT_TRUE(iron_type_equals(t1, t1));
    TEST_ASSERT_TRUE(iron_type_equals(t2, t2));
    TEST_ASSERT_TRUE(iron_type_equals(t3, t3));
    TEST_ASSERT_TRUE(iron_type_equals(t4, t4));

    /* All six cross-pairs must be FALSE */
    TEST_ASSERT_FALSE(iron_type_equals(t1, t2)); /* *Int != *var Int */
    TEST_ASSERT_FALSE(iron_type_equals(t1, t3)); /* *Int != *unchecked Int */
    TEST_ASSERT_FALSE(iron_type_equals(t1, t4)); /* *Int != *var unchecked Int */
    TEST_ASSERT_FALSE(iron_type_equals(t2, t3)); /* *var Int != *unchecked Int */
    TEST_ASSERT_FALSE(iron_type_equals(t2, t4)); /* *var Int != *var unchecked Int */
    TEST_ASSERT_FALSE(iron_type_equals(t3, t4)); /* *unchecked Int != *var unchecked Int */
}

/* PTR-02: structural equality within the same regime (same pointee). */
void test_ptr_unchecked_structural_equality(void) {
    Iron_Type *int_t = iron_type_make_primitive(IRON_TYPE_INT);

    Iron_Type *a = iron_type_make_ptr(&arena, int_t, false, true);
    Iron_Type *b = iron_type_make_ptr(&arena, int_t, false, true);

    /* Two independently created *unchecked Int must be equal (structural). */
    TEST_ASSERT_TRUE(iron_type_equals(a, b));
}

/* PTR-02: cross-pointee *unchecked pairs are NOT equal even in same regime. */
void test_ptr_unchecked_cross_pointee_not_equal(void) {
    Iron_Type *int_t  = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_Type *bool_t = iron_type_make_primitive(IRON_TYPE_BOOL);

    Iron_Type *p_int  = iron_type_make_ptr(&arena, int_t,  false, true);
    Iron_Type *p_bool = iron_type_make_ptr(&arena, bool_t, false, true);

    TEST_ASSERT_FALSE(iron_type_equals(p_int, p_bool));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_ptr_unchecked_is_distinct_from_checked);
    RUN_TEST(test_ptr_unchecked_structural_equality);
    RUN_TEST(test_ptr_unchecked_cross_pointee_not_equal);
    return UNITY_END();
}
