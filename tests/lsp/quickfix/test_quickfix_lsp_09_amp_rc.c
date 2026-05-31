/* Phase 34 Plan 34-04 (LSP-09) — quickfix_amp_rc driver. */

#include "quickfix_fixture_runner.h"

void setUp(void)    {}
void tearDown(void) {}

static void test_lsp_09_amp_rc(void) {
    char src_path[1024];
    qf34_fixture_path(src_path, sizeof(src_path), "quickfix_amp_rc.iron");
    char *src = qf34_slurp(src_path);
    TEST_ASSERT_NOT_NULL(src);

    char exp_path[1024];
    qf34_fixture_path(exp_path, sizeof(exp_path),
                      "quickfix_amp_rc.expected_edit");
    char *exp = qf34_slurp(exp_path);
    TEST_ASSERT_NOT_NULL(exp);

    char exp_title[256];
    uint32_t sl=0, sc=0, el=0, ec=0;
    char exp_new[1024];
    TEST_ASSERT_TRUE(qf34_parse_single(exp, exp_title, sizeof(exp_title),
                                       &sl, &sc, &el, &ec,
                                       exp_new, sizeof(exp_new)));

    IronLsp_Document *doc = ilsp_document_create("file:///lsp_09.iron",
                                                   src, strlen(src), 1);
    TEST_ASSERT_NOT_NULL(doc);

    Iron_Arena arena = iron_arena_create(64 * 1024);
    /* `&r` expression on 1-indexed line 7, cols 13..15. The fixture
     * file's line 7 is `    val p = &r` — `&r` starts at the 13th
     * 1-indexed col and is 2 bytes wide. */
    Iron_Diagnostic d = qf34_mk_diag(IRON_ERR_PTR_AMP_ON_RC,
                                       7, 13, 7, 15,
                                       "lsp_09.iron",
                                       "cannot take address of rc value");

    IronLsp_CodeAction out[ILSP_QUICKFIX_MAX_VARIANTS];
    size_t n = 0;
    ilsp_quickfix_amp_on_rc(&d, doc, NULL, &arena, out,
                              ILSP_QUICKFIX_MAX_VARIANTS, &n);

    TEST_ASSERT_EQUAL_size_t_MESSAGE(1, n, "LSP-09 must emit 1 action");
    TEST_ASSERT_EQUAL_STRING(exp_title, out[0].title);
    TEST_ASSERT_EQUAL_STRING("quickfix", out[0].kind);
    TEST_ASSERT_TRUE(out[0].is_preferred);
    TEST_ASSERT_EQUAL_UINT(sl, out[0].edit_start_line);
    TEST_ASSERT_EQUAL_UINT(sc, out[0].edit_start_char);
    TEST_ASSERT_EQUAL_UINT(el, out[0].edit_end_line);
    TEST_ASSERT_EQUAL_UINT(ec, out[0].edit_end_char);
    TEST_ASSERT_EQUAL_STRING(exp_new, out[0].edit_new_text);

    iron_arena_free(&arena);
    ilsp_document_destroy(doc);
    free(src);
    free(exp);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_lsp_09_amp_rc);
    return UNITY_END();
}
