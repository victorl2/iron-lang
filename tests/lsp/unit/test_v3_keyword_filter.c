/* test_v3_keyword_filter -- Phase 12 Plan 12-02 (KW-03, D-04..D-10).
 *
 * Real predicate-matrix assertions over ilsp_keyword_visible_at across
 * the 6 v3 keywords plus 4 baseline pre-v3 keywords. Each case
 * constructs a tiny in-memory IronLsp_Document via ilsp_document_create
 * and synthesizes (or omits) a one-OBJECT_DECL Iron_Program in an
 * arena, positions the cursor, invokes the predicate, asserts the
 * boolean result.
 *
 * Coverage matrix (per VALIDATION.md row 12-02-KW-03):
 *   - pub at decl-head, top-level (true), in expr position (false)
 *   - pub at decl-head inside object body (true), inside patch body (true)
 *   - init inside object body (true), at top-level (false)
 *   - readonly/pure with `func` next (true) and without (false)
 *   - mut in receiver position (true) and outside parens (false)
 *   - mut in broken-syntax case `func update(mu` (true) — Pitfall 10
 *   - 4 baseline keywords (val/var/func/if) under EXPR_HEAD (true) and
 *     UNKNOWN-equivalent (false) — exercises default arm.
 */

#include "lsp/facade/edit/complete/keyword_filter.h"
#include "lsp/facade/edit/complete/context_classify.h"
#include "lsp/store/document.h"
#include "parser/ast.h"
#include "util/arena.h"

#include "unity.h"

#include <stdint.h>
#include <string.h>

void setUp(void)    {}
void tearDown(void) {}

/* ── Helpers ──────────────────────────────────────────────────────── */

/* Build a tiny single-OBJECT_DECL Iron_Program in `arena`. The object
 * decl has the requested span (1-indexed inclusive line range). The
 * Iron_Program is allocated in the same arena and its decls[] points
 * at the one node. */
static Iron_Program *make_one_object_program(Iron_Arena *arena,
                                                uint32_t obj_start_line,
                                                uint32_t obj_end_line,
                                                bool     is_patch) {
    Iron_ObjectDecl *od = (Iron_ObjectDecl *)iron_arena_alloc(
        arena, sizeof(*od), _Alignof(Iron_ObjectDecl));
    TEST_ASSERT_NOT_NULL(od);
    memset(od, 0, sizeof(*od));
    od->kind = IRON_NODE_OBJECT_DECL;
    od->span.line     = obj_start_line;
    od->span.col      = 1;
    od->span.end_line = obj_end_line;
    od->span.end_col  = 1;
    od->name          = "T";
    od->is_patch      = is_patch;
    od->target_type_name = is_patch ? "T" : NULL;

    Iron_Node **decls = (Iron_Node **)iron_arena_alloc(
        arena, sizeof(Iron_Node *) * 1, _Alignof(Iron_Node *));
    TEST_ASSERT_NOT_NULL(decls);
    decls[0] = (Iron_Node *)od;

    Iron_Program *prog = (Iron_Program *)iron_arena_alloc(
        arena, sizeof(*prog), _Alignof(Iron_Program));
    TEST_ASSERT_NOT_NULL(prog);
    memset(prog, 0, sizeof(*prog));
    prog->kind       = IRON_NODE_PROGRAM;
    prog->decls      = decls;
    prog->decl_count = 1;
    prog->sealed     = true;
    return prog;
}

/* Convert a 1-indexed (line, col) into a doc-friendly 0-indexed pair.
 * Defensive: callers pass 1-indexed for source readability. */
static void li_col_to_lsp(uint32_t line_1, uint32_t col_1,
                            uint32_t *out_line, uint32_t *out_col) {
    *out_line = line_1 - 1;
    *out_col  = col_1 - 1;
}

/* ── pub ──────────────────────────────────────────────────────────── */

