/* test_lsp_utf_translation -- Phase 2 Plan 04 Task 01 (CORE-11).
 *
 * Asserts correctness of UTF-8 ↔ UTF-16 ↔ byte-offset conversion for a
 * line of UTF-8 text. The conversion is the single chokepoint that lets
 * every downstream feature (diagnostics / hover / goto) emit ranges that
 * match the client's negotiated position encoding without divergence from
 * what `ironc` reports.
 *
 * Six scenarios:
 *   1. ASCII fast path ("hello") -- byte N ↔ UTF-16 col N
 *   2. 2-byte UTF-8 / 1 UTF-16 unit ("héllo", é = 0xC3 0xA9)
 *   3. 3-byte UTF-8 / 1 UTF-16 unit ("h€llo", € = 0xE2 0x82 0xAC)
 *   4. 4-byte UTF-8 / 2 UTF-16 units surrogate pair ("h𝄞llo",
 *      𝄞 U+1D11E = 0xF0 0x9D 0x84 0x9E)
 *   5. Combining mark ("á" as a + U+0301 = 0x61 0xCC 0x81)
 *   6. BOM prefix (0xEF 0xBB 0xBF + "hello"): BOM is a 3-byte UTF-8
 *      codepoint decoding to U+FEFF = 1 UTF-16 code unit; byte 3
 *      (first char after BOM) = UTF-16 col 1. */
#include "unity.h"
#include "lsp/store/utf.h"

#include <string.h>

void setUp(void) {}
void tearDown(void) {}

/* ── 1. ASCII fast path ────────────────────────────────────────────── */
static void test_ascii_byte_to_utf16_column(void) {
    const char line[] = "hello";
    /* byte 0 -> col 0; byte 1 -> col 1; byte 5 (end) -> col 5. */
    TEST_ASSERT_EQUAL_UINT32(0, ilsp_utf8_byte_to_utf16_column(line, 5, 0));
    TEST_ASSERT_EQUAL_UINT32(1, ilsp_utf8_byte_to_utf16_column(line, 5, 1));
    TEST_ASSERT_EQUAL_UINT32(5, ilsp_utf8_byte_to_utf16_column(line, 5, 5));
    /* Overshoot clamps to len. */
    TEST_ASSERT_EQUAL_UINT32(5, ilsp_utf8_byte_to_utf16_column(line, 5, 99));
}

static void test_ascii_utf16_column_to_byte(void) {
    const char line[] = "hello";
    TEST_ASSERT_EQUAL_size_t(0, ilsp_utf16_column_to_utf8_byte(line, 5, 0));
    TEST_ASSERT_EQUAL_size_t(3, ilsp_utf16_column_to_utf8_byte(line, 5, 3));
    TEST_ASSERT_EQUAL_size_t(5, ilsp_utf16_column_to_utf8_byte(line, 5, 5));
    /* Overshoot clamps to len. */
    TEST_ASSERT_EQUAL_size_t(5, ilsp_utf16_column_to_utf8_byte(line, 5, 99));
}

/* ── 2. 2-byte UTF-8 / 1 UTF-16 unit ───────────────────────────────── */
static void test_two_byte_utf8_bmp(void) {
    /* "héllo" = h (1 byte) + é (2 bytes) + llo (3 bytes) = 6 bytes. */
    const unsigned char src[] = { 'h', 0xC3, 0xA9, 'l', 'l', 'o' };
    const char *line = (const char *)src;

    /* Byte 0 (start of 'h') -> col 0. */
    TEST_ASSERT_EQUAL_UINT32(0, ilsp_utf8_byte_to_utf16_column(line, 6, 0));
    /* Byte 1 (start of 'é') -> col 1. */
    TEST_ASSERT_EQUAL_UINT32(1, ilsp_utf8_byte_to_utf16_column(line, 6, 1));
    /* Byte 3 (start of 'l' -- post-é) -> col 2. */
    TEST_ASSERT_EQUAL_UINT32(2, ilsp_utf8_byte_to_utf16_column(line, 6, 3));
    /* End byte 6 -> col 5. */
    TEST_ASSERT_EQUAL_UINT32(5, ilsp_utf8_byte_to_utf16_column(line, 6, 6));

    /* Inverse: col 2 -> byte 3 (past the 2-byte é). */
    TEST_ASSERT_EQUAL_size_t(3, ilsp_utf16_column_to_utf8_byte(line, 6, 2));
    TEST_ASSERT_EQUAL_size_t(6, ilsp_utf16_column_to_utf8_byte(line, 6, 5));
}

/* ── 3. 3-byte UTF-8 / 1 UTF-16 unit ───────────────────────────────── */
static void test_three_byte_utf8_bmp(void) {
    /* "h€llo" = h (1) + € (3: 0xE2 0x82 0xAC) + llo (3) = 7 bytes. */
    const unsigned char src[] = { 'h', 0xE2, 0x82, 0xAC, 'l', 'l', 'o' };
    const char *line = (const char *)src;

    TEST_ASSERT_EQUAL_UINT32(0, ilsp_utf8_byte_to_utf16_column(line, 7, 0));
    TEST_ASSERT_EQUAL_UINT32(1, ilsp_utf8_byte_to_utf16_column(line, 7, 1));
    /* Byte 4 (first 'l') -> col 2. */
    TEST_ASSERT_EQUAL_UINT32(2, ilsp_utf8_byte_to_utf16_column(line, 7, 4));
    TEST_ASSERT_EQUAL_UINT32(5, ilsp_utf8_byte_to_utf16_column(line, 7, 7));

    TEST_ASSERT_EQUAL_size_t(4, ilsp_utf16_column_to_utf8_byte(line, 7, 2));
}

