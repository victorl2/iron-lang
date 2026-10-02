/* test_runtime_collections.c — Unity tests for Iron generic collections.
 *
 * Tests the List[T] macro-generated implementation (Map/Set: test_runtime_hash.c).
 * The collection function names match the codegen monomorphization naming
 * from gen_types.c mangle_generic() exactly.
 */

#include "unity.h"
#include "runtime/iron_runtime.h"

#include <stdlib.h>
#include <string.h>

/* ── Unity boilerplate ───────────────────────────────────────────────────── */

void setUp(void)    { iron_runtime_init(0, NULL); }
void tearDown(void) { iron_runtime_shutdown(); }

/* ── Helper: make an Iron_String from a C string literal ─────────────────── */

static Iron_String S(const char *cstr) {
    return iron_string_from_literal(cstr, strlen(cstr));
}

/* ══════════════════════════════════════════════════════════════════════════
 * List[int64_t] tests
 * ══════════════════════════════════════════════════════════════════════════ */

void test_list_create_empty(void) {
    Iron_List_int64_t list = Iron_List_int64_t_create();
    TEST_ASSERT_EQUAL_INT64(0, Iron_List_int64_t_len(&list));
    Iron_List_int64_t_free(&list);
}

void test_list_push_and_get(void) {
    Iron_List_int64_t list = Iron_List_int64_t_create();
    Iron_List_int64_t_push(&list, 1);
    Iron_List_int64_t_push(&list, 2);
    Iron_List_int64_t_push(&list, 3);
    TEST_ASSERT_EQUAL_INT64(1, Iron_List_int64_t_get(&list, 0));
    TEST_ASSERT_EQUAL_INT64(2, Iron_List_int64_t_get(&list, 1));
    TEST_ASSERT_EQUAL_INT64(3, Iron_List_int64_t_get(&list, 2));
    Iron_List_int64_t_free(&list);
}

void test_list_len(void) {
    Iron_List_int64_t list = Iron_List_int64_t_create();
    for (int64_t i = 0; i < 5; i++) {
        Iron_List_int64_t_push(&list, i * 10);
    }
    TEST_ASSERT_EQUAL_INT64(5, Iron_List_int64_t_len(&list));
    Iron_List_int64_t_free(&list);
}

void test_list_set(void) {
    Iron_List_int64_t list = Iron_List_int64_t_create();
    Iron_List_int64_t_push(&list, 1);
    Iron_List_int64_t_push(&list, 2);
    Iron_List_int64_t_push(&list, 3);
    Iron_List_int64_t_set(&list, 1, 99);
    TEST_ASSERT_EQUAL_INT64(99, Iron_List_int64_t_get(&list, 1));
    /* Other indices unchanged */
    TEST_ASSERT_EQUAL_INT64(1, Iron_List_int64_t_get(&list, 0));
    TEST_ASSERT_EQUAL_INT64(3, Iron_List_int64_t_get(&list, 2));
    Iron_List_int64_t_free(&list);
}

void test_list_pop(void) {
    Iron_List_int64_t list = Iron_List_int64_t_create();
    Iron_List_int64_t_push(&list, 1);
    Iron_List_int64_t_push(&list, 2);
    Iron_List_int64_t_push(&list, 3);
    int64_t val = Iron_List_int64_t_pop(&list);
    TEST_ASSERT_EQUAL_INT64(3, val);
    TEST_ASSERT_EQUAL_INT64(2, Iron_List_int64_t_len(&list));
    Iron_List_int64_t_free(&list);
}

void test_list_grow(void) {
    Iron_List_int64_t list = Iron_List_int64_t_create();
    /* Push 100 items — exercises multiple doublings (8 -> 16 -> 32 -> 64 -> 128) */
    for (int64_t i = 0; i < 100; i++) {
        Iron_List_int64_t_push(&list, i * 7);
    }
    TEST_ASSERT_EQUAL_INT64(100, Iron_List_int64_t_len(&list));
    /* Spot-check several values */
    TEST_ASSERT_EQUAL_INT64(0,   Iron_List_int64_t_get(&list, 0));
    TEST_ASSERT_EQUAL_INT64(7,   Iron_List_int64_t_get(&list, 1));
    TEST_ASSERT_EQUAL_INT64(49,  Iron_List_int64_t_get(&list, 7));
    TEST_ASSERT_EQUAL_INT64(693, Iron_List_int64_t_get(&list, 99));
    Iron_List_int64_t_free(&list);
}

