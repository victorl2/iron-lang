/* test_hir_data.c -- Unity tests for HIR data structure construction.
 *
 * Tests cover HIR-01 (structured control flow), HIR-02 (named variables),
 * HIR-03 (method/field/index expressions), and HIR-04 (closures, spawn,
 * parallel-for, defer).
 *
 * All modules are hand-constructed; no lowering code is used.
 */

#include "unity.h"
#include "hir/hir.h"
#include "analyzer/types.h"
#include "diagnostics/diagnostics.h"

#include <string.h>
#include <stdbool.h>

/* ── Fixtures ────────────────────────────────────────────────────────────── */

static IronHIR_Module *g_mod = NULL;

void setUp(void) {
    iron_types_init(NULL);  /* initialize primitive type singletons */
    g_mod = iron_hir_module_create("test_module");
}

void tearDown(void) {
    iron_hir_module_destroy(g_mod);
    g_mod = NULL;
}

/* ── Helpers ──────────────────────────────────────────────────────────────── */

static Iron_Span zero_span(void) {
    return iron_span_make("test.iron", 1, 1, 1, 1);
}

/* ── Test 1: Module create/destroy ───────────────────────────────────────── */

void test_hir_module_create_destroy(void) {
    IronHIR_Module *mod = iron_hir_module_create("mymod");

    TEST_ASSERT_NOT_NULL(mod);
    TEST_ASSERT_EQUAL_STRING("mymod", mod->name);
    TEST_ASSERT_EQUAL_UINT(1, mod->next_var_id);  /* starts at 1 */
    TEST_ASSERT_EQUAL_INT(0, mod->func_count);

    iron_hir_module_destroy(mod);
    /* If we reach here, destroy did not crash — no memory error */
}

/* ── Test 2: Variable allocation ─────────────────────────────────────────── */

void test_hir_alloc_var(void) {
    Iron_Type *int_type = iron_type_make_primitive(IRON_TYPE_INT);

    IronHIR_VarId x = iron_hir_alloc_var(g_mod, "x", int_type, false);
    IronHIR_VarId y = iron_hir_alloc_var(g_mod, "y", int_type, true);
    IronHIR_VarId z = iron_hir_alloc_var(g_mod, "z", int_type, false);

    TEST_ASSERT_EQUAL_UINT(1, x);
    TEST_ASSERT_EQUAL_UINT(2, y);
    TEST_ASSERT_EQUAL_UINT(3, z);

    TEST_ASSERT_EQUAL_STRING("x", iron_hir_var_name(g_mod, x));
    TEST_ASSERT_EQUAL_STRING("y", iron_hir_var_name(g_mod, y));
    TEST_ASSERT_EQUAL_STRING("z", iron_hir_var_name(g_mod, z));
}

/* ── Test 3: Structured control flow (HIR-01) ────────────────────────────── */