/* ── 4. 4-byte UTF-8 / surrogate pair (2 UTF-16 units) ─────────────── */
static void test_surrogate_pair(void) {
    /* "h𝄞llo" = h (1) + 𝄞 (4: 0xF0 0x9D 0x84 0x9E) + llo (3) = 8 bytes. */
    const unsigned char src[] = { 'h', 0xF0, 0x9D, 0x84, 0x9E, 'l', 'l', 'o' };
    const char *line = (const char *)src;

    TEST_ASSERT_EQUAL_UINT32(0, ilsp_utf8_byte_to_utf16_column(line, 8, 0));
    TEST_ASSERT_EQUAL_UINT32(1, ilsp_utf8_byte_to_utf16_column(line, 8, 1));
    /* Byte 5 (first 'l') -> col 3 (1 + 2 surrogate units). */
    TEST_ASSERT_EQUAL_UINT32(3, ilsp_utf8_byte_to_utf16_column(line, 8, 5));
    /* End byte 8 -> col 6 (1 + 2 + 3). */
    TEST_ASSERT_EQUAL_UINT32(6, ilsp_utf8_byte_to_utf16_column(line, 8, 8));

    /* Inverse: col 3 -> byte 5 (past the 4-byte surrogate). */
    TEST_ASSERT_EQUAL_size_t(5, ilsp_utf16_column_to_utf8_byte(line, 8, 3));
    /* Col 2 (between surrogate halves) -> rounds up to byte 5 (whole codepoint). */
    TEST_ASSERT_EQUAL_size_t(5, ilsp_utf16_column_to_utf8_byte(line, 8, 2));
}

/* ── 5. Combining mark (2 codepoints, 2 UTF-16 units) ──────────────── */
static void test_combining_mark(void) {
    /* "á" as a + U+0301 combining acute = 'a' (1) + 0xCC 0x81 (2) = 3 bytes.
     * 2 codepoints, each BMP => 2 UTF-16 units. No NFC normalization. */
    const unsigned char src[] = { 'a', 0xCC, 0x81 };
    const char *line = (const char *)src;

    TEST_ASSERT_EQUAL_UINT32(0, ilsp_utf8_byte_to_utf16_column(line, 3, 0));
    TEST_ASSERT_EQUAL_UINT32(1, ilsp_utf8_byte_to_utf16_column(line, 3, 1));
    /* Byte 3 (end) -> col 2 (base + combining as 2 code units). */
    TEST_ASSERT_EQUAL_UINT32(2, ilsp_utf8_byte_to_utf16_column(line, 3, 3));

    TEST_ASSERT_EQUAL_size_t(3, ilsp_utf16_column_to_utf8_byte(line, 3, 2));
}

/* ── 6. BOM prefix ─────────────────────────────────────────────────── */
static void test_bom_prefix(void) {
    /* UTF-8 BOM = 0xEF 0xBB 0xBF (3 bytes decoding to U+FEFF, 1 UTF-16 unit).
     * "hello" follows. LSP treats the BOM as position 0; next char is pos 1. */
    const unsigned char src[] = { 0xEF, 0xBB, 0xBF, 'h', 'e', 'l', 'l', 'o' };
    const char *line = (const char *)src;

    /* Byte 0 -> col 0 (before BOM). */
    TEST_ASSERT_EQUAL_UINT32(0, ilsp_utf8_byte_to_utf16_column(line, 8, 0));
    /* Byte 3 (first 'h' after BOM) -> col 1. */
    TEST_ASSERT_EQUAL_UINT32(1, ilsp_utf8_byte_to_utf16_column(line, 8, 3));
    /* Byte 8 (end) -> col 6. */
    TEST_ASSERT_EQUAL_UINT32(6, ilsp_utf8_byte_to_utf16_column(line, 8, 8));

    TEST_ASSERT_EQUAL_size_t(3, ilsp_utf16_column_to_utf8_byte(line, 8, 1));
}

/* ── UTF-8-identity API (for UTF-8 encoding negotiation path) ──────── */
static void test_utf8_column_identity(void) {
    /* UTF-8 column == codepoint index (same code-unit count as UTF-16 for
     * BMP code points; but for supplementary, UTF-8 is 1 codepoint while
     * UTF-16 is 2 code units). The ilsp_utf8_*_utf8_* variants return
     * the codepoint index -- NOT the byte index. This function is called
     * at the facade boundary when the negotiated encoding is UTF-8. */
    const unsigned char src[] = { 'h', 0xE2, 0x82, 0xAC, 'l' };  /* "h€l" */
    const char *line = (const char *)src;
    /* Byte 0 -> col 0; byte 4 (past €) -> col 2; byte 5 -> col 3. */
    TEST_ASSERT_EQUAL_UINT32(0, ilsp_utf8_byte_to_utf8_column(line, 5, 0));
    TEST_ASSERT_EQUAL_UINT32(2, ilsp_utf8_byte_to_utf8_column(line, 5, 4));
    TEST_ASSERT_EQUAL_UINT32(3, ilsp_utf8_byte_to_utf8_column(line, 5, 5));
    /* Inverse: col 2 -> byte 4 (start of 'l' past €). */
    TEST_ASSERT_EQUAL_size_t(4, ilsp_utf8_column_to_utf8_byte(line, 5, 2));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_ascii_byte_to_utf16_column);
    RUN_TEST(test_ascii_utf16_column_to_byte);
    RUN_TEST(test_two_byte_utf8_bmp);
    RUN_TEST(test_three_byte_utf8_bmp);
    RUN_TEST(test_surrogate_pair);
    RUN_TEST(test_combining_mark);
    RUN_TEST(test_bom_prefix);
    RUN_TEST(test_utf8_column_identity);
    return UNITY_END();
}
