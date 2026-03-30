/* test_hir_lower.c — Unity unit tests for AST-to-HIR lowering pass.
 *
 * Each test builds a minimal Iron AST programmatically, calls iron_hir_lower(),
 * and verifies the structure of the resulting HIR module.
 *
 * Covers (50 tests total):
 * Smoke tests (15):
 *   - Empty program
 *   - Simple func declaration
 *   - Val/var bindings (STMT_LET)
 *   - If/else (STMT_IF)
 *   - While loop (STMT_WHILE)
 *   - For-range desugaring to while (STMT_WHILE)
 *   - Return value (STMT_RETURN)
 *   - Binary expression (EXPR_BINOP)
 *   - Function call (EXPR_CALL)
 *   - Integer literal (EXPR_INT_LIT)
 *   - String literal (EXPR_STRING_LIT)
 *   - Elif desugaring (nested STMT_IF)
 *   - iron_hir_verify passes on output module
 *
 * Feature-matrix tests (21):
 *   Literals & expressions: float, bool, null, interp-string, unary-neg,
 *     unary-not, all binary ops
 *   Statement desugaring: for-range-int, for-array-iter, compound-assign-add,
 *     compound-assign-sub, match-stmt
 *   Memory: heap-alloc, rc-alloc, free-stmt, leak-stmt
 *   Calls: func-with-params, call-with-args, method-call, field-access,
 *     index-access, cast, construct
 *   Higher-order/concurrency: lambda, spawn, parallel-for, await, defer
 *   Scope: nested-scope, func-ref
 *
 * Edge-case tests (14):
 *   Deep nesting: nested-if-5-levels, while-inside-if, match-inside-while
 *   Defer: multiple-defers, defer-with-return, nested-defer-scopes
 *   Variable scoping: shadowed-variable, global-constant-lazy, global-mutable-var
 *   Complex exprs: nested-call, chained-field-access, index-of-call,
 *     array-literal-empty
 *   Error handling: error-node, verify-output (large)
 */

#include "unity.h"
#include "hir/hir_lower.h"
#include "hir/hir.h"
#include "parser/ast.h"
#include "analyzer/types.h"
#include "diagnostics/diagnostics.h"
#include "util/arena.h"
#include "lexer/lexer.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>

/* ── Fixtures ────────────────────────────────────────────────────────────── */

static Iron_Arena  g_ast_arena;
static Iron_DiagList g_diags;

void setUp(void) {
    g_ast_arena = iron_arena_create(128 * 1024);
    iron_types_init(&g_ast_arena);
    g_diags = iron_diaglist_create();
}

void tearDown(void) {
    iron_diaglist_free(&g_diags);
    iron_arena_free(&g_ast_arena);
}

/* ── AST construction helpers ────────────────────────────────────────────── */

static Iron_Span test_span(void) {
    return iron_span_make("test.iron", 1, 1, 1, 1);
}

static Iron_Node *make_int(Iron_Arena *a, const char *value, Iron_Type *ty) {
    Iron_IntLit *n = ARENA_ALLOC(a, Iron_IntLit);
    memset(n, 0, sizeof(*n));
    n->span = test_span();
    n->kind = IRON_NODE_INT_LIT;
    n->value = value;
    n->resolved_type = ty ? ty : iron_type_make_primitive(IRON_TYPE_INT);
    return (Iron_Node *)n;
}

static Iron_Node *make_bool_lit(Iron_Arena *a, bool value) {
    Iron_BoolLit *n = ARENA_ALLOC(a, Iron_BoolLit);
    memset(n, 0, sizeof(*n));
    n->span = test_span();
    n->kind = IRON_NODE_BOOL_LIT;
    n->value = value;
    n->resolved_type = iron_type_make_primitive(IRON_TYPE_BOOL);
    return (Iron_Node *)n;
}

static Iron_Node *make_string_lit(Iron_Arena *a, const char *value) {
    Iron_StringLit *n = ARENA_ALLOC(a, Iron_StringLit);
    memset(n, 0, sizeof(*n));
    n->span = test_span();
    n->kind = IRON_NODE_STRING_LIT;
    n->value = value;
    n->resolved_type = iron_type_make_primitive(IRON_TYPE_STRING);
    return (Iron_Node *)n;
}

static Iron_Node *make_ident(Iron_Arena *a, const char *name, Iron_Type *ty) {
    Iron_Ident *n = ARENA_ALLOC(a, Iron_Ident);
    memset(n, 0, sizeof(*n));
    n->span = test_span();
    n->kind = IRON_NODE_IDENT;
    n->name = name;
    n->resolved_type = ty;
    return (Iron_Node *)n;
}

static Iron_Node *make_binary(Iron_Arena *a, Iron_Node *left, int op,
                               Iron_Node *right, Iron_Type *result_ty) {
    Iron_BinaryExpr *n = ARENA_ALLOC(a, Iron_BinaryExpr);
    memset(n, 0, sizeof(*n));
    n->span = test_span();
    n->kind = IRON_NODE_BINARY;
    n->left = left;
    n->op   = op;
    n->right = right;
    n->resolved_type = result_ty;
    return (Iron_Node *)n;
}

static Iron_Node *make_val_decl(Iron_Arena *a, const char *name,
                                 Iron_Node *init, Iron_Type *declared_ty) {
    Iron_ValDecl *n = ARENA_ALLOC(a, Iron_ValDecl);
    memset(n, 0, sizeof(*n));
    n->span = test_span();
    n->kind = IRON_NODE_VAL_DECL;
    n->name = name;
    n->init = init;
    n->declared_type = declared_ty;
    return (Iron_Node *)n;
}

static Iron_Node *make_var_decl(Iron_Arena *a, const char *name,
                                 Iron_Node *init, Iron_Type *declared_ty) {
    Iron_VarDecl *n = ARENA_ALLOC(a, Iron_VarDecl);
    memset(n, 0, sizeof(*n));
    n->span = test_span();
    n->kind = IRON_NODE_VAR_DECL;
    n->name = name;
    n->init = init;
    n->declared_type = declared_ty;
    return (Iron_Node *)n;
}

static Iron_Node *make_return(Iron_Arena *a, Iron_Node *value) {
    Iron_ReturnStmt *n = ARENA_ALLOC(a, Iron_ReturnStmt);
    memset(n, 0, sizeof(*n));
    n->span = test_span();
    n->kind = IRON_NODE_RETURN;
    n->value = value;
    return (Iron_Node *)n;
}

static Iron_Block *make_block(Iron_Arena *a, Iron_Node **stmts, int count) {
    Iron_Block *b = ARENA_ALLOC(a, Iron_Block);
    memset(b, 0, sizeof(*b));
    b->span = test_span();
    b->kind = IRON_NODE_BLOCK;
    if (count > 0) {
        b->stmts = (Iron_Node **)iron_arena_alloc(a,
                       (size_t)count * sizeof(Iron_Node *), _Alignof(Iron_Node *));
        memcpy(b->stmts, stmts, (size_t)count * sizeof(Iron_Node *));
    }
    b->stmt_count = count;
    return b;
}

static Iron_Node *make_if_stmt(Iron_Arena *a, Iron_Node *cond,
                                Iron_Block *body, Iron_Block *else_body) {
    Iron_IfStmt *n = ARENA_ALLOC(a, Iron_IfStmt);
    memset(n, 0, sizeof(*n));
    n->span = test_span();
    n->kind = IRON_NODE_IF;
    n->condition = cond;
    n->body = (Iron_Node *)body;
    n->else_body = else_body ? (Iron_Node *)else_body : NULL;
    n->elif_conds  = NULL;
    n->elif_bodies = NULL;
    n->elif_count  = 0;
    return (Iron_Node *)n;
}

static Iron_Node *make_while_stmt(Iron_Arena *a, Iron_Node *cond, Iron_Block *body) {
    Iron_WhileStmt *n = ARENA_ALLOC(a, Iron_WhileStmt);
    memset(n, 0, sizeof(*n));
    n->span = test_span();
    n->kind = IRON_NODE_WHILE;
    n->condition = cond;
    n->body = (Iron_Node *)body;
    return (Iron_Node *)n;
}

static Iron_Node *make_for_range(Iron_Arena *a, const char *var_name,
                                  Iron_Node *iterable) {
    Iron_ForStmt *n = ARENA_ALLOC(a, Iron_ForStmt);
    memset(n, 0, sizeof(*n));
    n->span = test_span();
    n->kind = IRON_NODE_FOR;
    n->var_name   = var_name;
    n->iterable   = iterable;
    n->is_parallel = false;
    /* Build empty body block */
    n->body = (Iron_Node *)make_block(a, NULL, 0);
    return (Iron_Node *)n;
}

static Iron_Node *make_call(Iron_Arena *a, Iron_Node *callee,
                             Iron_Node **args, int arg_count, Iron_Type *ty) {
    Iron_CallExpr *n = ARENA_ALLOC(a, Iron_CallExpr);
    memset(n, 0, sizeof(*n));
    n->span = test_span();
    n->kind = IRON_NODE_CALL;
    n->callee = callee;
    n->arg_count = arg_count;
    n->resolved_type = ty;
    if (arg_count > 0) {
        n->args = (Iron_Node **)iron_arena_alloc(a,
                      (size_t)arg_count * sizeof(Iron_Node *), _Alignof(Iron_Node *));
        memcpy(n->args, args, (size_t)arg_count * sizeof(Iron_Node *));
    }
    return (Iron_Node *)n;
}

static Iron_FuncDecl *make_func_decl(Iron_Arena *a, const char *name,
                                      Iron_Block *body, Iron_Type *ret_ty) {
    Iron_FuncDecl *fd = ARENA_ALLOC(a, Iron_FuncDecl);
    memset(fd, 0, sizeof(*fd));
    fd->span = test_span();
    fd->kind = IRON_NODE_FUNC_DECL;
    fd->name = name;
    fd->params = NULL;
    fd->param_count = 0;
    fd->body = (Iron_Node *)body;
    fd->resolved_return_type = ret_ty;
    return fd;
}

static Iron_Program *make_program(Iron_Arena *a, Iron_Node **decls, int count) {
    Iron_Program *prog = ARENA_ALLOC(a, Iron_Program);
    memset(prog, 0, sizeof(*prog));
    prog->span = test_span();
    prog->kind = IRON_NODE_PROGRAM;
    if (count > 0) {
        prog->decls = (Iron_Node **)iron_arena_alloc(a,
                          (size_t)count * sizeof(Iron_Node *), _Alignof(Iron_Node *));
        memcpy(prog->decls, decls, (size_t)count * sizeof(Iron_Node *));
    }
    prog->decl_count = count;
    return prog;
}

/* ── Test 1: Empty program ──────────────────────────────────────────────── */

void test_hir_lower_empty_program(void) {
    Iron_Program *prog = make_program(&g_ast_arena, NULL, 0);

    IronHIR_Module *mod = iron_hir_lower(prog, NULL, NULL, &g_diags);

    TEST_ASSERT_NOT_NULL(mod);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);
    TEST_ASSERT_EQUAL_INT(0, mod->func_count);

    iron_hir_module_destroy(mod);
}

/* ── Test 2: Simple func declaration ────────────────────────────────────── */

void test_hir_lower_simple_func(void) {
    Iron_Block    *body = make_block(&g_ast_arena, NULL, 0);
    Iron_FuncDecl *fd   = make_func_decl(&g_ast_arena, "main", body, NULL);
    Iron_Node     *decls[] = { (Iron_Node *)fd };
    Iron_Program  *prog = make_program(&g_ast_arena, decls, 1);

    IronHIR_Module *mod = iron_hir_lower(prog, NULL, NULL, &g_diags);

    TEST_ASSERT_NOT_NULL(mod);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);
    TEST_ASSERT_EQUAL_INT(1, mod->func_count);
    TEST_ASSERT_EQUAL_STRING("main", mod->funcs[0]->name);

    iron_hir_module_destroy(mod);
}

/* ── Test 3: Val binding (STMT_LET, is_mutable = false) ──────────────────── */

void test_hir_lower_val_binding(void) {
    Iron_Type *int_ty = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_Node *init   = make_int(&g_ast_arena, "42", int_ty);
    Iron_Node *vd     = make_val_decl(&g_ast_arena, "x", init, int_ty);

    Iron_Node *stmts[] = { vd };
    Iron_Block    *body = make_block(&g_ast_arena, stmts, 1);
    Iron_FuncDecl *fd   = make_func_decl(&g_ast_arena, "main", body, NULL);
    Iron_Node     *decls[] = { (Iron_Node *)fd };
    Iron_Program  *prog = make_program(&g_ast_arena, decls, 1);

    IronHIR_Module *mod = iron_hir_lower(prog, NULL, NULL, &g_diags);

    TEST_ASSERT_NOT_NULL(mod);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);
    TEST_ASSERT_EQUAL_INT(1, mod->func_count);

    IronHIR_Block *hir_body = mod->funcs[0]->body;
    TEST_ASSERT_NOT_NULL(hir_body);
    TEST_ASSERT_EQUAL_INT(1, hir_body->stmt_count);

    IronHIR_Stmt *let = hir_body->stmts[0];
    TEST_ASSERT_NOT_NULL(let);
    TEST_ASSERT_EQUAL_INT(IRON_HIR_STMT_LET, let->kind);
    TEST_ASSERT_FALSE(let->let.is_mutable);

    iron_hir_module_destroy(mod);
}

/* ── Test 4: Var binding (STMT_LET, is_mutable = true) ───────────────────── */

void test_hir_lower_var_binding(void) {
    Iron_Type *int_ty = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_Node *init   = make_int(&g_ast_arena, "42", int_ty);
    Iron_Node *vd     = make_var_decl(&g_ast_arena, "y", init, int_ty);

    Iron_Node *stmts[] = { vd };
    Iron_Block    *body = make_block(&g_ast_arena, stmts, 1);
    Iron_FuncDecl *fd   = make_func_decl(&g_ast_arena, "main", body, NULL);
    Iron_Node     *decls[] = { (Iron_Node *)fd };
    Iron_Program  *prog = make_program(&g_ast_arena, decls, 1);

    IronHIR_Module *mod = iron_hir_lower(prog, NULL, NULL, &g_diags);

    TEST_ASSERT_NOT_NULL(mod);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);

    IronHIR_Block *hir_body = mod->funcs[0]->body;
    TEST_ASSERT_EQUAL_INT(1, hir_body->stmt_count);

    IronHIR_Stmt *let = hir_body->stmts[0];
    TEST_ASSERT_EQUAL_INT(IRON_HIR_STMT_LET, let->kind);
    TEST_ASSERT_TRUE(let->let.is_mutable);

    iron_hir_module_destroy(mod);
}

/* ── Test 5: If/else (STMT_IF with both branches) ────────────────────────── */

void test_hir_lower_if_else(void) {
    Iron_Type *bool_ty = iron_type_make_primitive(IRON_TYPE_BOOL);
    Iron_Node *cond = make_bool_lit(&g_ast_arena, true);

    Iron_Block *then_blk = make_block(&g_ast_arena, NULL, 0);
    Iron_Block *else_blk = make_block(&g_ast_arena, NULL, 0);
    Iron_Node  *if_node  = make_if_stmt(&g_ast_arena, cond, then_blk, else_blk);
    (void)bool_ty;

    Iron_Node *stmts[] = { if_node };
    Iron_Block    *body = make_block(&g_ast_arena, stmts, 1);
    Iron_FuncDecl *fd   = make_func_decl(&g_ast_arena, "main", body, NULL);
    Iron_Node     *decls[] = { (Iron_Node *)fd };
    Iron_Program  *prog = make_program(&g_ast_arena, decls, 1);

    IronHIR_Module *mod = iron_hir_lower(prog, NULL, NULL, &g_diags);

    TEST_ASSERT_NOT_NULL(mod);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);

    IronHIR_Block *hir_body = mod->funcs[0]->body;
    TEST_ASSERT_EQUAL_INT(1, hir_body->stmt_count);

    IronHIR_Stmt *if_stmt = hir_body->stmts[0];
    TEST_ASSERT_EQUAL_INT(IRON_HIR_STMT_IF, if_stmt->kind);
    TEST_ASSERT_NOT_NULL(if_stmt->if_else.condition);
    TEST_ASSERT_NOT_NULL(if_stmt->if_else.then_body);
    TEST_ASSERT_NOT_NULL(if_stmt->if_else.else_body);

    iron_hir_module_destroy(mod);
}

