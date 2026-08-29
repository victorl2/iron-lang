#include "unity.h"
#include "runtime/iron_runtime.h"

#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* ── Unity boilerplate ───────────────────────────────────────────────────── */

void setUp(void)    { iron_runtime_init(0, NULL); }
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

void test_string_release_consumes_owned_but_not_interned_storage(void) {
    const char *owned_text =
        "owned string storage longer than the SSO boundary";
    Iron_String owned = iron_string_from_cstr(
        owned_text, strlen(owned_text));
    TEST_ASSERT_EQUAL_UINT8(1, owned.heap.flags & 0x01);
    iron_string_release(&owned);
    TEST_ASSERT_EQUAL_UINT8(0, owned.heap.flags & 0x01);
    TEST_ASSERT_EQUAL_size_t(0, iron_string_byte_len(&owned));

    const char *literal_text =
        "interned string storage longer than the SSO boundary";
    Iron_String literal = iron_string_from_literal(
        literal_text, strlen(literal_text));
    const char *interned_data = iron_string_cstr(&literal);
    iron_string_release(&literal);
    Iron_String again = iron_string_from_literal(
        literal_text, strlen(literal_text));
    TEST_ASSERT_EQUAL_PTR(interned_data, iron_string_cstr(&again));
}

/* ── Iron_Rc tests ───────────────────────────────────────────────────────── */
/*
 * Phase 26 Plan 26-01: the pre-v4 Iron_Rc + Iron_Weak control-block-plus-value
 * API was REMOVED entirely (Task 1 commit 50b3399 + Task 2 GREEN rewrite of
 * iron_rc.c). The legacy API:
 *   - Iron_Rc { ctrl, value }, Iron_RcControl { strong_count, weak_count,
 *     destructor }, Iron_Weak
 *   - iron_rc_create, iron_rc_downgrade, iron_weak_upgrade
 * predated Phase 19's atomic-ordering convention (relaxed-inc / acquire-load)
 * and had zero codegen call sites (verified via
 *   grep iron_rc_retain\|iron_rc_release src/lir/ src/hir/ src/cli/
 *   → zero hits).
 *
 * The Phase 26 surface (iron_rc_alloc(size, drop_fn) /
 * iron_rc_retain(void *) / iron_rc_release(void *) +
 * iron_rc_header_of) is covered by:
 *   tests/unit/test_rc_layout.c            — sizeof/offsetof + balanced lifecycle
 *   tests/unit/test_rc_atomic_ordering.c   — single-thread atomic-ordering proof
 *   tests/unit/test_runtime_rc_concurrent.c — Linux+TSan 8-thread stress
 *
 * Weak rc support is deferred to Phase 27.
 */

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
    RUN_TEST(test_string_release_consumes_owned_but_not_interned_storage);

    /* Iron_Rc + Iron_Weak: legacy v1.x API removed in Phase 26 Plan 26-01.
     * Coverage migrated to tests/unit/test_rc_layout.c +
     * tests/unit/test_rc_atomic_ordering.c +
     * tests/unit/test_runtime_rc_concurrent.c. See note above. */

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
