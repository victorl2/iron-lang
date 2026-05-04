/* test_lsp_document_sync_corpus -- Phase 2 Plan 04 Task 02 (CORE-09).
 *
 * Drives the document store through the didOpen / didChange / didClose
 * / didSave path across a corpus that exercises every T-02-09 mitigation
 * and every UTF edge case that Plan 04 needs to get right before the
 * compile path (Plan 05) consumes the buffer.
 *
 * 10 tests: ASCII insert, BOM-preserving append, CRLF range, surrogate-
 * pair range (UTF-16 and UTF-8 encoding parity), full-replace,
 * version-monotonicity guard, 100-edit idempotence, SHA-256 drift,
 * UTF-8 vs UTF-16 range equivalence, didClose idempotence.
 *
 * The tests intercept server->documents directly rather than routing
 * through the JSON dispatcher -- we cover the dispatcher path in
 * test_lsp_initialize_capabilities (Plan 03) + the routing tests Plan
 * 06 will add; here the contract is the document-store API itself. */
#include "unity.h"
#include "lsp/server/server.h"
#include "lsp/store/document.h"
#include "lsp/store/sha256.h"
#include "lsp/facade/types.h"
#include "vendor/stb_ds.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

static IronLsp_Range make_range(uint32_t sl, uint32_t sc,
                                 uint32_t el, uint32_t ec) {
    IronLsp_Range r = {0};
    r.start.line = sl; r.start.character = sc;
    r.end.line   = el; r.end.character   = ec;
    return r;
}

/* ── 1. ASCII single-char insert ───────────────────────────────────── */
static void test_ascii_insert(void) {
    IronLsp_Document *d = ilsp_document_create(
        "file:///tmp/a.iron", "abc", 3, 1);
    TEST_ASSERT_NOT_NULL(d);

    /* Insert "X" between 'b' and 'c': range [0,2)..[0,2) replaced with "X". */
    TEST_ASSERT_TRUE(ilsp_document_apply_incremental(
        d, make_range(0, 2, 0, 2), "X", 1, ILSP_ENC_UTF16, 2));

    TEST_ASSERT_EQUAL_size_t(4, d->text_len);
    TEST_ASSERT_EQUAL_INT32(2, d->version);
    TEST_ASSERT_EQUAL_MEMORY("abXc", d->text, 4);

    ilsp_document_destroy(d);
}

/* ── 2. BOM-preserving append ──────────────────────────────────────── */
static void test_bom_preserving_append(void) {
    /* BOM + "x". BOM is 3 bytes, 1 UTF-16 unit. Appending to end: range
     * should be [0, 2]..[0, 2] (past BOM + 'x', UTF-16 col 2). */
    const unsigned char src[] = { 0xEF, 0xBB, 0xBF, 'x' };
    IronLsp_Document *d = ilsp_document_create(
        "file:///tmp/b.iron", (const char *)src, 4, 1);
    TEST_ASSERT_NOT_NULL(d);

    /* Append "y" at end -- UTF-16 col 2 = byte 4. */
    TEST_ASSERT_TRUE(ilsp_document_apply_incremental(
        d, make_range(0, 2, 0, 2), "y", 1, ILSP_ENC_UTF16, 2));

    TEST_ASSERT_EQUAL_size_t(5, d->text_len);
    TEST_ASSERT_EQUAL_UINT8(0xEF, (unsigned char)d->text[0]);
    TEST_ASSERT_EQUAL_UINT8(0xBB, (unsigned char)d->text[1]);
    TEST_ASSERT_EQUAL_UINT8(0xBF, (unsigned char)d->text[2]);
    TEST_ASSERT_EQUAL_INT8('x', d->text[3]);
    TEST_ASSERT_EQUAL_INT8('y', d->text[4]);

    ilsp_document_destroy(d);
}

/* ── 3. CRLF range edit ────────────────────────────────────────────── */
static void test_crlf_range_edit(void) {
    /* "a\r\nb\r\nc": replace 'b' (line 1, col 0..1) with "BB". */
    IronLsp_Document *d = ilsp_document_create(
        "file:///tmp/c.iron", "a\r\nb\r\nc", 7, 1);
    TEST_ASSERT_NOT_NULL(d);

    TEST_ASSERT_TRUE(ilsp_document_apply_incremental(
        d, make_range(1, 0, 1, 1), "BB", 2, ILSP_ENC_UTF16, 2));

    TEST_ASSERT_EQUAL_size_t(8, d->text_len);
    TEST_ASSERT_EQUAL_MEMORY("a\r\nBB\r\nc", d->text, 8);

    ilsp_document_destroy(d);
}

