/* test_hir_print.c -- Unity tests for the HIR printer.
 *
 * Each test builds a hand-constructed HIR module and verifies that
 * iron_hir_print() produces the expected indented tree output,
 * using a golden snapshot comparison pattern.
 */

#include "unity.h"
#include "hir/hir.h"
#include "analyzer/types.h"
#include "diagnostics/diagnostics.h"

#include <string.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>

/* ── Fixtures ────────────────────────────────────────────────────────────── */

static IronHIR_Module *g_mod = NULL;

void setUp(void) {
    iron_types_init(NULL);
    g_mod = iron_hir_module_create("test_module");
}

void tearDown(void) {
    iron_hir_module_destroy(g_mod);
    g_mod = NULL;
}

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static Iron_Span zero_span(void) {
    return iron_span_make("test.iron", 1, 1, 1, 1);
}

/* Read expected snapshot content from a file. Caller must free result. */
static char *read_snapshot(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) {
        printf("Snapshot file not found: %s\n", path);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)len + 1);
    if (!buf) { fclose(f); return NULL; }
    fread(buf, 1, (size_t)len, f);
    buf[len] = '\0';
    fclose(f);
    return buf;
}

/* Write a golden snapshot (creates the file). */
static void write_snapshot(const char *content, const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) { printf("Cannot write snapshot: %s\n", path); return; }
    fputs(content, f);
    fclose(f);
}

/* Compare printed output against snapshot. If snapshot missing, create it. */
static void assert_matches_snapshot(const char *printed, const char *snapshot_path) {
    char *expected = read_snapshot(snapshot_path);
    if (!expected) {
        /* Golden master pattern: write on first run */
        write_snapshot(printed, snapshot_path);
        return;
    }
    if (strcmp(printed, expected) != 0) {
        printf("Snapshot mismatch for: %s\n", snapshot_path);
        printf("--- expected ---\n%s\n--- got ---\n%s\n", expected, printed);
    }
    TEST_ASSERT_EQUAL_STRING(expected, printed);
    free(expected);
}

/* ── Test 1: Simple function with let binding and return ─────────────────── */

void test_hir_print_simple_func(void) {
    Iron_Span span = zero_span();
    Iron_Type *int_type = iron_type_make_primitive(IRON_TYPE_INT);

    /* Allocate parameters */
    IronHIR_VarId x_id = iron_hir_alloc_var(g_mod, "x", int_type, false);
    IronHIR_VarId y_id = iron_hir_alloc_var(g_mod, "y", int_type, false);

    IronHIR_Param params[2];
    params[0].var_id = x_id;
    params[0].name   = "x";
    params[0].type   = int_type;
    params[1].var_id = y_id;
    params[1].name   = "y";
    params[1].type   = int_type;

    IronHIR_Func *fn = iron_hir_func_create(g_mod, "add", params, 2, int_type);

    /* Build body: val result: Int = x + y; return result */
    IronHIR_VarId result_id = iron_hir_alloc_var(g_mod, "result", int_type, false);
    IronHIR_Expr *ex = iron_hir_expr_ident(g_mod, x_id, "x", int_type, span);
    IronHIR_Expr *ey = iron_hir_expr_ident(g_mod, y_id, "y", int_type, span);
    IronHIR_Expr *add = iron_hir_expr_binop(g_mod, IRON_HIR_BINOP_ADD, ex, ey, int_type, span);
    IronHIR_Stmt *let_result = iron_hir_stmt_let(g_mod, result_id, int_type, add, false, span);

    IronHIR_Expr *eresult = iron_hir_expr_ident(g_mod, result_id, "result", int_type, span);
    IronHIR_Stmt *ret = iron_hir_stmt_return(g_mod, eresult, span);

    IronHIR_Block *body = iron_hir_block_create(g_mod);
    iron_hir_block_add_stmt(body, let_result);
    iron_hir_block_add_stmt(body, ret);
    fn->body = body;

    iron_hir_module_add_func(g_mod, fn);

    char *printed = iron_hir_print(g_mod);
    TEST_ASSERT_NOT_NULL(printed);

    assert_matches_snapshot(printed, "tests/hir/snapshots/simple_func.txt");

    free(printed);
}

/* ── Test 2: Control flow (if/else, while, for) ──────────────────────────── */

