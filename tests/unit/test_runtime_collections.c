/* test_runtime_collections.c — Unity tests for Iron generic collections.
 *
 * Tests List[T], Map[K,V], and Set[T] macro-generated implementations.
 * The collection function names match the codegen monomorphization naming
 * from gen_types.c mangle_generic() exactly.
 */

#include "unity.h"
#include "runtime/iron_runtime.h"

#include <stdlib.h>
#include <string.h>

/* ── Unity boilerplate ───────────────────────────────────────────────────── */

void setUp(void)    { iron_runtime_init(); }
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
 * Map[Iron_String, int64_t] tests
 * ══════════════════════════════════════════════════════════════════════════ */

void test_map_put_and_get(void) {
    Iron_Map_Iron_String_int64_t map = Iron_Map_Iron_String_int64_t_create();
    Iron_Map_Iron_String_int64_t_put(&map, S("key1"), 42);
    TEST_ASSERT_EQUAL_INT64(42, Iron_Map_Iron_String_int64_t_get(&map, S("key1")));
    Iron_Map_Iron_String_int64_t_free(&map);
}

void test_map_has(void) {
    Iron_Map_Iron_String_int64_t map = Iron_Map_Iron_String_int64_t_create();
    Iron_Map_Iron_String_int64_t_put(&map, S("present"), 100);
    TEST_ASSERT_TRUE(Iron_Map_Iron_String_int64_t_has(&map, S("present")));
    TEST_ASSERT_FALSE(Iron_Map_Iron_String_int64_t_has(&map, S("absent")));
    Iron_Map_Iron_String_int64_t_free(&map);
}

void test_map_update(void) {
    Iron_Map_Iron_String_int64_t map = Iron_Map_Iron_String_int64_t_create();
    Iron_Map_Iron_String_int64_t_put(&map, S("x"), 10);
    Iron_Map_Iron_String_int64_t_put(&map, S("x"), 20); /* second put overwrites */
    TEST_ASSERT_EQUAL_INT64(20, Iron_Map_Iron_String_int64_t_get(&map, S("x")));
    TEST_ASSERT_EQUAL_INT64(1,  Iron_Map_Iron_String_int64_t_len(&map));
    Iron_Map_Iron_String_int64_t_free(&map);
}

void test_map_remove(void) {
    Iron_Map_Iron_String_int64_t map = Iron_Map_Iron_String_int64_t_create();
    Iron_Map_Iron_String_int64_t_put(&map, S("alpha"), 1);
    Iron_Map_Iron_String_int64_t_put(&map, S("beta"),  2);
    Iron_Map_Iron_String_int64_t_remove(&map, S("alpha"));
    TEST_ASSERT_FALSE(Iron_Map_Iron_String_int64_t_has(&map, S("alpha")));
    TEST_ASSERT_TRUE(Iron_Map_Iron_String_int64_t_has(&map, S("beta")));
    Iron_Map_Iron_String_int64_t_free(&map);
}

