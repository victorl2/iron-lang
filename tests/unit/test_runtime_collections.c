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

    /* Map tests */
    RUN_TEST(test_map_put_and_get);
    RUN_TEST(test_map_has);
    RUN_TEST(test_map_update);
    RUN_TEST(test_map_remove);
    RUN_TEST(test_map_len);

    /* Set tests */
    RUN_TEST(test_set_add_and_contains);
    RUN_TEST(test_set_no_duplicates);
    RUN_TEST(test_set_remove);

    /* Additional types */
    RUN_TEST(test_map_string_string);
    RUN_TEST(test_set_string);

    return UNITY_END();
}