void test_hir_structured_control_flow(void) {
    Iron_Span span = zero_span();
    Iron_Type *int_type  = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_Type *bool_type = iron_type_make_primitive(IRON_TYPE_BOOL);

    /* condition: x > 0 */
    IronHIR_VarId x_id = iron_hir_alloc_var(g_mod, "x", int_type, false);
    IronHIR_Expr *x_ref  = iron_hir_expr_ident(g_mod, x_id, "x", int_type, span);
    IronHIR_Expr *zero   = iron_hir_expr_int_lit(g_mod, 0, int_type, span);
    IronHIR_Expr *cond   = iron_hir_expr_binop(g_mod, IRON_HIR_BINOP_GT,
                                                x_ref, zero, bool_type, span);

    /* then block: val a: Int = 1 */
    IronHIR_VarId a_id = iron_hir_alloc_var(g_mod, "a", int_type, false);
    IronHIR_Expr *one  = iron_hir_expr_int_lit(g_mod, 1, int_type, span);
    IronHIR_Stmt *let_a = iron_hir_stmt_let(g_mod, a_id, int_type, one, false, span);
    IronHIR_Block *then_body = iron_hir_block_create(g_mod);
    iron_hir_block_add_stmt(then_body, let_a);

    /* else block: a = 0 (assign) */
    IronHIR_Expr *a_ref    = iron_hir_expr_ident(g_mod, a_id, "a", int_type, span);
    IronHIR_Expr *zero2    = iron_hir_expr_int_lit(g_mod, 0, int_type, span);
    IronHIR_Stmt *assign_a = iron_hir_stmt_assign(g_mod, a_ref, zero2, span);
    IronHIR_Block *else_body = iron_hir_block_create(g_mod);
    iron_hir_block_add_stmt(else_body, assign_a);

    /* if statement */
    IronHIR_Stmt *if_stmt = iron_hir_stmt_if(g_mod, cond, then_body, else_body, span);

    TEST_ASSERT_NOT_NULL(if_stmt);
    TEST_ASSERT_EQUAL_INT(IRON_HIR_STMT_IF, if_stmt->kind);
    TEST_ASSERT_NOT_NULL(if_stmt->if_else.condition);
    TEST_ASSERT_NOT_NULL(if_stmt->if_else.then_body);
    TEST_ASSERT_NOT_NULL(if_stmt->if_else.else_body);
    TEST_ASSERT_EQUAL_INT(1, if_stmt->if_else.then_body->stmt_count);
    TEST_ASSERT_EQUAL_INT(1, if_stmt->if_else.else_body->stmt_count);
    TEST_ASSERT_EQUAL_INT(IRON_HIR_EXPR_BINOP, if_stmt->if_else.condition->kind);
    TEST_ASSERT_EQUAL_INT(IRON_HIR_BINOP_GT, if_stmt->if_else.condition->binop.op);
}

/* ── Test 4: For / while / match (HIR-01) ────────────────────────────────── */

void test_hir_for_while_match(void) {
    Iron_Span span = zero_span();
    Iron_Type *int_type  = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_Type *bool_type = iron_type_make_primitive(IRON_TYPE_BOOL);

    /* for loop: for i in range_expr {} */
    IronHIR_VarId i_id  = iron_hir_alloc_var(g_mod, "i", int_type, false);
    IronHIR_Expr *range = iron_hir_expr_int_lit(g_mod, 10, int_type, span);
    IronHIR_Block *for_body = iron_hir_block_create(g_mod);
    IronHIR_Stmt *for_stmt  = iron_hir_stmt_for(g_mod, i_id, range, for_body, span);

    TEST_ASSERT_NOT_NULL(for_stmt);
    TEST_ASSERT_EQUAL_INT(IRON_HIR_STMT_FOR, for_stmt->kind);
    TEST_ASSERT_EQUAL_UINT(i_id, for_stmt->for_loop.var_id);
    TEST_ASSERT_NOT_NULL(for_stmt->for_loop.iterable);
    TEST_ASSERT_NOT_NULL(for_stmt->for_loop.body);

    /* while loop: while cond {} */
    IronHIR_Expr *w_cond  = iron_hir_expr_bool_lit(g_mod, true, bool_type, span);
    IronHIR_Block *w_body = iron_hir_block_create(g_mod);
    IronHIR_Stmt *while_stmt = iron_hir_stmt_while(g_mod, w_cond, w_body, span);

    TEST_ASSERT_NOT_NULL(while_stmt);
    TEST_ASSERT_EQUAL_INT(IRON_HIR_STMT_WHILE, while_stmt->kind);
    TEST_ASSERT_NOT_NULL(while_stmt->while_loop.condition);
    TEST_ASSERT_NOT_NULL(while_stmt->while_loop.body);

    /* match with 2 arms */
    IronHIR_VarId val_id = iron_hir_alloc_var(g_mod, "n", int_type, false);
    IronHIR_Expr *scrutinee = iron_hir_expr_ident(g_mod, val_id, "n", int_type, span);

    IronHIR_MatchArm arms[2];
    arms[0].pattern = iron_hir_expr_int_lit(g_mod, 0, int_type, span);
    arms[0].guard   = NULL;
    arms[0].body    = iron_hir_block_create(g_mod);
    arms[1].pattern = iron_hir_expr_int_lit(g_mod, 1, int_type, span);
    arms[1].guard   = NULL;
    arms[1].body    = iron_hir_block_create(g_mod);

    IronHIR_Stmt *match_stmt = iron_hir_stmt_match(g_mod, scrutinee, arms, 2, span);

    TEST_ASSERT_NOT_NULL(match_stmt);
    TEST_ASSERT_EQUAL_INT(IRON_HIR_STMT_MATCH, match_stmt->kind);
    TEST_ASSERT_NOT_NULL(match_stmt->match_stmt.scrutinee);
    TEST_ASSERT_EQUAL_INT(2, match_stmt->match_stmt.arm_count);
    TEST_ASSERT_NOT_NULL(match_stmt->match_stmt.arms);
}

