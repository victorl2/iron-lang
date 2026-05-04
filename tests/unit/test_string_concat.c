/* test_string_concat.c — Unity tests for the iron_string_concat runtime
 * helper. Phase 96 STR-01 wires the C compiler to lower `String + String`
 * binops into calls to this helper; the helper itself already shipped at
 * src/runtime/iron_string.c:129. These tests lock the contract that the
 * compiler-side rewrite relies on:
 *
 *   - SSO-fits-in-place when total <= IRON_STRING_SSO_MAX (23 bytes).
 *   - Heap-allocated buffer when total > 23 bytes.
 *   - Empty operands behave as identity for the other operand.
 *   - NULL operands are treated as empty strings (defense-in-depth so the
 *     compiler can lower interpolation / partial chains without inserting
 *     extra null guards).
 *   - Chained calls (concat of concat) produce the correct concatenation.
 *
 * Source convention mirrors tests/unit/test_runtime_string.c.
 */

#include "unity.h"
#include "runtime/iron_runtime.h"

#include <stdlib.h>
#include <string.h>
#include <stdint.h>

void setUp(void)    { iron_runtime_init(0, NULL); }
void tearDown(void) { iron_runtime_shutdown(); }

/* ── Test 1: SSO + SSO stays in SSO ───────────────────────────────────────── */

void test_concat_sso_plus_sso_stays_sso(void) {
    Iron_String a   = iron_string_from_cstr("hello", 5);
    Iron_String b   = iron_string_from_cstr("world", 5);
    Iron_String cat = iron_string_concat(&a, &b);
    TEST_ASSERT_EQUAL_size_t(10, iron_string_byte_len(&cat));
    TEST_ASSERT_EQUAL_STRING("helloworld", iron_string_cstr(&cat));
    /* 10 bytes <= 23 -> stays in SSO; flag bit 0 must be 0. */
    TEST_ASSERT_EQUAL_UINT8(0, cat.heap.flags & 0x01);
}

/* ── Test 2: SSO + SSO crossing SSO_MAX promotes to heap ──────────────────── */

void test_concat_crossing_sso_max_promotes_to_heap(void) {
    /* 12 + 12 = 24 bytes > IRON_STRING_SSO_MAX (23). */
    Iron_String a   = iron_string_from_cstr("aaaaaaaaaaaa", 12);
    Iron_String b   = iron_string_from_cstr("bbbbbbbbbbbb", 12);
    Iron_String cat = iron_string_concat(&a, &b);
    TEST_ASSERT_EQUAL_size_t(24, iron_string_byte_len(&cat));
    TEST_ASSERT_EQUAL_STRING(
        "aaaaaaaaaaaabbbbbbbbbbbb", iron_string_cstr(&cat));
    /* Heap path: flag bit 0 must be 1. */
    TEST_ASSERT_EQUAL_UINT8(1, cat.heap.flags & 0x01);
    free(cat.heap.data);
}

/* ── Test 3: empty-string identity in both directions ─────────────────────── */

void test_concat_empty_identity(void) {
    Iron_String empty = iron_string_from_cstr("", 0);
    Iron_String x     = iron_string_from_cstr("x", 1);

    /* empty + empty = "" */
    {
        Iron_String cat = iron_string_concat(&empty, &empty);
        TEST_ASSERT_EQUAL_size_t(0, iron_string_byte_len(&cat));
        TEST_ASSERT_EQUAL_STRING("", iron_string_cstr(&cat));
    }
    /* empty + "x" = "x" */
    {
        Iron_String cat = iron_string_concat(&empty, &x);
        TEST_ASSERT_EQUAL_size_t(1, iron_string_byte_len(&cat));
        TEST_ASSERT_EQUAL_STRING("x", iron_string_cstr(&cat));
    }
    /* "x" + empty = "x" */
    {
        Iron_String cat = iron_string_concat(&x, &empty);
        TEST_ASSERT_EQUAL_size_t(1, iron_string_byte_len(&cat));
        TEST_ASSERT_EQUAL_STRING("x", iron_string_cstr(&cat));
    }
}

/* ── Test 4: NULL-safety — NULL operand treated as empty ──────────────────── */

void test_concat_null_treated_as_empty(void) {
    Iron_String x = iron_string_from_cstr("x", 1);

    /* NULL + "x" = "x" */
    {
        Iron_String cat = iron_string_concat(NULL, &x);
        TEST_ASSERT_EQUAL_size_t(1, iron_string_byte_len(&cat));
        TEST_ASSERT_EQUAL_STRING("x", iron_string_cstr(&cat));
    }
    /* "x" + NULL = "x" */
    {
        Iron_String cat = iron_string_concat(&x, NULL);
        TEST_ASSERT_EQUAL_size_t(1, iron_string_byte_len(&cat));
        TEST_ASSERT_EQUAL_STRING("x", iron_string_cstr(&cat));
    }
    /* NULL + NULL = "" */
    {
        Iron_String cat = iron_string_concat(NULL, NULL);
        TEST_ASSERT_EQUAL_size_t(0, iron_string_byte_len(&cat));
        TEST_ASSERT_EQUAL_STRING("", iron_string_cstr(&cat));
    }
}

/* ── Test 5: chained concat — concat(concat("a","b"), "c") = "abc" ────────── */

void test_concat_chained_left_associative(void) {
    Iron_String a   = iron_string_from_cstr("a", 1);
    Iron_String b   = iron_string_from_cstr("b", 1);
    Iron_String c   = iron_string_from_cstr("c", 1);
    Iron_String ab  = iron_string_concat(&a, &b);
    Iron_String abc = iron_string_concat(&ab, &c);
    TEST_ASSERT_EQUAL_size_t(3, iron_string_byte_len(&abc));
    TEST_ASSERT_EQUAL_STRING("abc", iron_string_cstr(&abc));
}

/* ── main ─────────────────────────────────────────────────────────────────── */

int main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_concat_sso_plus_sso_stays_sso);
    RUN_TEST(test_concat_crossing_sso_max_promotes_to_heap);
    RUN_TEST(test_concat_empty_identity);
    RUN_TEST(test_concat_null_treated_as_empty);
    RUN_TEST(test_concat_chained_left_associative);

    return UNITY_END();
}
