#include "unity.h"
#include "diagnostics/diagnostics.h"
#include "util/arena.h"

#include <string.h>

void setUp(void) {}
void tearDown(void) {}

static void test_diaglist_create_empty(void) {
    Iron_DiagList list = iron_diaglist_create();
    TEST_ASSERT_EQUAL(0, list.count);
    TEST_ASSERT_EQUAL(0, list.error_count);
    TEST_ASSERT_EQUAL(0, list.warning_count);
    TEST_ASSERT_NULL(list.items);
    iron_diaglist_free(&list);
}

static void test_diag_emit_error_increments_count(void) {
    Iron_Arena    arena = iron_arena_create(4096);
    Iron_DiagList list  = iron_diaglist_create();
    Iron_Span     span  = iron_span_make("test.iron", 1, 1, 1, 5);

    iron_diag_emit(&list, &arena,
                   IRON_DIAG_ERROR, IRON_ERR_UNTERMINATED_STRING,
                   span, "unterminated string", NULL);

    TEST_ASSERT_EQUAL(1, list.count);
    TEST_ASSERT_EQUAL(1, list.error_count);
    TEST_ASSERT_EQUAL(0, list.warning_count);

    iron_diaglist_free(&list);
    iron_arena_free(&arena);
}

static void test_diag_emit_warning_increments_warning_count(void) {
    Iron_Arena    arena = iron_arena_create(4096);
    Iron_DiagList list  = iron_diaglist_create();
    Iron_Span     span  = iron_span_make("test.iron", 2, 1, 2, 10);

    iron_diag_emit(&list, &arena,
                   IRON_DIAG_WARNING, IRON_ERR_INVALID_CHAR,
                   span, "unusual character", NULL);

    TEST_ASSERT_EQUAL(1, list.count);
    TEST_ASSERT_EQUAL(1, list.warning_count);
    TEST_ASSERT_EQUAL(0, list.error_count);

    iron_diaglist_free(&list);
    iron_arena_free(&arena);
}

static void test_diag_emit_preserves_message(void) {
    Iron_Arena    arena = iron_arena_create(4096);
    Iron_DiagList list  = iron_diaglist_create();
    Iron_Span     span  = iron_span_make("test.iron", 1, 1, 1, 5);

    iron_diag_emit(&list, &arena,
                   IRON_DIAG_ERROR, IRON_ERR_UNTERMINATED_STRING,
                   span, "unterminated string", NULL);

    TEST_ASSERT_EQUAL_STRING("unterminated string", list.items[0].message);

    iron_diaglist_free(&list);
    iron_arena_free(&arena);
}

static void test_diag_emit_preserves_span(void) {
    Iron_Arena    arena = iron_arena_create(4096);
    Iron_DiagList list  = iron_diaglist_create();
    Iron_Span     span  = iron_span_make("test.iron", 5, 10, 5, 15);

    iron_diag_emit(&list, &arena,
                   IRON_DIAG_ERROR, IRON_ERR_INVALID_CHAR,
                   span, "bad char", NULL);

    TEST_ASSERT_EQUAL(5,  list.items[0].span.line);
    TEST_ASSERT_EQUAL(10, list.items[0].span.col);
    TEST_ASSERT_EQUAL(5,  list.items[0].span.end_line);
    TEST_ASSERT_EQUAL(15, list.items[0].span.end_col);

    iron_diaglist_free(&list);
    iron_arena_free(&arena);
}

static void test_span_merge(void) {
    Iron_Span a = iron_span_make("test.iron", 1, 1, 1, 5);
    Iron_Span b = iron_span_make("test.iron", 3, 1, 3, 10);

    Iron_Span merged = iron_span_merge(a, b);

    TEST_ASSERT_EQUAL(1,  merged.line);
    TEST_ASSERT_EQUAL(1,  merged.col);
    TEST_ASSERT_EQUAL(3,  merged.end_line);
    TEST_ASSERT_EQUAL(10, merged.end_col);
    TEST_ASSERT_EQUAL_STRING("test.iron", merged.filename);
}

static void test_diag_emit_suggestion(void) {
    Iron_Arena    arena = iron_arena_create(4096);
    Iron_DiagList list  = iron_diaglist_create();
    Iron_Span     span  = iron_span_make("test.iron", 1, 1, 1, 1);

    iron_diag_emit(&list, &arena,
                   IRON_DIAG_ERROR, IRON_ERR_UNEXPECTED_TOKEN,
                   span, "unexpected token", "did you forget a semicolon?");

    TEST_ASSERT_NOT_NULL(list.items[0].suggestion);
    TEST_ASSERT_EQUAL_STRING("did you forget a semicolon?", list.items[0].suggestion);

    iron_diaglist_free(&list);
    iron_arena_free(&arena);
}

static void test_diaglist_free_clears_fields(void) {
    Iron_Arena    arena = iron_arena_create(4096);
    Iron_DiagList list  = iron_diaglist_create();
    Iron_Span     span  = iron_span_make("test.iron", 1, 1, 1, 1);

    iron_diag_emit(&list, &arena,
                   IRON_DIAG_ERROR, IRON_ERR_INVALID_CHAR,
                   span, "bad", NULL);

    iron_diaglist_free(&list);
    TEST_ASSERT_NULL(list.items);
    TEST_ASSERT_EQUAL(0, list.count);
    TEST_ASSERT_EQUAL(0, list.error_count);

    iron_arena_free(&arena);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_diaglist_create_empty);
    RUN_TEST(test_diag_emit_error_increments_count);
    RUN_TEST(test_diag_emit_warning_increments_warning_count);
    RUN_TEST(test_diag_emit_preserves_message);
    RUN_TEST(test_diag_emit_preserves_span);
    RUN_TEST(test_span_merge);
    RUN_TEST(test_diag_emit_suggestion);
    RUN_TEST(test_diaglist_free_clears_fields);
    return UNITY_END();
}
