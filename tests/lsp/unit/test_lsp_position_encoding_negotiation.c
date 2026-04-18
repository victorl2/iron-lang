/* test_lsp_position_encoding_negotiation -- Phase 2 Plan 03 Task 02.
 *
 * Exercises the 4 canonical scenarios of
 * ilsp_capabilities_negotiate_position_encoding against the LSP 3.17
 * spec (CORE-07):
 *   1. client general.positionEncodings = ["utf-8"]          -> UTF-8
 *   2. client general.positionEncodings = ["utf-16"]         -> UTF-16
 *   3. client general.positionEncodings = ["utf-8","utf-16"] -> UTF-8 (preferred)
 *   4. client omits general.positionEncodings                -> UTF-16 (spec default)
 *
 * The test builds a mini client-capabilities yyjson object for each
 * scenario and calls the negotiator directly -- no dispatcher or
 * writer needed. */
#include "unity.h"
#include "lsp/server/capabilities.h"
#include "lsp/facade/types.h"
#include "vendor/yyjson/yyjson.h"

#include <stdlib.h>
#include <string.h>

void setUp(void)    {}
void tearDown(void) {}

/* Build a { general: { positionEncodings: [...] } } object and return a
 * value pointer into it. Caller owns the mut_doc lifetime. */
static yyjson_val *build_client_caps(yyjson_mut_doc *md, const char *enc1,
                                     const char *enc2) {
    yyjson_mut_val *root = yyjson_mut_obj(md);
    yyjson_mut_doc_set_root(md, root);
    yyjson_mut_val *general = yyjson_mut_obj(md);
    yyjson_mut_obj_add_val(md, root, "general", general);

    if (enc1 || enc2) {
        yyjson_mut_val *arr = yyjson_mut_arr(md);
        if (enc1) yyjson_mut_arr_add_strcpy(md, arr, enc1);
        if (enc2) yyjson_mut_arr_add_strcpy(md, arr, enc2);
        yyjson_mut_obj_add_val(md, general, "positionEncodings", arr);
    }

    /* Serialize then reparse so we get an immutable yyjson_doc. */
    size_t n = 0;
    char *s = yyjson_mut_write(md, 0, &n);
    TEST_ASSERT_NOT_NULL(s);
    yyjson_doc *imd = yyjson_read(s, n, 0);
    TEST_ASSERT_NOT_NULL(imd);
    /* Leak imd intentionally -- the test process exits shortly. Freeing
     * would invalidate the returned val. We retain a tiny bounded leak
     * per test run, acceptable for the hermetic Unity invocation. */
    (void)s;
    yyjson_val *caps_root = yyjson_doc_get_root(imd);
    /* Don't free `s` -- yyjson_read uses an internal arena; freeing the
     * buffer is fine but we need imd alive. */
    free(s);
    return caps_root;
}

/* ── Test 1: UTF-8 only ─────────────────────────────────────────────── */
static void test_utf8_only(void) {
    yyjson_mut_doc *md = yyjson_mut_doc_new(NULL);
    yyjson_val *cc = build_client_caps(md, "utf-8", NULL);
    IronLsp_PositionEncoding e =
        ilsp_capabilities_negotiate_position_encoding(cc);
    TEST_ASSERT_EQUAL_INT(ILSP_ENC_UTF8, e);
    yyjson_mut_doc_free(md);
}

/* ── Test 2: UTF-16 only ────────────────────────────────────────────── */
static void test_utf16_only(void) {
    yyjson_mut_doc *md = yyjson_mut_doc_new(NULL);
    yyjson_val *cc = build_client_caps(md, "utf-16", NULL);
    IronLsp_PositionEncoding e =
        ilsp_capabilities_negotiate_position_encoding(cc);
    TEST_ASSERT_EQUAL_INT(ILSP_ENC_UTF16, e);
    yyjson_mut_doc_free(md);
}

/* ── Test 3: both offered -> prefer UTF-8 ──────────────────────────── */
static void test_both_prefers_utf8(void) {
    yyjson_mut_doc *md = yyjson_mut_doc_new(NULL);
    yyjson_val *cc = build_client_caps(md, "utf-8", "utf-16");
    IronLsp_PositionEncoding e =
        ilsp_capabilities_negotiate_position_encoding(cc);
    TEST_ASSERT_EQUAL_INT(ILSP_ENC_UTF8, e);
    yyjson_mut_doc_free(md);

    /* Also order-independent: utf-16 listed first should still choose utf-8. */
    yyjson_mut_doc *md2 = yyjson_mut_doc_new(NULL);
    yyjson_val *cc2 = build_client_caps(md2, "utf-16", "utf-8");
    IronLsp_PositionEncoding e2 =
        ilsp_capabilities_negotiate_position_encoding(cc2);
    TEST_ASSERT_EQUAL_INT(ILSP_ENC_UTF8, e2);
    yyjson_mut_doc_free(md2);
}

/* ── Test 4: client silent -> spec default UTF-16 ──────────────────── */
static void test_client_silent_defaults_to_utf16(void) {
    /* Case A: positionEncodings key absent entirely. */
    yyjson_mut_doc *md = yyjson_mut_doc_new(NULL);
    yyjson_val *cc = build_client_caps(md, NULL, NULL);
    IronLsp_PositionEncoding e =
        ilsp_capabilities_negotiate_position_encoding(cc);
    TEST_ASSERT_EQUAL_INT(ILSP_ENC_UTF16, e);
    yyjson_mut_doc_free(md);

    /* Case B: client caps is NULL entirely. */
    IronLsp_PositionEncoding e2 =
        ilsp_capabilities_negotiate_position_encoding(NULL);
    TEST_ASSERT_EQUAL_INT(ILSP_ENC_UTF16, e2);
}

/* ── Test 5: canonical string helpers ──────────────────────────────── */
static void test_encoding_string_helper(void) {
    TEST_ASSERT_EQUAL_STRING("utf-8",  ilsp_position_encoding_str(ILSP_ENC_UTF8));
    TEST_ASSERT_EQUAL_STRING("utf-16", ilsp_position_encoding_str(ILSP_ENC_UTF16));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_utf8_only);
    RUN_TEST(test_utf16_only);
    RUN_TEST(test_both_prefers_utf8);
    RUN_TEST(test_client_silent_defaults_to_utf16);
    RUN_TEST(test_encoding_string_helper);
    return UNITY_END();
}