/* ── Test 6: While loop (STMT_WHILE) ─────────────────────────────────────── */

void test_hir_lower_while_loop(void) {
    Iron_Node  *cond     = make_bool_lit(&g_ast_arena, true);
    Iron_Block *loop_blk = make_block(&g_ast_arena, NULL, 0);
    Iron_Node  *while_node = make_while_stmt(&g_ast_arena, cond, loop_blk);

    Iron_Node *stmts[] = { while_node };
    Iron_Block    *body = make_block(&g_ast_arena, stmts, 1);
    Iron_FuncDecl *fd   = make_func_decl(&g_ast_arena, "main", body, NULL);
    Iron_Node     *decls[] = { (Iron_Node *)fd };
    Iron_Program  *prog = make_program(&g_ast_arena, decls, 1);

    IronHIR_Module *mod = iron_hir_lower(prog, NULL, NULL, &g_diags);

    TEST_ASSERT_NOT_NULL(mod);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);

    IronHIR_Block *hir_body = mod->funcs[0]->body;
    TEST_ASSERT_EQUAL_INT(1, hir_body->stmt_count);

    IronHIR_Stmt *while_stmt = hir_body->stmts[0];
    TEST_ASSERT_EQUAL_INT(IRON_HIR_STMT_WHILE, while_stmt->kind);
    TEST_ASSERT_NOT_NULL(while_stmt->while_loop.condition);
    TEST_ASSERT_NOT_NULL(while_stmt->while_loop.body);

    iron_hir_module_destroy(mod);
}

/* ── Test 7: For-range desugaring (STMT_WHILE) ──────────────────────────── */

void test_hir_lower_for_range_desugars(void) {
    /* for i in 0..10 — iterable is binary DOTDOT */
    Iron_Type *int_ty = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_Node *start  = make_int(&g_ast_arena, "0", int_ty);
    Iron_Node *end    = make_int(&g_ast_arena, "10", int_ty);
    /* Build 0..10 binary node */
    Iron_BinaryExpr *range_bin = ARENA_ALLOC(&g_ast_arena, Iron_BinaryExpr);
    memset(range_bin, 0, sizeof(*range_bin));
    range_bin->span = test_span();
    range_bin->kind = IRON_NODE_BINARY;
    range_bin->left = start;
    range_bin->op   = IRON_TOK_DOTDOT;
    range_bin->right = end;
    range_bin->resolved_type = int_ty;

    Iron_Node *for_node = make_for_range(&g_ast_arena, "i",
                                          (Iron_Node *)range_bin);

    Iron_Node *stmts[] = { for_node };
    Iron_Block    *body = make_block(&g_ast_arena, stmts, 1);
    Iron_FuncDecl *fd   = make_func_decl(&g_ast_arena, "main", body, NULL);
    Iron_Node     *decls[] = { (Iron_Node *)fd };
    Iron_Program  *prog = make_program(&g_ast_arena, decls, 1);

    IronHIR_Module *mod = iron_hir_lower(prog, NULL, NULL, &g_diags);

    TEST_ASSERT_NOT_NULL(mod);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);

    IronHIR_Block *hir_body = mod->funcs[0]->body;
    /* Desugared: STMT_LET i = 0 + STMT_WHILE(i < 10, { ...; i = i + 1 }) */
    /* Expect at least 2 statements: the let and the while */
    TEST_ASSERT_TRUE(hir_body->stmt_count >= 2);

    /* First statement: LET (the loop variable init) */
    TEST_ASSERT_EQUAL_INT(IRON_HIR_STMT_LET, hir_body->stmts[0]->kind);

    /* Second statement: WHILE (the desugared loop) */
    TEST_ASSERT_EQUAL_INT(IRON_HIR_STMT_WHILE, hir_body->stmts[1]->kind);
    /* The while condition should be a binop (i < 10) */
    IronHIR_Stmt *ws = hir_body->stmts[1];
    TEST_ASSERT_EQUAL_INT(IRON_HIR_EXPR_BINOP, ws->while_loop.condition->kind);
    TEST_ASSERT_EQUAL_INT(IRON_HIR_BINOP_LT, ws->while_loop.condition->binop.op);

    iron_hir_module_destroy(mod);
}

/* ── Test 8: Return value (STMT_RETURN) ─────────────────────────────────── */

void test_hir_lower_return_value(void) {
    Iron_Type *int_ty  = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_Node *ret_val = make_int(&g_ast_arena, "42", int_ty);
    Iron_Node *ret     = make_return(&g_ast_arena, ret_val);

    Iron_Node *stmts[] = { ret };
    Iron_Block    *body = make_block(&g_ast_arena, stmts, 1);
    Iron_FuncDecl *fd   = make_func_decl(&g_ast_arena, "answer", body, int_ty);
    Iron_Node     *decls[] = { (Iron_Node *)fd };
    Iron_Program  *prog = make_program(&g_ast_arena, decls, 1);

    IronHIR_Module *mod = iron_hir_lower(prog, NULL, NULL, &g_diags);

    TEST_ASSERT_NOT_NULL(mod);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);

    IronHIR_Block *hir_body = mod->funcs[0]->body;
    TEST_ASSERT_EQUAL_INT(1, hir_body->stmt_count);

    IronHIR_Stmt *ret_stmt = hir_body->stmts[0];
    TEST_ASSERT_EQUAL_INT(IRON_HIR_STMT_RETURN, ret_stmt->kind);
    TEST_ASSERT_NOT_NULL(ret_stmt->return_stmt.value);

    iron_hir_module_destroy(mod);
}

/* ── Test 9: Binary expression (EXPR_BINOP ADD) ─────────────────────────── */

void test_hir_lower_binary_expr(void) {
    Iron_Type *int_ty  = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_Node *left    = make_int(&g_ast_arena, "1", int_ty);
    Iron_Node *right   = make_int(&g_ast_arena, "2", int_ty);
    Iron_Node *binop   = make_binary(&g_ast_arena, left, IRON_TOK_PLUS,
                                      right, int_ty);
    Iron_Node *ret     = make_return(&g_ast_arena, binop);

    Iron_Node *stmts[] = { ret };
    Iron_Block    *body = make_block(&g_ast_arena, stmts, 1);
    Iron_FuncDecl *fd   = make_func_decl(&g_ast_arena, "add", body, int_ty);
    Iron_Node     *decls[] = { (Iron_Node *)fd };
    Iron_Program  *prog = make_program(&g_ast_arena, decls, 1);

    IronHIR_Module *mod = iron_hir_lower(prog, NULL, NULL, &g_diags);

    TEST_ASSERT_NOT_NULL(mod);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);

    IronHIR_Block *hir_body = mod->funcs[0]->body;
    IronHIR_Stmt  *ret_stmt = hir_body->stmts[0];
    TEST_ASSERT_EQUAL_INT(IRON_HIR_STMT_RETURN, ret_stmt->kind);

    IronHIR_Expr *expr = ret_stmt->return_stmt.value;
    TEST_ASSERT_EQUAL_INT(IRON_HIR_EXPR_BINOP, expr->kind);
    TEST_ASSERT_EQUAL_INT(IRON_HIR_BINOP_ADD, expr->binop.op);

    iron_hir_module_destroy(mod);
}

/* ── Test 10: Function call (EXPR_CALL) ─────────────────────────────────── */

void test_hir_lower_call_expr(void) {
    Iron_Type *int_ty  = iron_type_make_primitive(IRON_TYPE_INT);
    /* callee: reference to "foo" function */
    Iron_Node *callee  = make_ident(&g_ast_arena, "foo", NULL);
    Iron_Node *arg     = make_int(&g_ast_arena, "5", int_ty);
    Iron_Node *args[]  = { arg };
    Iron_Node *call    = make_call(&g_ast_arena, callee, args, 1, int_ty);
    Iron_Node *ret     = make_return(&g_ast_arena, call);

    Iron_Node *stmts[] = { ret };
    Iron_Block    *body = make_block(&g_ast_arena, stmts, 1);
    Iron_FuncDecl *fd   = make_func_decl(&g_ast_arena, "main", body, int_ty);

    /* Also add foo as a func decl so it appears in module */
    Iron_Block    *foo_body = make_block(&g_ast_arena, NULL, 0);
    Iron_FuncDecl *foo_fd   = make_func_decl(&g_ast_arena, "foo", foo_body, int_ty);

    Iron_Node *decls[] = { (Iron_Node *)foo_fd, (Iron_Node *)fd };
    Iron_Program *prog = make_program(&g_ast_arena, decls, 2);

    IronHIR_Module *mod = iron_hir_lower(prog, NULL, NULL, &g_diags);

    TEST_ASSERT_NOT_NULL(mod);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);
    TEST_ASSERT_EQUAL_INT(2, mod->func_count);

    /* Find main func and check return has a call expression */
    IronHIR_Func *main_fn = NULL;
    for (int i = 0; i < mod->func_count; i++) {
        if (strcmp(mod->funcs[i]->name, "main") == 0) { main_fn = mod->funcs[i]; break; }
    }
    TEST_ASSERT_NOT_NULL(main_fn);
    TEST_ASSERT_EQUAL_INT(1, main_fn->body->stmt_count);

    IronHIR_Stmt *ret_stmt = main_fn->body->stmts[0];
    TEST_ASSERT_EQUAL_INT(IRON_HIR_STMT_RETURN, ret_stmt->kind);

    IronHIR_Expr *call_expr = ret_stmt->return_stmt.value;
    TEST_ASSERT_EQUAL_INT(IRON_HIR_EXPR_CALL, call_expr->kind);
    TEST_ASSERT_EQUAL_INT(1, call_expr->call.arg_count);

    iron_hir_module_destroy(mod);
}

/* ── Test 11: Integer literal (EXPR_INT_LIT) ─────────────────────────────── */

void test_hir_lower_int_literal(void) {
    Iron_Type *int_ty   = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_Node *lit_node = make_int(&g_ast_arena, "99", int_ty);
    Iron_Node *ret      = make_return(&g_ast_arena, lit_node);

    Iron_Node *stmts[] = { ret };
    Iron_Block    *body = make_block(&g_ast_arena, stmts, 1);
    Iron_FuncDecl *fd   = make_func_decl(&g_ast_arena, "n", body, int_ty);
    Iron_Node     *decls[] = { (Iron_Node *)fd };
    Iron_Program  *prog = make_program(&g_ast_arena, decls, 1);

    IronHIR_Module *mod = iron_hir_lower(prog, NULL, NULL, &g_diags);

    TEST_ASSERT_NOT_NULL(mod);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);

    IronHIR_Expr *val = mod->funcs[0]->body->stmts[0]->return_stmt.value;
    TEST_ASSERT_NOT_NULL(val);
    TEST_ASSERT_EQUAL_INT(IRON_HIR_EXPR_INT_LIT, val->kind);
    TEST_ASSERT_EQUAL_INT64(99, val->int_lit.value);

    iron_hir_module_destroy(mod);
}

/* ── Test 12: String literal (EXPR_STRING_LIT) ───────────────────────────── */

void test_hir_lower_string_literal(void) {
    Iron_Type *str_ty   = iron_type_make_primitive(IRON_TYPE_STRING);
    Iron_Node *lit_node = make_string_lit(&g_ast_arena, "hello");
    Iron_Node *ret      = make_return(&g_ast_arena, lit_node);

    Iron_Node *stmts[] = { ret };
    Iron_Block    *body = make_block(&g_ast_arena, stmts, 1);
    Iron_FuncDecl *fd   = make_func_decl(&g_ast_arena, "greet", body, str_ty);
    Iron_Node     *decls[] = { (Iron_Node *)fd };
    Iron_Program  *prog = make_program(&g_ast_arena, decls, 1);

    IronHIR_Module *mod = iron_hir_lower(prog, NULL, NULL, &g_diags);

    TEST_ASSERT_NOT_NULL(mod);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);

    IronHIR_Expr *val = mod->funcs[0]->body->stmts[0]->return_stmt.value;
    TEST_ASSERT_NOT_NULL(val);
    TEST_ASSERT_EQUAL_INT(IRON_HIR_EXPR_STRING_LIT, val->kind);
    TEST_ASSERT_EQUAL_STRING("hello", val->string_lit.value);

    iron_hir_module_destroy(mod);
}

/* ── Test 13: Elif desugaring (nested STMT_IF in else branch) ───────────── */

void test_hir_lower_elif_desugars(void) {
    /* if true { } elif true { } else { } */
    Iron_Arena *a = &g_ast_arena;

    Iron_Node *cond1  = make_bool_lit(a, true);
    Iron_Node *cond2  = make_bool_lit(a, false);

    Iron_IfStmt *if_node = ARENA_ALLOC(a, Iron_IfStmt);
    memset(if_node, 0, sizeof(*if_node));
    if_node->span = test_span();
    if_node->kind = IRON_NODE_IF;
    if_node->condition = cond1;
    if_node->body = (Iron_Node *)make_block(a, NULL, 0);

    /* elif_conds / elif_bodies: single elif */
    if_node->elif_count = 1;
    if_node->elif_conds = (Iron_Node **)iron_arena_alloc(a,
                               sizeof(Iron_Node *), _Alignof(Iron_Node *));
    if_node->elif_bodies = (Iron_Node **)iron_arena_alloc(a,
                               sizeof(Iron_Node *), _Alignof(Iron_Node *));
    if_node->elif_conds[0]  = cond2;
    if_node->elif_bodies[0] = (Iron_Node *)make_block(a, NULL, 0);

    /* else body */
    if_node->else_body = (Iron_Node *)make_block(a, NULL, 0);

    Iron_Node *stmts[] = { (Iron_Node *)if_node };
    Iron_Block    *body = make_block(a, stmts, 1);
    Iron_FuncDecl *fd   = make_func_decl(a, "main", body, NULL);
    Iron_Node     *decls[] = { (Iron_Node *)fd };
    Iron_Program  *prog = make_program(a, decls, 1);

    IronHIR_Module *mod = iron_hir_lower(prog, NULL, NULL, &g_diags);

    TEST_ASSERT_NOT_NULL(mod);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);

    IronHIR_Block *hir_body = mod->funcs[0]->body;
    TEST_ASSERT_EQUAL_INT(1, hir_body->stmt_count);

    IronHIR_Stmt *outer_if = hir_body->stmts[0];
    TEST_ASSERT_EQUAL_INT(IRON_HIR_STMT_IF, outer_if->kind);

    /* The else branch should be a block containing a nested if (the elif) */
    IronHIR_Block *else_blk = outer_if->if_else.else_body;
    TEST_ASSERT_NOT_NULL(else_blk);
    TEST_ASSERT_EQUAL_INT(1, else_blk->stmt_count);
    TEST_ASSERT_EQUAL_INT(IRON_HIR_STMT_IF, else_blk->stmts[0]->kind);

    iron_hir_module_destroy(mod);
}

/* ── Test 14: Multiple functions (func_count) ───────────────────────────── */

