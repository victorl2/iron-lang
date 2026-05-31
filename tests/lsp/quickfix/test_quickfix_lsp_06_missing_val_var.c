/* Phase 34 Plan 34-04 (LSP-06) — quickfix_missing_val_var driver.
 *
 * Synthesizes an IRON_ERR_MISSING_VAL_VAR=176 diagnostic anchored at
 * the `x` identifier in the LSP-06 fixture, dispatches the handler,
 * and diffs the produced CodeAction against the .expected_edit file. */

#include "quickfix_fixture_runner.h"

void setUp(void)    {}
void tearDown(void) {}

static void test_lsp_06_missing_val_var(void) {
    char src_path[1024];
    qf34_fixture_path(src_path, sizeof(src_path), "quickfix_missing_val_var.iron");
    char *src = qf34_slurp(src_path);
    TEST_ASSERT_NOT_NULL_MESSAGE(src, "fixture .iron must load");

    char exp_path[1024];
    qf34_fixture_path(exp_path, sizeof(exp_path),
                      "quickfix_missing_val_var.expected_edit");
    char *exp = qf34_slurp(exp_path);
    TEST_ASSERT_NOT_NULL_MESSAGE(exp, "fixture .expected_edit must load");

    char exp_title[256];
    uint32_t sl=0, sc=0, el=0, ec=0;
    char exp_new[1024];
    TEST_ASSERT_TRUE_MESSAGE(
        qf34_parse_single(exp, exp_title, sizeof(exp_title),
                          &sl, &sc, &el, &ec, exp_new, sizeof(exp_new)),
        "expected_edit must parse");

    IronLsp_Document *doc = ilsp_document_create("file:///lsp_06.iron",
                                                   src, strlen(src), 1);
    TEST_ASSERT_NOT_NULL(doc);

    Iron_Arena arena = iron_arena_create(64 * 1024);
    /* The `x` identifier sits at 1-indexed line 5, col 5 (file line 5 =
     * `    x = 10` with 4-space indent; `x` is the 5th char). */
    Iron_Diagnostic d = qf34_mk_diag(IRON_ERR_MISSING_VAL_VAR,
                                       5, 5, 5, 6,
                                       "lsp_06.iron",
                                       "binding missing val/var");

    IronLsp_CodeAction out[ILSP_QUICKFIX_MAX_VARIANTS];
    size_t n = 0;
    ilsp_quickfix_missing_val_var(&d, doc, NULL, &arena, out,
                                    ILSP_QUICKFIX_MAX_VARIANTS, &n);

    TEST_ASSERT_EQUAL_size_t_MESSAGE(1, n, "LSP-06 must emit 1 action");
    TEST_ASSERT_NOT_NULL(out[0].title);
    TEST_ASSERT_EQUAL_STRING(exp_title, out[0].title);
    TEST_ASSERT_EQUAL_STRING("quickfix", out[0].kind);
    TEST_ASSERT_TRUE(out[0].is_preferred);
    TEST_ASSERT_EQUAL_UINT(sl, out[0].edit_start_line);
    TEST_ASSERT_EQUAL_UINT(sc, out[0].edit_start_char);
    TEST_ASSERT_EQUAL_UINT(el, out[0].edit_end_line);
    TEST_ASSERT_EQUAL_UINT(ec, out[0].edit_end_char);
    TEST_ASSERT_NOT_NULL(out[0].edit_new_text);
    TEST_ASSERT_EQUAL_STRING(exp_new, out[0].edit_new_text);

    iron_arena_free(&arena);
    ilsp_document_destroy(doc);
    free(src);
    free(exp);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_lsp_06_missing_val_var);
    return UNITY_END();
}