/* ── Test 5: Named variables (HIR-02) ────────────────────────────────────── */

void test_hir_named_variables(void) {
    Iron_Span span = zero_span();
    Iron_Type *int_type = iron_type_make_primitive(IRON_TYPE_INT);

    /* val x: Int = 42 */
    IronHIR_VarId x_id = iron_hir_alloc_var(g_mod, "x", int_type, false);
    IronHIR_Expr *init_x = iron_hir_expr_int_lit(g_mod, 42, int_type, span);
    IronHIR_Stmt *let_x  = iron_hir_stmt_let(g_mod, x_id, int_type, init_x, false, span);

    TEST_ASSERT_NOT_NULL(let_x);
    TEST_ASSERT_EQUAL_INT(IRON_HIR_STMT_LET, let_x->kind);
    TEST_ASSERT_EQUAL_UINT(x_id, let_x->let.var_id);
    TEST_ASSERT_FALSE(let_x->let.is_mutable);
    TEST_ASSERT_EQUAL_STRING("x", iron_hir_var_name(g_mod, x_id));

    /* var y: Int = 0 */
    IronHIR_VarId y_id = iron_hir_alloc_var(g_mod, "y", int_type, true);
    IronHIR_Expr *init_y = iron_hir_expr_int_lit(g_mod, 0, int_type, span);
    IronHIR_Stmt *let_y  = iron_hir_stmt_let(g_mod, y_id, int_type, init_y, true, span);

    TEST_ASSERT_NOT_NULL(let_y);
    TEST_ASSERT_EQUAL_INT(IRON_HIR_STMT_LET, let_y->kind);
    TEST_ASSERT_EQUAL_UINT(y_id, let_y->let.var_id);
    TEST_ASSERT_TRUE(let_y->let.is_mutable);
    TEST_ASSERT_EQUAL_STRING("y", iron_hir_var_name(g_mod, y_id));

    /* VarIds resolve correctly via name_table */
    TEST_ASSERT_EQUAL_STRING("x", g_mod->name_table[x_id].name);
    TEST_ASSERT_EQUAL_STRING("y", g_mod->name_table[y_id].name);
    TEST_ASSERT_FALSE(g_mod->name_table[x_id].is_mutable);
    TEST_ASSERT_TRUE(g_mod->name_table[y_id].is_mutable);
}

/* ── Test 6: Method call / field access / index (HIR-03) ─────────────────── */

