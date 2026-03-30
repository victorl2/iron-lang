/* test_hir_verify.c -- Unity tests for the HIR structural verifier.
 *
 * Each test builds a hand-constructed HIR module and exercises one specific
 * verifier invariant -- either expecting it to pass cleanly or to report a
 * specific error code.
 */

#include "unity.h"
#include "hir/hir.h"
#include "diagnostics/diagnostics.h"
#include "util/arena.h"
#include "analyzer/types.h"

#include <string.h>
#include <stdbool.h>
#include <stdlib.h>

/* ── Fixtures ────────────────────────────────────────────────────────────── */

static IronHIR_Module *g_mod  = NULL;
static Iron_DiagList   g_diags;
static Iron_Arena      g_arena;

void setUp(void) {
    iron_types_init(NULL);
    g_mod   = iron_hir_module_create("test_module");
    g_arena = iron_arena_create(65536);
    g_diags = iron_diaglist_create();
}

void tearDown(void) {
    iron_hir_module_destroy(g_mod);
    g_mod = NULL;
    iron_diaglist_free(&g_diags);
    iron_arena_free(&g_arena);
}

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static Iron_Span zero_span(void) {
    return iron_span_make("test.iron", 1, 1, 1, 1);
}

/* Return true if any diagnostic in list has the given error code. */
static bool has_error(const Iron_DiagList *diags, int code) {
    for (int i = 0; i < diags->count; i++) {
        if (diags->items[i].code == code) return true;
    }
    return false;
}

/* ── Test 1: valid tree ────────────────────────────────────────────────────
 * Build a well-formed function with let bindings, if/else, and return.
 * All variables declared before use. Verifier should return true, 0 errors.
 */
void test_hir_verify_valid_tree(void) {
    Iron_Span span = zero_span();
    Iron_Type *int_type = iron_type_make_primitive(IRON_TYPE_INT);

    IronHIR_VarId x_id = iron_hir_alloc_var(g_mod, "x", int_type, false);
    IronHIR_VarId y_id = iron_hir_alloc_var(g_mod, "y", int_type, false);
    IronHIR_Param params[2];
    params[0].var_id = x_id; params[0].name = "x"; params[0].type = int_type;
    params[1].var_id = y_id; params[1].name = "y"; params[1].type = int_type;

    IronHIR_Func *fn = iron_hir_func_create(g_mod, "add", params, 2, int_type);
    IronHIR_Block *body = iron_hir_block_create(g_mod);

    /* val result = x + y */
    IronHIR_VarId result_id = iron_hir_alloc_var(g_mod, "result", int_type, false);
    IronHIR_Expr *ex = iron_hir_expr_ident(g_mod, x_id, "x", int_type, span);
    IronHIR_Expr *ey = iron_hir_expr_ident(g_mod, y_id, "y", int_type, span);
    IronHIR_Expr *add = iron_hir_expr_binop(g_mod, IRON_HIR_BINOP_ADD, ex, ey, int_type, span);
    iron_hir_block_add_stmt(body, iron_hir_stmt_let(g_mod, result_id, int_type, add, false, span));

    /* if result > 0 { return result } else { return 0 } */
    IronHIR_Expr *eresult = iron_hir_expr_ident(g_mod, result_id, "result", int_type, span);
    IronHIR_Expr *zero_lit = iron_hir_expr_int_lit(g_mod, 0, int_type, span);
    IronHIR_Expr *cond = iron_hir_expr_binop(g_mod, IRON_HIR_BINOP_GT, eresult, zero_lit, int_type, span);

    IronHIR_Block *then_blk = iron_hir_block_create(g_mod);
    IronHIR_Expr *eresult2 = iron_hir_expr_ident(g_mod, result_id, "result", int_type, span);
    iron_hir_block_add_stmt(then_blk, iron_hir_stmt_return(g_mod, eresult2, span));

    IronHIR_Block *else_blk = iron_hir_block_create(g_mod);
    IronHIR_Expr *zero_lit2 = iron_hir_expr_int_lit(g_mod, 0, int_type, span);
    iron_hir_block_add_stmt(else_blk, iron_hir_stmt_return(g_mod, zero_lit2, span));

    iron_hir_block_add_stmt(body, iron_hir_stmt_if(g_mod, cond, then_blk, else_blk, span));

    fn->body = body;
    iron_hir_module_add_func(g_mod, fn);

    bool result = iron_hir_verify(g_mod, &g_diags, &g_arena);
    TEST_ASSERT_TRUE(result);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);
}