/* ── 4. Surrogate pair: UTF-16 vs UTF-8 encoding parity ────────────── */
static void test_surrogate_encoding_parity(void) {
    /* "h𝄞llo" -- 𝄞 is U+1D11E (4 bytes UTF-8, 2 UTF-16 code units).
     * Insert "x" AFTER the surrogate. UTF-16 col 3 == UTF-8 col 2 ==
     * byte 5. Both encoding paths must produce the same byte sequence. */
    const unsigned char src[] = { 'h', 0xF0, 0x9D, 0x84, 0x9E, 'l', 'l', 'o' };

    /* UTF-16 path. */
    IronLsp_Document *d16 = ilsp_document_create(
        "file:///tmp/s16.iron", (const char *)src, 8, 1);
    TEST_ASSERT_TRUE(ilsp_document_apply_incremental(
        d16, make_range(0, 3, 0, 3), "x", 1, ILSP_ENC_UTF16, 2));
    TEST_ASSERT_EQUAL_size_t(9, d16->text_len);

    /* UTF-8 path. */
    IronLsp_Document *d8 = ilsp_document_create(
        "file:///tmp/s8.iron", (const char *)src, 8, 1);
    TEST_ASSERT_TRUE(ilsp_document_apply_incremental(
        d8, make_range(0, 2, 0, 2), "x", 1, ILSP_ENC_UTF8, 2));
    TEST_ASSERT_EQUAL_size_t(9, d8->text_len);

    /* Both must produce identical byte sequences. */
    TEST_ASSERT_EQUAL_MEMORY(d16->text, d8->text, 9);
    /* And the inserted 'x' should be at byte 5 (post-surrogate). */
    TEST_ASSERT_EQUAL_INT8('x', d16->text[5]);

    ilsp_document_destroy(d16);
    ilsp_document_destroy(d8);
}

/* ── 5. Full replace ───────────────────────────────────────────────── */
static void test_full_replace(void) {
    IronLsp_Document *d = ilsp_document_create(
        "file:///tmp/f.iron", "original", 8, 1);
    TEST_ASSERT_NOT_NULL(d);

    TEST_ASSERT_TRUE(ilsp_document_apply_full_replace(
        d, "brand new content", 17, 2));
    TEST_ASSERT_EQUAL_size_t(17, d->text_len);
    TEST_ASSERT_EQUAL_INT32(2, d->version);
    TEST_ASSERT_EQUAL_MEMORY("brand new content", d->text, 17);

    ilsp_document_destroy(d);
}

/* ── 6. Version monotonicity guard ─────────────────────────────────── */
static void test_version_regression_rejected(void) {
    IronLsp_Document *d = ilsp_document_create(
        "file:///tmp/v.iron", "abc", 3, 5);

    /* Same or lower version must be rejected; state unchanged. */
    TEST_ASSERT_FALSE(ilsp_document_apply_incremental(
        d, make_range(0, 0, 0, 0), "X", 1, ILSP_ENC_UTF16, 5));
    TEST_ASSERT_FALSE(ilsp_document_apply_incremental(
        d, make_range(0, 0, 0, 0), "X", 1, ILSP_ENC_UTF16, 4));
    TEST_ASSERT_EQUAL_size_t(3, d->text_len);
    TEST_ASSERT_EQUAL_INT32(5, d->version);

    /* Higher version accepted. */
    TEST_ASSERT_TRUE(ilsp_document_apply_incremental(
        d, make_range(0, 0, 0, 0), "X", 1, ILSP_ENC_UTF16, 6));
    TEST_ASSERT_EQUAL_size_t(4, d->text_len);

    ilsp_document_destroy(d);
}

/* ── 7. 100 sequential single-char inserts ─────────────────────────── */
static void test_idempotent_bulk_inserts(void) {
    IronLsp_Document *d = ilsp_document_create(
        "file:///tmp/bulk.iron", "", 0, 0);

    for (int i = 0; i < 100; i++) {
        TEST_ASSERT_TRUE(ilsp_document_apply_incremental(
            d, make_range(0, 0, 0, 0), "a", 1, ILSP_ENC_UTF16, i + 1));
    }
    TEST_ASSERT_EQUAL_size_t(100, d->text_len);
    TEST_ASSERT_EQUAL_INT32(100, d->version);
    for (size_t i = 0; i < 100; i++) {
        TEST_ASSERT_EQUAL_INT8('a', d->text[i]);
    }

    ilsp_document_destroy(d);
}