void test_hir_lower_multiple_funcs(void) {
    Iron_Type *int_ty = iron_type_make_primitive(IRON_TYPE_INT);

    Iron_Block    *body1 = make_block(&g_ast_arena, NULL, 0);
    Iron_FuncDecl *fd1   = make_func_decl(&g_ast_arena, "foo", body1, int_ty);

    Iron_Block    *body2 = make_block(&g_ast_arena, NULL, 0);
    Iron_FuncDecl *fd2   = make_func_decl(&g_ast_arena, "bar", body2, int_ty);

    Iron_Block    *body3 = make_block(&g_ast_arena, NULL, 0);
    Iron_FuncDecl *fd3   = make_func_decl(&g_ast_arena, "baz", body3, int_ty);

    Iron_Node *decls[] = {
        (Iron_Node *)fd1, (Iron_Node *)fd2, (Iron_Node *)fd3
    };
    Iron_Program *prog = make_program(&g_ast_arena, decls, 3);

    IronHIR_Module *mod = iron_hir_lower(prog, NULL, NULL, &g_diags);

    TEST_ASSERT_NOT_NULL(mod);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);
    TEST_ASSERT_EQUAL_INT(3, mod->func_count);

    iron_hir_module_destroy(mod);
}

/* ── Test 15: Output module passes iron_hir_verify ──────────────────────── */

void test_hir_lower_verify_passes(void) {
    /* Build a more complex program: func with val, var, return */
    Iron_Type *int_ty = iron_type_make_primitive(IRON_TYPE_INT);

    Iron_Node *lit42  = make_int(&g_ast_arena, "42", int_ty);
    Iron_Node *vd     = make_val_decl(&g_ast_arena, "answer", lit42, int_ty);
    Iron_Node *ident  = make_ident(&g_ast_arena, "answer", int_ty);
    Iron_Node *ret    = make_return(&g_ast_arena, ident);

    Iron_Node *stmts[] = { vd, ret };
    Iron_Block    *body = make_block(&g_ast_arena, stmts, 2);
    Iron_FuncDecl *fd   = make_func_decl(&g_ast_arena, "main", body, int_ty);
    Iron_Node     *decls[] = { (Iron_Node *)fd };
    Iron_Program  *prog = make_program(&g_ast_arena, decls, 1);

    IronHIR_Module *mod = iron_hir_lower(prog, NULL, NULL, &g_diags);

    /* iron_hir_lower already runs iron_hir_verify internally.
     * NULL result would indicate verify failure. */
    TEST_ASSERT_NOT_NULL(mod);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);

    /* Explicitly verify once more to confirm module is structurally valid */
    Iron_DiagList vdiags = iron_diaglist_create();
    Iron_Arena    varena = iron_arena_create(32 * 1024);
    bool ok = iron_hir_verify(mod, &vdiags, &varena);
    iron_arena_free(&varena);
    iron_diaglist_free(&vdiags);
    TEST_ASSERT_TRUE(ok);

    iron_hir_module_destroy(mod);
}

/* ══════════════════════════════════════════════════════════════════════════════
 * FEATURE-MATRIX TESTS (Tests 16-36)
 * ══════════════════════════════════════════════════════════════════════════════ */

/* ── Test 16: Float literal (EXPR_FLOAT_LIT) ─────────────────────────────── */

void test_hir_lower_float_literal(void) {
    Iron_FloatLit *n = ARENA_ALLOC(&g_ast_arena, Iron_FloatLit);
    memset(n, 0, sizeof(*n));
    n->span = test_span();
    n->kind = IRON_NODE_FLOAT_LIT;
    n->value = "3.14";
    n->resolved_type = iron_type_make_primitive(IRON_TYPE_FLOAT);

    Iron_Node *ret = make_return(&g_ast_arena, (Iron_Node *)n);
    Iron_Node *stmts[] = { ret };
    Iron_Block    *body = make_block(&g_ast_arena, stmts, 1);
    Iron_FuncDecl *fd   = make_func_decl(&g_ast_arena, "pi",
                                          body, iron_type_make_primitive(IRON_TYPE_FLOAT));
    Iron_Node *decls[] = { (Iron_Node *)fd };
    Iron_Program *prog = make_program(&g_ast_arena, decls, 1);

    IronHIR_Module *mod = iron_hir_lower(prog, NULL, NULL, &g_diags);
    TEST_ASSERT_NOT_NULL(mod);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);

    IronHIR_Expr *val = mod->funcs[0]->body->stmts[0]->return_stmt.value;
    TEST_ASSERT_EQUAL_INT(IRON_HIR_EXPR_FLOAT_LIT, val->kind);
    TEST_ASSERT_DOUBLE_WITHIN(1e-6, 3.14, val->float_lit.value);

    iron_hir_module_destroy(mod);
}

/* ── Test 17: Bool literal (EXPR_BOOL_LIT) ───────────────────────────────── */

void test_hir_lower_bool_literal(void) {
    Iron_Node *tval = make_bool_lit(&g_ast_arena, true);
    Iron_Node *fval = make_bool_lit(&g_ast_arena, false);
    Iron_Node *ret  = make_return(&g_ast_arena, tval);
    Iron_Node *stmts[] = { (Iron_Node *)fval, ret };
    Iron_Block    *body = make_block(&g_ast_arena, stmts, 2);
    Iron_FuncDecl *fd   = make_func_decl(&g_ast_arena, "bools", body, NULL);
    Iron_Node *decls[] = { (Iron_Node *)fd };
    Iron_Program *prog = make_program(&g_ast_arena, decls, 1);

    IronHIR_Module *mod = iron_hir_lower(prog, NULL, NULL, &g_diags);
    TEST_ASSERT_NOT_NULL(mod);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);

    IronHIR_Block *hb = mod->funcs[0]->body;
    /* first stmt is an expr-stmt (the false literal), second is return(true) */
    TEST_ASSERT_EQUAL_INT(IRON_HIR_STMT_EXPR, hb->stmts[0]->kind);
    IronHIR_Expr *fexpr = hb->stmts[0]->expr_stmt.expr;
    TEST_ASSERT_EQUAL_INT(IRON_HIR_EXPR_BOOL_LIT, fexpr->kind);
    TEST_ASSERT_FALSE(fexpr->bool_lit.value);

    IronHIR_Expr *texpr = hb->stmts[1]->return_stmt.value;
    TEST_ASSERT_EQUAL_INT(IRON_HIR_EXPR_BOOL_LIT, texpr->kind);
    TEST_ASSERT_TRUE(texpr->bool_lit.value);

    iron_hir_module_destroy(mod);
}

/* ── Test 18: Null literal (EXPR_NULL_LIT) ───────────────────────────────── */

void test_hir_lower_null_literal(void) {
    Iron_NullLit *n = ARENA_ALLOC(&g_ast_arena, Iron_NullLit);
    memset(n, 0, sizeof(*n));
    n->span = test_span();
    n->kind = IRON_NODE_NULL_LIT;
    n->resolved_type = NULL;

    Iron_Node *ret = make_return(&g_ast_arena, (Iron_Node *)n);
    Iron_Node *stmts[] = { ret };
    Iron_Block    *body = make_block(&g_ast_arena, stmts, 1);
    Iron_FuncDecl *fd   = make_func_decl(&g_ast_arena, "nullfn", body, NULL);
    Iron_Node *decls[] = { (Iron_Node *)fd };
    Iron_Program *prog = make_program(&g_ast_arena, decls, 1);

    IronHIR_Module *mod = iron_hir_lower(prog, NULL, NULL, &g_diags);
    TEST_ASSERT_NOT_NULL(mod);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);

    IronHIR_Expr *val = mod->funcs[0]->body->stmts[0]->return_stmt.value;
    TEST_ASSERT_EQUAL_INT(IRON_HIR_EXPR_NULL_LIT, val->kind);

    iron_hir_module_destroy(mod);
}

/* ── Test 19: Interpolated string (EXPR_INTERP_STRING) ───────────────────── */

void test_hir_lower_string_interp(void) {
    /* "hello " + ident(x) — model as INTERP_STRING with 2 parts */
    Iron_InterpString *is = ARENA_ALLOC(&g_ast_arena, Iron_InterpString);
    memset(is, 0, sizeof(*is));
    is->span = test_span();
    is->kind = IRON_NODE_INTERP_STRING;
    is->resolved_type = iron_type_make_primitive(IRON_TYPE_STRING);

    Iron_Node *part0 = make_string_lit(&g_ast_arena, "hello ");
    Iron_Node *part1 = make_ident(&g_ast_arena, "world",
                                   iron_type_make_primitive(IRON_TYPE_STRING));
    is->parts = (Iron_Node **)iron_arena_alloc(&g_ast_arena,
                    2 * sizeof(Iron_Node *), _Alignof(Iron_Node *));
    is->parts[0] = part0;
    is->parts[1] = part1;
    is->part_count = 2;

    /* Declare "world" in a val so the ident resolves */
    Iron_Node *world_val = make_val_decl(&g_ast_arena, "world",
                                          make_string_lit(&g_ast_arena, "world"),
                                          iron_type_make_primitive(IRON_TYPE_STRING));
    Iron_Node *ret = make_return(&g_ast_arena, (Iron_Node *)is);
    Iron_Node *stmts[] = { world_val, ret };
    Iron_Block    *body = make_block(&g_ast_arena, stmts, 2);
    Iron_FuncDecl *fd   = make_func_decl(&g_ast_arena, "greet", body,
                                          iron_type_make_primitive(IRON_TYPE_STRING));
    Iron_Node *decls[] = { (Iron_Node *)fd };
    Iron_Program *prog = make_program(&g_ast_arena, decls, 1);

    IronHIR_Module *mod = iron_hir_lower(prog, NULL, NULL, &g_diags);
    TEST_ASSERT_NOT_NULL(mod);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);

    /* Second stmt should be return(interp_string) */
    IronHIR_Block *hb = mod->funcs[0]->body;
    TEST_ASSERT_TRUE(hb->stmt_count >= 2);
    IronHIR_Expr *ret_val = hb->stmts[hb->stmt_count - 1]->return_stmt.value;
    TEST_ASSERT_EQUAL_INT(IRON_HIR_EXPR_INTERP_STRING, ret_val->kind);
    TEST_ASSERT_EQUAL_INT(2, ret_val->interp_string.part_count);

    iron_hir_module_destroy(mod);
}

/* ── Test 20: Unary negation (EXPR_UNOP NEG) ─────────────────────────────── */

void test_hir_lower_unary_neg(void) {
    Iron_Type *int_ty = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_Node *operand = make_int(&g_ast_arena, "5", int_ty);
    Iron_UnaryExpr *un = ARENA_ALLOC(&g_ast_arena, Iron_UnaryExpr);
    memset(un, 0, sizeof(*un));
    un->span = test_span();
    un->kind = IRON_NODE_UNARY;
    un->op   = IRON_TOK_MINUS;
    un->operand = operand;
    un->resolved_type = int_ty;

    Iron_Node *ret = make_return(&g_ast_arena, (Iron_Node *)un);
    Iron_Node *stmts[] = { ret };
    Iron_Block    *body = make_block(&g_ast_arena, stmts, 1);
    Iron_FuncDecl *fd   = make_func_decl(&g_ast_arena, "neg", body, int_ty);
    Iron_Node *decls[] = { (Iron_Node *)fd };
    Iron_Program *prog = make_program(&g_ast_arena, decls, 1);

    IronHIR_Module *mod = iron_hir_lower(prog, NULL, NULL, &g_diags);
    TEST_ASSERT_NOT_NULL(mod);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);

    IronHIR_Expr *val = mod->funcs[0]->body->stmts[0]->return_stmt.value;
    TEST_ASSERT_EQUAL_INT(IRON_HIR_EXPR_UNOP, val->kind);
    TEST_ASSERT_EQUAL_INT(IRON_HIR_UNOP_NEG, val->unop.op);
    TEST_ASSERT_NOT_NULL(val->unop.operand);

    iron_hir_module_destroy(mod);
}

/* ── Test 21: Unary not (EXPR_UNOP NOT) ──────────────────────────────────── */

void test_hir_lower_unary_not(void) {
    Iron_Type *bool_ty = iron_type_make_primitive(IRON_TYPE_BOOL);
    Iron_Node *operand = make_bool_lit(&g_ast_arena, true);
    Iron_UnaryExpr *un = ARENA_ALLOC(&g_ast_arena, Iron_UnaryExpr);
    memset(un, 0, sizeof(*un));
    un->span = test_span();
    un->kind = IRON_NODE_UNARY;
    un->op   = IRON_TOK_NOT;
    un->operand = operand;
    un->resolved_type = bool_ty;

    Iron_Node *ret = make_return(&g_ast_arena, (Iron_Node *)un);
    Iron_Node *stmts[] = { ret };
    Iron_Block    *body = make_block(&g_ast_arena, stmts, 1);
    Iron_FuncDecl *fd   = make_func_decl(&g_ast_arena, "notfn", body, bool_ty);
    Iron_Node *decls[] = { (Iron_Node *)fd };
    Iron_Program *prog = make_program(&g_ast_arena, decls, 1);

    IronHIR_Module *mod = iron_hir_lower(prog, NULL, NULL, &g_diags);
    TEST_ASSERT_NOT_NULL(mod);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);

    IronHIR_Expr *val = mod->funcs[0]->body->stmts[0]->return_stmt.value;
    TEST_ASSERT_EQUAL_INT(IRON_HIR_EXPR_UNOP, val->kind);
    TEST_ASSERT_EQUAL_INT(IRON_HIR_UNOP_NOT, val->unop.op);

    iron_hir_module_destroy(mod);
}

/* ── Test 22: All binary ops map correctly ───────────────────────────────── */

void test_hir_lower_binary_all_ops(void) {
    /* Table of (ast_op, expected_hir_op) pairs */
    static const struct { int ast_op; IronHIR_BinOp hir_op; } ops[] = {
        { IRON_TOK_PLUS,        IRON_HIR_BINOP_ADD },
        { IRON_TOK_MINUS,       IRON_HIR_BINOP_SUB },
        { IRON_TOK_STAR,        IRON_HIR_BINOP_MUL },
        { IRON_TOK_SLASH,       IRON_HIR_BINOP_DIV },
        { IRON_TOK_PERCENT,     IRON_HIR_BINOP_MOD },
        { IRON_TOK_EQUALS,      IRON_HIR_BINOP_EQ  },
        { IRON_TOK_NOT_EQUALS,  IRON_HIR_BINOP_NEQ },
        { IRON_TOK_LESS,        IRON_HIR_BINOP_LT  },
        { IRON_TOK_LESS_EQ,     IRON_HIR_BINOP_LTE },
        { IRON_TOK_GREATER,     IRON_HIR_BINOP_GT  },
        { IRON_TOK_GREATER_EQ,  IRON_HIR_BINOP_GTE },
        { IRON_TOK_AND,         IRON_HIR_BINOP_AND },
        { IRON_TOK_OR,          IRON_HIR_BINOP_OR  },
    };
    int n = (int)(sizeof(ops) / sizeof(ops[0]));

    Iron_Type *int_ty  = iron_type_make_primitive(IRON_TYPE_INT);

    for (int i = 0; i < n; i++) {
        Iron_Node *lhs  = make_int(&g_ast_arena, "1", int_ty);
        Iron_Node *rhs  = make_int(&g_ast_arena, "2", int_ty);
        Iron_Node *bin  = make_binary(&g_ast_arena, lhs, ops[i].ast_op, rhs, int_ty);
        Iron_Node *ret  = make_return(&g_ast_arena, bin);
        Iron_Node *stmts[] = { ret };
        Iron_Block    *body = make_block(&g_ast_arena, stmts, 1);
        Iron_FuncDecl *fd   = make_func_decl(&g_ast_arena, "f", body, int_ty);
        Iron_Node *decls[] = { (Iron_Node *)fd };
        Iron_Program *prog = make_program(&g_ast_arena, decls, 1);

        IronHIR_Module *mod = iron_hir_lower(prog, NULL, NULL, &g_diags);
        TEST_ASSERT_NOT_NULL(mod);
        TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);

        IronHIR_Expr *val = mod->funcs[0]->body->stmts[0]->return_stmt.value;
        TEST_ASSERT_EQUAL_INT(IRON_HIR_EXPR_BINOP, val->kind);
        TEST_ASSERT_EQUAL_INT(ops[i].hir_op, val->binop.op);

        iron_hir_module_destroy(mod);
    }
}

