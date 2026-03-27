#include "unity.h"
#include "util/arena.h"

#include <stdint.h>
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

static void test_arena_create_and_free(void) {
    Iron_Arena a = iron_arena_create(4096);
    TEST_ASSERT_NOT_NULL(a.base);
    TEST_ASSERT_EQUAL(0, a.used);
    TEST_ASSERT_EQUAL(4096, a.capacity);

    iron_arena_free(&a);
    TEST_ASSERT_NULL(a.base);
    TEST_ASSERT_EQUAL(0, a.used);
    TEST_ASSERT_EQUAL(0, a.capacity);
}

static void test_arena_alloc_returns_aligned_pointer(void) {
    Iron_Arena a = iron_arena_create(4096);
    TEST_ASSERT_NOT_NULL(a.base);

    int *ptr = (int *)iron_arena_alloc(&a, sizeof(int), _Alignof(int));
    TEST_ASSERT_NOT_NULL(ptr);
    TEST_ASSERT_EQUAL(0, (uintptr_t)ptr % _Alignof(int));

    double *dptr = (double *)iron_arena_alloc(&a, sizeof(double), _Alignof(double));
    TEST_ASSERT_NOT_NULL(dptr);
    TEST_ASSERT_EQUAL(0, (uintptr_t)dptr % _Alignof(double));

    iron_arena_free(&a);
}

static void test_arena_strdup_copies_string(void) {
    Iron_Arena  a   = iron_arena_create(4096);
    const char *src = "hello";
    char       *dup = iron_arena_strdup(&a, src, 5);

    TEST_ASSERT_NOT_NULL(dup);
    TEST_ASSERT_EQUAL_STRING("hello", dup);
    /* Must be a different pointer — it was copied into the arena. */
    TEST_ASSERT_NOT_EQUAL(src, dup);

    iron_arena_free(&a);
}

static void test_arena_grows_on_overflow(void) {
    /* Start with a very small arena so we force a grow. */
    Iron_Arena a = iron_arena_create(64);
    TEST_ASSERT_NOT_NULL(a.base);

    /* Request 128 bytes — larger than initial capacity. */
    void *ptr = iron_arena_alloc(&a, 128, 1);
    TEST_ASSERT_NOT_NULL(ptr);
    TEST_ASSERT_GREATER_OR_EQUAL(128, (int)a.capacity);

    iron_arena_free(&a);
}

static void test_arena_alloc_macro(void) {
    Iron_Arena a = iron_arena_create(4096);

    typedef struct { int x; double y; } MyStruct;
    MyStruct *s = ARENA_ALLOC(&a, MyStruct);
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_EQUAL(0, (uintptr_t)s % _Alignof(MyStruct));

    iron_arena_free(&a);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_arena_create_and_free);
    RUN_TEST(test_arena_alloc_returns_aligned_pointer);
    RUN_TEST(test_arena_strdup_copies_string);
    RUN_TEST(test_arena_grows_on_overflow);
    RUN_TEST(test_arena_alloc_macro);
    return UNITY_END();
}