/* ── Test 2: use-before-def ────────────────────────────────────────────────
 * Build a function that references a var_id never declared.
 * Verifier should return false with IRON_ERR_HIR_USE_BEFORE_DEF.
 */
void test_hir_verify_use_before_def(void) {
    Iron_Span span = zero_span();
    Iron_Type *int_type = iron_type_make_primitive(IRON_TYPE_INT);

    IronHIR_Func *fn = iron_hir_func_create(g_mod, "bad_use", NULL, 0, int_type);
    IronHIR_Block *body = iron_hir_block_create(g_mod);

    /* Reference a var_id (42) that was never declared */
    IronHIR_VarId ghost_id = 42;  /* not registered via iron_hir_alloc_var */
    IronHIR_Expr *ident_expr = iron_hir_expr_ident(g_mod, ghost_id, "ghost", int_type, span);
    iron_hir_block_add_stmt(body, iron_hir_stmt_return(g_mod, ident_expr, span));

    fn->body = body;
    iron_hir_module_add_func(g_mod, fn);

    bool result = iron_hir_verify(g_mod, &g_diags, &g_arena);
    TEST_ASSERT_FALSE(result);
    TEST_ASSERT_TRUE(g_diags.error_count >= 1);
    TEST_ASSERT_TRUE(has_error(&g_diags, IRON_ERR_HIR_USE_BEFORE_DEF));
}

/* ── Test 3: duplicate binding ────────────────────────────────────────────
 * Build a block with two let stmts declaring the same VarId.
 * Verifier should return false with IRON_ERR_HIR_DUPLICATE_BINDING.
 */
void test_hir_verify_duplicate_binding(void) {
    Iron_Span span = zero_span();
    Iron_Type *int_type = iron_type_make_primitive(IRON_TYPE_INT);

    IronHIR_Func *fn = iron_hir_func_create(g_mod, "dup_bind", NULL, 0, int_type);
    IronHIR_Block *body = iron_hir_block_create(g_mod);

    IronHIR_VarId x_id = iron_hir_alloc_var(g_mod, "x", int_type, false);

    /* Declare x twice in the same scope */
    IronHIR_Expr *lit1 = iron_hir_expr_int_lit(g_mod, 1, int_type, span);
    IronHIR_Expr *lit2 = iron_hir_expr_int_lit(g_mod, 2, int_type, span);
    iron_hir_block_add_stmt(body, iron_hir_stmt_let(g_mod, x_id, int_type, lit1, false, span));
    iron_hir_block_add_stmt(body, iron_hir_stmt_let(g_mod, x_id, int_type, lit2, false, span));

    fn->body = body;
    iron_hir_module_add_func(g_mod, fn);

    bool result = iron_hir_verify(g_mod, &g_diags, &g_arena);
    TEST_ASSERT_FALSE(result);
    TEST_ASSERT_TRUE(has_error(&g_diags, IRON_ERR_HIR_DUPLICATE_BINDING));
}

/* ── Test 4: scope isolation ──────────────────────────────────────────────
 * Variable declared inside then-branch is not visible after the if.
 * Verifier should return false (use-before-def after if).
 */
void test_hir_verify_scope_isolation(void) {
    Iron_Span span = zero_span();
    Iron_Type *int_type = iron_type_make_primitive(IRON_TYPE_INT);

    IronHIR_VarId cond_id = iron_hir_alloc_var(g_mod, "flag", int_type, false);
    IronHIR_Param params[1];
    params[0].var_id = cond_id; params[0].name = "flag"; params[0].type = int_type;

    IronHIR_Func *fn = iron_hir_func_create(g_mod, "scope_test", params, 1, int_type);
    IronHIR_Block *body = iron_hir_block_create(g_mod);

    /* if flag > 0 { val inner = 42 } ... use inner (out of scope) */
    IronHIR_Expr *eflag = iron_hir_expr_ident(g_mod, cond_id, "flag", int_type, span);
    IronHIR_Expr *zero_lit = iron_hir_expr_int_lit(g_mod, 0, int_type, span);
    IronHIR_Expr *cond = iron_hir_expr_binop(g_mod, IRON_HIR_BINOP_GT, eflag, zero_lit, int_type, span);

    IronHIR_VarId inner_id = iron_hir_alloc_var(g_mod, "inner", int_type, false);
    IronHIR_Block *then_blk = iron_hir_block_create(g_mod);
    IronHIR_Expr *lit42 = iron_hir_expr_int_lit(g_mod, 42, int_type, span);
    iron_hir_block_add_stmt(then_blk, iron_hir_stmt_let(g_mod, inner_id, int_type, lit42, false, span));

    iron_hir_block_add_stmt(body, iron_hir_stmt_if(g_mod, cond, then_blk, NULL, span));

    /* Use inner outside the if -- scope violation */
    IronHIR_Expr *einner = iron_hir_expr_ident(g_mod, inner_id, "inner", int_type, span);
    iron_hir_block_add_stmt(body, iron_hir_stmt_return(g_mod, einner, span));

    fn->body = body;
    iron_hir_module_add_func(g_mod, fn);

    bool result = iron_hir_verify(g_mod, &g_diags, &g_arena);
    TEST_ASSERT_FALSE(result);
    TEST_ASSERT_TRUE(has_error(&g_diags, IRON_ERR_HIR_USE_BEFORE_DEF));
}