/* ── Test 23: For-range (integer) desugars to STMT_WHILE ─────────────────── */

void test_hir_lower_for_range_int_desugars(void) {
    /* for i in 10  — single int literal; desugars to 0..n while */
    Iron_Type *int_ty = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_Node *bound  = make_int(&g_ast_arena, "10", int_ty);
    Iron_Node *for_node = make_for_range(&g_ast_arena, "i", bound);

    Iron_Node *stmts[] = { for_node };
    Iron_Block    *body = make_block(&g_ast_arena, stmts, 1);
    Iron_FuncDecl *fd   = make_func_decl(&g_ast_arena, "count", body, NULL);
    Iron_Node *decls[] = { (Iron_Node *)fd };
    Iron_Program *prog = make_program(&g_ast_arena, decls, 1);

    IronHIR_Module *mod = iron_hir_lower(prog, NULL, NULL, &g_diags);
    TEST_ASSERT_NOT_NULL(mod);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);

    IronHIR_Block *hb = mod->funcs[0]->body;
    TEST_ASSERT_TRUE(hb->stmt_count >= 2);
    /* First: mutable LET for loop var */
    TEST_ASSERT_EQUAL_INT(IRON_HIR_STMT_LET, hb->stmts[0]->kind);
    TEST_ASSERT_TRUE(hb->stmts[0]->let.is_mutable);
    /* Second: WHILE */
    TEST_ASSERT_EQUAL_INT(IRON_HIR_STMT_WHILE, hb->stmts[1]->kind);

    iron_hir_module_destroy(mod);
}

/* ── Test 24: For-array-iter stays as STMT_FOR ───────────────────────────── */

void test_hir_lower_for_array_iter(void) {
    /* for x in arr — iterable is string type (array-like), no range desugaring */
    Iron_Type *str_ty  = iron_type_make_primitive(IRON_TYPE_STRING);
    Iron_Node *arr_id  = make_ident(&g_ast_arena, "arr", str_ty);

    /* First declare arr */
    Iron_Node *arr_decl = make_val_decl(&g_ast_arena, "arr",
                                         make_string_lit(&g_ast_arena, "hello"),
                                         str_ty);

    Iron_ForStmt *fs = ARENA_ALLOC(&g_ast_arena, Iron_ForStmt);
    memset(fs, 0, sizeof(*fs));
    fs->span      = test_span();
    fs->kind      = IRON_NODE_FOR;
    fs->var_name  = "x";
    fs->iterable  = arr_id;
    fs->body      = (Iron_Node *)make_block(&g_ast_arena, NULL, 0);
    fs->is_parallel = false;

    Iron_Node *stmts[] = { arr_decl, (Iron_Node *)fs };
    Iron_Block    *body = make_block(&g_ast_arena, stmts, 2);
    Iron_FuncDecl *fd   = make_func_decl(&g_ast_arena, "iterate", body, NULL);
    Iron_Node *decls[] = { (Iron_Node *)fd };
    Iron_Program *prog = make_program(&g_ast_arena, decls, 1);

    IronHIR_Module *mod = iron_hir_lower(prog, NULL, NULL, &g_diags);
    TEST_ASSERT_NOT_NULL(mod);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);

    IronHIR_Block *hb = mod->funcs[0]->body;
    /* Last stmt should be STMT_FOR (no desugaring for non-integer iterable) */
    IronHIR_Stmt *last = hb->stmts[hb->stmt_count - 1];
    TEST_ASSERT_EQUAL_INT(IRON_HIR_STMT_FOR, last->kind);
    TEST_ASSERT_NOT_NULL(last->for_loop.iterable);

    iron_hir_module_destroy(mod);
}

/* ── Test 25: Compound assign += desugars to binop+assign ────────────────── */

void test_hir_lower_compound_assign_add(void) {
    Iron_Type *int_ty = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_Node *init   = make_int(&g_ast_arena, "5", int_ty);
    Iron_Node *vd     = make_var_decl(&g_ast_arena, "x", init, int_ty);

    /* x += 3 */
    Iron_AssignStmt *as = ARENA_ALLOC(&g_ast_arena, Iron_AssignStmt);
    memset(as, 0, sizeof(*as));
    as->span   = test_span();
    as->kind   = IRON_NODE_ASSIGN;
    as->target = make_ident(&g_ast_arena, "x", int_ty);
    as->value  = make_int(&g_ast_arena, "3", int_ty);
    as->op     = IRON_TOK_PLUS_ASSIGN;

    Iron_Node *stmts[] = { vd, (Iron_Node *)as };
    Iron_Block    *body = make_block(&g_ast_arena, stmts, 2);
    Iron_FuncDecl *fd   = make_func_decl(&g_ast_arena, "incr", body, NULL);
    Iron_Node *decls[] = { (Iron_Node *)fd };
    Iron_Program *prog = make_program(&g_ast_arena, decls, 1);

    IronHIR_Module *mod = iron_hir_lower(prog, NULL, NULL, &g_diags);
    TEST_ASSERT_NOT_NULL(mod);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);

    IronHIR_Block *hb = mod->funcs[0]->body;
    /* stmts: LET x=5, ASSIGN x = BINOP(ADD, x, 3) */
    TEST_ASSERT_TRUE(hb->stmt_count >= 2);
    IronHIR_Stmt *assign = hb->stmts[1];
    TEST_ASSERT_EQUAL_INT(IRON_HIR_STMT_ASSIGN, assign->kind);
    /* value should be a BINOP ADD */
    TEST_ASSERT_EQUAL_INT(IRON_HIR_EXPR_BINOP, assign->assign.value->kind);
    TEST_ASSERT_EQUAL_INT(IRON_HIR_BINOP_ADD, assign->assign.value->binop.op);

    iron_hir_module_destroy(mod);
}

/* ── Test 26: Compound assign -= desugars to binop+assign ────────────────── */

void test_hir_lower_compound_assign_sub(void) {
    Iron_Type *int_ty = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_Node *init   = make_int(&g_ast_arena, "10", int_ty);
    Iron_Node *vd     = make_var_decl(&g_ast_arena, "n", init, int_ty);

    /* n -= 1 */
    Iron_AssignStmt *as = ARENA_ALLOC(&g_ast_arena, Iron_AssignStmt);
    memset(as, 0, sizeof(*as));
    as->span   = test_span();
    as->kind   = IRON_NODE_ASSIGN;
    as->target = make_ident(&g_ast_arena, "n", int_ty);
    as->value  = make_int(&g_ast_arena, "1", int_ty);
    as->op     = IRON_TOK_MINUS_ASSIGN;

    Iron_Node *stmts[] = { vd, (Iron_Node *)as };
    Iron_Block    *body = make_block(&g_ast_arena, stmts, 2);
    Iron_FuncDecl *fd   = make_func_decl(&g_ast_arena, "decr", body, NULL);
    Iron_Node *decls[] = { (Iron_Node *)fd };
    Iron_Program *prog = make_program(&g_ast_arena, decls, 1);

    IronHIR_Module *mod = iron_hir_lower(prog, NULL, NULL, &g_diags);
    TEST_ASSERT_NOT_NULL(mod);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);

    IronHIR_Block *hb = mod->funcs[0]->body;
    TEST_ASSERT_TRUE(hb->stmt_count >= 2);
    IronHIR_Stmt *assign = hb->stmts[1];
    TEST_ASSERT_EQUAL_INT(IRON_HIR_STMT_ASSIGN, assign->kind);
    TEST_ASSERT_EQUAL_INT(IRON_HIR_EXPR_BINOP, assign->assign.value->kind);
    TEST_ASSERT_EQUAL_INT(IRON_HIR_BINOP_SUB, assign->assign.value->binop.op);

    iron_hir_module_destroy(mod);
}

/* ── Test 27: Match statement (STMT_MATCH with 3 arms) ───────────────────── */

void test_hir_lower_match_stmt(void) {
    Iron_Type *int_ty = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_Node *subj   = make_int(&g_ast_arena, "42", int_ty);

    /* Build 3 match cases */
    Iron_MatchCase *c0 = ARENA_ALLOC(&g_ast_arena, Iron_MatchCase);
    memset(c0, 0, sizeof(*c0));
    c0->span = test_span();
    c0->kind = IRON_NODE_MATCH_CASE;
    c0->pattern = make_int(&g_ast_arena, "1", int_ty);
    c0->body    = (Iron_Node *)make_block(&g_ast_arena, NULL, 0);

    Iron_MatchCase *c1 = ARENA_ALLOC(&g_ast_arena, Iron_MatchCase);
    memset(c1, 0, sizeof(*c1));
    c1->span = test_span();
    c1->kind = IRON_NODE_MATCH_CASE;
    c1->pattern = make_int(&g_ast_arena, "2", int_ty);
    c1->body    = (Iron_Node *)make_block(&g_ast_arena, NULL, 0);

    Iron_MatchCase *c2 = ARENA_ALLOC(&g_ast_arena, Iron_MatchCase);
    memset(c2, 0, sizeof(*c2));
    c2->span = test_span();
    c2->kind = IRON_NODE_MATCH_CASE;
    c2->pattern = make_int(&g_ast_arena, "3", int_ty);
    c2->body    = (Iron_Node *)make_block(&g_ast_arena, NULL, 0);

    Iron_MatchStmt *ms = ARENA_ALLOC(&g_ast_arena, Iron_MatchStmt);
    memset(ms, 0, sizeof(*ms));
    ms->span = test_span();
    ms->kind = IRON_NODE_MATCH;
    ms->subject = subj;
    ms->cases = (Iron_Node **)iron_arena_alloc(&g_ast_arena,
                    3 * sizeof(Iron_Node *), _Alignof(Iron_Node *));
    ms->cases[0] = (Iron_Node *)c0;
    ms->cases[1] = (Iron_Node *)c1;
    ms->cases[2] = (Iron_Node *)c2;
    ms->case_count = 3;
    ms->else_body  = NULL;

    Iron_Node *stmts[] = { (Iron_Node *)ms };
    Iron_Block    *body = make_block(&g_ast_arena, stmts, 1);
    Iron_FuncDecl *fd   = make_func_decl(&g_ast_arena, "matchme", body, NULL);
    Iron_Node *decls[] = { (Iron_Node *)fd };
    Iron_Program *prog = make_program(&g_ast_arena, decls, 1);

    IronHIR_Module *mod = iron_hir_lower(prog, NULL, NULL, &g_diags);
    TEST_ASSERT_NOT_NULL(mod);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);

    IronHIR_Stmt *m = mod->funcs[0]->body->stmts[0];
    TEST_ASSERT_EQUAL_INT(IRON_HIR_STMT_MATCH, m->kind);
    TEST_ASSERT_EQUAL_INT(3, m->match_stmt.arm_count);
    TEST_ASSERT_NOT_NULL(m->match_stmt.scrutinee);

    iron_hir_module_destroy(mod);
}

/* ── Test 28: Heap allocation (EXPR_HEAP) ────────────────────────────────── */

void test_hir_lower_heap_alloc(void) {
    Iron_Type *int_ty = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_Node *inner  = make_int(&g_ast_arena, "1", int_ty);

    Iron_HeapExpr *he = ARENA_ALLOC(&g_ast_arena, Iron_HeapExpr);
    memset(he, 0, sizeof(*he));
    he->span = test_span();
    he->kind = IRON_NODE_HEAP;
    he->inner = inner;
    he->auto_free = true;
    he->escapes   = false;
    he->resolved_type = int_ty;

    Iron_Node *vd = make_val_decl(&g_ast_arena, "p", (Iron_Node *)he, int_ty);
    Iron_Node *stmts[] = { vd };
    Iron_Block    *body = make_block(&g_ast_arena, stmts, 1);
    Iron_FuncDecl *fd   = make_func_decl(&g_ast_arena, "alloc_test", body, NULL);
    Iron_Node *decls[] = { (Iron_Node *)fd };
    Iron_Program *prog = make_program(&g_ast_arena, decls, 1);

    IronHIR_Module *mod = iron_hir_lower(prog, NULL, NULL, &g_diags);
    TEST_ASSERT_NOT_NULL(mod);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);

    IronHIR_Stmt *let = mod->funcs[0]->body->stmts[0];
    TEST_ASSERT_EQUAL_INT(IRON_HIR_STMT_LET, let->kind);
    TEST_ASSERT_EQUAL_INT(IRON_HIR_EXPR_HEAP, let->let.init->kind);
    TEST_ASSERT_TRUE(let->let.init->heap.auto_free);

    iron_hir_module_destroy(mod);
}

/* ── Test 29: RC allocation (EXPR_RC) ────────────────────────────────────── */

void test_hir_lower_rc_alloc(void) {
    Iron_Type *int_ty = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_Node *inner  = make_int(&g_ast_arena, "42", int_ty);

    Iron_RcExpr *rc = ARENA_ALLOC(&g_ast_arena, Iron_RcExpr);
    memset(rc, 0, sizeof(*rc));
    rc->span = test_span();
    rc->kind = IRON_NODE_RC;
    rc->inner = inner;
    rc->resolved_type = int_ty;

    Iron_Node *vd = make_val_decl(&g_ast_arena, "r", (Iron_Node *)rc, int_ty);
    Iron_Node *stmts[] = { vd };
    Iron_Block    *body = make_block(&g_ast_arena, stmts, 1);
    Iron_FuncDecl *fd   = make_func_decl(&g_ast_arena, "rc_test", body, NULL);
    Iron_Node *decls[] = { (Iron_Node *)fd };
    Iron_Program *prog = make_program(&g_ast_arena, decls, 1);

    IronHIR_Module *mod = iron_hir_lower(prog, NULL, NULL, &g_diags);
    TEST_ASSERT_NOT_NULL(mod);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);

    IronHIR_Stmt *let = mod->funcs[0]->body->stmts[0];
    TEST_ASSERT_EQUAL_INT(IRON_HIR_STMT_LET, let->kind);
    TEST_ASSERT_EQUAL_INT(IRON_HIR_EXPR_RC, let->let.init->kind);
    TEST_ASSERT_NOT_NULL(let->let.init->rc.inner);

    iron_hir_module_destroy(mod);
}

/* ── Test 30: Free statement (STMT_FREE) ─────────────────────────────────── */

