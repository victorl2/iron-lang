#include "unity.h"
#include "runtime/iron_runtime.h"

#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* ── Unity boilerplate ───────────────────────────────────────────────────── */

void setUp(void)    { iron_runtime_init(); }
void tearDown(void) { iron_runtime_shutdown(); }

/* ── Iron_String tests ───────────────────────────────────────────────────── */

void test_sso_short_string(void) {
    const char *s = "hello";
    Iron_String is = iron_string_from_cstr(s, 5);
    /* SSO: flags bit 0 should be 0 (not heap) */
    TEST_ASSERT_EQUAL_UINT8(0, is.heap.flags & 0x01);
    TEST_ASSERT_EQUAL_STRING("hello", iron_string_cstr(&is));
    TEST_ASSERT_EQUAL_size_t(5, iron_string_byte_len(&is));
}

void test_sso_boundary(void) {
    /* Exactly 23 bytes — should stay SSO */
    const char *s = "12345678901234567890123"; /* 23 chars */
    Iron_String is = iron_string_from_cstr(s, 23);
    TEST_ASSERT_EQUAL_UINT8(0, is.heap.flags & 0x01);
    TEST_ASSERT_EQUAL_STRING(s, iron_string_cstr(&is));
    TEST_ASSERT_EQUAL_size_t(23, iron_string_byte_len(&is));
}

void test_heap_long_string(void) {
    /* 24 bytes — should go to heap */
    const char *s = "123456789012345678901234"; /* 24 chars */
    Iron_String is = iron_string_from_cstr(s, 24);
    TEST_ASSERT_EQUAL_UINT8(1, is.heap.flags & 0x01);
    TEST_ASSERT_EQUAL_STRING(s, iron_string_cstr(&is));
    TEST_ASSERT_EQUAL_size_t(24, iron_string_byte_len(&is));
    free(is.heap.data);
}

void test_empty_string(void) {
    Iron_String is = iron_string_from_cstr("", 0);
    TEST_ASSERT_EQUAL_UINT8(0, is.heap.flags & 0x01);
    TEST_ASSERT_EQUAL_STRING("", iron_string_cstr(&is));
    TEST_ASSERT_EQUAL_size_t(0, iron_string_byte_len(&is));
}

void test_codepoint_count_ascii(void) {
    Iron_String is = iron_string_from_cstr("hello", 5);
    TEST_ASSERT_EQUAL_size_t(5, iron_string_codepoint_count(&is));
}

void test_codepoint_count_utf8(void) {
    /* "café" = 4 codepoints, 5 bytes (é is 2 bytes: 0xC3 0xA9) */
    const char *s = "caf\xC3\xA9";
    Iron_String is = iron_string_from_cstr(s, 5);
    TEST_ASSERT_EQUAL_size_t(4, iron_string_codepoint_count(&is));
}

void test_string_equals_same(void) {
    Iron_String a = iron_string_from_cstr("hello", 5);
    Iron_String b = iron_string_from_cstr("hello", 5);
    TEST_ASSERT_TRUE(iron_string_equals(&a, &b));
}

void test_string_equals_different(void) {
    Iron_String a = iron_string_from_cstr("hello", 5);
    Iron_String b = iron_string_from_cstr("world", 5);
    TEST_ASSERT_FALSE(iron_string_equals(&a, &b));
}

void test_string_equals_different_length(void) {
    Iron_String a = iron_string_from_cstr("hi", 2);
    Iron_String b = iron_string_from_cstr("hello", 5);
    TEST_ASSERT_FALSE(iron_string_equals(&a, &b));
}

void test_concat_sso(void) {
    Iron_String a   = iron_string_from_cstr("hello", 5);
    Iron_String b   = iron_string_from_cstr(" world", 6);
    Iron_String cat = iron_string_concat(&a, &b);
    TEST_ASSERT_EQUAL_STRING("hello world", iron_string_cstr(&cat));
    TEST_ASSERT_EQUAL_size_t(11, iron_string_byte_len(&cat));
    /* 11 bytes fits in SSO */
    TEST_ASSERT_EQUAL_UINT8(0, cat.heap.flags & 0x01);
}