void test_hir_method_field_index(void) {
    Iron_Span span     = zero_span();
    Iron_Type *int_type = iron_type_make_primitive(IRON_TYPE_INT);

    /* obj.method(arg) */
    IronHIR_VarId obj_id = iron_hir_alloc_var(g_mod, "obj", int_type, false);
    IronHIR_Expr *obj    = iron_hir_expr_ident(g_mod, obj_id, "obj", int_type, span);
    IronHIR_VarId arg_id = iron_hir_alloc_var(g_mod, "arg", int_type, false);
    IronHIR_Expr *arg    = iron_hir_expr_ident(g_mod, arg_id, "arg", int_type, span);

    /* Build args array on stack; constructor stores the pointer */
    IronHIR_Expr *args[1] = { arg };
    IronHIR_Expr *mcall   = iron_hir_expr_method_call(g_mod, obj, "length",
                                                        args, 1, int_type, span);

    TEST_ASSERT_NOT_NULL(mcall);
    TEST_ASSERT_EQUAL_INT(IRON_HIR_EXPR_METHOD_CALL, mcall->kind);
    TEST_ASSERT_EQUAL_PTR(obj, mcall->method_call.object);
    TEST_ASSERT_EQUAL_STRING("length", mcall->method_call.method);
    TEST_ASSERT_EQUAL_INT(1, mcall->method_call.arg_count);

    /* obj.field */
    IronHIR_Expr *fld = iron_hir_expr_field_access(g_mod, obj, "count",
                                                     int_type, span);

    TEST_ASSERT_NOT_NULL(fld);
    TEST_ASSERT_EQUAL_INT(IRON_HIR_EXPR_FIELD_ACCESS, fld->kind);
    TEST_ASSERT_EQUAL_PTR(obj, fld->field_access.object);
    TEST_ASSERT_EQUAL_STRING("count", fld->field_access.field);

    /* arr[idx] */
    IronHIR_VarId arr_id = iron_hir_alloc_var(g_mod, "arr", int_type, false);
    IronHIR_Expr *arr    = iron_hir_expr_ident(g_mod, arr_id, "arr", int_type, span);
    IronHIR_Expr *idx    = iron_hir_expr_int_lit(g_mod, 2, int_type, span);
    IronHIR_Expr *index_expr = iron_hir_expr_index(g_mod, arr, idx, int_type, span);

    TEST_ASSERT_NOT_NULL(index_expr);
    TEST_ASSERT_EQUAL_INT(IRON_HIR_EXPR_INDEX, index_expr->kind);
    TEST_ASSERT_EQUAL_PTR(arr, index_expr->index.array);
    TEST_ASSERT_EQUAL_PTR(idx, index_expr->index.index);
}

/* ── Test 7: Closures, spawn, parallel-for, defer (HIR-04) ──────────────── */

void test_hir_closures_spawn_parallel_defer(void) {
    Iron_Span span     = zero_span();
    Iron_Type *int_type = iron_type_make_primitive(IRON_TYPE_INT);

    /* Closure expr: |p: Int| -> Int { return p } */
    IronHIR_VarId p_id    = iron_hir_alloc_var(g_mod, "p", int_type, false);
    IronHIR_Param params[1];
    params[0].var_id = p_id;
    params[0].name   = "p";
    params[0].type   = int_type;

    IronHIR_Expr  *p_ref    = iron_hir_expr_ident(g_mod, p_id, "p", int_type, span);
    IronHIR_Stmt  *ret_p    = iron_hir_stmt_return(g_mod, p_ref, span);
    IronHIR_Block *cl_body  = iron_hir_block_create(g_mod);
    iron_hir_block_add_stmt(cl_body, ret_p);

    IronHIR_Expr *closure = iron_hir_expr_closure(g_mod, params, 1,
                                                    int_type, cl_body,
                                                    NULL, NULL, span);

    TEST_ASSERT_NOT_NULL(closure);
    TEST_ASSERT_EQUAL_INT(IRON_HIR_EXPR_CLOSURE, closure->kind);
    TEST_ASSERT_EQUAL_INT(1, closure->closure.param_count);
    TEST_ASSERT_NOT_NULL(closure->closure.body);
    TEST_ASSERT_EQUAL_INT(1, closure->closure.body->stmt_count);

    /* Spawn statement */
    IronHIR_Block *spawn_body = iron_hir_block_create(g_mod);
    IronHIR_Stmt  *spawn_stmt = iron_hir_stmt_spawn(g_mod, "task_handle",
                                                      spawn_body, NULL, span);

    TEST_ASSERT_NOT_NULL(spawn_stmt);
    TEST_ASSERT_EQUAL_INT(IRON_HIR_STMT_SPAWN, spawn_stmt->kind);
    TEST_ASSERT_EQUAL_STRING("task_handle", spawn_stmt->spawn.handle_name);
    TEST_ASSERT_NOT_NULL(spawn_stmt->spawn.body);

    /* Parallel-for expr: parallel_for i in range { } */
    IronHIR_VarId  pi_id   = iron_hir_alloc_var(g_mod, "pi", int_type, false);
    IronHIR_Expr  *pf_range = iron_hir_expr_int_lit(g_mod, 100, int_type, span);
    IronHIR_Block *pf_body  = iron_hir_block_create(g_mod);
    IronHIR_Expr  *pfor     = iron_hir_expr_parallel_for(g_mod, pi_id,
                                                          pf_range, pf_body,
                                                          NULL, NULL, span);

    TEST_ASSERT_NOT_NULL(pfor);
    TEST_ASSERT_EQUAL_INT(IRON_HIR_EXPR_PARALLEL_FOR, pfor->kind);
    TEST_ASSERT_EQUAL_UINT(pi_id, pfor->parallel_for.var_id);
    TEST_ASSERT_NOT_NULL(pfor->parallel_for.range);
    TEST_ASSERT_NOT_NULL(pfor->parallel_for.body);

    /* Defer statement */
    IronHIR_Block *defer_body = iron_hir_block_create(g_mod);
    IronHIR_Stmt  *defer_stmt = iron_hir_stmt_defer(g_mod, defer_body, span);

    TEST_ASSERT_NOT_NULL(defer_stmt);
    TEST_ASSERT_EQUAL_INT(IRON_HIR_STMT_DEFER, defer_stmt->kind);
    TEST_ASSERT_NOT_NULL(defer_stmt->defer.body);
}