void test_hir_lower_free_stmt(void) {
    Iron_Type *int_ty = iron_type_make_primitive(IRON_TYPE_INT);
    /* val p = ... ; free(p) */
    Iron_Node *init = make_int(&g_ast_arena, "0", int_ty);
    Iron_Node *vd   = make_val_decl(&g_ast_arena, "p", init, int_ty);

    Iron_FreeStmt *fs = ARENA_ALLOC(&g_ast_arena, Iron_FreeStmt);
    memset(fs, 0, sizeof(*fs));
    fs->span = test_span();
    fs->kind = IRON_NODE_FREE;
    fs->expr = make_ident(&g_ast_arena, "p", int_ty);

    Iron_Node *stmts[] = { vd, (Iron_Node *)fs };
    Iron_Block    *body = make_block(&g_ast_arena, stmts, 2);
    Iron_FuncDecl *fd   = make_func_decl(&g_ast_arena, "free_test", body, NULL);
    Iron_Node *decls[] = { (Iron_Node *)fd };
    Iron_Program *prog = make_program(&g_ast_arena, decls, 1);

    IronHIR_Module *mod = iron_hir_lower(prog, NULL, NULL, &g_diags);
    TEST_ASSERT_NOT_NULL(mod);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);

    IronHIR_Block *hb = mod->funcs[0]->body;
    /* Last stmt is FREE */
    IronHIR_Stmt *free_stmt = hb->stmts[hb->stmt_count - 1];
    TEST_ASSERT_EQUAL_INT(IRON_HIR_STMT_FREE, free_stmt->kind);
    TEST_ASSERT_NOT_NULL(free_stmt->free_stmt.value);

    iron_hir_module_destroy(mod);
}

/* ── Test 31: Leak statement (STMT_LEAK) ─────────────────────────────────── */

void test_hir_lower_leak_stmt(void) {
    Iron_Type *int_ty = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_Node *vd = make_val_decl(&g_ast_arena, "q",
                                   make_int(&g_ast_arena, "7", int_ty), int_ty);

    Iron_LeakStmt *ls = ARENA_ALLOC(&g_ast_arena, Iron_LeakStmt);
    memset(ls, 0, sizeof(*ls));
    ls->span = test_span();
    ls->kind = IRON_NODE_LEAK;
    ls->expr = make_ident(&g_ast_arena, "q", int_ty);

    Iron_Node *stmts[] = { vd, (Iron_Node *)ls };
    Iron_Block    *body = make_block(&g_ast_arena, stmts, 2);
    Iron_FuncDecl *fd   = make_func_decl(&g_ast_arena, "leak_test", body, NULL);
    Iron_Node *decls[] = { (Iron_Node *)fd };
    Iron_Program *prog = make_program(&g_ast_arena, decls, 1);

    IronHIR_Module *mod = iron_hir_lower(prog, NULL, NULL, &g_diags);
    TEST_ASSERT_NOT_NULL(mod);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);

    IronHIR_Block *hb = mod->funcs[0]->body;
    IronHIR_Stmt *leak_stmt = hb->stmts[hb->stmt_count - 1];
    TEST_ASSERT_EQUAL_INT(IRON_HIR_STMT_LEAK, leak_stmt->kind);
    TEST_ASSERT_NOT_NULL(leak_stmt->leak.value);

    iron_hir_module_destroy(mod);
}

/* ── Test 32: Call with args (EXPR_CALL, 2 args) ─────────────────────────── */

void test_hir_lower_call_with_args(void) {
    Iron_Type *int_ty = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_Node *callee = make_ident(&g_ast_arena, "add", NULL);
    Iron_Node *a1     = make_int(&g_ast_arena, "3", int_ty);
    Iron_Node *a2     = make_int(&g_ast_arena, "7", int_ty);
    Iron_Node *args[] = { a1, a2 };
    Iron_Node *call   = make_call(&g_ast_arena, callee, args, 2, int_ty);
    Iron_Node *ret    = make_return(&g_ast_arena, call);

    /* add func stub */
    Iron_Block    *add_body = make_block(&g_ast_arena, NULL, 0);
    Iron_FuncDecl *add_fd   = make_func_decl(&g_ast_arena, "add", add_body, int_ty);

    Iron_Node *stmts[] = { ret };
    Iron_Block    *body = make_block(&g_ast_arena, stmts, 1);
    Iron_FuncDecl *fd   = make_func_decl(&g_ast_arena, "main", body, int_ty);

    Iron_Node *decls[] = { (Iron_Node *)add_fd, (Iron_Node *)fd };
    Iron_Program *prog = make_program(&g_ast_arena, decls, 2);

    IronHIR_Module *mod = iron_hir_lower(prog, NULL, NULL, &g_diags);
    TEST_ASSERT_NOT_NULL(mod);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);

    /* Find main func */
    IronHIR_Func *main_fn = NULL;
    for (int i = 0; i < mod->func_count; i++) {
        if (strcmp(mod->funcs[i]->name, "main") == 0) { main_fn = mod->funcs[i]; break; }
    }
    TEST_ASSERT_NOT_NULL(main_fn);
    IronHIR_Expr *call_expr = main_fn->body->stmts[0]->return_stmt.value;
    TEST_ASSERT_EQUAL_INT(IRON_HIR_EXPR_CALL, call_expr->kind);
    TEST_ASSERT_EQUAL_INT(2, call_expr->call.arg_count);

    iron_hir_module_destroy(mod);
}

/* ── Test 33: Method call (EXPR_METHOD_CALL) ──────────────────────────────── */

void test_hir_lower_method_call(void) {
    Iron_Type *int_ty = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_Type *str_ty = iron_type_make_primitive(IRON_TYPE_STRING);
    Iron_Node *obj    = make_string_lit(&g_ast_arena, "hello");
    Iron_Node *arg    = make_int(&g_ast_arena, "0", int_ty);

    Iron_MethodCallExpr *mc = ARENA_ALLOC(&g_ast_arena, Iron_MethodCallExpr);
    memset(mc, 0, sizeof(*mc));
    mc->span = test_span();
    mc->kind = IRON_NODE_METHOD_CALL;
    mc->object = obj;
    mc->method = "charAt";
    mc->args = (Iron_Node **)iron_arena_alloc(&g_ast_arena,
                    sizeof(Iron_Node *), _Alignof(Iron_Node *));
    mc->args[0] = arg;
    mc->arg_count = 1;
    mc->resolved_type = str_ty;

    Iron_Node *ret = make_return(&g_ast_arena, (Iron_Node *)mc);
    Iron_Node *stmts[] = { ret };
    Iron_Block    *body = make_block(&g_ast_arena, stmts, 1);
    Iron_FuncDecl *fd   = make_func_decl(&g_ast_arena, "mtest", body, str_ty);
    Iron_Node *decls[] = { (Iron_Node *)fd };
    Iron_Program *prog = make_program(&g_ast_arena, decls, 1);

    IronHIR_Module *mod = iron_hir_lower(prog, NULL, NULL, &g_diags);
    TEST_ASSERT_NOT_NULL(mod);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);

    IronHIR_Expr *val = mod->funcs[0]->body->stmts[0]->return_stmt.value;
    TEST_ASSERT_EQUAL_INT(IRON_HIR_EXPR_METHOD_CALL, val->kind);
    TEST_ASSERT_EQUAL_STRING("charAt", val->method_call.method);
    TEST_ASSERT_EQUAL_INT(1, val->method_call.arg_count);

    iron_hir_module_destroy(mod);
}

/* ── Test 34: Field access (EXPR_FIELD_ACCESS) ───────────────────────────── */

void test_hir_lower_field_access(void) {
    Iron_Type *int_ty = iron_type_make_primitive(IRON_TYPE_INT);
    /* val obj = ...; return obj.x */
    Iron_Node *init = make_int(&g_ast_arena, "0", int_ty);
    Iron_Node *vd   = make_val_decl(&g_ast_arena, "obj", init, int_ty);

    Iron_FieldAccess *fa = ARENA_ALLOC(&g_ast_arena, Iron_FieldAccess);
    memset(fa, 0, sizeof(*fa));
    fa->span = test_span();
    fa->kind = IRON_NODE_FIELD_ACCESS;
    fa->object = make_ident(&g_ast_arena, "obj", int_ty);
    fa->field  = "x";
    fa->resolved_type = int_ty;

    Iron_Node *ret = make_return(&g_ast_arena, (Iron_Node *)fa);
    Iron_Node *stmts[] = { vd, ret };
    Iron_Block    *body = make_block(&g_ast_arena, stmts, 2);
    Iron_FuncDecl *fd   = make_func_decl(&g_ast_arena, "fa_test", body, int_ty);
    Iron_Node *decls[] = { (Iron_Node *)fd };
    Iron_Program *prog = make_program(&g_ast_arena, decls, 1);

    IronHIR_Module *mod = iron_hir_lower(prog, NULL, NULL, &g_diags);
    TEST_ASSERT_NOT_NULL(mod);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);

    IronHIR_Block *hb = mod->funcs[0]->body;
    IronHIR_Expr *val = hb->stmts[hb->stmt_count - 1]->return_stmt.value;
    TEST_ASSERT_EQUAL_INT(IRON_HIR_EXPR_FIELD_ACCESS, val->kind);
    TEST_ASSERT_EQUAL_STRING("x", val->field_access.field);

    iron_hir_module_destroy(mod);
}

/* ── Test 35: Index access (EXPR_INDEX) ──────────────────────────────────── */

void test_hir_lower_index_access(void) {
    Iron_Type *int_ty = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_Node *init   = make_int(&g_ast_arena, "0", int_ty);
    Iron_Node *vd     = make_val_decl(&g_ast_arena, "arr", init, int_ty);

    Iron_IndexExpr *ix = ARENA_ALLOC(&g_ast_arena, Iron_IndexExpr);
    memset(ix, 0, sizeof(*ix));
    ix->span = test_span();
    ix->kind = IRON_NODE_INDEX;
    ix->object = make_ident(&g_ast_arena, "arr", int_ty);
    ix->index  = make_int(&g_ast_arena, "2", int_ty);
    ix->resolved_type = int_ty;

    Iron_Node *ret = make_return(&g_ast_arena, (Iron_Node *)ix);
    Iron_Node *stmts[] = { vd, ret };
    Iron_Block    *body = make_block(&g_ast_arena, stmts, 2);
    Iron_FuncDecl *fd   = make_func_decl(&g_ast_arena, "idx_test", body, int_ty);
    Iron_Node *decls[] = { (Iron_Node *)fd };
    Iron_Program *prog = make_program(&g_ast_arena, decls, 1);

    IronHIR_Module *mod = iron_hir_lower(prog, NULL, NULL, &g_diags);
    TEST_ASSERT_NOT_NULL(mod);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);

    IronHIR_Block *hb = mod->funcs[0]->body;
    IronHIR_Expr *val = hb->stmts[hb->stmt_count - 1]->return_stmt.value;
    TEST_ASSERT_EQUAL_INT(IRON_HIR_EXPR_INDEX, val->kind);
    TEST_ASSERT_NOT_NULL(val->index.array);
    TEST_ASSERT_NOT_NULL(val->index.index);

    iron_hir_module_destroy(mod);
}

/* ── Test 36: Cast expression (EXPR_CAST) ────────────────────────────────── */

void test_hir_lower_cast(void) {
    /* Float(x) — is_primitive_cast=true → EXPR_CAST */
    Iron_Type *int_ty   = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_Type *float_ty = iron_type_make_primitive(IRON_TYPE_FLOAT);
    Iron_Node *arg = make_int(&g_ast_arena, "3", int_ty);

    /* Need ident callee named "Float" */
    Iron_Node *callee = make_ident(&g_ast_arena, "Float", NULL);

    Iron_CallExpr *ce = ARENA_ALLOC(&g_ast_arena, Iron_CallExpr);
    memset(ce, 0, sizeof(*ce));
    ce->span = test_span();
    ce->kind = IRON_NODE_CALL;
    ce->callee = callee;
    ce->args = (Iron_Node **)iron_arena_alloc(&g_ast_arena,
                    sizeof(Iron_Node *), _Alignof(Iron_Node *));
    ce->args[0] = arg;
    ce->arg_count = 1;
    ce->is_primitive_cast = true;
    ce->resolved_type = float_ty;

    Iron_Node *ret = make_return(&g_ast_arena, (Iron_Node *)ce);
    Iron_Node *stmts[] = { ret };
    Iron_Block    *body = make_block(&g_ast_arena, stmts, 1);
    Iron_FuncDecl *fd   = make_func_decl(&g_ast_arena, "cast_test", body, float_ty);
    Iron_Node *decls[] = { (Iron_Node *)fd };
    Iron_Program *prog = make_program(&g_ast_arena, decls, 1);

    IronHIR_Module *mod = iron_hir_lower(prog, NULL, NULL, &g_diags);
    TEST_ASSERT_NOT_NULL(mod);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);

    IronHIR_Expr *val = mod->funcs[0]->body->stmts[0]->return_stmt.value;
    TEST_ASSERT_EQUAL_INT(IRON_HIR_EXPR_CAST, val->kind);
    TEST_ASSERT_NOT_NULL(val->cast.value);
    TEST_ASSERT_NOT_NULL(val->cast.target_type);

    iron_hir_module_destroy(mod);
}

/* ── Test 37: Lambda expression (EXPR_CLOSURE) ───────────────────────────── */

void test_hir_lower_lambda(void) {
    Iron_Type *int_ty = iron_type_make_primitive(IRON_TYPE_INT);

    /* Build lambda: func() { return 1 } */
    Iron_Node *ret_one = make_return(&g_ast_arena, make_int(&g_ast_arena, "1", int_ty));
    Iron_Node *body_stmts[] = { ret_one };
    Iron_Block *lambda_body = make_block(&g_ast_arena, body_stmts, 1);

    Iron_LambdaExpr *le = ARENA_ALLOC(&g_ast_arena, Iron_LambdaExpr);
    memset(le, 0, sizeof(*le));
    le->span = test_span();
    le->kind = IRON_NODE_LAMBDA;
    le->params = NULL;
    le->param_count = 0;
    le->return_type = NULL;
    le->body = (Iron_Node *)lambda_body;
    le->resolved_type = int_ty;

    Iron_Node *vd = make_val_decl(&g_ast_arena, "f", (Iron_Node *)le, int_ty);
    Iron_Node *stmts[] = { vd };
    Iron_Block    *body = make_block(&g_ast_arena, stmts, 1);
    Iron_FuncDecl *fd   = make_func_decl(&g_ast_arena, "lambda_test", body, NULL);
    Iron_Node *decls[] = { (Iron_Node *)fd };
    Iron_Program *prog = make_program(&g_ast_arena, decls, 1);

    IronHIR_Module *mod = iron_hir_lower(prog, NULL, NULL, &g_diags);
    TEST_ASSERT_NOT_NULL(mod);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);

    IronHIR_Stmt *let = mod->funcs[0]->body->stmts[0];
    TEST_ASSERT_EQUAL_INT(IRON_HIR_STMT_LET, let->kind);
    TEST_ASSERT_EQUAL_INT(IRON_HIR_EXPR_CLOSURE, let->let.init->kind);
    /* Lifted name should start with "__lambda_" */
    TEST_ASSERT_NOT_NULL(let->let.init->closure.lifted_name);
    TEST_ASSERT_TRUE(strncmp(let->let.init->closure.lifted_name,
                              "__lambda_", 9) == 0);
    /* Lifted func should appear in module */
    TEST_ASSERT_TRUE(mod->func_count >= 2);

    iron_hir_module_destroy(mod);
}

/* ── Test 38: Spawn statement (STMT_SPAWN) ───────────────────────────────── */