/* ── Test 5: null pointer ─────────────────────────────────────────────────
 * A binop with NULL left operand.
 * Verifier should return false with IRON_ERR_HIR_STRUCTURAL.
 */
void test_hir_verify_null_pointer(void) {
    Iron_Span span = zero_span();
    Iron_Type *int_type = iron_type_make_primitive(IRON_TYPE_INT);

    IronHIR_Func *fn = iron_hir_func_create(g_mod, "null_test", NULL, 0, int_type);
    IronHIR_Block *body = iron_hir_block_create(g_mod);

    /* Binop with NULL left -- structural error */
    IronHIR_Expr *right = iron_hir_expr_int_lit(g_mod, 1, int_type, span);
    IronHIR_Expr *bad_binop = iron_hir_expr_binop(g_mod, IRON_HIR_BINOP_ADD, NULL, right, int_type, span);
    iron_hir_block_add_stmt(body, iron_hir_stmt_expr(g_mod, bad_binop, span));

    fn->body = body;
    iron_hir_module_add_func(g_mod, fn);

    bool result = iron_hir_verify(g_mod, &g_diags, &g_arena);
    TEST_ASSERT_FALSE(result);
    TEST_ASSERT_TRUE(
        has_error(&g_diags, IRON_ERR_HIR_STRUCTURAL) ||
        has_error(&g_diags, IRON_ERR_HIR_NULL_POINTER)
    );
}

/* ── Test 6: nested scopes ────────────────────────────────────────────────
 * Inner block can see outer variable (ok).
 * Using inner-only variable after inner block ends (error).
 */
void test_hir_verify_nested_scopes(void) {
    Iron_Span span = zero_span();
    Iron_Type *int_type = iron_type_make_primitive(IRON_TYPE_INT);

    IronHIR_Func *fn = iron_hir_func_create(g_mod, "nested", NULL, 0, int_type);
    IronHIR_Block *outer = iron_hir_block_create(g_mod);

    /* Outer: val outer_var = 10 */
    IronHIR_VarId outer_id = iron_hir_alloc_var(g_mod, "outer_var", int_type, false);
    IronHIR_Expr *lit10 = iron_hir_expr_int_lit(g_mod, 10, int_type, span);
    iron_hir_block_add_stmt(outer, iron_hir_stmt_let(g_mod, outer_id, int_type, lit10, false, span));

    /* Inner block: val inner_var = outer_var (valid -- outer visible) */
    IronHIR_VarId inner_id = iron_hir_alloc_var(g_mod, "inner_var", int_type, false);
    IronHIR_Block *inner_blk = iron_hir_block_create(g_mod);
    IronHIR_Expr *e_outer = iron_hir_expr_ident(g_mod, outer_id, "outer_var", int_type, span);
    iron_hir_block_add_stmt(inner_blk, iron_hir_stmt_let(g_mod, inner_id, int_type, e_outer, false, span));
    iron_hir_block_add_stmt(outer, iron_hir_stmt_block(g_mod, inner_blk, span));

    /* After inner block: try to use inner_var (out of scope) */
    IronHIR_Expr *e_inner = iron_hir_expr_ident(g_mod, inner_id, "inner_var", int_type, span);
    iron_hir_block_add_stmt(outer, iron_hir_stmt_return(g_mod, e_inner, span));

    fn->body = outer;
    iron_hir_module_add_func(g_mod, fn);

    bool result = iron_hir_verify(g_mod, &g_diags, &g_arena);
    TEST_ASSERT_FALSE(result);
    TEST_ASSERT_TRUE(has_error(&g_diags, IRON_ERR_HIR_USE_BEFORE_DEF));
}

/* ── Test 7: closure scope ────────────────────────────────────────────────
 * Closure params visible inside body; params not visible outside.
 */
