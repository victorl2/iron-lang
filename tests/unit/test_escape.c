/* test_escape.c — Unity tests for the Iron escape analysis pass.
 *
 * Tests cover:
 *   - Non-escaping heap allocation => auto_free=true
 *   - Heap allocation returned without free => escapes=true, E0207
 *   - Heap allocation returned with free => no E0207
 *   - Heap allocation with leak => no error
 *   - rc value — no escape error even if escapes
 *   - free on non-heap value => E0212
 *   - leak on non-heap value => E0213
 *   - leak on rc value => E0214
 *   - Heap value escapes via assignment rhs => E0207
 *   - Heap value returned with leak => no E0207
 */

#include "unity.h"
#include "analyzer/resolve.h"
#include "analyzer/typecheck.h"
#include "analyzer/escape.h"
#include "analyzer/scope.h"
#include "analyzer/types.h"
#include "parser/ast.h"
#include "parser/parser.h"
#include "lexer/lexer.h"
#include "diagnostics/diagnostics.h"
#include "util/arena.h"

#include <string.h>
#include <stdbool.h>

/* ── Module-level fixtures ───────────────────────────────────────────────── */

static Iron_Arena    g_arena;
static Iron_DiagList g_diags;

void setUp(void) {
    g_arena = iron_arena_create(1024 * 512);
    g_diags = iron_diaglist_create();
    iron_types_init(&g_arena);
}

void tearDown(void) {
    iron_arena_free(&g_arena);
    iron_diaglist_free(&g_diags);
}

/* ── Parse + resolve + typecheck + escape_analyze helper ──────────────────── */

static Iron_Program *run_escape(const char *src) {
    Iron_Lexer   l      = iron_lexer_create(src, "test.iron", &g_arena, &g_diags);
    Iron_Token  *tokens = iron_lex_all(&l);
    int          count  = 0;
    while (tokens[count].kind != IRON_TOK_EOF) count++;
    count++;
    Iron_Parser  p    = iron_parser_create(tokens, count, src, "test.iron", &g_arena, &g_diags);
    Iron_Node   *root = iron_parse(&p);
    Iron_Program *prog = (Iron_Program *)root;
    Iron_Scope   *global = iron_resolve(prog, &g_arena, &g_diags);
    iron_typecheck(prog, global, &g_arena, &g_diags);
    iron_escape_analyze(prog, global, &g_arena, &g_diags);
    return prog;
}

static bool has_error(int code) {
    for (int i = 0; i < g_diags.count; i++) {
        if (g_diags.items[i].code == code) return true;
    }
    return false;
}

/* Walk the first function body and find the first IRON_NODE_HEAP node. */
static Iron_HeapExpr *find_first_heap(Iron_Program *prog) {
    if (!prog || prog->decl_count == 0) return NULL;
    Iron_FuncDecl *fd = (Iron_FuncDecl *)prog->decls[0];
    if (!fd || fd->kind != IRON_NODE_FUNC_DECL) return NULL;
    Iron_Block *body = (Iron_Block *)fd->body;
    if (!body || body->stmt_count == 0) return NULL;
    Iron_ValDecl *vd = (Iron_ValDecl *)body->stmts[0];
    if (!vd || vd->kind != IRON_NODE_VAL_DECL) return NULL;
    if (!vd->init || vd->init->kind != IRON_NODE_HEAP) return NULL;
    return (Iron_HeapExpr *)vd->init;
}

/* ── Span helper ─────────────────────────────────────────────────────────── */
static Iron_Span ts(int l, int c) {
    return iron_span_make("test.iron", (uint32_t)l, (uint32_t)c,
                          (uint32_t)l, (uint32_t)(c + 1));
}

/* ── Run escape analysis on a manually built program ─────────────────────── */

/* Run resolve+typecheck quietly (suppressing their diagnostics), then run
 * escape analysis using g_diags.  Any escape errors land in g_diags starting
 * at index 0 (g_diags is freshly initialized per-test by setUp). */