void test_concat_heap(void) {
    /* Two strings whose combined length exceeds SSO_MAX */
    const char *sa = "abcdefghijklmno"; /* 15 */
    const char *sb = "pqrstuvwxyz123"; /* 14 = total 29 */
    Iron_String a   = iron_string_from_cstr(sa, 15);
    Iron_String b   = iron_string_from_cstr(sb, 14);
    Iron_String cat = iron_string_concat(&a, &b);
    TEST_ASSERT_EQUAL_STRING("abcdefghijklmnopqrstuvwxyz123", iron_string_cstr(&cat));
    TEST_ASSERT_EQUAL_size_t(29, iron_string_byte_len(&cat));
    TEST_ASSERT_EQUAL_UINT8(1, cat.heap.flags & 0x01);
    free(cat.heap.data);
}

void test_intern_deduplicates(void) {
    /* Use a long string (> SSO_MAX) so it heap-allocates; then the interned
     * copy shares the same heap pointer for both lookups.
     * For SSO strings, interning guarantees content equality, not pointer
     * identity (SSO data is embedded in the struct, not on the heap).
     */
    const char *long_str = "This string is definitely longer than 23 bytes!";
    size_t len = strlen(long_str);
    Iron_String a = iron_string_from_literal(long_str, len);
    Iron_String b = iron_string_from_literal(long_str, len);
    /* Both must share the same heap buffer pointer */
    TEST_ASSERT_EQUAL_PTR(iron_string_cstr(&a), iron_string_cstr(&b));
    /* Also verify SSO interning gives content equality */
    Iron_String s1 = iron_string_from_literal("hello", 5);
    Iron_String s2 = iron_string_from_literal("hello", 5);
    TEST_ASSERT_TRUE(iron_string_equals(&s1, &s2));
}

void test_intern_different_strings(void) {
    Iron_String a = iron_string_from_literal("alpha", 5);
    Iron_String b = iron_string_from_literal("beta",  4);
    TEST_ASSERT_FALSE(iron_string_equals(&a, &b));
}

/* ── Iron_Rc tests ───────────────────────────────────────────────────────── */

static void int_destructor(void *v) { (void)v; /* nothing to free for int */ }

void test_rc_create_and_release(void) {
    int val  = 42;
    Iron_Rc rc = iron_rc_create(&val, sizeof(int), int_destructor);
    TEST_ASSERT_NOT_NULL(rc.ctrl);
    TEST_ASSERT_NOT_NULL(rc.value);
    TEST_ASSERT_EQUAL_INT(42, *(int *)rc.value);
    iron_rc_release(&rc);
    TEST_ASSERT_NULL(rc.ctrl);
}

void test_rc_retain_release(void) {
    int val = 7;
    Iron_Rc rc  = iron_rc_create(&val, sizeof(int), int_destructor);
    Iron_Rc rc2 = rc;
    iron_rc_retain(&rc2);
    iron_rc_release(&rc);
    /* rc2 still alive */
    TEST_ASSERT_NOT_NULL(rc2.ctrl);
    TEST_ASSERT_EQUAL_INT(7, *(int *)rc2.value);
    iron_rc_release(&rc2);
}

void test_rc_null_safe(void) {
    Iron_Rc null_rc = {NULL, NULL};
    /* Should not crash */
    iron_rc_retain(&null_rc);
    iron_rc_release(&null_rc);
}

/* ── Iron_Weak tests ─────────────────────────────────────────────────────── */

void test_weak_upgrade_alive(void) {
    int val = 99;
    Iron_Rc   rc   = iron_rc_create(&val, sizeof(int), int_destructor);
    Iron_Weak weak = iron_rc_downgrade(&rc);
    Iron_Rc   rc2  = iron_weak_upgrade(&weak);
    TEST_ASSERT_NOT_NULL(rc2.ctrl);
    TEST_ASSERT_EQUAL_INT(99, *(int *)rc2.value);
    iron_rc_release(&rc2);
    iron_rc_release(&rc);
}