void test_map_len(void) {
    Iron_Map_Iron_String_int64_t map = Iron_Map_Iron_String_int64_t_create();
    Iron_Map_Iron_String_int64_t_put(&map, S("a"), 1);
    Iron_Map_Iron_String_int64_t_put(&map, S("b"), 2);
    Iron_Map_Iron_String_int64_t_put(&map, S("c"), 3);
    TEST_ASSERT_EQUAL_INT64(3, Iron_Map_Iron_String_int64_t_len(&map));
    Iron_Map_Iron_String_int64_t_free(&map);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Set[int64_t] tests
 * ══════════════════════════════════════════════════════════════════════════ */

void test_set_add_and_contains(void) {
    Iron_Set_int64_t set = Iron_Set_int64_t_create();
    Iron_Set_int64_t_add(&set, 1);
    Iron_Set_int64_t_add(&set, 2);
    Iron_Set_int64_t_add(&set, 3);
    TEST_ASSERT_TRUE(Iron_Set_int64_t_contains(&set, 2));
    TEST_ASSERT_FALSE(Iron_Set_int64_t_contains(&set, 5));
    Iron_Set_int64_t_free(&set);
}

void test_set_no_duplicates(void) {
    Iron_Set_int64_t set = Iron_Set_int64_t_create();
    Iron_Set_int64_t_add(&set, 42);
    Iron_Set_int64_t_add(&set, 42); /* duplicate — must not grow count */
    TEST_ASSERT_EQUAL_INT64(1, Iron_Set_int64_t_len(&set));
    Iron_Set_int64_t_free(&set);
}

void test_set_remove(void) {
    Iron_Set_int64_t set = Iron_Set_int64_t_create();
    Iron_Set_int64_t_add(&set, 10);
    Iron_Set_int64_t_add(&set, 20);
    Iron_Set_int64_t_remove(&set, 10);
    TEST_ASSERT_FALSE(Iron_Set_int64_t_contains(&set, 10));
    TEST_ASSERT_TRUE(Iron_Set_int64_t_contains(&set, 20));
    TEST_ASSERT_EQUAL_INT64(1, Iron_Set_int64_t_len(&set));
    Iron_Set_int64_t_free(&set);
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
 * Map clone and create_with_capacity tests (COLL-02)
 * ══════════════════════════════════════════════════════════════════════════ */

void test_map_create_with_capacity(void) {
    Iron_Map_Iron_String_int64_t map = Iron_Map_Iron_String_int64_t_create_with_capacity(16);
    TEST_ASSERT_EQUAL_INT64(0, map.count);
    TEST_ASSERT_EQUAL_INT64(16, map.capacity);
    TEST_ASSERT_NOT_NULL(map.keys);
    TEST_ASSERT_NOT_NULL(map.values);
    Iron_Map_Iron_String_int64_t_free(&map);
}

void test_map_clone_independent(void) {
    Iron_Map_Iron_String_int64_t orig = Iron_Map_Iron_String_int64_t_create();
    Iron_Map_Iron_String_int64_t_put(&orig, S("a"), 1);
    Iron_Map_Iron_String_int64_t_put(&orig, S("b"), 2);

    Iron_Map_Iron_String_int64_t clone = Iron_Map_Iron_String_int64_t_clone(&orig);

    /* Clone should have same contents */
    TEST_ASSERT_EQUAL_INT64(2, clone.count);
    TEST_ASSERT_EQUAL_INT64(1, Iron_Map_Iron_String_int64_t_get(&clone, S("a")));
    TEST_ASSERT_EQUAL_INT64(2, Iron_Map_Iron_String_int64_t_get(&clone, S("b")));

    /* Modify clone — original should be unchanged */
    Iron_Map_Iron_String_int64_t_put(&clone, S("a"), 99);
    TEST_ASSERT_EQUAL_INT64(99, Iron_Map_Iron_String_int64_t_get(&clone, S("a")));
    TEST_ASSERT_EQUAL_INT64(1,  Iron_Map_Iron_String_int64_t_get(&orig, S("a")));

    /* Keys/values pointers must be different (deep copy) */
    TEST_ASSERT_NOT_EQUAL(orig.keys,   clone.keys);
    TEST_ASSERT_NOT_EQUAL(orig.values, clone.values);

    Iron_Map_Iron_String_int64_t_free(&orig);
    Iron_Map_Iron_String_int64_t_free(&clone);
}

void test_map_clone_empty(void) {
    Iron_Map_Iron_String_int64_t orig = Iron_Map_Iron_String_int64_t_create();
    Iron_Map_Iron_String_int64_t clone = Iron_Map_Iron_String_int64_t_clone(&orig);
    TEST_ASSERT_EQUAL_INT64(0, clone.count);
    TEST_ASSERT_NULL(clone.keys);
    TEST_ASSERT_NULL(clone.values);
    Iron_Map_Iron_String_int64_t_free(&orig);
    Iron_Map_Iron_String_int64_t_free(&clone);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Set clone and create_with_capacity tests (COLL-03)
 * ══════════════════════════════════════════════════════════════════════════ */

void test_set_create_with_capacity(void) {
    Iron_Set_int64_t set = Iron_Set_int64_t_create_with_capacity(16);
    TEST_ASSERT_EQUAL_INT64(0, set.count);
    TEST_ASSERT_EQUAL_INT64(16, set.capacity);
    TEST_ASSERT_NOT_NULL(set.items);
    Iron_Set_int64_t_free(&set);
}

void test_set_clone_independent(void) {
    Iron_Set_int64_t orig = Iron_Set_int64_t_create();
    Iron_Set_int64_t_add(&orig, 10);
    Iron_Set_int64_t_add(&orig, 20);
    Iron_Set_int64_t_add(&orig, 30);

    Iron_Set_int64_t clone = Iron_Set_int64_t_clone(&orig);

    /* Clone should have same contents */
    TEST_ASSERT_EQUAL_INT64(3, clone.count);
    TEST_ASSERT_TRUE(Iron_Set_int64_t_contains(&clone, 10));
    TEST_ASSERT_TRUE(Iron_Set_int64_t_contains(&clone, 20));
    TEST_ASSERT_TRUE(Iron_Set_int64_t_contains(&clone, 30));

    /* Modify clone — original should be unchanged */
    Iron_Set_int64_t_remove(&clone, 10);
    TEST_ASSERT_FALSE(Iron_Set_int64_t_contains(&clone, 10));
    TEST_ASSERT_TRUE(Iron_Set_int64_t_contains(&orig, 10));

    /* Items pointers must be different (deep copy) */
    TEST_ASSERT_NOT_EQUAL(orig.items, clone.items);

    Iron_Set_int64_t_free(&orig);
    Iron_Set_int64_t_free(&clone);
}

void test_set_clone_empty(void) {
    Iron_Set_int64_t orig = Iron_Set_int64_t_create();
    Iron_Set_int64_t clone = Iron_Set_int64_t_clone(&orig);
    TEST_ASSERT_EQUAL_INT64(0, clone.count);
    TEST_ASSERT_NULL(clone.items);
    Iron_Set_int64_t_free(&orig);
    Iron_Set_int64_t_free(&clone);
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

void test_map_free_resets_state(void) {
    Iron_Map_Iron_String_int64_t map = Iron_Map_Iron_String_int64_t_create();
    Iron_Map_Iron_String_int64_t_put(&map, S("x"), 1);
    Iron_Map_Iron_String_int64_t_free(&map);
    TEST_ASSERT_NULL(map.keys);
    TEST_ASSERT_NULL(map.values);
    TEST_ASSERT_EQUAL_INT64(0, map.count);
    TEST_ASSERT_EQUAL_INT64(0, map.capacity);
    /* Double-free should be safe */
    Iron_Map_Iron_String_int64_t_free(&map);
}

void test_set_free_resets_state(void) {
    Iron_Set_int64_t set = Iron_Set_int64_t_create();
    Iron_Set_int64_t_add(&set, 42);
    Iron_Set_int64_t_free(&set);
    TEST_ASSERT_NULL(set.items);
    TEST_ASSERT_EQUAL_INT64(0, set.count);
    TEST_ASSERT_EQUAL_INT64(0, set.capacity);
    /* Double-free should be safe */
    Iron_Set_int64_t_free(&set);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Additional coverage: Map[String, String] and Set[Iron_String]
 * ══════════════════════════════════════════════════════════════════════════ */

void test_map_string_string(void) {
    Iron_Map_Iron_String_Iron_String map = Iron_Map_Iron_String_Iron_String_create();
    Iron_Map_Iron_String_Iron_String_put(&map, S("lang"), S("iron"));
    Iron_Map_Iron_String_Iron_String_put(&map, S("year"), S("2025"));
    Iron_String lang_key = S("lang");
    TEST_ASSERT_TRUE(Iron_Map_Iron_String_Iron_String_has(&map, lang_key));
    Iron_String val      = Iron_Map_Iron_String_Iron_String_get(&map, S("lang"));
    Iron_String expected = S("iron");
    TEST_ASSERT_TRUE(iron_string_equals(&val, &expected));
    Iron_Map_Iron_String_Iron_String_free(&map);
}

void test_set_string(void) {
    Iron_Set_Iron_String set = Iron_Set_Iron_String_create();
    Iron_Set_Iron_String_add(&set, S("apple"));
    Iron_Set_Iron_String_add(&set, S("banana"));
    Iron_Set_Iron_String_add(&set, S("apple")); /* duplicate */
    TEST_ASSERT_EQUAL_INT64(2, Iron_Set_Iron_String_len(&set));
    TEST_ASSERT_TRUE(Iron_Set_Iron_String_contains(&set, S("banana")));
    TEST_ASSERT_FALSE(Iron_Set_Iron_String_contains(&set, S("cherry")));
    Iron_Set_Iron_String_free(&set);
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

    /* Map tests */
    RUN_TEST(test_map_put_and_get);
    RUN_TEST(test_map_has);
    RUN_TEST(test_map_update);
    RUN_TEST(test_map_remove);
    RUN_TEST(test_map_len);

    /* Map lifecycle (COLL-02) */
    RUN_TEST(test_map_create_with_capacity);
    RUN_TEST(test_map_clone_independent);
    RUN_TEST(test_map_clone_empty);

    /* Set tests */
    RUN_TEST(test_set_add_and_contains);
    RUN_TEST(test_set_no_duplicates);
    RUN_TEST(test_set_remove);

    /* Set lifecycle (COLL-03) */
    RUN_TEST(test_set_create_with_capacity);
    RUN_TEST(test_set_clone_independent);
    RUN_TEST(test_set_clone_empty);

    /* Free semantics */
    RUN_TEST(test_list_free_resets_state);
    RUN_TEST(test_map_free_resets_state);
    RUN_TEST(test_set_free_resets_state);

    /* Additional types */
    RUN_TEST(test_map_string_string);
    RUN_TEST(test_set_string);

    return UNITY_END();
}