void test_hir_lower_spawn(void) {
    /* spawn { } */
    Iron_SpawnStmt *ss = ARENA_ALLOC(&g_ast_arena, Iron_SpawnStmt);
    memset(ss, 0, sizeof(*ss));
    ss->span = test_span();
    ss->kind = IRON_NODE_SPAWN;
    ss->name = NULL;
    ss->pool_expr = NULL;
    ss->body = (Iron_Node *)make_block(&g_ast_arena, NULL, 0);
    ss->handle_name = "h";

    Iron_Node *stmts[] = { (Iron_Node *)ss };
    Iron_Block    *body = make_block(&g_ast_arena, stmts, 1);
    Iron_FuncDecl *fd   = make_func_decl(&g_ast_arena, "spawn_test", body, NULL);
    Iron_Node *decls[] = { (Iron_Node *)fd };
    Iron_Program *prog = make_program(&g_ast_arena, decls, 1);

    IronHIR_Module *mod = iron_hir_lower(prog, NULL, NULL, &g_diags);
    TEST_ASSERT_NOT_NULL(mod);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);

    IronHIR_Stmt *spawn_stmt = mod->funcs[0]->body->stmts[0];
    TEST_ASSERT_EQUAL_INT(IRON_HIR_STMT_SPAWN, spawn_stmt->kind);
    TEST_ASSERT_NOT_NULL(spawn_stmt->spawn.lifted_name);
    TEST_ASSERT_TRUE(strncmp(spawn_stmt->spawn.lifted_name, "__spawn_", 8) == 0);
    /* Lifted func should appear in module */
    TEST_ASSERT_TRUE(mod->func_count >= 2);

    iron_hir_module_destroy(mod);
}

/* ── Test 39: Parallel for (EXPR_PARALLEL_FOR) ───────────────────────────── */

void test_hir_lower_parallel_for(void) {
    Iron_Type *int_ty = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_Node *bound  = make_int(&g_ast_arena, "8", int_ty);

    Iron_ForStmt *fs = ARENA_ALLOC(&g_ast_arena, Iron_ForStmt);
    memset(fs, 0, sizeof(*fs));
    fs->span       = test_span();
    fs->kind       = IRON_NODE_FOR;
    fs->var_name   = "i";
    fs->iterable   = bound;
    fs->body       = (Iron_Node *)make_block(&g_ast_arena, NULL, 0);
    fs->is_parallel = true;

    Iron_Node *stmts[] = { (Iron_Node *)fs };
    Iron_Block    *body = make_block(&g_ast_arena, stmts, 1);
    Iron_FuncDecl *fd   = make_func_decl(&g_ast_arena, "pfor_test", body, NULL);
    Iron_Node *decls[] = { (Iron_Node *)fd };
    Iron_Program *prog = make_program(&g_ast_arena, decls, 1);

    IronHIR_Module *mod = iron_hir_lower(prog, NULL, NULL, &g_diags);
    TEST_ASSERT_NOT_NULL(mod);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);

    IronHIR_Stmt *s = mod->funcs[0]->body->stmts[0];
    TEST_ASSERT_EQUAL_INT(IRON_HIR_STMT_EXPR, s->kind);
    TEST_ASSERT_EQUAL_INT(IRON_HIR_EXPR_PARALLEL_FOR, s->expr_stmt.expr->kind);
    TEST_ASSERT_NOT_NULL(s->expr_stmt.expr->parallel_for.lifted_name);
    TEST_ASSERT_TRUE(strncmp(s->expr_stmt.expr->parallel_for.lifted_name,
                              "__pfor_", 7) == 0);
    /* Lifted func should appear in module */
    TEST_ASSERT_TRUE(mod->func_count >= 2);

    iron_hir_module_destroy(mod);
}

/* ── Test 40: Await expression (EXPR_AWAIT) ──────────────────────────────── */

void test_hir_lower_await(void) {
    Iron_Type *int_ty = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_Node *handle_val = make_int(&g_ast_arena, "0", int_ty);

    Iron_AwaitExpr *ae = ARENA_ALLOC(&g_ast_arena, Iron_AwaitExpr);
    memset(ae, 0, sizeof(*ae));
    ae->span = test_span();
    ae->kind = IRON_NODE_AWAIT;
    ae->handle = handle_val;
    ae->resolved_type = int_ty;

    Iron_Node *ret = make_return(&g_ast_arena, (Iron_Node *)ae);
    Iron_Node *stmts[] = { ret };
    Iron_Block    *body = make_block(&g_ast_arena, stmts, 1);
    Iron_FuncDecl *fd   = make_func_decl(&g_ast_arena, "await_test", body, int_ty);
    Iron_Node *decls[] = { (Iron_Node *)fd };
    Iron_Program *prog = make_program(&g_ast_arena, decls, 1);

    IronHIR_Module *mod = iron_hir_lower(prog, NULL, NULL, &g_diags);
    TEST_ASSERT_NOT_NULL(mod);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);

    IronHIR_Expr *val = mod->funcs[0]->body->stmts[0]->return_stmt.value;
    TEST_ASSERT_EQUAL_INT(IRON_HIR_EXPR_AWAIT, val->kind);
    TEST_ASSERT_NOT_NULL(val->await_expr.handle);

    iron_hir_module_destroy(mod);
}

/* ── Test 41: Defer statement (STMT_DEFER) ───────────────────────────────── */

void test_hir_lower_defer(void) {
    /* defer { cleanup() } where cleanup is a no-arg call */
    Iron_Type *void_ty = iron_type_make_primitive(IRON_TYPE_VOID);

    /* Declare cleanup func */
    Iron_Block    *cu_body = make_block(&g_ast_arena, NULL, 0);
    Iron_FuncDecl *cu_fd   = make_func_decl(&g_ast_arena, "cleanup", cu_body, void_ty);

    Iron_Node *call_callee = make_ident(&g_ast_arena, "cleanup", NULL);
    Iron_Node *call_node   = make_call(&g_ast_arena, call_callee, NULL, 0, void_ty);

    Iron_DeferStmt *ds = ARENA_ALLOC(&g_ast_arena, Iron_DeferStmt);
    memset(ds, 0, sizeof(*ds));
    ds->span = test_span();
    ds->kind = IRON_NODE_DEFER;
    ds->expr = call_node;

    Iron_Node *stmts[] = { (Iron_Node *)ds };
    Iron_Block    *body = make_block(&g_ast_arena, stmts, 1);
    Iron_FuncDecl *fd   = make_func_decl(&g_ast_arena, "defer_test", body, NULL);
    Iron_Node *decls[] = { (Iron_Node *)cu_fd, (Iron_Node *)fd };
    Iron_Program *prog = make_program(&g_ast_arena, decls, 2);

    IronHIR_Module *mod = iron_hir_lower(prog, NULL, NULL, &g_diags);
    TEST_ASSERT_NOT_NULL(mod);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);

    /* Find defer_test func */
    IronHIR_Func *def_fn = NULL;
    for (int i = 0; i < mod->func_count; i++) {
        if (strcmp(mod->funcs[i]->name, "defer_test") == 0) {
            def_fn = mod->funcs[i]; break;
        }
    }
    TEST_ASSERT_NOT_NULL(def_fn);
    TEST_ASSERT_EQUAL_INT(1, def_fn->body->stmt_count);
    TEST_ASSERT_EQUAL_INT(IRON_HIR_STMT_DEFER, def_fn->body->stmts[0]->kind);
    TEST_ASSERT_NOT_NULL(def_fn->body->stmts[0]->defer.body);

    iron_hir_module_destroy(mod);
}

/* ── Test 42: Nested scope (inner var gets unique VarId) ─────────────────── */

void test_hir_lower_nested_scope(void) {
    Iron_Type *int_ty = iron_type_make_primitive(IRON_TYPE_INT);
    /* val x = 1; { val x = 2; } */
    Iron_Node *outer_x = make_val_decl(&g_ast_arena, "x",
                                        make_int(&g_ast_arena, "1", int_ty), int_ty);

    Iron_Node *inner_x  = make_val_decl(&g_ast_arena, "x",
                                         make_int(&g_ast_arena, "2", int_ty), int_ty);
    Iron_Node *inner_stmts[] = { inner_x };
    Iron_Block *inner_blk = make_block(&g_ast_arena, inner_stmts, 1);

    Iron_Node *stmts[] = { outer_x, (Iron_Node *)inner_blk };
    Iron_Block    *body = make_block(&g_ast_arena, stmts, 2);
    Iron_FuncDecl *fd   = make_func_decl(&g_ast_arena, "scope_test", body, NULL);
    Iron_Node *decls[] = { (Iron_Node *)fd };
    Iron_Program *prog = make_program(&g_ast_arena, decls, 1);

    IronHIR_Module *mod = iron_hir_lower(prog, NULL, NULL, &g_diags);
    TEST_ASSERT_NOT_NULL(mod);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);

    IronHIR_Block *hb = mod->funcs[0]->body;
    /* outer LET x */
    IronHIR_Stmt *outer_let = hb->stmts[0];
    TEST_ASSERT_EQUAL_INT(IRON_HIR_STMT_LET, outer_let->kind);
    /* inner BLOCK stmt */
    IronHIR_Stmt *block_stmt = hb->stmts[1];
    TEST_ASSERT_EQUAL_INT(IRON_HIR_STMT_BLOCK, block_stmt->kind);
    /* inner block has a LET with a different VarId */
    IronHIR_Stmt *inner_let = block_stmt->block.block->stmts[0];
    TEST_ASSERT_EQUAL_INT(IRON_HIR_STMT_LET, inner_let->kind);
    TEST_ASSERT_NOT_EQUAL(outer_let->let.var_id, inner_let->let.var_id);

    iron_hir_module_destroy(mod);
}

/* ── Test 43: Function reference (EXPR_FUNC_REF) ─────────────────────────── */

void test_hir_lower_func_ref(void) {
    /* val fn_ref = foo — referencing a function name produces EXPR_FUNC_REF */
    Iron_Type *int_ty  = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_Node *fn_ident = make_ident(&g_ast_arena, "foo", NULL);
    Iron_Node *vd = make_val_decl(&g_ast_arena, "fn_ref", fn_ident, int_ty);

    /* foo func stub */
    Iron_Block    *foo_body = make_block(&g_ast_arena, NULL, 0);
    Iron_FuncDecl *foo_fd   = make_func_decl(&g_ast_arena, "foo", foo_body, int_ty);

    Iron_Node *stmts[] = { vd };
    Iron_Block    *body = make_block(&g_ast_arena, stmts, 1);
    Iron_FuncDecl *fd   = make_func_decl(&g_ast_arena, "main", body, NULL);

    Iron_Node *decls[] = { (Iron_Node *)foo_fd, (Iron_Node *)fd };
    Iron_Program *prog = make_program(&g_ast_arena, decls, 2);

    IronHIR_Module *mod = iron_hir_lower(prog, NULL, NULL, &g_diags);
    TEST_ASSERT_NOT_NULL(mod);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);

    /* Find main func */
    IronHIR_Func *main_fn = NULL;
    for (int i = 0; i < mod->func_count; i++) {
        if (strcmp(mod->funcs[i]->name, "main") == 0) { main_fn = mod->funcs[i]; break; }
    }
    TEST_ASSERT_NOT_NULL(main_fn);
    IronHIR_Stmt *let = main_fn->body->stmts[0];
    TEST_ASSERT_EQUAL_INT(IRON_HIR_STMT_LET, let->kind);
    TEST_ASSERT_EQUAL_INT(IRON_HIR_EXPR_FUNC_REF, let->let.init->kind);
    TEST_ASSERT_EQUAL_STRING("foo", let->let.init->func_ref.func_name);

    iron_hir_module_destroy(mod);
}

/* ══════════════════════════════════════════════════════════════════════════════
 * EDGE-CASE TESTS (Tests 44-57)
 * ══════════════════════════════════════════════════════════════════════════════ */

/* ── Test 44: Deeply nested if (5 levels) ────────────────────────────────── */

void test_hir_lower_nested_if_5_levels(void) {
    /* Build from innermost out */
    Iron_Block *body5 = make_block(&g_ast_arena, NULL, 0);
    Iron_Node  *if5   = make_if_stmt(&g_ast_arena, make_bool_lit(&g_ast_arena, true),
                                      body5, NULL);

    Iron_Node *stmts4[] = { if5 };
    Iron_Block *body4   = make_block(&g_ast_arena, stmts4, 1);
    Iron_Node  *if4     = make_if_stmt(&g_ast_arena, make_bool_lit(&g_ast_arena, true),
                                        body4, NULL);

    Iron_Node *stmts3[] = { if4 };
    Iron_Block *body3   = make_block(&g_ast_arena, stmts3, 1);
    Iron_Node  *if3     = make_if_stmt(&g_ast_arena, make_bool_lit(&g_ast_arena, true),
                                        body3, NULL);

    Iron_Node *stmts2[] = { if3 };
    Iron_Block *body2   = make_block(&g_ast_arena, stmts2, 1);
    Iron_Node  *if2     = make_if_stmt(&g_ast_arena, make_bool_lit(&g_ast_arena, true),
                                        body2, NULL);

    Iron_Node *stmts1[] = { if2 };
    Iron_Block *body1   = make_block(&g_ast_arena, stmts1, 1);
    Iron_Node  *if1     = make_if_stmt(&g_ast_arena, make_bool_lit(&g_ast_arena, true),
                                        body1, NULL);

    Iron_Node *stmts[] = { if1 };
    Iron_Block    *body = make_block(&g_ast_arena, stmts, 1);
    Iron_FuncDecl *fd   = make_func_decl(&g_ast_arena, "deep_if", body, NULL);
    Iron_Node *decls[] = { (Iron_Node *)fd };
    Iron_Program *prog = make_program(&g_ast_arena, decls, 1);

    IronHIR_Module *mod = iron_hir_lower(prog, NULL, NULL, &g_diags);
    TEST_ASSERT_NOT_NULL(mod);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);

    /* Trace 5 levels of STMT_IF nesting */
    IronHIR_Stmt *cur = mod->funcs[0]->body->stmts[0];
    for (int level = 0; level < 5; level++) {
        TEST_ASSERT_EQUAL_INT(IRON_HIR_STMT_IF, cur->kind);
        TEST_ASSERT_NOT_NULL(cur->if_else.then_body);
        if (level < 4) {
            TEST_ASSERT_EQUAL_INT(1, cur->if_else.then_body->stmt_count);
            cur = cur->if_else.then_body->stmts[0];
        }
    }

    iron_hir_module_destroy(mod);
}

/* ── Test 45: While inside if ────────────────────────────────────────────── */

void test_hir_lower_while_inside_if(void) {
    Iron_Node *cond      = make_bool_lit(&g_ast_arena, true);
    Iron_Node *while_cond = make_bool_lit(&g_ast_arena, false);
    Iron_Block *while_blk = make_block(&g_ast_arena, NULL, 0);
    Iron_Node  *while_n  = make_while_stmt(&g_ast_arena, while_cond, while_blk);

    Iron_Node *then_stmts[] = { while_n };
    Iron_Block *then_blk    = make_block(&g_ast_arena, then_stmts, 1);
    Iron_Node  *if_node     = make_if_stmt(&g_ast_arena, cond, then_blk, NULL);

    Iron_Node *stmts[] = { if_node };
    Iron_Block    *body = make_block(&g_ast_arena, stmts, 1);
    Iron_FuncDecl *fd   = make_func_decl(&g_ast_arena, "wif", body, NULL);
    Iron_Node *decls[] = { (Iron_Node *)fd };
    Iron_Program *prog = make_program(&g_ast_arena, decls, 1);

    IronHIR_Module *mod = iron_hir_lower(prog, NULL, NULL, &g_diags);
    TEST_ASSERT_NOT_NULL(mod);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);

    IronHIR_Stmt *if_stmt = mod->funcs[0]->body->stmts[0];
    TEST_ASSERT_EQUAL_INT(IRON_HIR_STMT_IF, if_stmt->kind);
    TEST_ASSERT_EQUAL_INT(1, if_stmt->if_else.then_body->stmt_count);
    IronHIR_Stmt *inner = if_stmt->if_else.then_body->stmts[0];
    TEST_ASSERT_EQUAL_INT(IRON_HIR_STMT_WHILE, inner->kind);

    iron_hir_module_destroy(mod);
}

