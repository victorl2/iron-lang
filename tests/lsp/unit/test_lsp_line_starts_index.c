/* test_lsp_line_starts_index -- Phase 2 Plan 04 Task 01 (CORE-10).
 *
 * Asserts:
 *   - line_starts[] is rebuilt correctly on LF / CRLF / trailing / empty /
 *     no-newline buffers.
 *   - ilsp_line_of_byte is O(log n) binary search with correct semantics
 *     at line boundaries.
 *   - ilsp_byte_of_line inverse round-trips.
 *
 * Invariant (verified on every input): line_starts[0] == 0, and
 * line_count == number of \n + 1 (unless the buffer is empty).
 */
#include "unity.h"
#include "lsp/store/line_index.h"
#include "vendor/stb_ds.h"

#include <string.h>

void setUp(void) {}
void tearDown(void) {}

/* Helper: fetch a line-start offset with bounds guard so UNITY failures
 * produce readable messages instead of segfaults. */
static uint32_t starts_at(const IronLsp_LineIndex *idx, size_t i) {
    TEST_ASSERT_TRUE((ptrdiff_t)i < arrlen(idx->starts));
    return idx->starts[i];
}

/* ── 1. LF-only ────────────────────────────────────────────────────── */
static void test_lf_three_lines(void) {
    /* "a\nb\nc" => line_starts = [0, 2, 4]; 3 lines. */
    IronLsp_LineIndex idx;
    ilsp_line_index_init(&idx);
    ilsp_line_index_rebuild(&idx, "a\nb\nc", 5);

    TEST_ASSERT_EQUAL_size_t(3, (size_t)arrlen(idx.starts));
    TEST_ASSERT_EQUAL_UINT32(0, starts_at(&idx, 0));
    TEST_ASSERT_EQUAL_UINT32(2, starts_at(&idx, 1));
    TEST_ASSERT_EQUAL_UINT32(4, starts_at(&idx, 2));

    /* Binary-search semantics. */
    TEST_ASSERT_EQUAL_UINT32(0, ilsp_line_of_byte(&idx, 0));   /* 'a' on line 0 */
    TEST_ASSERT_EQUAL_UINT32(0, ilsp_line_of_byte(&idx, 1));   /* '\n' still line 0 */
    TEST_ASSERT_EQUAL_UINT32(1, ilsp_line_of_byte(&idx, 2));   /* 'b' on line 1 */
    TEST_ASSERT_EQUAL_UINT32(2, ilsp_line_of_byte(&idx, 4));   /* 'c' on line 2 */
    TEST_ASSERT_EQUAL_UINT32(2, ilsp_line_of_byte(&idx, 5));   /* past end -> last */

    /* Inverse. */
    TEST_ASSERT_EQUAL_size_t(0, ilsp_byte_of_line(&idx, 0));
    TEST_ASSERT_EQUAL_size_t(2, ilsp_byte_of_line(&idx, 1));
    TEST_ASSERT_EQUAL_size_t(4, ilsp_byte_of_line(&idx, 2));

    ilsp_line_index_destroy(&idx);
}

/* ── 2. CRLF ───────────────────────────────────────────────────────── */
static void test_crlf_three_lines(void) {
    /* "a\r\nb\r\nc" => \n at byte 2 and byte 5; line_starts = [0, 3, 6]. */
    IronLsp_LineIndex idx;
    ilsp_line_index_init(&idx);
    ilsp_line_index_rebuild(&idx, "a\r\nb\r\nc", 7);

    TEST_ASSERT_EQUAL_size_t(3, (size_t)arrlen(idx.starts));
    TEST_ASSERT_EQUAL_UINT32(0, starts_at(&idx, 0));
    TEST_ASSERT_EQUAL_UINT32(3, starts_at(&idx, 1));
    TEST_ASSERT_EQUAL_UINT32(6, starts_at(&idx, 2));

    /* Byte 3 (start of 'b') -> line 1. */
    TEST_ASSERT_EQUAL_UINT32(1, ilsp_line_of_byte(&idx, 3));
    TEST_ASSERT_EQUAL_UINT32(2, ilsp_line_of_byte(&idx, 6));

    ilsp_line_index_destroy(&idx);
}

/* ── 3. Trailing newline ───────────────────────────────────────────── */
static void test_trailing_newline(void) {
    /* "a\n" => line_starts = [0, 2]; 2 lines (the second is empty). */
    IronLsp_LineIndex idx;
    ilsp_line_index_init(&idx);
    ilsp_line_index_rebuild(&idx, "a\n", 2);

    TEST_ASSERT_EQUAL_size_t(2, (size_t)arrlen(idx.starts));
    TEST_ASSERT_EQUAL_UINT32(0, starts_at(&idx, 0));
    TEST_ASSERT_EQUAL_UINT32(2, starts_at(&idx, 1));
    TEST_ASSERT_EQUAL_UINT32(1, ilsp_line_of_byte(&idx, 2));

    ilsp_line_index_destroy(&idx);
}

/* ── 4. Empty buffer ───────────────────────────────────────────────── */
static void test_empty_buffer(void) {
    /* "" => line_starts = [0]; 1 line. */
    IronLsp_LineIndex idx;
    ilsp_line_index_init(&idx);
    ilsp_line_index_rebuild(&idx, "", 0);

    TEST_ASSERT_EQUAL_size_t(1, (size_t)arrlen(idx.starts));
    TEST_ASSERT_EQUAL_UINT32(0, starts_at(&idx, 0));
    TEST_ASSERT_EQUAL_UINT32(0, ilsp_line_of_byte(&idx, 0));

    ilsp_line_index_destroy(&idx);
}

/* ── 5. No newline at all ──────────────────────────────────────────── */
static void test_no_newline(void) {
    /* "abc" => line_starts = [0]; 1 line. */
    IronLsp_LineIndex idx;
    ilsp_line_index_init(&idx);
    ilsp_line_index_rebuild(&idx, "abc", 3);

    TEST_ASSERT_EQUAL_size_t(1, (size_t)arrlen(idx.starts));
    TEST_ASSERT_EQUAL_UINT32(0, ilsp_line_of_byte(&idx, 0));
    TEST_ASSERT_EQUAL_UINT32(0, ilsp_line_of_byte(&idx, 2));

    ilsp_line_index_destroy(&idx);
}

/* ── 6. Rebuild idempotence ────────────────────────────────────────── */
static void test_rebuild_idempotent(void) {
    /* Rebuilding from scratch wipes prior state. */
    IronLsp_LineIndex idx;
    ilsp_line_index_init(&idx);
    ilsp_line_index_rebuild(&idx, "a\nb\nc\nd", 7);
    TEST_ASSERT_EQUAL_size_t(4, (size_t)arrlen(idx.starts));

    ilsp_line_index_rebuild(&idx, "x", 1);
    TEST_ASSERT_EQUAL_size_t(1, (size_t)arrlen(idx.starts));
    TEST_ASSERT_EQUAL_UINT32(0, starts_at(&idx, 0));

    ilsp_line_index_destroy(&idx);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_lf_three_lines);
    RUN_TEST(test_crlf_three_lines);
    RUN_TEST(test_trailing_newline);
    RUN_TEST(test_empty_buffer);
    RUN_TEST(test_no_newline);
    RUN_TEST(test_rebuild_idempotent);
    return UNITY_END();
}