static void test_pub_at_top_level_decl_head_true(void) {
    /* `\n    p|` — line 2 column 5. No program -> top-level fallback OK. */
    const char *text = "\n    p";
    IronLsp_Document *doc = ilsp_document_create("file:///t.iron",
                                                    text, strlen(text), 1);
    TEST_ASSERT_NOT_NULL(doc);
    uint32_t line, col;
    li_col_to_lsp(2, 6, &line, &col);  /* cursor right after `p` */
    bool v = ilsp_keyword_visible_at("pub", doc, NULL, line, col,
                                       ILSP_CCTX_STATEMENT_HEAD);
    TEST_ASSERT_TRUE(v);
    ilsp_document_destroy(doc);
}

static void test_pub_in_expr_position_false(void) {
    /* `let x = a + p|` — operator before cursor disqualifies decl-head. */
    const char *text = "let x = a + p";
    IronLsp_Document *doc = ilsp_document_create("file:///t.iron",
                                                    text, strlen(text), 1);
    TEST_ASSERT_NOT_NULL(doc);
    uint32_t line = 0;
    uint32_t col  = (uint32_t)strlen(text);
    bool v = ilsp_keyword_visible_at("pub", doc, NULL, line, col,
                                       ILSP_CCTX_EXPR_HEAD);
    TEST_ASSERT_FALSE(v);
    ilsp_document_destroy(doc);
}

static void test_pub_inside_object_body_true(void) {
    /* Tiny program: an OBJECT_DECL spanning lines 1..3. Cursor on line
     * 2 column 5 (inside body). */
    Iron_Arena arena = iron_arena_create(4096);
    Iron_Program *prog = make_one_object_program(&arena, 1, 3, false);

    const char *text = "object T {\n    p\n}\n";
    IronLsp_Document *doc = ilsp_document_create("file:///t.iron",
                                                    text, strlen(text), 1);
    TEST_ASSERT_NOT_NULL(doc);
    uint32_t line, col;
    li_col_to_lsp(2, 6, &line, &col);
    bool v = ilsp_keyword_visible_at("pub", doc, prog, line, col,
                                       ILSP_CCTX_STATEMENT_HEAD);
    TEST_ASSERT_TRUE(v);
    ilsp_document_destroy(doc);
    iron_arena_free(&arena);
}

static void test_pub_inside_patch_body_true(void) {
    /* Same shape but is_patch=true. Predicate must NOT filter is_patch
     * out — both classic + patch are valid pub homes. */
    Iron_Arena arena = iron_arena_create(4096);
    Iron_Program *prog = make_one_object_program(&arena, 1, 3, true);

    const char *text = "patch object T {\n    p\n}\n";
    IronLsp_Document *doc = ilsp_document_create("file:///t.iron",
                                                    text, strlen(text), 1);
    TEST_ASSERT_NOT_NULL(doc);
    uint32_t line, col;
    li_col_to_lsp(2, 6, &line, &col);
    bool v = ilsp_keyword_visible_at("pub", doc, prog, line, col,
                                       ILSP_CCTX_STATEMENT_HEAD);
    TEST_ASSERT_TRUE(v);
    ilsp_document_destroy(doc);
    iron_arena_free(&arena);
}

/* ── init ─────────────────────────────────────────────────────────── */

static void test_init_inside_object_body_true(void) {
    Iron_Arena arena = iron_arena_create(4096);
    Iron_Program *prog = make_one_object_program(&arena, 1, 3, false);

    const char *text = "object T {\n    i\n}\n";
    IronLsp_Document *doc = ilsp_document_create("file:///t.iron",
                                                    text, strlen(text), 1);
    TEST_ASSERT_NOT_NULL(doc);
    uint32_t line, col;
    li_col_to_lsp(2, 6, &line, &col);
    bool v = ilsp_keyword_visible_at("init", doc, prog, line, col,
                                       ILSP_CCTX_STATEMENT_HEAD);
    TEST_ASSERT_TRUE(v);
    ilsp_document_destroy(doc);
    iron_arena_free(&arena);
}

static void test_init_at_top_level_false(void) {
    /* No program, or cursor outside any object decl -> strict refusal. */
    const char *text = "i";
    IronLsp_Document *doc = ilsp_document_create("file:///t.iron",
                                                    text, strlen(text), 1);
    TEST_ASSERT_NOT_NULL(doc);
    uint32_t line = 0, col = 1;
    bool v = ilsp_keyword_visible_at("init", doc, NULL, line, col,
                                       ILSP_CCTX_STATEMENT_HEAD);
    TEST_ASSERT_FALSE(v);
    ilsp_document_destroy(doc);
}