static Iron_Scope *resolve_quiet(Iron_Program *prog, Iron_Arena *a) {
    Iron_DiagList quiet = iron_diaglist_create();
    Iron_Scope *global  = iron_resolve(prog, a, &quiet);
    iron_typecheck(prog, global, a, &quiet);
    iron_diaglist_free(&quiet);
    return global;
}

/* ── AST builder helpers ─────────────────────────────────────────────────── */

static Iron_Program *make_prog(Iron_Arena *a, const char *fn_name,
                                Iron_Node **stmts, int stmt_count) {
    Iron_Block *body = ARENA_ALLOC(a, Iron_Block);
    body->span       = ts(1, 14);
    body->kind       = IRON_NODE_BLOCK;
    body->stmts      = stmts;
    body->stmt_count = stmt_count;

    Iron_FuncDecl *fn = ARENA_ALLOC(a, Iron_FuncDecl);
    fn->span        = ts(1, 1);
    fn->kind        = IRON_NODE_FUNC_DECL;
    fn->name        = fn_name;
    fn->params      = NULL;
    fn->param_count = 0;
    fn->return_type = NULL;
    fn->body        = (Iron_Node *)body;
    fn->is_private  = false;
    fn->generic_params      = NULL;
    fn->generic_param_count = 0;
    fn->resolved_return_type = NULL;

    Iron_Node **decls = iron_arena_alloc(a, sizeof(Iron_Node *), _Alignof(Iron_Node *));
    decls[0] = (Iron_Node *)fn;

    Iron_Program *prog = ARENA_ALLOC(a, Iron_Program);
    prog->span       = ts(1, 1);
    prog->kind       = IRON_NODE_PROGRAM;
    prog->decls      = decls;
    prog->decl_count = 1;
    return prog;
}

static Iron_HeapExpr *make_heap_expr(Iron_Arena *a) {
    Iron_ArrayLit *arr = ARENA_ALLOC(a, Iron_ArrayLit);
    arr->span          = ts(2, 11);
    arr->kind          = IRON_NODE_ARRAY_LIT;
    arr->resolved_type = NULL;
    arr->type_ann      = NULL;
    arr->size          = NULL;
    arr->elements      = NULL;
    arr->element_count = 0;

    Iron_HeapExpr *he = ARENA_ALLOC(a, Iron_HeapExpr);
    he->span          = ts(2, 7);
    he->kind          = IRON_NODE_HEAP;
    he->resolved_type = NULL;
    he->inner         = (Iron_Node *)arr;
    he->auto_free     = false;
    he->escapes       = false;
    return he;
}

static Iron_ValDecl *make_val(Iron_Arena *a, const char *name, Iron_Node *init) {
    Iron_ValDecl *vd = ARENA_ALLOC(a, Iron_ValDecl);
    vd->span         = ts(2, 3);
    vd->kind         = IRON_NODE_VAL_DECL;
    vd->name         = name;
    vd->type_ann     = NULL;
    vd->init         = init;
    vd->declared_type = NULL;
    return vd;
}

static Iron_Ident *make_ident(Iron_Arena *a, const char *name) {
    Iron_Ident *id = ARENA_ALLOC(a, Iron_Ident);
    id->span          = ts(3, 10);
    id->kind          = IRON_NODE_IDENT;
    id->resolved_type = NULL;
    id->name          = name;
    id->resolved_sym  = NULL;
    return id;
}

static Iron_ReturnStmt *make_return(Iron_Arena *a, Iron_Node *value) {
    Iron_ReturnStmt *ret = ARENA_ALLOC(a, Iron_ReturnStmt);
    ret->span  = ts(3, 3);
    ret->kind  = IRON_NODE_RETURN;
    ret->value = value;
    return ret;
}

static Iron_FreeStmt *make_free(Iron_Arena *a, Iron_Node *expr) {
    Iron_FreeStmt *fs = ARENA_ALLOC(a, Iron_FreeStmt);
    fs->span = ts(3, 3);
    fs->kind = IRON_NODE_FREE;
    fs->expr = expr;
    return fs;
}