/* ── Test 8: Func and module ─────────────────────────────────────────────── */

void test_hir_func_and_module(void) {
    Iron_Span span      = zero_span();
    Iron_Type *int_type = iron_type_make_primitive(IRON_TYPE_INT);

    IronHIR_VarId a_id = iron_hir_alloc_var(g_mod, "a", int_type, false);
    IronHIR_VarId b_id = iron_hir_alloc_var(g_mod, "b", int_type, false);

    IronHIR_Param params[2];
    params[0].var_id = a_id;
    params[0].name   = "a";
    params[0].type   = int_type;
    params[1].var_id = b_id;
    params[1].name   = "b";
    params[1].type   = int_type;

    IronHIR_Func *fn = iron_hir_func_create(g_mod, "add", params, 2, int_type);

    IronHIR_Expr *ret_val  = iron_hir_expr_int_lit(g_mod, 0, int_type, span);
    IronHIR_Stmt *ret_stmt = iron_hir_stmt_return(g_mod, ret_val, span);
    IronHIR_Block *body    = iron_hir_block_create(g_mod);
    iron_hir_block_add_stmt(body, ret_stmt);
    fn->body = body;

    iron_hir_module_add_func(g_mod, fn);

    TEST_ASSERT_EQUAL_INT(1, g_mod->func_count);
    TEST_ASSERT_EQUAL_PTR(fn, g_mod->funcs[0]);
    TEST_ASSERT_EQUAL_INT(2, fn->param_count);
    TEST_ASSERT_EQUAL_INT(1, fn->body->stmt_count);
    TEST_ASSERT_EQUAL_STRING("add", fn->name);
}

/* ── Test 9: Literal expressions ─────────────────────────────────────────── */

