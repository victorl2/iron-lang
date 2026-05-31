/* Phase 34 Plan 34-04 (LSP-10) — quickfix_readonly_memory driver.
 *
 * Synthesizes an IRON_ERR_READONLY_MEMORY=820 diagnostic anchored at
 * the offending `heap Buffer(64)` allocation inside a `readonly func`,
 * dispatches the two-variant handler, and diffs both produced
 * CodeActions against the .expected_edit file.
 *
 * Compiler-side emission for code 820 is reserved by Plan 34-01 (the
 * symbol is in diagnostics.h) but the analyzer emit site lands in a
 * follow-up plan. Synthesizing the diag here decouples handler-
 * correctness verification from emit-site readiness — the handler is
 * the surface this plan ships. */

#include "quickfix_fixture_runner.h"

void setUp(void)    {}
void tearDown(void) {}

/* Parse a multi-variant .expected_edit. Returns the number of variants
 * filled into the caller-provided parallel arrays. */
static size_t qf34_parse_multi(const char *content,
                                  char     titles[][256],
                                  uint32_t sl[], uint32_t sc[],
                                  uint32_t el[], uint32_t ec[],
                                  char     new_texts[][1024],
                                  size_t   max_variants) {
    size_t n = 0;
    const char *p = content;
    const char *line_end;
    int cur_variant = -1;
    while ((line_end = strchr(p, '\n')) != NULL) {
        size_t llen = (size_t)(line_end - p);
        if (strncmp(p, "variant: ", 9) == 0) {
            int v = atoi(p + 9);
            if (v >= 0 && (size_t)v < max_variants) {
                cur_variant = v;
                if ((size_t)v >= n) n = (size_t)v + 1;
            } else {
                cur_variant = -1;
            }
        } else if (cur_variant >= 0) {
            if (strncmp(p, "title: ", 7) == 0) {
                size_t tn = llen - 7;
                if (tn >= 256) tn = 255;
                memcpy(titles[cur_variant], p + 7, tn);
                titles[cur_variant][tn] = '\0';
            } else if (strncmp(p, "range: ", 7) == 0) {
                sscanf(p + 7, "%u:%u-%u:%u",
                       &sl[cur_variant], &sc[cur_variant],
                       &el[cur_variant], &ec[cur_variant]);
            } else if (strncmp(p, "new_text: ", 10) == 0) {
                size_t tn = llen - 10;
                char raw[1024];
                if (tn >= sizeof(raw)) tn = sizeof(raw) - 1;
                memcpy(raw, p + 10, tn);
                raw[tn] = '\0';
                qf34_decode_escapes(raw, tn,
                                    new_texts[cur_variant], 1024);
            } else if (strncmp(p, "new_text:", 9) == 0 && llen == 9) {
                /* Empty new_text — the deletion case. */
                new_texts[cur_variant][0] = '\0';
            }
        }
        p = line_end + 1;
    }
    return n;
}

static void test_lsp_10_readonly_memory(void) {
    char src_path[1024];
    qf34_fixture_path(src_path, sizeof(src_path), "quickfix_readonly_memory.iron");
    char *src = qf34_slurp(src_path);
    TEST_ASSERT_NOT_NULL(src);

    char exp_path[1024];
    qf34_fixture_path(exp_path, sizeof(exp_path),
                      "quickfix_readonly_memory.expected_edit");
    char *exp = qf34_slurp(exp_path);
    TEST_ASSERT_NOT_NULL(exp);

    char exp_titles[2][256] = {{0}};
    uint32_t sl[2] = {0}, sc[2] = {0}, el[2] = {0}, ec[2] = {0};
    char exp_new[2][1024] = {{0}};
    size_t parsed = qf34_parse_multi(exp, exp_titles, sl, sc, el, ec,
                                       exp_new, 2);
    TEST_ASSERT_EQUAL_size_t_MESSAGE(2, parsed,
        "LSP-10 expected_edit must declare 2 variants");

    IronLsp_Document *doc = ilsp_document_create("file:///lsp_10.iron",
                                                   src, strlen(src), 1);
    TEST_ASSERT_NOT_NULL(doc);

    Iron_Arena arena = iron_arena_create(64 * 1024);
    /* `heap Buffer(64)` allocation expression sits on 1-indexed line 7,
     * starting at col 17 ("    val extra = heap Buffer(64)"). The
     * compiler-side emit site (deferred plan) would anchor here. */
    Iron_Diagnostic d = qf34_mk_diag(IRON_ERR_READONLY_MEMORY,
                                       7, 17, 7, 32,
                                       "lsp_10.iron",
                                       "readonly fn touches heap allocation");

    IronLsp_CodeAction out[ILSP_QUICKFIX_MAX_VARIANTS];
    size_t n = 0;
    ilsp_quickfix_readonly_memory(&d, doc, NULL, &arena, out,
                                    ILSP_QUICKFIX_MAX_VARIANTS, &n);

    TEST_ASSERT_EQUAL_size_t_MESSAGE(2, n,
        "LSP-10 must emit 2 actions (Remove 'readonly' + Extract mutating block)");

    /* Variant 0. */
    TEST_ASSERT_NOT_NULL(out[0].title);
    TEST_ASSERT_EQUAL_STRING(exp_titles[0], out[0].title);
    TEST_ASSERT_EQUAL_STRING("quickfix", out[0].kind);
    TEST_ASSERT_FALSE_MESSAGE(out[0].is_preferred,
        "LSP-10 variant 0 must be is_preferred=false (D-31 ambiguity)");
    TEST_ASSERT_EQUAL_UINT(sl[0], out[0].edit_start_line);
    TEST_ASSERT_EQUAL_UINT(sc[0], out[0].edit_start_char);
    TEST_ASSERT_EQUAL_UINT(el[0], out[0].edit_end_line);
    TEST_ASSERT_EQUAL_UINT(ec[0], out[0].edit_end_char);
    TEST_ASSERT_NOT_NULL(out[0].edit_new_text);
    TEST_ASSERT_EQUAL_STRING(exp_new[0], out[0].edit_new_text);

    /* Variant 1. */
    TEST_ASSERT_NOT_NULL(out[1].title);
    TEST_ASSERT_EQUAL_STRING(exp_titles[1], out[1].title);
    TEST_ASSERT_EQUAL_STRING("quickfix", out[1].kind);
    TEST_ASSERT_FALSE_MESSAGE(out[1].is_preferred,
        "LSP-10 variant 1 must be is_preferred=false (D-31 ambiguity)");
    TEST_ASSERT_EQUAL_UINT(sl[1], out[1].edit_start_line);
    TEST_ASSERT_EQUAL_UINT(sc[1], out[1].edit_start_char);
    TEST_ASSERT_EQUAL_UINT(el[1], out[1].edit_end_line);
    TEST_ASSERT_EQUAL_UINT(ec[1], out[1].edit_end_char);
    TEST_ASSERT_NOT_NULL(out[1].edit_new_text);
    TEST_ASSERT_EQUAL_STRING(exp_new[1], out[1].edit_new_text);

    iron_arena_free(&arena);
    ilsp_document_destroy(doc);
    free(src);
    free(exp);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_lsp_10_readonly_memory);
    return UNITY_END();
}