/* ── readonly / pure ──────────────────────────────────────────────── */

static void test_readonly_with_func_follows_true(void) {
    /* Cursor at start, then ` func` follows. */
    const char *text = " func foo() {}";
    IronLsp_Document *doc = ilsp_document_create("file:///t.iron",
                                                    text, strlen(text), 1);
    TEST_ASSERT_NOT_NULL(doc);
    uint32_t line = 0, col = 0;
    bool v = ilsp_keyword_visible_at("readonly", doc, NULL, line, col,
                                       ILSP_CCTX_STATEMENT_HEAD);
    TEST_ASSERT_TRUE(v);
    ilsp_document_destroy(doc);
}

static void test_pure_with_func_follows_true(void) {
    const char *text = "  func bar() {}";
    IronLsp_Document *doc = ilsp_document_create("file:///t.iron",
                                                    text, strlen(text), 1);
    TEST_ASSERT_NOT_NULL(doc);
    uint32_t line = 0, col = 0;
    bool v = ilsp_keyword_visible_at("pure", doc, NULL, line, col,
                                       ILSP_CCTX_STATEMENT_HEAD);
    TEST_ASSERT_TRUE(v);
    ilsp_document_destroy(doc);
}

static void test_readonly_without_func_false(void) {
    /* Cursor before `val` — `func` does not follow. */
    const char *text = " val x = 1";
    IronLsp_Document *doc = ilsp_document_create("file:///t.iron",
                                                    text, strlen(text), 1);
    TEST_ASSERT_NOT_NULL(doc);
    uint32_t line = 0, col = 0;
    bool v = ilsp_keyword_visible_at("readonly", doc, NULL, line, col,
                                       ILSP_CCTX_STATEMENT_HEAD);
    TEST_ASSERT_FALSE(v);
    ilsp_document_destroy(doc);
}

/* ── mut ──────────────────────────────────────────────────────────── */

static void test_mut_in_receiver_position_true(void) {
    /* `func (` then cursor — mut in receiver pos. */
    const char *text = "func (";
    IronLsp_Document *doc = ilsp_document_create("file:///t.iron",
                                                    text, strlen(text), 1);
    TEST_ASSERT_NOT_NULL(doc);
    uint32_t line = 0;
    uint32_t col = (uint32_t)strlen(text);
    bool v = ilsp_keyword_visible_at("mut", doc, NULL, line, col,
                                       ILSP_CCTX_EXPR_HEAD);
    TEST_ASSERT_TRUE(v);
    ilsp_document_destroy(doc);
}

static void test_mut_broken_syntax_partial_ident_true(void) {
    /* Pitfall 10: `func (mu` truncated source — predicate fires
     * because partial-ident skip + ws + `(` + `func` chain matches.
     * v2 receiver syntax is `func (recv: T) name(...)`; the user is
     * mid-typing `mut` immediately inside the receiver parens, before
     * any identifier or type annotation has been written. */
    const char *text = "func (mu";
    IronLsp_Document *doc = ilsp_document_create("file:///t.iron",
                                                    text, strlen(text), 1);
    TEST_ASSERT_NOT_NULL(doc);
    uint32_t line = 0;
    uint32_t col = (uint32_t)strlen(text);
    bool v = ilsp_keyword_visible_at("mut", doc, NULL, line, col,
                                       ILSP_CCTX_EXPR_HEAD);
    TEST_ASSERT_TRUE(v);
    ilsp_document_destroy(doc);
}

static void test_mut_outside_parens_false(void) {
    /* No `(` before cursor's enclosing chain. */
    const char *text = "let m = ";
    IronLsp_Document *doc = ilsp_document_create("file:///t.iron",
                                                    text, strlen(text), 1);
    TEST_ASSERT_NOT_NULL(doc);
    uint32_t line = 0;
    uint32_t col = (uint32_t)strlen(text);
    bool v = ilsp_keyword_visible_at("mut", doc, NULL, line, col,
                                       ILSP_CCTX_EXPR_HEAD);
    TEST_ASSERT_FALSE(v);
    ilsp_document_destroy(doc);
}