void test_list_string(void) {
    Iron_List_Iron_String list = Iron_List_Iron_String_create();
    Iron_List_Iron_String_push(&list, S("hello"));
    Iron_List_Iron_String_push(&list, S("world"));
    Iron_List_Iron_String_push(&list, S("iron"));

    Iron_String a = Iron_List_Iron_String_get(&list, 0);
    Iron_String b = Iron_List_Iron_String_get(&list, 1);
    Iron_String c = Iron_List_Iron_String_get(&list, 2);
    Iron_String ex_hello = S("hello");
    Iron_String ex_world = S("world");
    Iron_String ex_iron  = S("iron");

    TEST_ASSERT_TRUE(iron_string_equals(&a, &ex_hello));
    TEST_ASSERT_TRUE(iron_string_equals(&b, &ex_world));
    TEST_ASSERT_TRUE(iron_string_equals(&c, &ex_iron));
    Iron_List_Iron_String_free(&list);
}

void test_list_free(void) {
    /* Verifies that free does not crash and resets the list state.
     * ASan will catch any leaks or double-frees if they occur. */
    Iron_List_int64_t list = Iron_List_int64_t_create();
    for (int i = 0; i < 20; i++) {
        Iron_List_int64_t_push(&list, (int64_t)i);
    }
    Iron_List_int64_t_free(&list);
    TEST_ASSERT_EQUAL_INT64(0, list.count);
    TEST_ASSERT_EQUAL_INT64(0, list.capacity);
    TEST_ASSERT_NULL(list.items);
}

/* ══════════════════════════════════════════════════════════════════════════
 * List clone and create_with_capacity tests (COLL-01)
 * ══════════════════════════════════════════════════════════════════════════ */

void test_list_create_with_capacity(void) {
    Iron_List_int64_t list = Iron_List_int64_t_create_with_capacity(16);
    TEST_ASSERT_EQUAL_INT64(0, list.count);
    TEST_ASSERT_EQUAL_INT64(16, list.capacity);
    TEST_ASSERT_NOT_NULL(list.items);
    /* Push should not trigger realloc until capacity is exceeded */
    for (int64_t i = 0; i < 16; i++) {
        Iron_List_int64_t_push(&list, i);
    }
    TEST_ASSERT_EQUAL_INT64(16, list.count);
    TEST_ASSERT_EQUAL_INT64(16, list.capacity);
    Iron_List_int64_t_free(&list);
}

void test_list_create_with_capacity_zero(void) {
    Iron_List_int64_t list = Iron_List_int64_t_create_with_capacity(0);
    TEST_ASSERT_EQUAL_INT64(0, list.count);
    TEST_ASSERT_EQUAL_INT64(0, list.capacity);
    TEST_ASSERT_NULL(list.items);
    /* Should still work when pushing */
    Iron_List_int64_t_push(&list, 42);
    TEST_ASSERT_EQUAL_INT64(1, list.count);
    TEST_ASSERT_EQUAL_INT64(42, Iron_List_int64_t_get(&list, 0));
    Iron_List_int64_t_free(&list);
}