static Iron_LeakStmt *make_leak(Iron_Arena *a, Iron_Node *expr) {
    Iron_LeakStmt *ls = ARENA_ALLOC(a, Iron_LeakStmt);
    ls->span = ts(3, 3);
    ls->kind = IRON_NODE_LEAK;
    ls->expr = expr;
    return ls;
}

static Iron_Node **make_stmts(Iron_Arena *a, int n) {
    return iron_arena_alloc(a, (size_t)n * sizeof(Iron_Node *), _Alignof(Iron_Node *));
}

/* ── Test 1: non-escaping heap => auto_free=true ─────────────────────────── */

void test_non_escaping_auto_free(void) {
    const char *src =
        "func foo() {\n"
        "  val d = heap [UInt8; 100]\n"
        "}\n";
    Iron_Program *prog = run_escape(src);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);

    Iron_HeapExpr *he = find_first_heap(prog);
    TEST_ASSERT_NOT_NULL(he);
    TEST_ASSERT_TRUE(he->auto_free);
    TEST_ASSERT_FALSE(he->escapes);
}

/* ── Test 2: heap used locally, non-heap returned => auto_free=true ────────── */

void test_heap_local_no_escape(void) {
    Iron_HeapExpr *he = make_heap_expr(&g_arena);
    Iron_ValDecl  *vd = make_val(&g_arena, "d", (Iron_Node *)he);

    Iron_IntLit *zero = ARENA_ALLOC(&g_arena, Iron_IntLit);
    zero->span          = ts(3, 10);
    zero->kind          = IRON_NODE_INT_LIT;
    zero->resolved_type = NULL;
    zero->value         = "0";

    Iron_ReturnStmt *ret = make_return(&g_arena, (Iron_Node *)zero);

    Iron_Node **stmts = make_stmts(&g_arena, 2);
    stmts[0] = (Iron_Node *)vd;
    stmts[1] = (Iron_Node *)ret;

    Iron_Program *prog   = make_prog(&g_arena, "heap_local", stmts, 2);
    Iron_Scope   *global = resolve_quiet(prog, &g_arena);

    iron_escape_analyze(prog, global, &g_arena, &g_diags);

    TEST_ASSERT_TRUE(he->auto_free);
    TEST_ASSERT_FALSE(has_error(IRON_ERR_ESCAPE_NO_FREE));
}

/* ── Test 3: heap returned via ident without free => escapes, E0207 ────────── */

void test_heap_returned_no_free_emits_e0207(void) {
    Iron_HeapExpr *he = make_heap_expr(&g_arena);
    Iron_ValDecl  *vd = make_val(&g_arena, "d", (Iron_Node *)he);

    Iron_Ident      *ret_id = make_ident(&g_arena, "d");
    Iron_ReturnStmt *ret    = make_return(&g_arena, (Iron_Node *)ret_id);

    Iron_Node **stmts = make_stmts(&g_arena, 2);
    stmts[0] = (Iron_Node *)vd;
    stmts[1] = (Iron_Node *)ret;

    Iron_Program *prog   = make_prog(&g_arena, "heap_ret_nofree", stmts, 2);
    Iron_Scope   *global = resolve_quiet(prog, &g_arena);

    iron_escape_analyze(prog, global, &g_arena, &g_diags);

    TEST_ASSERT_TRUE(has_error(IRON_ERR_ESCAPE_NO_FREE));
    TEST_ASSERT_TRUE(he->escapes);
}

/* ── Test 4: heap with free present, returned => no E0207 ─────────────────── */