/* ── Test 46: Match inside while body ────────────────────────────────────── */

void test_hir_lower_match_inside_while(void) {
    Iron_Type *int_ty = iron_type_make_primitive(IRON_TYPE_INT);

    Iron_MatchCase *c0 = ARENA_ALLOC(&g_ast_arena, Iron_MatchCase);
    memset(c0, 0, sizeof(*c0));
    c0->span = test_span();
    c0->kind = IRON_NODE_MATCH_CASE;
    c0->pattern = make_int(&g_ast_arena, "0", int_ty);
    c0->body    = (Iron_Node *)make_block(&g_ast_arena, NULL, 0);

    Iron_MatchStmt *ms = ARENA_ALLOC(&g_ast_arena, Iron_MatchStmt);
    memset(ms, 0, sizeof(*ms));
    ms->span = test_span();
    ms->kind = IRON_NODE_MATCH;
    ms->subject = make_int(&g_ast_arena, "1", int_ty);
    ms->cases   = (Iron_Node **)iron_arena_alloc(&g_ast_arena,
                      sizeof(Iron_Node *), _Alignof(Iron_Node *));
    ms->cases[0] = (Iron_Node *)c0;
    ms->case_count = 1;
    ms->else_body  = NULL;

    Iron_Node *while_stmts[] = { (Iron_Node *)ms };
    Iron_Block *while_blk = make_block(&g_ast_arena, while_stmts, 1);
    Iron_Node  *while_n   = make_while_stmt(&g_ast_arena,
                                             make_bool_lit(&g_ast_arena, false),
                                             while_blk);

    Iron_Node *stmts[] = { while_n };
    Iron_Block    *body = make_block(&g_ast_arena, stmts, 1);
    Iron_FuncDecl *fd   = make_func_decl(&g_ast_arena, "mw", body, NULL);
    Iron_Node *decls[] = { (Iron_Node *)fd };
    Iron_Program *prog = make_program(&g_ast_arena, decls, 1);

    IronHIR_Module *mod = iron_hir_lower(prog, NULL, NULL, &g_diags);
    TEST_ASSERT_NOT_NULL(mod);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);

    IronHIR_Stmt *w = mod->funcs[0]->body->stmts[0];
    TEST_ASSERT_EQUAL_INT(IRON_HIR_STMT_WHILE, w->kind);
    TEST_ASSERT_EQUAL_INT(1, w->while_loop.body->stmt_count);
    TEST_ASSERT_EQUAL_INT(IRON_HIR_STMT_MATCH,
                           w->while_loop.body->stmts[0]->kind);

    iron_hir_module_destroy(mod);
}

/* ── Test 47: Multiple defers (3 STMT_DEFER nodes) ───────────────────────── */

void test_hir_lower_multiple_defers(void) {
    Iron_Type *int_ty  = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_Type *void_ty = iron_type_make_primitive(IRON_TYPE_VOID);

    /* Declare cleanup func */
    Iron_Block    *cu_body = make_block(&g_ast_arena, NULL, 0);
    Iron_FuncDecl *cu_fd   = make_func_decl(&g_ast_arena, "cleanup", cu_body, void_ty);

    /* 3 defer statements */
    Iron_DeferStmt *d0 = ARENA_ALLOC(&g_ast_arena, Iron_DeferStmt);
    memset(d0, 0, sizeof(*d0));
    d0->span = test_span(); d0->kind = IRON_NODE_DEFER;
    d0->expr = make_call(&g_ast_arena,
                          make_ident(&g_ast_arena, "cleanup", NULL), NULL, 0, void_ty);

    Iron_DeferStmt *d1 = ARENA_ALLOC(&g_ast_arena, Iron_DeferStmt);
    memset(d1, 0, sizeof(*d1));
    d1->span = test_span(); d1->kind = IRON_NODE_DEFER;
    d1->expr = make_call(&g_ast_arena,
                          make_ident(&g_ast_arena, "cleanup", NULL), NULL, 0, void_ty);

    Iron_DeferStmt *d2 = ARENA_ALLOC(&g_ast_arena, Iron_DeferStmt);
    memset(d2, 0, sizeof(*d2));
    d2->span = test_span(); d2->kind = IRON_NODE_DEFER;
    d2->expr = make_int(&g_ast_arena, "0", int_ty);

    Iron_Node *stmts[] = { (Iron_Node *)d0, (Iron_Node *)d1, (Iron_Node *)d2 };
    Iron_Block    *body = make_block(&g_ast_arena, stmts, 3);
    Iron_FuncDecl *fd   = make_func_decl(&g_ast_arena, "multi_defer", body, NULL);
    Iron_Node *decls[] = { (Iron_Node *)cu_fd, (Iron_Node *)fd };
    Iron_Program *prog = make_program(&g_ast_arena, decls, 2);

    IronHIR_Module *mod = iron_hir_lower(prog, NULL, NULL, &g_diags);
    TEST_ASSERT_NOT_NULL(mod);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);

    /* Find multi_defer func */
    IronHIR_Func *mdf = NULL;
    for (int i = 0; i < mod->func_count; i++) {
        if (strcmp(mod->funcs[i]->name, "multi_defer") == 0) {
            mdf = mod->funcs[i]; break;
        }
    }
    TEST_ASSERT_NOT_NULL(mdf);
    TEST_ASSERT_EQUAL_INT(3, mdf->body->stmt_count);
    TEST_ASSERT_EQUAL_INT(IRON_HIR_STMT_DEFER, mdf->body->stmts[0]->kind);
    TEST_ASSERT_EQUAL_INT(IRON_HIR_STMT_DEFER, mdf->body->stmts[1]->kind);
    TEST_ASSERT_EQUAL_INT(IRON_HIR_STMT_DEFER, mdf->body->stmts[2]->kind);

    iron_hir_module_destroy(mod);
}

/* ── Test 48: Defer + early return (both present in HIR) ─────────────────── */

void test_hir_lower_defer_with_return(void) {
    Iron_Type *int_ty  = iron_type_make_primitive(IRON_TYPE_INT);

    /* defer { 0 } ; return 1 */
    Iron_DeferStmt *ds = ARENA_ALLOC(&g_ast_arena, Iron_DeferStmt);
    memset(ds, 0, sizeof(*ds));
    ds->span = test_span();
    ds->kind = IRON_NODE_DEFER;
    ds->expr = make_int(&g_ast_arena, "0", int_ty);

    Iron_Node *ret = make_return(&g_ast_arena, make_int(&g_ast_arena, "1", int_ty));

    Iron_Node *stmts[] = { (Iron_Node *)ds, ret };
    Iron_Block    *body = make_block(&g_ast_arena, stmts, 2);
    Iron_FuncDecl *fd   = make_func_decl(&g_ast_arena, "dr_test", body, int_ty);
    Iron_Node *decls[] = { (Iron_Node *)fd };
    Iron_Program *prog = make_program(&g_ast_arena, decls, 1);

    IronHIR_Module *mod = iron_hir_lower(prog, NULL, NULL, &g_diags);
    TEST_ASSERT_NOT_NULL(mod);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);

    IronHIR_Block *hb = mod->funcs[0]->body;
    TEST_ASSERT_EQUAL_INT(2, hb->stmt_count);
    TEST_ASSERT_EQUAL_INT(IRON_HIR_STMT_DEFER, hb->stmts[0]->kind);
    TEST_ASSERT_EQUAL_INT(IRON_HIR_STMT_RETURN, hb->stmts[1]->kind);

    iron_hir_module_destroy(mod);
}

/* ── Test 49: Shadowed variable gets new VarId ───────────────────────────── */

void test_hir_lower_shadowed_variable(void) {
    Iron_Type *int_ty = iron_type_make_primitive(IRON_TYPE_INT);
    /* val x = 1 ; { val x = 2 } */
    Iron_Node *outer = make_val_decl(&g_ast_arena, "x",
                                      make_int(&g_ast_arena, "1", int_ty), int_ty);
    Iron_Node *inner = make_val_decl(&g_ast_arena, "x",
                                      make_int(&g_ast_arena, "2", int_ty), int_ty);
    Iron_Node *inner_stmts[] = { inner };
    Iron_Block *inner_blk = make_block(&g_ast_arena, inner_stmts, 1);

    Iron_Node *stmts[] = { outer, (Iron_Node *)inner_blk };
    Iron_Block    *body = make_block(&g_ast_arena, stmts, 2);
    Iron_FuncDecl *fd   = make_func_decl(&g_ast_arena, "shadow_test", body, NULL);
    Iron_Node *decls[] = { (Iron_Node *)fd };
    Iron_Program *prog = make_program(&g_ast_arena, decls, 1);

    IronHIR_Module *mod = iron_hir_lower(prog, NULL, NULL, &g_diags);
    TEST_ASSERT_NOT_NULL(mod);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);

    IronHIR_Block *hb = mod->funcs[0]->body;
    IronHIR_VarId outer_id = hb->stmts[0]->let.var_id;
    IronHIR_VarId inner_id = hb->stmts[1]->block.block->stmts[0]->let.var_id;
    /* Shadowed variable must have different VarId */
    TEST_ASSERT_NOT_EQUAL(outer_id, inner_id);

    iron_hir_module_destroy(mod);
}

/* ── Test 50: Global constant lazy lowering ──────────────────────────────── */

void test_hir_lower_global_constant_lazy(void) {
    /* Top-level val GLOBAL = 42 ; func f { return GLOBAL } */
    Iron_Type *int_ty  = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_Node *global_init = make_int(&g_ast_arena, "42", int_ty);
    Iron_Node *global_vd   = make_val_decl(&g_ast_arena, "GLOBAL",
                                            global_init, int_ty);

    Iron_Node *ref = make_ident(&g_ast_arena, "GLOBAL", int_ty);
    Iron_Node *ret = make_return(&g_ast_arena, ref);
    Iron_Node *stmts[] = { ret };
    Iron_Block    *body = make_block(&g_ast_arena, stmts, 1);
    Iron_FuncDecl *fd   = make_func_decl(&g_ast_arena, "f", body, int_ty);

    Iron_Node *decls[] = { global_vd, (Iron_Node *)fd };
    Iron_Program *prog = make_program(&g_ast_arena, decls, 2);

    IronHIR_Module *mod = iron_hir_lower(prog, NULL, NULL, &g_diags);
    TEST_ASSERT_NOT_NULL(mod);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);

    /* Find func f */
    IronHIR_Func *fn = NULL;
    for (int i = 0; i < mod->func_count; i++) {
        if (strcmp(mod->funcs[i]->name, "f") == 0) { fn = mod->funcs[i]; break; }
    }
    TEST_ASSERT_NOT_NULL(fn);
    /* Should have injected a STMT_LET for GLOBAL before the return */
    TEST_ASSERT_TRUE(fn->body->stmt_count >= 2);
    TEST_ASSERT_EQUAL_INT(IRON_HIR_STMT_LET, fn->body->stmts[0]->kind);
    TEST_ASSERT_EQUAL_INT(IRON_HIR_STMT_RETURN, fn->body->stmts[1]->kind);

    iron_hir_module_destroy(mod);
}

/* ── Test 51: Nested call (f(g(x), h(y))) ────────────────────────────────── */

void test_hir_lower_nested_call(void) {
    Iron_Type *int_ty = iron_type_make_primitive(IRON_TYPE_INT);

    /* g stub */
    Iron_Block    *g_body = make_block(&g_ast_arena, NULL, 0);
    Iron_FuncDecl *g_fd   = make_func_decl(&g_ast_arena, "g", g_body, int_ty);
    /* h stub */
    Iron_Block    *h_body = make_block(&g_ast_arena, NULL, 0);
    Iron_FuncDecl *h_fd   = make_func_decl(&g_ast_arena, "h", h_body, int_ty);
    /* f stub */
    Iron_Block    *f_body = make_block(&g_ast_arena, NULL, 0);
    Iron_FuncDecl *f_fd   = make_func_decl(&g_ast_arena, "f", f_body, int_ty);

    Iron_Node *x     = make_int(&g_ast_arena, "1", int_ty);
    Iron_Node *y     = make_int(&g_ast_arena, "2", int_ty);
    Iron_Node *gx_args[] = { x };
    Iron_Node *gx    = make_call(&g_ast_arena, make_ident(&g_ast_arena, "g", NULL),
                                  gx_args, 1, int_ty);
    Iron_Node *hy_args[] = { y };
    Iron_Node *hy    = make_call(&g_ast_arena, make_ident(&g_ast_arena, "h", NULL),
                                  hy_args, 1, int_ty);
    Iron_Node *outer_args[] = { gx, hy };
    Iron_Node *outer = make_call(&g_ast_arena, make_ident(&g_ast_arena, "f", NULL),
                                  outer_args, 2, int_ty);
    Iron_Node *ret   = make_return(&g_ast_arena, outer);

    Iron_Node *stmts[] = { ret };
    Iron_Block    *body = make_block(&g_ast_arena, stmts, 1);
    Iron_FuncDecl *fd   = make_func_decl(&g_ast_arena, "main", body, int_ty);

    Iron_Node *decls[] = { (Iron_Node *)g_fd, (Iron_Node *)h_fd,
                            (Iron_Node *)f_fd, (Iron_Node *)fd };
    Iron_Program *prog = make_program(&g_ast_arena, decls, 4);

    IronHIR_Module *mod = iron_hir_lower(prog, NULL, NULL, &g_diags);
    TEST_ASSERT_NOT_NULL(mod);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);

    /* Find main func */
    IronHIR_Func *main_fn = NULL;
    for (int i = 0; i < mod->func_count; i++) {
        if (strcmp(mod->funcs[i]->name, "main") == 0) { main_fn = mod->funcs[i]; break; }
    }
    TEST_ASSERT_NOT_NULL(main_fn);
    IronHIR_Expr *outer_call = main_fn->body->stmts[0]->return_stmt.value;
    TEST_ASSERT_EQUAL_INT(IRON_HIR_EXPR_CALL, outer_call->kind);
    TEST_ASSERT_EQUAL_INT(2, outer_call->call.arg_count);
    /* Both args are nested calls */
    TEST_ASSERT_EQUAL_INT(IRON_HIR_EXPR_CALL, outer_call->call.args[0]->kind);
    TEST_ASSERT_EQUAL_INT(IRON_HIR_EXPR_CALL, outer_call->call.args[1]->kind);

    iron_hir_module_destroy(mod);
}

/* ── Test 52: Chained field access (a.b.c) ───────────────────────────────── */

