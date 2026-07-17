/* Phase 20 Wave 0 (Plan 20-01): TDD scaffold for `&` unary operator.
 *
 * Authored RED first; flips GREEN once Plan 20-01 lands the
 * IRON_TOK_AMP case in iron_parse_primary's unary-prefix block (mirroring
 * IRON_TOK_MINUS / IRON_TOK_NOT / IRON_TOK_TILDE).
 *
 * Disambiguation: `a & b` (binary AND) must STILL parse correctly because
 * iron_parse_primary consumes prefix `&` only when it appears in primary
 * (left-most) position; the binary `&` handler at PREC_BIT_AND remains.
 */
#include "unity.h"
#include "lexer/lexer.h"
#include "parser/parser.h"
#include "parser/ast.h"
#include "diagnostics/diagnostics.h"
#include "util/arena.h"
#include "stb_ds.h"

#include <string.h>

static Iron_Arena    arena;
static Iron_DiagList diags;

void setUp(void) {
    arena = iron_arena_create(131072);
    diags = iron_diaglist_create();
}

void tearDown(void) {
    iron_diaglist_free(&diags);
    iron_arena_free(&arena);
}

static Iron_Program *parse_src(const char *src) {
    Iron_Lexer lx = iron_lexer_create(src, "test.iron", &arena, &diags);
    Iron_Token *toks = iron_lex_all(&lx);
    int n = (int)arrlen(toks);
    Iron_Parser p = iron_parser_create(toks, n, src, "test.iron",
                                       &arena, &diags);
    Iron_Node *prog = iron_parse(&p);
    return (Iron_Program *)prog;
}

/* Walk a function body for the first val-decl init and return it. */
static Iron_Node *first_val_init(Iron_Program *prog, const char *fn_name) {
    if (!prog || !prog->decls) return NULL;
    for (int i = 0; i < prog->decl_count; i++) {
        Iron_Node *d = prog->decls[i];
        if (!d || d->kind != IRON_NODE_FUNC_DECL) continue;
        Iron_FuncDecl *fd = (Iron_FuncDecl *)d;
        if (!fd->name || strcmp(fd->name, fn_name) != 0) continue;
        if (!fd->body || fd->body->kind != IRON_NODE_BLOCK) continue;
        Iron_Block *blk = (Iron_Block *)fd->body;
        for (int j = 0; j < blk->stmt_count; j++) {
            Iron_Node *s = blk->stmts[j];
            if (!s) continue;
            if (s->kind == IRON_NODE_VAL_DECL) {
                Iron_ValDecl *vd = (Iron_ValDecl *)s;
                return vd->init;
            }
        }
    }
    return NULL;
}

/* `val x = &a` produces Iron_UnaryExpr{op=IRON_TOK_AMP, operand=IDENT(a)}. */
void test_amp_unary_on_ident(void) {
    Iron_Program *prog = parse_src(
        "func main() {\n"
        "    val a: Int = 5\n"
        "    val x = &a\n"
        "}\n");
    Iron_Node *init = first_val_init(prog, "main");
    /* main has TWO val decls; first_val_init returns the first; we want the
     * second `val x = &a` -- so iterate manually. */
    TEST_ASSERT_NOT_NULL(prog);
    Iron_Node *amp_init = NULL;
    for (int i = 0; i < prog->decl_count && !amp_init; i++) {
        Iron_Node *d = prog->decls[i];
        if (!d || d->kind != IRON_NODE_FUNC_DECL) continue;
        Iron_FuncDecl *fd = (Iron_FuncDecl *)d;
        if (!fd->body || fd->body->kind != IRON_NODE_BLOCK) continue;
        Iron_Block *blk = (Iron_Block *)fd->body;
        for (int j = 0; j < blk->stmt_count; j++) {
            Iron_Node *s = blk->stmts[j];
            if (s && s->kind == IRON_NODE_VAL_DECL) {
                Iron_ValDecl *vd = (Iron_ValDecl *)s;
                if (vd->name && strcmp(vd->name, "x") == 0) {
                    amp_init = vd->init;
                    break;
                }
            }
        }
    }
    (void)init;
    TEST_ASSERT_NOT_NULL(amp_init);
    TEST_ASSERT_EQUAL_INT(IRON_NODE_UNARY, amp_init->kind);
    Iron_UnaryExpr *u = (Iron_UnaryExpr *)amp_init;
    TEST_ASSERT_EQUAL_INT(IRON_TOK_AMP, (int)u->op);
    TEST_ASSERT_NOT_NULL(u->operand);
    TEST_ASSERT_EQUAL_INT(IRON_NODE_IDENT, u->operand->kind);
}