void test_hir_literal_expressions(void) {
    Iron_Span span       = zero_span();
    Iron_Type *int_type  = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_Type *f64_type  = iron_type_make_primitive(IRON_TYPE_FLOAT64);
    Iron_Type *str_type  = iron_type_make_primitive(IRON_TYPE_STRING);
    Iron_Type *bool_type = iron_type_make_primitive(IRON_TYPE_BOOL);
    Iron_Type *null_type = iron_type_make_primitive(IRON_TYPE_NULL);

    IronHIR_Expr *i = iron_hir_expr_int_lit(g_mod, 42, int_type, span);
    TEST_ASSERT_NOT_NULL(i);
    TEST_ASSERT_EQUAL_INT(IRON_HIR_EXPR_INT_LIT, i->kind);
    TEST_ASSERT_EQUAL_INT64(42, i->int_lit.value);

    IronHIR_Expr *f = iron_hir_expr_float_lit(g_mod, 3.14, f64_type, span);
    TEST_ASSERT_NOT_NULL(f);
    TEST_ASSERT_EQUAL_INT(IRON_HIR_EXPR_FLOAT_LIT, f->kind);
    TEST_ASSERT_EQUAL_DOUBLE(3.14, f->float_lit.value);

    IronHIR_Expr *s = iron_hir_expr_string_lit(g_mod, "hello", str_type, span);
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_EQUAL_INT(IRON_HIR_EXPR_STRING_LIT, s->kind);
    TEST_ASSERT_EQUAL_STRING("hello", s->string_lit.value);

    IronHIR_Expr *b = iron_hir_expr_bool_lit(g_mod, true, bool_type, span);
    TEST_ASSERT_NOT_NULL(b);
    TEST_ASSERT_EQUAL_INT(IRON_HIR_EXPR_BOOL_LIT, b->kind);
    TEST_ASSERT_TRUE(b->bool_lit.value);

    IronHIR_Expr *n = iron_hir_expr_null_lit(g_mod, null_type, span);
    TEST_ASSERT_NOT_NULL(n);
    TEST_ASSERT_EQUAL_INT(IRON_HIR_EXPR_NULL_LIT, n->kind);
}

/* ── Test 10: Complex expressions ────────────────────────────────────────── */