void test_hir_lower_chained_field_access(void) {
    Iron_Type *int_ty = iron_type_make_primitive(IRON_TYPE_INT);

    /* Build a.b.c: FIELD_ACCESS(FIELD_ACCESS(ident(a), "b"), "c") */
    Iron_Node *a = make_val_decl(&g_ast_arena, "a",
                                  make_int(&g_ast_arena, "0", int_ty), int_ty);

    Iron_FieldAccess *ab = ARENA_ALLOC(&g_ast_arena, Iron_FieldAccess);
    memset(ab, 0, sizeof(*ab));
    ab->span = test_span();
    ab->kind = IRON_NODE_FIELD_ACCESS;
    ab->object = make_ident(&g_ast_arena, "a", int_ty);
    ab->field  = "b";
    ab->resolved_type = int_ty;

    Iron_FieldAccess *abc = ARENA_ALLOC(&g_ast_arena, Iron_FieldAccess);
    memset(abc, 0, sizeof(*abc));
    abc->span = test_span();
    abc->kind = IRON_NODE_FIELD_ACCESS;
    abc->object = (Iron_Node *)ab;
    abc->field  = "c";
    abc->resolved_type = int_ty;

    Iron_Node *ret = make_return(&g_ast_arena, (Iron_Node *)abc);
    Iron_Node *stmts[] = { a, ret };
    Iron_Block    *body = make_block(&g_ast_arena, stmts, 2);
    Iron_FuncDecl *fd   = make_func_decl(&g_ast_arena, "chain", body, int_ty);
    Iron_Node *decls[] = { (Iron_Node *)fd };
    Iron_Program *prog = make_program(&g_ast_arena, decls, 1);

    IronHIR_Module *mod = iron_hir_lower(prog, NULL, NULL, &g_diags);
    TEST_ASSERT_NOT_NULL(mod);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);

    IronHIR_Block *hb = mod->funcs[0]->body;
    IronHIR_Expr  *chain = hb->stmts[hb->stmt_count - 1]->return_stmt.value;
    TEST_ASSERT_EQUAL_INT(IRON_HIR_EXPR_FIELD_ACCESS, chain->kind);
    TEST_ASSERT_EQUAL_STRING("c", chain->field_access.field);
    TEST_ASSERT_EQUAL_INT(IRON_HIR_EXPR_FIELD_ACCESS, chain->field_access.object->kind);
    TEST_ASSERT_EQUAL_STRING("b", chain->field_access.object->field_access.field);

    iron_hir_module_destroy(mod);
}

/* ── Test 53: Index of call result (f()[0]) ──────────────────────────────── */

void test_hir_lower_index_of_call(void) {
    Iron_Type *int_ty = iron_type_make_primitive(IRON_TYPE_INT);

    /* getarr func stub */
    Iron_Block    *ga_body = make_block(&g_ast_arena, NULL, 0);
    Iron_FuncDecl *ga_fd   = make_func_decl(&g_ast_arena, "getarr", ga_body, int_ty);

    Iron_Node *call = make_call(&g_ast_arena,
                                 make_ident(&g_ast_arena, "getarr", NULL),
                                 NULL, 0, int_ty);

    Iron_IndexExpr *ix = ARENA_ALLOC(&g_ast_arena, Iron_IndexExpr);
    memset(ix, 0, sizeof(*ix));
    ix->span = test_span();
    ix->kind = IRON_NODE_INDEX;
    ix->object = call;
    ix->index  = make_int(&g_ast_arena, "0", int_ty);
    ix->resolved_type = int_ty;

    Iron_Node *ret = make_return(&g_ast_arena, (Iron_Node *)ix);
    Iron_Node *stmts[] = { ret };
    Iron_Block    *body = make_block(&g_ast_arena, stmts, 1);
    Iron_FuncDecl *fd   = make_func_decl(&g_ast_arena, "ioc", body, int_ty);
    Iron_Node *decls[] = { (Iron_Node *)ga_fd, (Iron_Node *)fd };
    Iron_Program *prog = make_program(&g_ast_arena, decls, 2);

    IronHIR_Module *mod = iron_hir_lower(prog, NULL, NULL, &g_diags);
    TEST_ASSERT_NOT_NULL(mod);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);

    /* Find ioc func */
    IronHIR_Func *fn = NULL;
    for (int i = 0; i < mod->func_count; i++) {
        if (strcmp(mod->funcs[i]->name, "ioc") == 0) { fn = mod->funcs[i]; break; }
    }
    TEST_ASSERT_NOT_NULL(fn);
    IronHIR_Expr *val = fn->body->stmts[0]->return_stmt.value;
    TEST_ASSERT_EQUAL_INT(IRON_HIR_EXPR_INDEX, val->kind);
    TEST_ASSERT_EQUAL_INT(IRON_HIR_EXPR_CALL, val->index.array->kind);

    iron_hir_module_destroy(mod);
}

/* ── Test 54: Empty array literal (EXPR_ARRAY_LIT, 0 elements) ───────────── */

void test_hir_lower_array_literal_empty(void) {
    Iron_Type *int_ty = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_Type *arr_ty = iron_type_make_array(&g_ast_arena, int_ty, -1);

    Iron_ArrayLit *al = ARENA_ALLOC(&g_ast_arena, Iron_ArrayLit);
    memset(al, 0, sizeof(*al));
    al->span = test_span();
    al->kind = IRON_NODE_ARRAY_LIT;
    al->resolved_type  = arr_ty;
    al->elements       = NULL;
    al->element_count  = 0;

    Iron_Node *vd = make_val_decl(&g_ast_arena, "empty",
                                   (Iron_Node *)al, arr_ty);
    Iron_Node *stmts[] = { vd };
    Iron_Block    *body = make_block(&g_ast_arena, stmts, 1);
    Iron_FuncDecl *fd   = make_func_decl(&g_ast_arena, "arr_empty", body, NULL);
    Iron_Node *decls[] = { (Iron_Node *)fd };
    Iron_Program *prog = make_program(&g_ast_arena, decls, 1);

    IronHIR_Module *mod = iron_hir_lower(prog, NULL, NULL, &g_diags);
    TEST_ASSERT_NOT_NULL(mod);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);

    IronHIR_Stmt *let = mod->funcs[0]->body->stmts[0];
    TEST_ASSERT_EQUAL_INT(IRON_HIR_STMT_LET, let->kind);
    TEST_ASSERT_EQUAL_INT(IRON_HIR_EXPR_ARRAY_LIT, let->let.init->kind);
    TEST_ASSERT_EQUAL_INT(0, let->let.init->array_lit.element_count);

    iron_hir_module_destroy(mod);
}

/* ── Test 55: Error node produces diagnostic, not crash ──────────────────── */

void test_hir_lower_error_node(void) {
    /* Wrap an IRON_NODE_ERROR node inside a return — should not crash */
    Iron_ErrorNode *err = ARENA_ALLOC(&g_ast_arena, Iron_ErrorNode);
    memset(err, 0, sizeof(*err));
    err->span = test_span();
    err->kind = IRON_NODE_ERROR;

    Iron_Node *ret = make_return(&g_ast_arena, (Iron_Node *)err);
    Iron_Node *stmts[] = { ret };
    Iron_Block    *body = make_block(&g_ast_arena, stmts, 1);
    Iron_FuncDecl *fd   = make_func_decl(&g_ast_arena, "err_test", body, NULL);
    Iron_Node *decls[] = { (Iron_Node *)fd };
    Iron_Program *prog = make_program(&g_ast_arena, decls, 1);

    /* Must not crash — errors lower to null_lit */
    IronHIR_Module *mod = iron_hir_lower(prog, NULL, NULL, &g_diags);
    TEST_ASSERT_NOT_NULL(mod);
    /* Result is a null_lit fallback; verify the return expr is non-NULL */
    IronHIR_Stmt *ret_stmt = mod->funcs[0]->body->stmts[0];
    TEST_ASSERT_EQUAL_INT(IRON_HIR_STMT_RETURN, ret_stmt->kind);
    /* lowerer uses null_lit as poison for error nodes */
    TEST_ASSERT_EQUAL_INT(IRON_HIR_EXPR_NULL_LIT,
                           ret_stmt->return_stmt.value->kind);

    iron_hir_module_destroy(mod);
}

/* ── Test 56: Defer in nested scopes ─────────────────────────────────────── */

void test_hir_lower_nested_defer_scopes(void) {
    Iron_Type *int_ty = iron_type_make_primitive(IRON_TYPE_INT);

    /* outer defer */
    Iron_DeferStmt *d_outer = ARENA_ALLOC(&g_ast_arena, Iron_DeferStmt);
    memset(d_outer, 0, sizeof(*d_outer));
    d_outer->span = test_span();
    d_outer->kind = IRON_NODE_DEFER;
    d_outer->expr = make_int(&g_ast_arena, "1", int_ty);

    /* inner block with its own defer */
    Iron_DeferStmt *d_inner = ARENA_ALLOC(&g_ast_arena, Iron_DeferStmt);
    memset(d_inner, 0, sizeof(*d_inner));
    d_inner->span = test_span();
    d_inner->kind = IRON_NODE_DEFER;
    d_inner->expr = make_int(&g_ast_arena, "2", int_ty);

    Iron_Node *inner_stmts[] = { (Iron_Node *)d_inner };
    Iron_Block *inner_blk = make_block(&g_ast_arena, inner_stmts, 1);

    Iron_Node *stmts[] = { (Iron_Node *)d_outer, (Iron_Node *)inner_blk };
    Iron_Block    *body = make_block(&g_ast_arena, stmts, 2);
    Iron_FuncDecl *fd   = make_func_decl(&g_ast_arena, "nested_defer", body, NULL);
    Iron_Node *decls[] = { (Iron_Node *)fd };
    Iron_Program *prog = make_program(&g_ast_arena, decls, 1);

    IronHIR_Module *mod = iron_hir_lower(prog, NULL, NULL, &g_diags);
    TEST_ASSERT_NOT_NULL(mod);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);

    IronHIR_Block *hb = mod->funcs[0]->body;
    TEST_ASSERT_EQUAL_INT(2, hb->stmt_count);
    TEST_ASSERT_EQUAL_INT(IRON_HIR_STMT_DEFER, hb->stmts[0]->kind);
    TEST_ASSERT_EQUAL_INT(IRON_HIR_STMT_BLOCK, hb->stmts[1]->kind);
    /* Inner block has its own defer */
    IronHIR_Block *inner = hb->stmts[1]->block.block;
    TEST_ASSERT_EQUAL_INT(1, inner->stmt_count);
    TEST_ASSERT_EQUAL_INT(IRON_HIR_STMT_DEFER, inner->stmts[0]->kind);

    iron_hir_module_destroy(mod);
}

/* ── Test 57: Large program passes iron_hir_verify ───────────────────────── */

void test_hir_lower_verify_output(void) {
    /* Build a moderately complex program to exercise verify */
    Iron_Type *int_ty  = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_Type *bool_ty = iron_type_make_primitive(IRON_TYPE_BOOL);
    Iron_Type *void_ty = iron_type_make_primitive(IRON_TYPE_VOID);

    /* helper func */
    Iron_Block    *helper_body = make_block(&g_ast_arena, NULL, 0);
    Iron_FuncDecl *helper_fd   = make_func_decl(&g_ast_arena, "helper",
                                                  helper_body, void_ty);

    /* main func with val, if, while, return */
    Iron_Node *lit1 = make_int(&g_ast_arena, "1", int_ty);
    Iron_Node *vd   = make_val_decl(&g_ast_arena, "v", lit1, int_ty);

    Iron_Node *cond  = make_bool_lit(&g_ast_arena, true);
    Iron_Block *then  = make_block(&g_ast_arena, NULL, 0);
    Iron_Node *if_n  = make_if_stmt(&g_ast_arena, cond, then, NULL);

    Iron_Node *wcond  = make_bool_lit(&g_ast_arena, false);
    Iron_Block *wbody = make_block(&g_ast_arena, NULL, 0);
    Iron_Node *wn    = make_while_stmt(&g_ast_arena, wcond, wbody);

    Iron_Node *ret_v  = make_return(&g_ast_arena,
                                     make_int(&g_ast_arena, "42", int_ty));

    Iron_Node *stmts[] = { vd, if_n, wn, ret_v };
    Iron_Block    *body = make_block(&g_ast_arena, stmts, 4);
    Iron_FuncDecl *fd   = make_func_decl(&g_ast_arena, "main", body, int_ty);
    (void)bool_ty;

    Iron_Node *decls[] = { (Iron_Node *)helper_fd, (Iron_Node *)fd };
    Iron_Program *prog = make_program(&g_ast_arena, decls, 2);

    IronHIR_Module *mod = iron_hir_lower(prog, NULL, NULL, &g_diags);
    TEST_ASSERT_NOT_NULL(mod);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);

    Iron_DiagList vdiags = iron_diaglist_create();
    Iron_Arena    varena = iron_arena_create(64 * 1024);
    bool ok = iron_hir_verify(mod, &vdiags, &varena);
    iron_arena_free(&varena);
    iron_diaglist_free(&vdiags);
    TEST_ASSERT_TRUE(ok);

    iron_hir_module_destroy(mod);
}

/* ── Runner ──────────────────────────────────────────────────────────────── */

int main(void) {
    UNITY_BEGIN();
    /* Smoke tests (15) */
    RUN_TEST(test_hir_lower_empty_program);
    RUN_TEST(test_hir_lower_simple_func);
    RUN_TEST(test_hir_lower_val_binding);
    RUN_TEST(test_hir_lower_var_binding);
    RUN_TEST(test_hir_lower_if_else);
    RUN_TEST(test_hir_lower_while_loop);
    RUN_TEST(test_hir_lower_for_range_desugars);
    RUN_TEST(test_hir_lower_return_value);
    RUN_TEST(test_hir_lower_binary_expr);
    RUN_TEST(test_hir_lower_call_expr);
    RUN_TEST(test_hir_lower_int_literal);
    RUN_TEST(test_hir_lower_string_literal);
    RUN_TEST(test_hir_lower_elif_desugars);
    RUN_TEST(test_hir_lower_multiple_funcs);
    RUN_TEST(test_hir_lower_verify_passes);
    /* Feature-matrix tests (28) */
    RUN_TEST(test_hir_lower_float_literal);
    RUN_TEST(test_hir_lower_bool_literal);
    RUN_TEST(test_hir_lower_null_literal);
    RUN_TEST(test_hir_lower_string_interp);
    RUN_TEST(test_hir_lower_unary_neg);
    RUN_TEST(test_hir_lower_unary_not);
    RUN_TEST(test_hir_lower_binary_all_ops);
    RUN_TEST(test_hir_lower_for_range_int_desugars);
    RUN_TEST(test_hir_lower_for_array_iter);
    RUN_TEST(test_hir_lower_compound_assign_add);
    RUN_TEST(test_hir_lower_compound_assign_sub);
    RUN_TEST(test_hir_lower_match_stmt);
    RUN_TEST(test_hir_lower_heap_alloc);
    RUN_TEST(test_hir_lower_rc_alloc);
    RUN_TEST(test_hir_lower_free_stmt);
    RUN_TEST(test_hir_lower_leak_stmt);
    RUN_TEST(test_hir_lower_call_with_args);
    RUN_TEST(test_hir_lower_method_call);
    RUN_TEST(test_hir_lower_field_access);
    RUN_TEST(test_hir_lower_index_access);
    RUN_TEST(test_hir_lower_cast);
    RUN_TEST(test_hir_lower_lambda);
    RUN_TEST(test_hir_lower_spawn);
    RUN_TEST(test_hir_lower_parallel_for);
    RUN_TEST(test_hir_lower_await);
    RUN_TEST(test_hir_lower_defer);
    RUN_TEST(test_hir_lower_nested_scope);
    RUN_TEST(test_hir_lower_func_ref);
    /* Edge-case tests (14) */
    RUN_TEST(test_hir_lower_nested_if_5_levels);
    RUN_TEST(test_hir_lower_while_inside_if);
    RUN_TEST(test_hir_lower_match_inside_while);
    RUN_TEST(test_hir_lower_multiple_defers);
    RUN_TEST(test_hir_lower_defer_with_return);
    RUN_TEST(test_hir_lower_shadowed_variable);
    RUN_TEST(test_hir_lower_global_constant_lazy);
    RUN_TEST(test_hir_lower_nested_call);
    RUN_TEST(test_hir_lower_chained_field_access);
    RUN_TEST(test_hir_lower_index_of_call);
    RUN_TEST(test_hir_lower_array_literal_empty);
    RUN_TEST(test_hir_lower_error_node);
    RUN_TEST(test_hir_lower_nested_defer_scopes);
    RUN_TEST(test_hir_lower_verify_output);
    return UNITY_END();
}