/* `val z = a & b` parses as a binary expression — disambiguation regression. */
void test_amp_binary_still_works(void) {
    Iron_Program *prog = parse_src(
        "func main() {\n"
        "    val a: Int = 1\n"
        "    val b: Int = 2\n"
        "    val z = a & b\n"
        "}\n");
    TEST_ASSERT_NOT_NULL(prog);
    Iron_Node *bin_init = NULL;
    for (int i = 0; i < prog->decl_count && !bin_init; i++) {
        Iron_Node *d = prog->decls[i];
        if (!d || d->kind != IRON_NODE_FUNC_DECL) continue;
        Iron_FuncDecl *fd = (Iron_FuncDecl *)d;
        if (!fd->body || fd->body->kind != IRON_NODE_BLOCK) continue;
        Iron_Block *blk = (Iron_Block *)fd->body;
        for (int j = 0; j < blk->stmt_count; j++) {
            Iron_Node *s = blk->stmts[j];
            if (s && s->kind == IRON_NODE_VAL_DECL) {
                Iron_ValDecl *vd = (Iron_ValDecl *)s;
                if (vd->name && strcmp(vd->name, "z") == 0) {
                    bin_init = vd->init;
                    break;
                }
            }
        }
    }
    TEST_ASSERT_NOT_NULL(bin_init);
    TEST_ASSERT_EQUAL_INT(IRON_NODE_BINARY, bin_init->kind);
}

/* `val z = &a.b` — `&` binds tighter than `.`? In a Pratt parser with
 * IRON_TOK_DOT at PREC_CALL and unary at PREC_UNARY (PREC_UNARY < PREC_CALL),
 * the prefix `&` actually applies AFTER `.b` is consumed by the operand
 * parser. So `&a.b` parses as Unary(AMP, FieldAccess(a, b)). */
void test_amp_unary_on_field_access(void) {
    Iron_Program *prog = parse_src(
        "object Point {\n"
        "    var x: Int\n"
        "    init() { self.x = 0 }\n"
        "}\n"
        "func main() {\n"
        "    var p = Point()\n"
        "    val z = &p.x\n"
        "}\n");
    TEST_ASSERT_NOT_NULL(prog);
    Iron_Node *amp_init = NULL;
    for (int i = 0; i < prog->decl_count && !amp_init; i++) {
        Iron_Node *d = prog->decls[i];
        if (!d || d->kind != IRON_NODE_FUNC_DECL) continue;
        Iron_FuncDecl *fd = (Iron_FuncDecl *)d;
        if (!fd->body || fd->body->kind != IRON_NODE_BLOCK) continue;
        Iron_Block *blk = (Iron_Block *)fd->body;
        for (int j = 0; j < blk->stmt_count; j++) {
            Iron_Node *s = blk->stmts[j];
            if (s && s->kind == IRON_NODE_VAL_DECL) {
                Iron_ValDecl *vd = (Iron_ValDecl *)s;
                if (vd->name && strcmp(vd->name, "z") == 0) {
                    amp_init = vd->init;
                    break;
                }
            }
        }
    }
    TEST_ASSERT_NOT_NULL(amp_init);
    TEST_ASSERT_EQUAL_INT(IRON_NODE_UNARY, amp_init->kind);
    Iron_UnaryExpr *u = (Iron_UnaryExpr *)amp_init;
    TEST_ASSERT_EQUAL_INT(IRON_TOK_AMP, (int)u->op);
    TEST_ASSERT_NOT_NULL(u->operand);
    /* operand is field-access (`p.x`). */
    TEST_ASSERT_EQUAL_INT(IRON_NODE_FIELD_ACCESS, u->operand->kind);
}

/* `& & a` — multi-level address-of legal at parse time. */
void test_amp_unary_nested(void) {
    Iron_Program *prog = parse_src(
        "func main() {\n"
        "    val a: Int = 5\n"
        "    val w = & &a\n"
        "}\n");
    TEST_ASSERT_NOT_NULL(prog);
    Iron_Node *outer = NULL;
    for (int i = 0; i < prog->decl_count && !outer; i++) {
        Iron_Node *d = prog->decls[i];
        if (!d || d->kind != IRON_NODE_FUNC_DECL) continue;
        Iron_FuncDecl *fd = (Iron_FuncDecl *)d;
        if (!fd->body || fd->body->kind != IRON_NODE_BLOCK) continue;
        Iron_Block *blk = (Iron_Block *)fd->body;
        for (int j = 0; j < blk->stmt_count; j++) {
            Iron_Node *s = blk->stmts[j];
            if (s && s->kind == IRON_NODE_VAL_DECL) {
                Iron_ValDecl *vd = (Iron_ValDecl *)s;
                if (vd->name && strcmp(vd->name, "w") == 0) {
                    outer = vd->init;
                    break;
                }
            }
        }
    }
    TEST_ASSERT_NOT_NULL(outer);
    TEST_ASSERT_EQUAL_INT(IRON_NODE_UNARY, outer->kind);
    Iron_UnaryExpr *uo = (Iron_UnaryExpr *)outer;
    TEST_ASSERT_EQUAL_INT(IRON_TOK_AMP, (int)uo->op);
    TEST_ASSERT_NOT_NULL(uo->operand);
    TEST_ASSERT_EQUAL_INT(IRON_NODE_UNARY, uo->operand->kind);
    Iron_UnaryExpr *ui = (Iron_UnaryExpr *)uo->operand;
    TEST_ASSERT_EQUAL_INT(IRON_TOK_AMP, (int)ui->op);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_amp_unary_on_ident);
    RUN_TEST(test_amp_binary_still_works);
    RUN_TEST(test_amp_unary_on_field_access);
    RUN_TEST(test_amp_unary_nested);
    return UNITY_END();
}
