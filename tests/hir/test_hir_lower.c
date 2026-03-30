/* test_hir_lower.c — Unity smoke tests for AST-to-HIR lowering pass.
 *
 * Each test builds a minimal Iron AST programmatically, calls iron_hir_lower(),
 * and verifies the structure of the resulting HIR module.
 *
 * Covers:
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

/* ── Runner ──────────────────────────────────────────────────────────────── */

int main(void) {
    UNITY_BEGIN();
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
    return UNITY_END();
}
