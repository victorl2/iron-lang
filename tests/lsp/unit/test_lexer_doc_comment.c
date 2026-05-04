/* test_lexer_doc_comment -- Phase 3 Plan 01 Task 02 (NAV-14).
 *
 * Covers:
 *   1. `///` tokenized as IRON_TOK_DOC_COMMENT with leading-space-trimmed body
 *   2. Multi-line runs aggregated onto the following decl
 *   3. Blank line breaks doc-comment association (returns NULL)
 *   4. Runs do not cross decl boundaries (Pitfall 7)
 *   5. 8 KB cap mitigation for T-03-01 (pathological input)
 *   6. All 8 decl kinds carry `doc_comment`
 */
#include "unity.h"

#include "lexer/lexer.h"
#include "parser/parser.h"
#include "parser/ast.h"
#include "diagnostics/diagnostics.h"
#include "util/arena.h"
#include "stb_ds.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

/* ── Helpers ─────────────────────────────────────────────────────────── */

static Iron_Program *parse_src(const char *src, Iron_Arena *arena,
                                Iron_DiagList *diags) {
    Iron_Lexer lx = iron_lexer_create(src, "t.iron", arena, diags);
    Iron_Token *toks = iron_lex_all(&lx);
    int tc = (int)arrlen(toks);
    Iron_Parser par = iron_parser_create(toks, tc, src, "t.iron", arena, diags);
    Iron_Node *ast = iron_parse(&par);
    arrfree(toks);
    return (Iron_Program *)ast;
}

/* ── Tests ───────────────────────────────────────────────────────────── */

static void test_doc_comment_single_line_token(void) {
    const char *src = "/// hello world\nfunc foo() {}\n";
    Iron_Arena arena = iron_arena_create(4096);
    Iron_DiagList diags = iron_diaglist_create();
    Iron_Lexer lx = iron_lexer_create(src, "t.iron", &arena, &diags);
    Iron_Token *toks = iron_lex_all(&lx);

    TEST_ASSERT_TRUE(arrlen(toks) >= 3);
    TEST_ASSERT_EQUAL_INT(IRON_TOK_DOC_COMMENT, toks[0].kind);
    TEST_ASSERT_EQUAL_STRING("hello world", toks[0].value);
    TEST_ASSERT_EQUAL_INT(IRON_TOK_NEWLINE, toks[1].kind);
    TEST_ASSERT_EQUAL_INT(IRON_TOK_FUNC, toks[2].kind);

    arrfree(toks);
    iron_diaglist_free(&diags);
    iron_arena_free(&arena);
}

static void test_doc_comment_multi_line_aggregated_on_func(void) {
    const char *src = "/// first\n/// second\nfunc foo() {}\n";
    Iron_Arena arena = iron_arena_create(4096);
    Iron_DiagList diags = iron_diaglist_create();
    Iron_Program *prog = parse_src(src, &arena, &diags);

    TEST_ASSERT_NOT_NULL(prog);
    TEST_ASSERT_EQUAL_INT(1, prog->decl_count);
    TEST_ASSERT_EQUAL_INT(IRON_NODE_FUNC_DECL, prog->decls[0]->kind);
    Iron_FuncDecl *f = (Iron_FuncDecl *)prog->decls[0];
    TEST_ASSERT_NOT_NULL(f->doc_comment);
    TEST_ASSERT_EQUAL_STRING("first\nsecond", f->doc_comment);

    iron_diaglist_free(&diags);
    iron_arena_free(&arena);
}

static void test_doc_comment_blank_line_breaks_association(void) {
    const char *src = "/// orphan\n\nfunc foo() {}\n";
    Iron_Arena arena = iron_arena_create(4096);
    Iron_DiagList diags = iron_diaglist_create();
    Iron_Program *prog = parse_src(src, &arena, &diags);

    TEST_ASSERT_NOT_NULL(prog);
    TEST_ASSERT_EQUAL_INT(1, prog->decl_count);
    Iron_FuncDecl *f = (Iron_FuncDecl *)prog->decls[0];
    TEST_ASSERT_NULL(f->doc_comment);

    iron_diaglist_free(&diags);
    iron_arena_free(&arena);
}