void test_weak_upgrade_dead(void) {
    int val = 5;
    Iron_Rc   rc   = iron_rc_create(&val, sizeof(int), int_destructor);
    Iron_Weak weak = iron_rc_downgrade(&rc);
    iron_rc_release(&rc);
    /* Strong ref is dead — upgrade should return NULL ctrl */
    Iron_Rc dead = iron_weak_upgrade(&weak);
    TEST_ASSERT_NULL(dead.ctrl);
}

/* ── Built-in function tests ─────────────────────────────────────────────── */

void test_Iron_print_no_crash(void) {
    Iron_String s = iron_string_from_cstr("test", 4);
    Iron_print(s);   /* just verify no crash */
}

void test_Iron_println_no_crash(void) {
    Iron_String s = iron_string_from_cstr("line", 4);
    Iron_println(s); /* just verify no crash */
}

void test_Iron_len(void) {
    Iron_String s = iron_string_from_cstr("hello", 5);
    TEST_ASSERT_EQUAL_INT64(5, Iron_len(s));
}

void test_Iron_len_utf8(void) {
    /* "café" = 4 codepoints */
    const char *raw = "caf\xC3\xA9";
    Iron_String s = iron_string_from_cstr(raw, 5);
    TEST_ASSERT_EQUAL_INT64(4, Iron_len(s));
}

void test_Iron_min(void) {
    TEST_ASSERT_EQUAL_INT64(3,  Iron_min(3, 7));
    TEST_ASSERT_EQUAL_INT64(-5, Iron_min(-5, 0));
    TEST_ASSERT_EQUAL_INT64(0,  Iron_min(0, 0));
}

void test_Iron_max(void) {
    TEST_ASSERT_EQUAL_INT64(7,   Iron_max(3, 7));
    TEST_ASSERT_EQUAL_INT64(0,   Iron_max(-5, 0));
    TEST_ASSERT_EQUAL_INT64(100, Iron_max(100, 100));
}

void test_Iron_clamp(void) {
    TEST_ASSERT_EQUAL_INT64(5,  Iron_clamp(5,  0, 10));
    TEST_ASSERT_EQUAL_INT64(0,  Iron_clamp(-3, 0, 10));
    TEST_ASSERT_EQUAL_INT64(10, Iron_clamp(15, 0, 10));
}

void test_Iron_abs(void) {
    TEST_ASSERT_EQUAL_INT64(5, Iron_abs(5));
    TEST_ASSERT_EQUAL_INT64(5, Iron_abs(-5));
    TEST_ASSERT_EQUAL_INT64(0, Iron_abs(0));
}

/* ── main ────────────────────────────────────────────────────────────────── */

int main(void) {
    UNITY_BEGIN();

    /* Iron_String */
    RUN_TEST(test_sso_short_string);
    RUN_TEST(test_sso_boundary);
    RUN_TEST(test_heap_long_string);
    RUN_TEST(test_empty_string);
    RUN_TEST(test_codepoint_count_ascii);
    RUN_TEST(test_codepoint_count_utf8);
    RUN_TEST(test_string_equals_same);
    RUN_TEST(test_string_equals_different);
    RUN_TEST(test_string_equals_different_length);
    RUN_TEST(test_concat_sso);
    RUN_TEST(test_concat_heap);
    RUN_TEST(test_intern_deduplicates);
    RUN_TEST(test_intern_different_strings);

    /* Iron_Rc */
    RUN_TEST(test_rc_create_and_release);
    RUN_TEST(test_rc_retain_release);
    RUN_TEST(test_rc_null_safe);

    /* Iron_Weak */
    RUN_TEST(test_weak_upgrade_alive);
    RUN_TEST(test_weak_upgrade_dead);

    /* Builtins */
    RUN_TEST(test_Iron_print_no_crash);
    RUN_TEST(test_Iron_println_no_crash);
    RUN_TEST(test_Iron_len);
    RUN_TEST(test_Iron_len_utf8);
    RUN_TEST(test_Iron_min);
    RUN_TEST(test_Iron_max);
    RUN_TEST(test_Iron_clamp);
    RUN_TEST(test_Iron_abs);

    return UNITY_END();
}