void test_heap_with_free_no_error(void) {
    Iron_HeapExpr *he = make_heap_expr(&g_arena);
    Iron_ValDecl  *vd = make_val(&g_arena, "d", (Iron_Node *)he);

    Iron_Ident    *free_id = make_ident(&g_arena, "d");
    Iron_FreeStmt *free_s  = make_free(&g_arena, (Iron_Node *)free_id);

    Iron_Ident      *ret_id = make_ident(&g_arena, "d");
    Iron_ReturnStmt *ret    = make_return(&g_arena, (Iron_Node *)ret_id);

    Iron_Node **stmts = make_stmts(&g_arena, 3);
    stmts[0] = (Iron_Node *)vd;
    stmts[1] = (Iron_Node *)free_s;
    stmts[2] = (Iron_Node *)ret;

    Iron_Program *prog   = make_prog(&g_arena, "heap_free_ok", stmts, 3);
    Iron_Scope   *global = resolve_quiet(prog, &g_arena);

    iron_escape_analyze(prog, global, &g_arena, &g_diags);

    TEST_ASSERT_FALSE(has_error(IRON_ERR_ESCAPE_NO_FREE));
}

/* ── Test 5: heap with leak => no E0207, no E0213 ────────────────────────── */

void test_heap_with_leak_no_error(void) {
    Iron_HeapExpr *he = make_heap_expr(&g_arena);
    Iron_ValDecl  *vd = make_val(&g_arena, "d", (Iron_Node *)he);

    Iron_Ident    *leak_id = make_ident(&g_arena, "d");
    Iron_LeakStmt *leak_s  = make_leak(&g_arena, (Iron_Node *)leak_id);

    Iron_Node **stmts = make_stmts(&g_arena, 2);
    stmts[0] = (Iron_Node *)vd;
    stmts[1] = (Iron_Node *)leak_s;

    Iron_Program *prog   = make_prog(&g_arena, "heap_leak_ok", stmts, 2);
    Iron_Scope   *global = resolve_quiet(prog, &g_arena);

    iron_escape_analyze(prog, global, &g_arena, &g_diags);

    TEST_ASSERT_FALSE(has_error(IRON_ERR_ESCAPE_NO_FREE));
    TEST_ASSERT_FALSE(has_error(IRON_ERR_LEAK_NON_HEAP));
}

/* ── Test 6: free on non-heap value => E0212 ─────────────────────────────── */

void test_free_non_heap_emits_e0212(void) {
    Iron_IntLit *lit = ARENA_ALLOC(&g_arena, Iron_IntLit);
    lit->span          = ts(2, 11);
    lit->kind          = IRON_NODE_INT_LIT;
    lit->resolved_type = NULL;
    lit->value         = "42";

    Iron_ValDecl  *val_x  = make_val(&g_arena, "x", (Iron_Node *)lit);
    Iron_Ident    *free_id = make_ident(&g_arena, "x");
    Iron_FreeStmt *free_s  = make_free(&g_arena, (Iron_Node *)free_id);

    Iron_Node **stmts = make_stmts(&g_arena, 2);
    stmts[0] = (Iron_Node *)val_x;
    stmts[1] = (Iron_Node *)free_s;

    Iron_Program *prog   = make_prog(&g_arena, "free_nonheap", stmts, 2);
    Iron_Scope   *global = resolve_quiet(prog, &g_arena);

    iron_escape_analyze(prog, global, &g_arena, &g_diags);

    TEST_ASSERT_TRUE(has_error(IRON_ERR_FREE_NON_HEAP));
}

/* ── Test 7: leak on non-heap value => E0213 ─────────────────────────────── */

void test_leak_non_heap_emits_e0213(void) {
    Iron_IntLit *lit = ARENA_ALLOC(&g_arena, Iron_IntLit);
    lit->span          = ts(2, 11);
    lit->kind          = IRON_NODE_INT_LIT;
    lit->resolved_type = NULL;
    lit->value         = "42";

    Iron_ValDecl  *val_x  = make_val(&g_arena, "x", (Iron_Node *)lit);
    Iron_Ident    *leak_id = make_ident(&g_arena, "x");
    Iron_LeakStmt *leak_s  = make_leak(&g_arena, (Iron_Node *)leak_id);

    Iron_Node **stmts = make_stmts(&g_arena, 2);
    stmts[0] = (Iron_Node *)val_x;
    stmts[1] = (Iron_Node *)leak_s;

    Iron_Program *prog   = make_prog(&g_arena, "leak_nonheap", stmts, 2);
    Iron_Scope   *global = resolve_quiet(prog, &g_arena);

    iron_escape_analyze(prog, global, &g_arena, &g_diags);

    TEST_ASSERT_TRUE(has_error(IRON_ERR_LEAK_NON_HEAP));
}