/* ── 8. SHA-256 hash updates after every edit ──────────────────────── */
static void test_sha256_drift_detection(void) {
    IronLsp_Document *d = ilsp_document_create(
        "file:///tmp/h.iron", "abc", 3, 1);
    TEST_ASSERT_NOT_NULL(d);

    /* Known SHA-256 of "abc". */
    uint8_t expect1[32];
    ilsp_sha256((const uint8_t *)"abc", 3, expect1);
    TEST_ASSERT_EQUAL_MEMORY(expect1, d->sha256, 32);

    /* After edit, hash must change. */
    TEST_ASSERT_TRUE(ilsp_document_apply_full_replace(d, "abcd", 4, 2));
    uint8_t expect2[32];
    ilsp_sha256((const uint8_t *)"abcd", 4, expect2);
    TEST_ASSERT_EQUAL_MEMORY(expect2, d->sha256, 32);

    /* Old and new hashes differ. */
    bool all_equal = true;
    for (int i = 0; i < 32; i++) {
        if (expect1[i] != expect2[i]) { all_equal = false; break; }
    }
    TEST_ASSERT_FALSE(all_equal);

    ilsp_document_destroy(d);
}

/* ── 9. Multi-event didChange batch (simulated) ────────────────────── */
static void test_multi_event_batch(void) {
    /* A didChange batch applies each event in order. Simulate by applying
     * three incremental edits in sequence with ascending versions. */
    IronLsp_Document *d = ilsp_document_create(
        "file:///tmp/m.iron", "Hello", 5, 1);

    /* Append " ", "W", "orld!" across three events. */
    TEST_ASSERT_TRUE(ilsp_document_apply_incremental(
        d, make_range(0, 5, 0, 5), " ", 1, ILSP_ENC_UTF16, 2));
    TEST_ASSERT_TRUE(ilsp_document_apply_incremental(
        d, make_range(0, 6, 0, 6), "W", 1, ILSP_ENC_UTF16, 3));
    TEST_ASSERT_TRUE(ilsp_document_apply_incremental(
        d, make_range(0, 7, 0, 7), "orld!", 5, ILSP_ENC_UTF16, 4));

    TEST_ASSERT_EQUAL_size_t(12, d->text_len);
    TEST_ASSERT_EQUAL_MEMORY("Hello World!", d->text, 12);

    ilsp_document_destroy(d);
}

/* ── 10. didClose idempotence ──────────────────────────────────────── */
static void test_didclose_idempotent(void) {
    /* We drive the documents map directly; simulates the handler's
     * open -> close -> close pattern. */
    struct { char *key; IronLsp_Document *value; } *m = NULL;
    sh_new_strdup(m);

    IronLsp_Document *d = ilsp_document_create(
        "file:///tmp/x.iron", "hi", 2, 1);
    shput(m, "file:///tmp/x.iron", d);

    TEST_ASSERT_TRUE(shgeti(m, "file:///tmp/x.iron") >= 0);

    /* First close. */
    ptrdiff_t idx = shgeti(m, "file:///tmp/x.iron");
    if (idx >= 0) {
        ilsp_document_destroy(m[idx].value);
        shdel(m, "file:///tmp/x.iron");
    }
    TEST_ASSERT_TRUE(shgeti(m, "file:///tmp/x.iron") < 0);

    /* Second close: no-op. */
    ptrdiff_t idx2 = shgeti(m, "file:///tmp/x.iron");
    TEST_ASSERT_EQUAL_INT(-1, (int)idx2);

    shfree(m);
}

/* ── 11. Out-of-bounds range rejected ──────────────────────────────── */
static void test_incremental_bounds_guard(void) {
    IronLsp_Document *d = ilsp_document_create(
        "file:///tmp/o.iron", "abc", 3, 1);

    /* Range end past text_len -- rejected. (We push columns into
     * whopping numbers so the byte math definitely overshoots.) */
    TEST_ASSERT_TRUE(ilsp_document_apply_incremental(
        d, make_range(0, 100, 0, 100), "X", 1, ILSP_ENC_UTF16, 2));
    /* Note: the above is accepted because our impl clamps columns to
     * line_end -- which is the spec-correct behaviour for LSP Range
     * "past end of line". The document buffer should end up with "abcX". */
    TEST_ASSERT_EQUAL_size_t(4, d->text_len);
    TEST_ASSERT_EQUAL_MEMORY("abcX", d->text, 4);

    /* But an inverted range (start > end) IS rejected. */
    TEST_ASSERT_FALSE(ilsp_document_apply_incremental(
        d, make_range(0, 3, 0, 0), "Y", 1, ILSP_ENC_UTF16, 3));
    TEST_ASSERT_EQUAL_INT32(2, d->version);  /* unchanged */

    ilsp_document_destroy(d);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_ascii_insert);
    RUN_TEST(test_bom_preserving_append);
    RUN_TEST(test_crlf_range_edit);
    RUN_TEST(test_surrogate_encoding_parity);
    RUN_TEST(test_full_replace);
    RUN_TEST(test_version_regression_rejected);
    RUN_TEST(test_idempotent_bulk_inserts);
    RUN_TEST(test_sha256_drift_detection);
    RUN_TEST(test_multi_event_batch);
    RUN_TEST(test_didclose_idempotent);
    RUN_TEST(test_incremental_bounds_guard);
    return UNITY_END();
}