void test_hir_complex_expressions(void) {
    Iron_Span span      = zero_span();
    Iron_Type *int_type = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_Type *str_type = iron_type_make_primitive(IRON_TYPE_STRING);

    IronHIR_VarId v_id = iron_hir_alloc_var(g_mod, "v", int_type, false);
    IronHIR_Expr *v    = iron_hir_expr_ident(g_mod, v_id, "v", int_type, span);

    /* call */
    IronHIR_Expr *callee = iron_hir_expr_func_ref(g_mod, "add", int_type, span);
    IronHIR_Expr *call_args[1] = { v };
    IronHIR_Expr *call   = iron_hir_expr_call(g_mod, callee, call_args, 1, int_type, span);
    TEST_ASSERT_NOT_NULL(call);
    TEST_ASSERT_EQUAL_INT(IRON_HIR_EXPR_CALL, call->kind);

    /* construct */
    IronHIR_Expr *construct = iron_hir_expr_construct(g_mod, int_type,
                                                        NULL, NULL, 0, span);
    TEST_ASSERT_NOT_NULL(construct);
    TEST_ASSERT_EQUAL_INT(IRON_HIR_EXPR_CONSTRUCT, construct->kind);

    /* array_lit */
    IronHIR_Expr *elems[2] = { iron_hir_expr_int_lit(g_mod, 1, int_type, span),
                                iron_hir_expr_int_lit(g_mod, 2, int_type, span) };
    IronHIR_Expr *arr_lit  = iron_hir_expr_array_lit(g_mod, int_type, elems, 2, NULL, span);
    TEST_ASSERT_NOT_NULL(arr_lit);
    TEST_ASSERT_EQUAL_INT(IRON_HIR_EXPR_ARRAY_LIT, arr_lit->kind);
    TEST_ASSERT_EQUAL_INT(2, arr_lit->array_lit.element_count);

    /* cast */
    IronHIR_Expr *cst = iron_hir_expr_cast(g_mod, v, int_type, span);
    TEST_ASSERT_NOT_NULL(cst);
    TEST_ASSERT_EQUAL_INT(IRON_HIR_EXPR_CAST, cst->kind);

    /* heap */
    IronHIR_Expr *hval = iron_hir_expr_heap(g_mod, v, true, false, NULL, span);
    TEST_ASSERT_NOT_NULL(hval);
    TEST_ASSERT_EQUAL_INT(IRON_HIR_EXPR_HEAP, hval->kind);

    /* rc */
    IronHIR_Expr *rcval = iron_hir_expr_rc(g_mod, v, NULL, span);
    TEST_ASSERT_NOT_NULL(rcval);
    TEST_ASSERT_EQUAL_INT(IRON_HIR_EXPR_RC, rcval->kind);

    /* await */
    IronHIR_Expr *aw = iron_hir_expr_await(g_mod, v, int_type, span);
    TEST_ASSERT_NOT_NULL(aw);
    TEST_ASSERT_EQUAL_INT(IRON_HIR_EXPR_AWAIT, aw->kind);

    /* func_ref */
    IronHIR_Expr *fr = iron_hir_expr_func_ref(g_mod, "my_func", NULL, span);
    TEST_ASSERT_NOT_NULL(fr);
    TEST_ASSERT_EQUAL_INT(IRON_HIR_EXPR_FUNC_REF, fr->kind);
    TEST_ASSERT_EQUAL_STRING("my_func", fr->func_ref.func_name);

    /* comptime */
    IronHIR_Expr *ct = iron_hir_expr_comptime(g_mod, v, int_type, span);
    TEST_ASSERT_NOT_NULL(ct);
    TEST_ASSERT_EQUAL_INT(IRON_HIR_EXPR_COMPTIME, ct->kind);

    /* is_null */
    IronHIR_Expr *isn = iron_hir_expr_is_null(g_mod, v, span);
    TEST_ASSERT_NOT_NULL(isn);
    TEST_ASSERT_EQUAL_INT(IRON_HIR_EXPR_IS_NULL, isn->kind);

    /* is_not_null */
    IronHIR_Expr *inn = iron_hir_expr_is_not_null(g_mod, v, span);
    TEST_ASSERT_NOT_NULL(inn);
    TEST_ASSERT_EQUAL_INT(IRON_HIR_EXPR_IS_NOT_NULL, inn->kind);

    /* is_check */
    IronHIR_Expr *is_chk = iron_hir_expr_is(g_mod, v, int_type, span);
    TEST_ASSERT_NOT_NULL(is_chk);
    TEST_ASSERT_EQUAL_INT(IRON_HIR_EXPR_IS, is_chk->kind);
    TEST_ASSERT_EQUAL_PTR(int_type, is_chk->is_check.check_type);

    /* slice */
    IronHIR_Expr *start = iron_hir_expr_int_lit(g_mod, 0, int_type, span);
    IronHIR_Expr *end   = iron_hir_expr_int_lit(g_mod, 5, int_type, span);
    IronHIR_Expr *slc   = iron_hir_expr_slice(g_mod, v, start, end, NULL, span);
    TEST_ASSERT_NOT_NULL(slc);
    TEST_ASSERT_EQUAL_INT(IRON_HIR_EXPR_SLICE, slc->kind);

    /* interp_string */
    IronHIR_Expr *iparts[2] = {
        iron_hir_expr_string_lit(g_mod, "hello ", str_type, span),
        iron_hir_expr_ident(g_mod, v_id, "v", int_type, span)
    };
    IronHIR_Expr *istr = iron_hir_expr_interp_string(g_mod, iparts, 2, str_type, span);
    TEST_ASSERT_NOT_NULL(istr);
    TEST_ASSERT_EQUAL_INT(IRON_HIR_EXPR_INTERP_STRING, istr->kind);
    TEST_ASSERT_EQUAL_INT(2, istr->interp_string.part_count);
}

/* ── main ────────────────────────────────────────────────────────────────── */

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_hir_module_create_destroy);
    RUN_TEST(test_hir_alloc_var);
    RUN_TEST(test_hir_structured_control_flow);
    RUN_TEST(test_hir_for_while_match);
    RUN_TEST(test_hir_named_variables);
    RUN_TEST(test_hir_method_field_index);
    RUN_TEST(test_hir_closures_spawn_parallel_defer);
    RUN_TEST(test_hir_func_and_module);
    RUN_TEST(test_hir_literal_expressions);
    RUN_TEST(test_hir_complex_expressions);
    return UNITY_END();
}