/* ── Test 8: leak on rc value => E0214 ───────────────────────────────────── */

void test_leak_rc_emits_e0214(void) {
    Iron_IntLit *lit = ARENA_ALLOC(&g_arena, Iron_IntLit);
    lit->span          = ts(2, 10);
    lit->kind          = IRON_NODE_INT_LIT;
    lit->resolved_type = iron_type_make_primitive(IRON_TYPE_INT);
    lit->value         = "42";

    Iron_RcExpr *rc_expr = ARENA_ALLOC(&g_arena, Iron_RcExpr);
    rc_expr->span          = ts(2, 7);
    rc_expr->kind          = IRON_NODE_RC;
    rc_expr->resolved_type = iron_type_make_rc(&g_arena, iron_type_make_primitive(IRON_TYPE_INT));
    rc_expr->inner         = (Iron_Node *)lit;

    Iron_ValDecl *val_r = make_val(&g_arena, "r", (Iron_Node *)rc_expr);
    val_r->declared_type = rc_expr->resolved_type;

    /* The leak target is an ident "r" with resolved_type = rc */
    Iron_Ident *leak_id = make_ident(&g_arena, "r");
    leak_id->resolved_type = rc_expr->resolved_type;

    Iron_LeakStmt *leak_s = make_leak(&g_arena, (Iron_Node *)leak_id);

    Iron_Node **stmts = make_stmts(&g_arena, 2);
    stmts[0] = (Iron_Node *)val_r;
    stmts[1] = (Iron_Node *)leak_s;

    Iron_Program *prog   = make_prog(&g_arena, "leak_rc", stmts, 2);
    Iron_Scope   *global = resolve_quiet(prog, &g_arena);

    /* Manually set the symbol type for 'r' to rc so escape can check it. */
    Iron_Symbol *r_sym = iron_scope_lookup(global, "r");
    if (r_sym) r_sym->type = rc_expr->resolved_type;
    leak_id->resolved_sym = r_sym;

    iron_escape_analyze(prog, global, &g_arena, &g_diags);

    TEST_ASSERT_TRUE(has_error(IRON_ERR_LEAK_RC));
}

/* ── Test 9: rc value — no escape error (rc exempt) ─────────────────────── */

void test_rc_exempt_from_escape(void) {
    Iron_IntLit *lit = ARENA_ALLOC(&g_arena, Iron_IntLit);
    lit->span          = ts(2, 10);
    lit->kind          = IRON_NODE_INT_LIT;
    lit->resolved_type = iron_type_make_primitive(IRON_TYPE_INT);
    lit->value         = "42";

    Iron_RcExpr *rc_expr = ARENA_ALLOC(&g_arena, Iron_RcExpr);
    rc_expr->span          = ts(2, 7);
    rc_expr->kind          = IRON_NODE_RC;
    rc_expr->resolved_type = iron_type_make_rc(&g_arena, iron_type_make_primitive(IRON_TYPE_INT));
    rc_expr->inner         = (Iron_Node *)lit;

    Iron_ValDecl *val_s = make_val(&g_arena, "s", (Iron_Node *)rc_expr);
    val_s->declared_type = rc_expr->resolved_type;

    Iron_Ident      *ret_id = make_ident(&g_arena, "s");
    ret_id->resolved_type = rc_expr->resolved_type;
    Iron_ReturnStmt *ret   = make_return(&g_arena, (Iron_Node *)ret_id);

    Iron_Node **stmts = make_stmts(&g_arena, 2);
    stmts[0] = (Iron_Node *)val_s;
    stmts[1] = (Iron_Node *)ret;

    Iron_Program *prog   = make_prog(&g_arena, "rc_exempt", stmts, 2);
    Iron_Scope   *global = resolve_quiet(prog, &g_arena);

    Iron_Symbol *s_sym = iron_scope_lookup(global, "s");
    if (s_sym) s_sym->type = rc_expr->resolved_type;
    ret_id->resolved_sym = s_sym;

    iron_escape_analyze(prog, global, &g_arena, &g_diags);

    TEST_ASSERT_FALSE(has_error(IRON_ERR_ESCAPE_NO_FREE));
}