void test_list_clone_independent(void) {
    Iron_List_int64_t orig = Iron_List_int64_t_create();
    Iron_List_int64_t_push(&orig, 10);
    Iron_List_int64_t_push(&orig, 20);
    Iron_List_int64_t_push(&orig, 30);

    Iron_List_int64_t clone = Iron_List_int64_t_clone(&orig);

    /* Clone should have same contents */
    TEST_ASSERT_EQUAL_INT64(3, clone.count);
    TEST_ASSERT_EQUAL_INT64(10, Iron_List_int64_t_get(&clone, 0));
    TEST_ASSERT_EQUAL_INT64(20, Iron_List_int64_t_get(&clone, 1));
    TEST_ASSERT_EQUAL_INT64(30, Iron_List_int64_t_get(&clone, 2));

    /* Modify clone — original should be unchanged */
    Iron_List_int64_t_set(&clone, 0, 999);
    TEST_ASSERT_EQUAL_INT64(999, Iron_List_int64_t_get(&clone, 0));
    TEST_ASSERT_EQUAL_INT64(10,  Iron_List_int64_t_get(&orig, 0));

    /* Modify original — clone should be unchanged */
    Iron_List_int64_t_push(&orig, 40);
    TEST_ASSERT_EQUAL_INT64(4, orig.count);
    TEST_ASSERT_EQUAL_INT64(3, clone.count);

    /* Items pointers must be different (deep copy) */
    TEST_ASSERT_NOT_EQUAL(orig.items, clone.items);

    Iron_List_int64_t_free(&orig);
    Iron_List_int64_t_free(&clone);
}

void test_list_clone_empty(void) {
    Iron_List_int64_t orig = Iron_List_int64_t_create();
    Iron_List_int64_t clone = Iron_List_int64_t_clone(&orig);
    TEST_ASSERT_EQUAL_INT64(0, clone.count);
    TEST_ASSERT_NULL(clone.items);
    Iron_List_int64_t_free(&orig);
    Iron_List_int64_t_free(&clone);
}

void test_list_clone_string(void) {
    Iron_List_Iron_String orig = Iron_List_Iron_String_create();
    Iron_List_Iron_String_push(&orig, S("hello"));
    Iron_List_Iron_String_push(&orig, S("world"));

    Iron_List_Iron_String clone = Iron_List_Iron_String_clone(&orig);
    TEST_ASSERT_EQUAL_INT64(2, clone.count);

    Iron_String ex_hello = S("hello");
    Iron_String ex_world = S("world");
    Iron_String c0 = Iron_List_Iron_String_get(&clone, 0);
    Iron_String c1 = Iron_List_Iron_String_get(&clone, 1);
    TEST_ASSERT_TRUE(iron_string_equals(&c0, &ex_hello));
    TEST_ASSERT_TRUE(iron_string_equals(&c1, &ex_world));

    Iron_List_Iron_String_free(&orig);
    Iron_List_Iron_String_free(&clone);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Free semantics tests
 * ══════════════════════════════════════════════════════════════════════════ */

void test_list_free_resets_state(void) {
    Iron_List_int64_t list = Iron_List_int64_t_create();
    for (int i = 0; i < 20; i++) Iron_List_int64_t_push(&list, (int64_t)i);
    Iron_List_int64_t_free(&list);
    TEST_ASSERT_NULL(list.items);
    TEST_ASSERT_EQUAL_INT64(0, list.count);
    TEST_ASSERT_EQUAL_INT64(0, list.capacity);
    /* Double-free on a zeroed list should be safe (free(NULL) is no-op) */
    Iron_List_int64_t_free(&list);
    TEST_ASSERT_NULL(list.items);
}

/* ══════════════════════════════════════════════════════════════════════════
 * main
 * ══════════════════════════════════════════════════════════════════════════ */

int main(void) {
    UNITY_BEGIN();

    /* List tests */
    RUN_TEST(test_list_create_empty);
    RUN_TEST(test_list_push_and_get);
    RUN_TEST(test_list_len);
    RUN_TEST(test_list_set);
    RUN_TEST(test_list_pop);
    RUN_TEST(test_list_grow);
    RUN_TEST(test_list_string);
    RUN_TEST(test_list_free);

    /* List lifecycle (COLL-01) */
    RUN_TEST(test_list_create_with_capacity);
    RUN_TEST(test_list_create_with_capacity_zero);
    RUN_TEST(test_list_clone_independent);
    RUN_TEST(test_list_clone_empty);
    RUN_TEST(test_list_clone_string);





    /* Free semantics */
    RUN_TEST(test_list_free_resets_state);

    /* Additional types */

    return UNITY_END();
}