void test_hir_verify_closure_scope(void) {
    Iron_Span span = zero_span();
    Iron_Type *int_type = iron_type_make_primitive(IRON_TYPE_INT);

    IronHIR_Func *fn = iron_hir_func_create(g_mod, "closure_test", NULL, 0, int_type);
    IronHIR_Block *body = iron_hir_block_create(g_mod);

    /* Closure param cx -- visible inside closure body */
    IronHIR_VarId cx_id = iron_hir_alloc_var(g_mod, "cx", int_type, false);
    IronHIR_Param cparams[1];
    cparams[0].var_id = cx_id; cparams[0].name = "cx"; cparams[0].type = int_type;

    IronHIR_Block *closure_body = iron_hir_block_create(g_mod);
    IronHIR_Expr *ecx = iron_hir_expr_ident(g_mod, cx_id, "cx", int_type, span);
    iron_hir_block_add_stmt(closure_body, iron_hir_stmt_return(g_mod, ecx, span));

    IronHIR_Expr *closure_expr = iron_hir_expr_closure(g_mod, cparams, 1, int_type, closure_body, NULL, NULL, span);
    IronHIR_VarId f_id = iron_hir_alloc_var(g_mod, "f", NULL, false);
    iron_hir_block_add_stmt(body, iron_hir_stmt_let(g_mod, f_id, NULL, closure_expr, false, span));

    /* After closure: use cx (should fail -- cx is closure-scoped) */
    IronHIR_Expr *ecx_outside = iron_hir_expr_ident(g_mod, cx_id, "cx", int_type, span);
    iron_hir_block_add_stmt(body, iron_hir_stmt_return(g_mod, ecx_outside, span));

    fn->body = body;
    iron_hir_module_add_func(g_mod, fn);

    bool result = iron_hir_verify(g_mod, &g_diags, &g_arena);
    TEST_ASSERT_FALSE(result);
    TEST_ASSERT_TRUE(has_error(&g_diags, IRON_ERR_HIR_USE_BEFORE_DEF));
}

/* ── Test 8: collects all errors ──────────────────────────────────────────
 * Tree with 3 independent errors: two use-before-def (ghost1, ghost2) and
 * one structural (binop with NULL left). All 3 should be reported in one run.
 */
void test_hir_verify_collects_all_errors(void) {
    Iron_Span span = zero_span();
    Iron_Type *int_type = iron_type_make_primitive(IRON_TYPE_INT);

    IronHIR_Func *fn = iron_hir_func_create(g_mod, "many_errors", NULL, 0, int_type);
    IronHIR_Block *body = iron_hir_block_create(g_mod);

    /* Error 1: use-before-def for ghost1 */
    IronHIR_VarId ghost1 = 100;
    IronHIR_Expr *e1 = iron_hir_expr_ident(g_mod, ghost1, "ghost1", int_type, span);
    iron_hir_block_add_stmt(body, iron_hir_stmt_expr(g_mod, e1, span));

    /* Error 2: use-before-def for ghost2 */
    IronHIR_VarId ghost2 = 101;
    IronHIR_Expr *e2 = iron_hir_expr_ident(g_mod, ghost2, "ghost2", int_type, span);
    iron_hir_block_add_stmt(body, iron_hir_stmt_expr(g_mod, e2, span));

    /* Error 3: structural -- binop with NULL left */
    IronHIR_Expr *right = iron_hir_expr_int_lit(g_mod, 5, int_type, span);
    IronHIR_Expr *bad_binop = iron_hir_expr_binop(g_mod, IRON_HIR_BINOP_ADD, NULL, right, int_type, span);
    iron_hir_block_add_stmt(body, iron_hir_stmt_expr(g_mod, bad_binop, span));

    fn->body = body;
    iron_hir_module_add_func(g_mod, fn);

    bool result = iron_hir_verify(g_mod, &g_diags, &g_arena);
    TEST_ASSERT_FALSE(result);
    /* All 3 errors should be reported -- proves collect-all behavior */
    TEST_ASSERT_TRUE(g_diags.error_count >= 3);
}

/* ── main ────────────────────────────────────────────────────────────────── */

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_hir_verify_valid_tree);
    RUN_TEST(test_hir_verify_use_before_def);
    RUN_TEST(test_hir_verify_duplicate_binding);
    RUN_TEST(test_hir_verify_scope_isolation);
    RUN_TEST(test_hir_verify_null_pointer);
    RUN_TEST(test_hir_verify_nested_scopes);
    RUN_TEST(test_hir_verify_closure_scope);
    RUN_TEST(test_hir_verify_collects_all_errors);
    return UNITY_END();
}