void test_hir_print_control_flow(void) {
    Iron_Span span = zero_span();
    Iron_Type *int_type = iron_type_make_primitive(IRON_TYPE_INT);

    IronHIR_VarId n_id = iron_hir_alloc_var(g_mod, "n", int_type, false);
    IronHIR_Param params[1];
    params[0].var_id = n_id;
    params[0].name   = "n";
    params[0].type   = int_type;

    IronHIR_Func *fn = iron_hir_func_create(g_mod, "control", params, 1, int_type);
    IronHIR_Block *body = iron_hir_block_create(g_mod);

    /* if n > 0 { return 1 } else { return 0 } */
    IronHIR_Expr *en = iron_hir_expr_ident(g_mod, n_id, "n", int_type, span);
    IronHIR_Expr *zero_lit = iron_hir_expr_int_lit(g_mod, 0, int_type, span);
    IronHIR_Expr *cond = iron_hir_expr_binop(g_mod, IRON_HIR_BINOP_GT, en, zero_lit, int_type, span);

    IronHIR_Block *then_blk = iron_hir_block_create(g_mod);
    IronHIR_Expr *ret1_val = iron_hir_expr_int_lit(g_mod, 1, int_type, span);
    iron_hir_block_add_stmt(then_blk, iron_hir_stmt_return(g_mod, ret1_val, span));

    IronHIR_Block *else_blk = iron_hir_block_create(g_mod);
    IronHIR_Expr *ret0_val = iron_hir_expr_int_lit(g_mod, 0, int_type, span);
    iron_hir_block_add_stmt(else_blk, iron_hir_stmt_return(g_mod, ret0_val, span));

    IronHIR_Stmt *if_stmt = iron_hir_stmt_if(g_mod, cond, then_blk, else_blk, span);
    iron_hir_block_add_stmt(body, if_stmt);

    /* while n > 0 { n } */
    IronHIR_Expr *en2 = iron_hir_expr_ident(g_mod, n_id, "n", int_type, span);
    IronHIR_Expr *zero_lit2 = iron_hir_expr_int_lit(g_mod, 0, int_type, span);
    IronHIR_Expr *while_cond = iron_hir_expr_binop(g_mod, IRON_HIR_BINOP_GT, en2, zero_lit2, int_type, span);
    IronHIR_Block *while_body = iron_hir_block_create(g_mod);
    IronHIR_Expr *en3 = iron_hir_expr_ident(g_mod, n_id, "n", int_type, span);
    iron_hir_block_add_stmt(while_body, iron_hir_stmt_expr(g_mod, en3, span));
    iron_hir_block_add_stmt(body, iron_hir_stmt_while(g_mod, while_cond, while_body, span));

    /* for i in n { i } */
    IronHIR_VarId i_id = iron_hir_alloc_var(g_mod, "i", int_type, false);
    IronHIR_Expr *en4 = iron_hir_expr_ident(g_mod, n_id, "n", int_type, span);
    IronHIR_Block *for_body = iron_hir_block_create(g_mod);
    IronHIR_Expr *ei = iron_hir_expr_ident(g_mod, i_id, "i", int_type, span);
    iron_hir_block_add_stmt(for_body, iron_hir_stmt_expr(g_mod, ei, span));
    iron_hir_block_add_stmt(body, iron_hir_stmt_for(g_mod, i_id, en4, for_body, span));

    /* return 0 */
    IronHIR_Expr *final_ret = iron_hir_expr_int_lit(g_mod, 0, int_type, span);
    iron_hir_block_add_stmt(body, iron_hir_stmt_return(g_mod, final_ret, span));

    fn->body = body;
    iron_hir_module_add_func(g_mod, fn);

    char *printed = iron_hir_print(g_mod);
    TEST_ASSERT_NOT_NULL(printed);

    assert_matches_snapshot(printed, "tests/hir/snapshots/control_flow.txt");

    free(printed);
}

/* ── Test 3: Closures, defer, spawn ──────────────────────────────────────── */

void test_hir_print_closures_concurrency(void) {
    Iron_Span span = zero_span();
    Iron_Type *int_type = iron_type_make_primitive(IRON_TYPE_INT);

    IronHIR_Func *fn = iron_hir_func_create(g_mod, "concurrent", NULL, 0, NULL);
    IronHIR_Block *body = iron_hir_block_create(g_mod);

    /* val f = |x: Int| -> { return x } */
    IronHIR_VarId cx_id = iron_hir_alloc_var(g_mod, "x", int_type, false);
    IronHIR_Param closure_params[1];
    closure_params[0].var_id = cx_id;
    closure_params[0].name   = "x";
    closure_params[0].type   = int_type;

    IronHIR_Block *closure_body = iron_hir_block_create(g_mod);
    IronHIR_Expr *ecx = iron_hir_expr_ident(g_mod, cx_id, "x", int_type, span);
    iron_hir_block_add_stmt(closure_body, iron_hir_stmt_return(g_mod, ecx, span));

    IronHIR_Expr *closure_expr = iron_hir_expr_closure(g_mod, closure_params, 1, int_type, closure_body, NULL, NULL, NULL, 0, span);

    IronHIR_VarId f_id = iron_hir_alloc_var(g_mod, "f", NULL, false);
    IronHIR_Stmt *let_f = iron_hir_stmt_let(g_mod, f_id, NULL, closure_expr, false, span);
    iron_hir_block_add_stmt(body, let_f);

    /* defer { 0 } */
    IronHIR_Block *defer_body = iron_hir_block_create(g_mod);
    IronHIR_Expr *defer_val = iron_hir_expr_int_lit(g_mod, 0, int_type, span);
    iron_hir_block_add_stmt(defer_body, iron_hir_stmt_expr(g_mod, defer_val, span));
    iron_hir_block_add_stmt(body, iron_hir_stmt_defer(g_mod, defer_body, span));

    /* spawn { 1 } */
    IronHIR_Block *spawn_body = iron_hir_block_create(g_mod);
    IronHIR_Expr *spawn_val = iron_hir_expr_int_lit(g_mod, 1, int_type, span);
    iron_hir_block_add_stmt(spawn_body, iron_hir_stmt_expr(g_mod, spawn_val, span));
    iron_hir_block_add_stmt(body, iron_hir_stmt_spawn(g_mod, "handle", spawn_body, NULL, span));

    fn->body = body;
    iron_hir_module_add_func(g_mod, fn);

    char *printed = iron_hir_print(g_mod);
    TEST_ASSERT_NOT_NULL(printed);

    assert_matches_snapshot(printed, "tests/hir/snapshots/closures_concurrency.txt");

    free(printed);
}

/* ── main ────────────────────────────────────────────────────────────────── */

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_hir_print_simple_func);
    RUN_TEST(test_hir_print_control_flow);
    RUN_TEST(test_hir_print_closures_concurrency);
    return UNITY_END();
}