/* ── default arm (38 pre-v3 keywords) ─────────────────────────────── */

static void test_default_keyword_in_expr_head_true(void) {
    /* Each of `val`, `var`, `func`, `if` should fire under EXPR_HEAD. */
    const char *text = " ";
    IronLsp_Document *doc = ilsp_document_create("file:///t.iron",
                                                    text, strlen(text), 1);
    TEST_ASSERT_NOT_NULL(doc);
    uint32_t line = 0, col = 0;
    TEST_ASSERT_TRUE(ilsp_keyword_visible_at("val",  doc, NULL, line, col,
                                                ILSP_CCTX_EXPR_HEAD));
    TEST_ASSERT_TRUE(ilsp_keyword_visible_at("var",  doc, NULL, line, col,
                                                ILSP_CCTX_EXPR_HEAD));
    TEST_ASSERT_TRUE(ilsp_keyword_visible_at("func", doc, NULL, line, col,
                                                ILSP_CCTX_EXPR_HEAD));
    TEST_ASSERT_TRUE(ilsp_keyword_visible_at("if",   doc, NULL, line, col,
                                                ILSP_CCTX_STATEMENT_HEAD));
    ilsp_document_destroy(doc);
}

static void test_default_keyword_in_member_after_dot_false(void) {
    /* MEMBER_AFTER_DOT must suppress default keywords (Phase 4 EDIT-06). */
    const char *text = " ";
    IronLsp_Document *doc = ilsp_document_create("file:///t.iron",
                                                    text, strlen(text), 1);
    TEST_ASSERT_NOT_NULL(doc);
    uint32_t line = 0, col = 0;
    TEST_ASSERT_FALSE(ilsp_keyword_visible_at("val",  doc, NULL, line, col,
                                                 ILSP_CCTX_MEMBER_AFTER_DOT));
    TEST_ASSERT_FALSE(ilsp_keyword_visible_at("if",   doc, NULL, line, col,
                                                 ILSP_CCTX_TYPE_POSITION));
    ilsp_document_destroy(doc);
}

/* ── NULL-safety ─────────────────────────────────────────────────── */

static void test_null_inputs_return_false(void) {
    TEST_ASSERT_FALSE(ilsp_keyword_visible_at(NULL, NULL, NULL, 0, 0,
                                                 ILSP_CCTX_EXPR_HEAD));
    const char *text = " ";
    IronLsp_Document *doc = ilsp_document_create("file:///t.iron",
                                                    text, strlen(text), 1);
    TEST_ASSERT_FALSE(ilsp_keyword_visible_at("pub", NULL, NULL, 0, 0,
                                                 ILSP_CCTX_EXPR_HEAD));
    TEST_ASSERT_FALSE(ilsp_keyword_visible_at(NULL, doc, NULL, 0, 0,
                                                 ILSP_CCTX_EXPR_HEAD));
    ilsp_document_destroy(doc);
}

/* ── Driver ───────────────────────────────────────────────────────── */

int main(void) {
    UNITY_BEGIN();
    /* pub */
    RUN_TEST(test_pub_at_top_level_decl_head_true);
    RUN_TEST(test_pub_in_expr_position_false);
    RUN_TEST(test_pub_inside_object_body_true);
    RUN_TEST(test_pub_inside_patch_body_true);
    /* init */
    RUN_TEST(test_init_inside_object_body_true);
    RUN_TEST(test_init_at_top_level_false);
    /* readonly / pure */
    RUN_TEST(test_readonly_with_func_follows_true);
    RUN_TEST(test_pure_with_func_follows_true);
    RUN_TEST(test_readonly_without_func_false);
    /* mut */
    RUN_TEST(test_mut_in_receiver_position_true);
    RUN_TEST(test_mut_broken_syntax_partial_ident_true);
    RUN_TEST(test_mut_outside_parens_false);
    /* default arm */
    RUN_TEST(test_default_keyword_in_expr_head_true);
    RUN_TEST(test_default_keyword_in_member_after_dot_false);
    /* NULL-safety */
    RUN_TEST(test_null_inputs_return_false);
    return UNITY_END();
}