/* ── Test 10: heap assignment (escape via assign rhs) => E0207 ───────────── */

void test_heap_escaped_via_assign(void) {
    /* val d = heap [...]; outer = d — d escapes via assign RHS */
    Iron_HeapExpr *he = make_heap_expr(&g_arena);
    Iron_ValDecl  *vd = make_val(&g_arena, "d", (Iron_Node *)he);

    Iron_Ident *assign_tgt = make_ident(&g_arena, "outer");
    assign_tgt->span = ts(3, 3);

    Iron_Ident *assign_val = make_ident(&g_arena, "d");
    assign_val->span = ts(3, 12);

    Iron_AssignStmt *as = ARENA_ALLOC(&g_arena, Iron_AssignStmt);
    as->span   = ts(3, 3);
    as->kind   = IRON_NODE_ASSIGN;
    as->target = (Iron_Node *)assign_tgt;
    as->value  = (Iron_Node *)assign_val;
    as->op     = 0;

    Iron_Node **stmts = make_stmts(&g_arena, 2);
    stmts[0] = (Iron_Node *)vd;
    stmts[1] = (Iron_Node *)as;

    Iron_Program *prog   = make_prog(&g_arena, "heap_escape_assign", stmts, 2);
    Iron_Scope   *global = resolve_quiet(prog, &g_arena);

    iron_escape_analyze(prog, global, &g_arena, &g_diags);

    TEST_ASSERT_TRUE(has_error(IRON_ERR_ESCAPE_NO_FREE));
    TEST_ASSERT_TRUE(he->escapes);
}

/* ── Test 11: heap returned with leak => no E0207 ────────────────────────── */

void test_heap_with_leak_return_no_error(void) {
    Iron_HeapExpr *he = make_heap_expr(&g_arena);
    Iron_ValDecl  *vd = make_val(&g_arena, "d", (Iron_Node *)he);

    Iron_Ident    *leak_id = make_ident(&g_arena, "d");
    Iron_LeakStmt *leak_s  = make_leak(&g_arena, (Iron_Node *)leak_id);

    Iron_Ident      *ret_id = make_ident(&g_arena, "d");
    Iron_ReturnStmt *ret    = make_return(&g_arena, (Iron_Node *)ret_id);

    Iron_Node **stmts = make_stmts(&g_arena, 3);
    stmts[0] = (Iron_Node *)vd;
    stmts[1] = (Iron_Node *)leak_s;
    stmts[2] = (Iron_Node *)ret;

    Iron_Program *prog   = make_prog(&g_arena, "heap_leak_ret", stmts, 3);
    Iron_Scope   *global = resolve_quiet(prog, &g_arena);

    iron_escape_analyze(prog, global, &g_arena, &g_diags);

    TEST_ASSERT_FALSE(has_error(IRON_ERR_ESCAPE_NO_FREE));
}

/* ── main ─────────────────────────────────────────────────────────────────── */

int main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_non_escaping_auto_free);
    RUN_TEST(test_heap_local_no_escape);
    RUN_TEST(test_heap_returned_no_free_emits_e0207);
    RUN_TEST(test_heap_with_free_no_error);
    RUN_TEST(test_heap_with_leak_no_error);
    RUN_TEST(test_free_non_heap_emits_e0212);
    RUN_TEST(test_leak_non_heap_emits_e0213);
    RUN_TEST(test_leak_rc_emits_e0214);
    RUN_TEST(test_rc_exempt_from_escape);
    RUN_TEST(test_heap_escaped_via_assign);
    RUN_TEST(test_heap_with_leak_return_no_error);

    return UNITY_END();
}