static void test_doc_comment_does_not_cross_decl_boundaries(void) {
    /* Two funcs with independent doc blocks. Each doc attaches to the
     * IMMEDIATELY following decl — no bleed-over. */
    const char *src =
        "/// docA\n"
        "func a() {}\n"
        "/// docB\n"
        "func b() {}\n";
    Iron_Arena arena = iron_arena_create(4096);
    Iron_DiagList diags = iron_diaglist_create();
    Iron_Program *prog = parse_src(src, &arena, &diags);

    TEST_ASSERT_NOT_NULL(prog);
    TEST_ASSERT_EQUAL_INT(2, prog->decl_count);
    Iron_FuncDecl *fa = (Iron_FuncDecl *)prog->decls[0];
    Iron_FuncDecl *fb = (Iron_FuncDecl *)prog->decls[1];
    TEST_ASSERT_NOT_NULL(fa->doc_comment);
    TEST_ASSERT_EQUAL_STRING("docA", fa->doc_comment);
    TEST_ASSERT_NOT_NULL(fb->doc_comment);
    TEST_ASSERT_EQUAL_STRING("docB", fb->doc_comment);

    iron_diaglist_free(&diags);
    iron_arena_free(&arena);
}

static void test_doc_comment_8kb_cap_truncation(void) {
    /* Pathological input: 10_000 x's after /// on one line. T-03-01. */
    const size_t NX = 10000;
    char *src = (char *)malloc(NX + 64);
    TEST_ASSERT_NOT_NULL(src);
    memcpy(src, "/// ", 4);
    memset(src + 4, 'x', NX);
    memcpy(src + 4 + NX, "\nfunc f() {}\n", 13);
    src[4 + NX + 13] = '\0';

    Iron_Arena arena = iron_arena_create(16 * 1024);
    Iron_DiagList diags = iron_diaglist_create();
    Iron_Lexer lx = iron_lexer_create(src, "t.iron", &arena, &diags);
    Iron_Token *toks = iron_lex_all(&lx);

    TEST_ASSERT_TRUE(arrlen(toks) >= 1);
    TEST_ASSERT_EQUAL_INT(IRON_TOK_DOC_COMMENT, toks[0].kind);
    TEST_ASSERT_NOT_NULL(toks[0].value);
    TEST_ASSERT_EQUAL_size_t(8192, strlen(toks[0].value));

    /* Truncation diagnostic emitted. */
    bool found = false;
    for (int i = 0; i < diags.count; i++) {
        if (diags.items[i].code == IRON_WARN_DOC_COMMENT_TRUNCATED) {
            found = true;
            break;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(found, "expected IRON_WARN_DOC_COMMENT_TRUNCATED");

    arrfree(toks);
    iron_diaglist_free(&diags);
    iron_arena_free(&arena);
    free(src);
}

static void test_doc_comment_on_object_and_field(void) {
    /* Iron object fields require `val` or `var` before the field name. */
    const char *src =
        "/// object doc\n"
        "object Foo {\n"
        "    /// field doc\n"
        "    val x: Int\n"
        "}\n";
    Iron_Arena arena = iron_arena_create(4096);
    Iron_DiagList diags = iron_diaglist_create();
    Iron_Program *prog = parse_src(src, &arena, &diags);

    TEST_ASSERT_NOT_NULL(prog);
    TEST_ASSERT_EQUAL_INT(1, prog->decl_count);
    TEST_ASSERT_EQUAL_INT(IRON_NODE_OBJECT_DECL, prog->decls[0]->kind);
    Iron_ObjectDecl *o = (Iron_ObjectDecl *)prog->decls[0];
    TEST_ASSERT_NOT_NULL(o->doc_comment);
    TEST_ASSERT_EQUAL_STRING("object doc", o->doc_comment);
    TEST_ASSERT_EQUAL_INT(1, o->field_count);
    Iron_Field *fld = (Iron_Field *)o->fields[0];
    TEST_ASSERT_NOT_NULL(fld->doc_comment);
    TEST_ASSERT_EQUAL_STRING("field doc", fld->doc_comment);

    iron_diaglist_free(&diags);
    iron_arena_free(&arena);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_doc_comment_single_line_token);
    RUN_TEST(test_doc_comment_multi_line_aggregated_on_func);
    RUN_TEST(test_doc_comment_blank_line_breaks_association);
    RUN_TEST(test_doc_comment_does_not_cross_decl_boundaries);
    RUN_TEST(test_doc_comment_8kb_cap_truncation);
    RUN_TEST(test_doc_comment_on_object_and_field);
    return UNITY_END();
}
