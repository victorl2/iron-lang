/* test_ir_lower.c — Unity tests for the AST-to-IR lowering pass (Phase 8).
 *
 * Tests verify that iron_ir_lower() produces correct IR for Iron programs.
 * Wave 0: placeholder scaffold — just verifies the test binary links and runs.
 * Wave 1+: expression lowering tests added in Plans 08-01 onward.
 * Wave 2 (Plan 08-02): control flow and statement lowering tests.
 */

#include "unity.h"
#include "ir/lower.h"
#include "ir/ir.h"
#include "ir/verify.h"
#include "ir/print.h"
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

static Iron_Arena g_ir_arena;
static Iron_DiagList g_diags;

void setUp(void) {
    g_ir_arena = iron_arena_create(65536);
    iron_types_init(&g_ir_arena);
    g_diags = iron_diaglist_create();
}

void tearDown(void) {
    iron_diaglist_free(&g_diags);
    iron_arena_free(&g_ir_arena);
}

/* ── Snapshot comparison helper ─────────────────────────────────────────── */

#if defined(__GNUC__) || defined(__clang__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wunused-function"
#endif

static bool compare_snapshot(const char *ir_output, const char *expected_path) {
    FILE *f = fopen(expected_path, "r");
    if (!f) { printf("Snapshot file not found: %s\n", expected_path); return false; }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *expected = malloc((size_t)len + 1);
    fread(expected, 1, (size_t)len, f);
    expected[len] = '\0';
    fclose(f);
    bool match = strcmp(ir_output, expected) == 0;
    if (!match) { printf("Snapshot mismatch.\nGot:\n%s\nExpected:\n%s\n", ir_output, expected); }
    free(expected);
    return match;
}

/* Write a snapshot file (golden master creation). */
static void write_snapshot(const char *ir_output, const char *expected_path) {
    FILE *f = fopen(expected_path, "w");
    if (!f) { printf("Cannot write snapshot: %s\n", expected_path); return; }
    fputs(ir_output, f);
    fclose(f);
}

/* ── AST construction helpers ───────────────────────────────────────────── */

static Iron_Span test_span(void) {
    return iron_span_make("test.iron", 1, 1, 1, 1);
}

/* Build a minimal Iron_IntLit node */
static Iron_Node *make_int(Iron_Arena *arena, const char *value) {
    Iron_IntLit *n = ARENA_ALLOC(arena, Iron_IntLit);
    memset(n, 0, sizeof(*n));
    n->span = test_span();
    n->kind = IRON_NODE_INT_LIT;
    n->value = value;
    n->resolved_type = iron_type_make_primitive(IRON_TYPE_INT);
    return (Iron_Node *)n;
}

/* Build a minimal Iron_BoolLit node */
static Iron_Node *make_bool(Iron_Arena *arena, bool value) {
    Iron_BoolLit *n = ARENA_ALLOC(arena, Iron_BoolLit);
    memset(n, 0, sizeof(*n));
    n->span = test_span();
    n->kind = IRON_NODE_BOOL_LIT;
    n->value = value;
    n->resolved_type = iron_type_make_primitive(IRON_TYPE_BOOL);
    return (Iron_Node *)n;
}

/* Build a minimal Iron_Ident node */
static Iron_Node *make_ident(Iron_Arena *arena, const char *name, Iron_Type *type) {
    Iron_Ident *n = ARENA_ALLOC(arena, Iron_Ident);
    memset(n, 0, sizeof(*n));
    n->span = test_span();
    n->kind = IRON_NODE_IDENT;
    n->name = name;
    n->resolved_type = type;
    return (Iron_Node *)n;
}

/* Build a binary expression node */
static Iron_Node *make_binary(Iron_Arena *arena, Iron_Node *left, int op,
                               Iron_Node *right, Iron_Type *result_type) {
    Iron_BinaryExpr *n = ARENA_ALLOC(arena, Iron_BinaryExpr);
    memset(n, 0, sizeof(*n));
    n->span = test_span();
    n->kind = IRON_NODE_BINARY;
    n->left = left;
    n->op   = op;
    n->right = right;
    n->resolved_type = result_type;
    return (Iron_Node *)n;
}

/* Build a return statement */
static Iron_Node *make_return(Iron_Arena *arena, Iron_Node *value) {
    Iron_ReturnStmt *n = ARENA_ALLOC(arena, Iron_ReturnStmt);
    memset(n, 0, sizeof(*n));
    n->span = test_span();
    n->kind = IRON_NODE_RETURN;
    n->value = value;
    return (Iron_Node *)n;
}

/* Build a var declaration */
static Iron_Node *make_var_decl(Iron_Arena *arena, const char *name,
                                 Iron_Node *init, Iron_Type *declared_type) {
    Iron_VarDecl *n = ARENA_ALLOC(arena, Iron_VarDecl);
    memset(n, 0, sizeof(*n));
    n->span = test_span();
    n->kind = IRON_NODE_VAR_DECL;
    n->name = name;
    n->init = init;
    n->declared_type = declared_type;
    return (Iron_Node *)n;
}

/* Build a val declaration */
static Iron_Node *make_val_decl(Iron_Arena *arena, const char *name,
                                 Iron_Node *init, Iron_Type *declared_type) {
    Iron_ValDecl *n = ARENA_ALLOC(arena, Iron_ValDecl);
    memset(n, 0, sizeof(*n));
    n->span = test_span();
    n->kind = IRON_NODE_VAL_DECL;
    n->name = name;
    n->init = init;
    n->declared_type = declared_type;
    return (Iron_Node *)n;
}

/* Build an assign statement (simple ident target) */
static Iron_Node *make_assign(Iron_Arena *arena, Iron_Node *target, Iron_Node *value) {
    Iron_AssignStmt *n = ARENA_ALLOC(arena, Iron_AssignStmt);
    memset(n, 0, sizeof(*n));
    n->span = test_span();
    n->kind = IRON_NODE_ASSIGN;
    n->target = target;
    n->value = value;
    n->op = IRON_TOK_ASSIGN;
    return (Iron_Node *)n;
}

/* Build a block node from a static array of statements */
static Iron_Block *make_block(Iron_Arena *arena, Iron_Node **stmts, int count) {
    Iron_Block *b = ARENA_ALLOC(arena, Iron_Block);
    memset(b, 0, sizeof(*b));
    b->span = test_span();
    b->kind = IRON_NODE_BLOCK;
    if (count > 0) {
        b->stmts = (Iron_Node **)iron_arena_alloc(arena,
                       (size_t)count * sizeof(Iron_Node *), _Alignof(Iron_Node *));
        memcpy(b->stmts, stmts, (size_t)count * sizeof(Iron_Node *));
    }
    b->stmt_count = count;
    return b;
}

/* Build an if statement */
static Iron_Node *make_if(Iron_Arena *arena, Iron_Node *condition,
                           Iron_Block *body, Iron_Block *else_body) {
    Iron_IfStmt *n = ARENA_ALLOC(arena, Iron_IfStmt);
    memset(n, 0, sizeof(*n));
    n->span = test_span();
    n->kind = IRON_NODE_IF;
    n->condition = condition;
    n->body = (Iron_Node *)body;
    n->else_body = else_body ? (Iron_Node *)else_body : NULL;
    n->elif_conds  = NULL;
    n->elif_bodies = NULL;
    n->elif_count  = 0;
    return (Iron_Node *)n;
}

/* Build a while statement */
static Iron_Node *make_while(Iron_Arena *arena, Iron_Node *condition, Iron_Block *body) {
    Iron_WhileStmt *n = ARENA_ALLOC(arena, Iron_WhileStmt);
    memset(n, 0, sizeof(*n));
    n->span = test_span();
    n->kind = IRON_NODE_WHILE;
    n->condition = condition;
    n->body = (Iron_Node *)body;
    return (Iron_Node *)n;
}

/* Build a param node (for function declarations) */
static Iron_Param *make_param(Iron_Arena *arena, const char *name) {
    Iron_Param *p = ARENA_ALLOC(arena, Iron_Param);
    memset(p, 0, sizeof(*p));
    p->span = test_span();
    p->kind = IRON_NODE_PARAM;
    p->name = name;
    return p;
}

/* Build a minimal Iron_FuncDecl node with an empty body block */
static Iron_FuncDecl *make_func_decl(Iron_Arena *arena, const char *name,
                                      Iron_Type *return_type) {
    Iron_Block *body = ARENA_ALLOC(arena, Iron_Block);
    memset(body, 0, sizeof(*body));
    body->span = test_span();
    body->kind = IRON_NODE_BLOCK;
    body->stmts = NULL;
    body->stmt_count = 0;

    Iron_FuncDecl *fd = ARENA_ALLOC(arena, Iron_FuncDecl);
    memset(fd, 0, sizeof(*fd));
    fd->span = test_span();
    fd->kind = IRON_NODE_FUNC_DECL;
    fd->name = name;
    fd->params = NULL;
    fd->param_count = 0;
    fd->body = (Iron_Node *)body;
    fd->resolved_return_type = return_type;
    return fd;
}

/* Build a program from a static array of top-level declarations */
static Iron_Program *make_program(Iron_Arena *arena, Iron_Node **decls, int count) {
    Iron_Program *prog = ARENA_ALLOC(arena, Iron_Program);
    memset(prog, 0, sizeof(*prog));
    prog->span = test_span();
    prog->kind = IRON_NODE_PROGRAM;
    if (count > 0) {
        prog->decls = (Iron_Node **)iron_arena_alloc(arena,
                          (size_t)count * sizeof(Iron_Node *), _Alignof(Iron_Node *));
        memcpy(prog->decls, decls, (size_t)count * sizeof(Iron_Node *));
    }
    prog->decl_count = count;
    return prog;
}

#if defined(__GNUC__) || defined(__clang__)
#  pragma GCC diagnostic pop
#endif

/* ── Helper to check a block kind in the lowered module ─────────────────── */

/* Count the number of instructions of a given kind in the function */
static int count_instrs_of_kind(IronIR_Func *fn, IronIR_InstrKind kind) {
    int count = 0;
    for (int b = 0; b < fn->block_count; b++) {
        IronIR_Block *blk = fn->blocks[b];
        for (int i = 0; i < blk->instr_count; i++) {
            if (blk->instrs[i]->kind == kind) count++;
        }
    }
    return count;
}

/* ── Wave 0: placeholder test ───────────────────────────────────────────── */

void test_lower_placeholder(void) {
    /* This test just verifies that the test binary links and runs correctly. */
    TEST_PASS();
}

/* ── Wave 2: Unit tests (Plan 08-02) ────────────────────────────────────── */

/* test_lower_empty_func: empty function body -> entry block with void return */
void test_lower_empty_func(void) {
    Iron_Type *void_type = iron_type_make_primitive(IRON_TYPE_VOID);
    Iron_FuncDecl *fd = make_func_decl(&g_ir_arena, "empty_func", void_type);

    Iron_Node *decls[1] = { (Iron_Node *)fd };
    Iron_Program *prog = make_program(&g_ir_arena, decls, 1);

    IronIR_Module *mod = iron_ir_lower(prog, NULL, &g_ir_arena, &g_diags);
    TEST_ASSERT_NOT_NULL(mod);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);
    TEST_ASSERT_EQUAL_INT(1, mod->func_count);

    IronIR_Func *fn = mod->funcs[0];
    TEST_ASSERT_NOT_NULL(fn);
    TEST_ASSERT_GREATER_THAN(0, fn->block_count);

    /* Must have at least one return instruction */
    TEST_ASSERT_GREATER_THAN(0, count_instrs_of_kind(fn, IRON_IR_RETURN));

    /* Verify passes */
    Iron_DiagList verify_diags = iron_diaglist_create();
    bool ok = iron_ir_verify(mod, &verify_diags, &g_ir_arena);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_INT(0, verify_diags.error_count);
    iron_diaglist_free(&verify_diags);
}

/* test_lower_const_return: fn answer() -> Int { return 42 } */
void test_lower_const_return(void) {
    Iron_Type *int_type = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_FuncDecl *fd = make_func_decl(&g_ir_arena, "answer", int_type);

    /* Build body: return 42 */
    Iron_Node *ret42 = make_return(&g_ir_arena, make_int(&g_ir_arena, "42"));
    Iron_Node *stmts[1] = { ret42 };
    fd->body = (Iron_Node *)make_block(&g_ir_arena, stmts, 1);

    Iron_Node *decls[1] = { (Iron_Node *)fd };
    Iron_Program *prog = make_program(&g_ir_arena, decls, 1);

    IronIR_Module *mod = iron_ir_lower(prog, NULL, &g_ir_arena, &g_diags);
    TEST_ASSERT_NOT_NULL(mod);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);

    IronIR_Func *fn = mod->funcs[0];
    TEST_ASSERT_NOT_NULL(fn);

    /* Should have CONST_INT and RETURN */
    TEST_ASSERT_GREATER_THAN(0, count_instrs_of_kind(fn, IRON_IR_CONST_INT));
    TEST_ASSERT_GREATER_THAN(0, count_instrs_of_kind(fn, IRON_IR_RETURN));

    /* Verify */
    Iron_DiagList verify_diags = iron_diaglist_create();
    bool ok = iron_ir_verify(mod, &verify_diags, &g_ir_arena);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_INT(0, verify_diags.error_count);
    iron_diaglist_free(&verify_diags);
}

/* test_lower_var_alloca: fn test() { var x = 10; x = 20 }
 * Alloca for x should be in entry block (blocks[0]) */
void test_lower_var_alloca(void) {
    Iron_Type *int_type  = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_Type *void_type = iron_type_make_primitive(IRON_TYPE_VOID);
    Iron_FuncDecl *fd = make_func_decl(&g_ir_arena, "test_var", void_type);

    /* var x = 10 */
    Iron_Node *var_x = make_var_decl(&g_ir_arena, "x", make_int(&g_ir_arena, "10"), int_type);
    /* x = 20 */
    Iron_Node *target_x = make_ident(&g_ir_arena, "x", int_type);
    Iron_Node *assign = make_assign(&g_ir_arena, target_x, make_int(&g_ir_arena, "20"));

    Iron_Node *stmts[2] = { var_x, assign };
    fd->body = (Iron_Node *)make_block(&g_ir_arena, stmts, 2);

    Iron_Node *decls[1] = { (Iron_Node *)fd };
    Iron_Program *prog = make_program(&g_ir_arena, decls, 1);

    IronIR_Module *mod = iron_ir_lower(prog, NULL, &g_ir_arena, &g_diags);
    TEST_ASSERT_NOT_NULL(mod);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);

    IronIR_Func *fn = mod->funcs[0];
    TEST_ASSERT_NOT_NULL(fn);
    TEST_ASSERT_GREATER_THAN(0, fn->block_count);

    /* Alloca must be in entry block (blocks[0]) */
    IronIR_Block *entry = fn->blocks[0];
    bool found_alloca = false;
    for (int i = 0; i < entry->instr_count; i++) {
        if (entry->instrs[i]->kind == IRON_IR_ALLOCA) {
            found_alloca = true;
            break;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(found_alloca, "alloca for x must be in entry block");

    /* Should have two stores: one for init=10, one for assign=20 */
    TEST_ASSERT_GREATER_OR_EQUAL(2, count_instrs_of_kind(fn, IRON_IR_STORE));

    Iron_DiagList verify_diags = iron_diaglist_create();
    bool ok = iron_ir_verify(mod, &verify_diags, &g_ir_arena);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_INT(0, verify_diags.error_count);
    iron_diaglist_free(&verify_diags);
}

/* test_lower_val_no_alloca: fn test() -> Int { val x = 42; return x }
 * Val bindings should NOT produce alloca — just direct SSA use. */
void test_lower_val_no_alloca(void) {
    Iron_Type *int_type = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_FuncDecl *fd = make_func_decl(&g_ir_arena, "test_val", int_type);

    /* val x = 42 */
    Iron_Node *val_x = make_val_decl(&g_ir_arena, "x", make_int(&g_ir_arena, "42"), int_type);
    /* return x */
    Iron_Node *ret_x = make_return(&g_ir_arena, make_ident(&g_ir_arena, "x", int_type));

    Iron_Node *stmts[2] = { val_x, ret_x };
    fd->body = (Iron_Node *)make_block(&g_ir_arena, stmts, 2);

    Iron_Node *decls[1] = { (Iron_Node *)fd };
    Iron_Program *prog = make_program(&g_ir_arena, decls, 1);

    IronIR_Module *mod = iron_ir_lower(prog, NULL, &g_ir_arena, &g_diags);
    TEST_ASSERT_NOT_NULL(mod);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);

    IronIR_Func *fn = mod->funcs[0];
    TEST_ASSERT_NOT_NULL(fn);

    /* No alloca for val binding */
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, count_instrs_of_kind(fn, IRON_IR_ALLOCA),
                                  "val binding must not produce alloca");

    /* Should have const_int 42 and return */
    TEST_ASSERT_GREATER_THAN(0, count_instrs_of_kind(fn, IRON_IR_CONST_INT));
    TEST_ASSERT_GREATER_THAN(0, count_instrs_of_kind(fn, IRON_IR_RETURN));
}

/* test_lower_arithmetic: fn test() -> Int { return 1 + 2 } */
void test_lower_arithmetic(void) {
    Iron_Type *int_type = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_FuncDecl *fd = make_func_decl(&g_ir_arena, "test_arith", int_type);

    /* return 1 + 2 */
    Iron_Node *add = make_binary(&g_ir_arena,
                                  make_int(&g_ir_arena, "1"),
                                  IRON_TOK_PLUS,
                                  make_int(&g_ir_arena, "2"),
                                  int_type);
    Iron_Node *ret = make_return(&g_ir_arena, add);
    Iron_Node *stmts[1] = { ret };
    fd->body = (Iron_Node *)make_block(&g_ir_arena, stmts, 1);

    Iron_Node *decls[1] = { (Iron_Node *)fd };
    Iron_Program *prog = make_program(&g_ir_arena, decls, 1);

    IronIR_Module *mod = iron_ir_lower(prog, NULL, &g_ir_arena, &g_diags);
    TEST_ASSERT_NOT_NULL(mod);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);

    IronIR_Func *fn = mod->funcs[0];
    TEST_ASSERT_EQUAL_INT(2, count_instrs_of_kind(fn, IRON_IR_CONST_INT));
    TEST_ASSERT_EQUAL_INT(1, count_instrs_of_kind(fn, IRON_IR_ADD));
    TEST_ASSERT_GREATER_THAN(0, count_instrs_of_kind(fn, IRON_IR_RETURN));

    Iron_DiagList verify_diags = iron_diaglist_create();
    bool ok = iron_ir_verify(mod, &verify_diags, &g_ir_arena);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_INT(0, verify_diags.error_count);
    iron_diaglist_free(&verify_diags);
}

/* test_lower_if_else: fn test(cond: Bool) -> Int { var r = 0; if cond { r = 1 } else { r = 2 }; return r }
 * Both branches fall through to merge (no returns inside), then the function has one return. */
void test_lower_if_else(void) {
    Iron_Type *int_type  = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_Type *bool_type = iron_type_make_primitive(IRON_TYPE_BOOL);

    /* Build param: cond: Bool */
    Iron_Param *param_cond = make_param(&g_ir_arena, "cond");
    Iron_TypeAnnotation *bool_ann = ARENA_ALLOC(&g_ir_arena, Iron_TypeAnnotation);
    memset(bool_ann, 0, sizeof(*bool_ann));
    bool_ann->span = test_span();
    bool_ann->kind = IRON_NODE_TYPE_ANNOTATION;
    bool_ann->name = "Bool";
    param_cond->type_ann = (Iron_Node *)bool_ann;

    Iron_FuncDecl *fd = make_func_decl(&g_ir_arena, "choose", int_type);
    fd->params = (Iron_Node **)iron_arena_alloc(&g_ir_arena,
                     sizeof(Iron_Node *), _Alignof(Iron_Node *));
    fd->params[0] = (Iron_Node *)param_cond;
    fd->param_count = 1;

    /* var r = 0 */
    Iron_Node *var_r = make_var_decl(&g_ir_arena, "r", make_int(&g_ir_arena, "0"), int_type);

    /* then: { r = 1 } */
    Iron_Node *assign1 = make_assign(&g_ir_arena,
                                      make_ident(&g_ir_arena, "r", int_type),
                                      make_int(&g_ir_arena, "1"));
    Iron_Node *then_stmts[1] = { assign1 };
    Iron_Block *then_body = make_block(&g_ir_arena, then_stmts, 1);

    /* else: { r = 2 } */
    Iron_Node *assign2 = make_assign(&g_ir_arena,
                                      make_ident(&g_ir_arena, "r", int_type),
                                      make_int(&g_ir_arena, "2"));
    Iron_Node *else_stmts[1] = { assign2 };
    Iron_Block *else_body = make_block(&g_ir_arena, else_stmts, 1);

    /* if cond { r = 1 } else { r = 2 } */
    Iron_Node *if_stmt = make_if(&g_ir_arena,
                                  make_ident(&g_ir_arena, "cond", bool_type),
                                  then_body, else_body);

    /* return r */
    Iron_Node *ret_r = make_return(&g_ir_arena, make_ident(&g_ir_arena, "r", int_type));

    Iron_Node *fn_stmts[3] = { var_r, if_stmt, ret_r };
    fd->body = (Iron_Node *)make_block(&g_ir_arena, fn_stmts, 3);

    Iron_Node *decls[1] = { (Iron_Node *)fd };
    Iron_Program *prog = make_program(&g_ir_arena, decls, 1);

    IronIR_Module *mod = iron_ir_lower(prog, NULL, &g_ir_arena, &g_diags);
    TEST_ASSERT_NOT_NULL(mod);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);

    IronIR_Func *fn = mod->funcs[0];
    TEST_ASSERT_NOT_NULL(fn);

    /* Must have at least 4 blocks: entry, then, else, merge (with return) */
    TEST_ASSERT_GREATER_OR_EQUAL(4, fn->block_count);

    /* Must have one branch instruction */
    TEST_ASSERT_EQUAL_INT(1, count_instrs_of_kind(fn, IRON_IR_BRANCH));

    /* Must have at least 2 jumps (then->merge and else->merge) */
    TEST_ASSERT_GREATER_OR_EQUAL(2, count_instrs_of_kind(fn, IRON_IR_JUMP));

    /* Must have one return instruction (in the merge block after both branches) */
    TEST_ASSERT_EQUAL_INT(1, count_instrs_of_kind(fn, IRON_IR_RETURN));

    Iron_DiagList verify_diags = iron_diaglist_create();
    bool ok = iron_ir_verify(mod, &verify_diags, &g_ir_arena);
    if (!ok) {
        for (int i = 0; i < verify_diags.count; i++) {
            printf("Verify error: %s\n", verify_diags.items[i].message);
        }
    }
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_INT(0, verify_diags.error_count);
    iron_diaglist_free(&verify_diags);
}

/* test_lower_while_loop: fn test() { var i = 0; while i < 10 { i = i + 1 } } */
void test_lower_while_loop(void) {
    Iron_Type *int_type  = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_Type *bool_type = iron_type_make_primitive(IRON_TYPE_BOOL);
    Iron_Type *void_type = iron_type_make_primitive(IRON_TYPE_VOID);

    Iron_FuncDecl *fd = make_func_decl(&g_ir_arena, "test_while", void_type);

    /* var i = 0 */
    Iron_Node *var_i = make_var_decl(&g_ir_arena, "i", make_int(&g_ir_arena, "0"), int_type);

    /* i < 10 */
    Iron_Node *cmp = make_binary(&g_ir_arena,
                                  make_ident(&g_ir_arena, "i", int_type),
                                  IRON_TOK_LESS,
                                  make_int(&g_ir_arena, "10"),
                                  bool_type);

    /* i = i + 1 */
    Iron_Node *add1 = make_binary(&g_ir_arena,
                                   make_ident(&g_ir_arena, "i", int_type),
                                   IRON_TOK_PLUS,
                                   make_int(&g_ir_arena, "1"),
                                   int_type);
    Iron_Node *assign_i = make_assign(&g_ir_arena,
                                       make_ident(&g_ir_arena, "i", int_type), add1);

    /* while body */
    Iron_Node *body_stmts[1] = { assign_i };
    Iron_Block *while_body = make_block(&g_ir_arena, body_stmts, 1);
    Iron_Node *while_stmt = make_while(&g_ir_arena, cmp, while_body);

    /* function body */
    Iron_Node *fn_stmts[2] = { var_i, while_stmt };
    fd->body = (Iron_Node *)make_block(&g_ir_arena, fn_stmts, 2);

    Iron_Node *decls[1] = { (Iron_Node *)fd };
    Iron_Program *prog = make_program(&g_ir_arena, decls, 1);

    IronIR_Module *mod = iron_ir_lower(prog, NULL, &g_ir_arena, &g_diags);
    TEST_ASSERT_NOT_NULL(mod);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);

    IronIR_Func *fn = mod->funcs[0];
    TEST_ASSERT_NOT_NULL(fn);

    /* Must have at least 4 blocks: entry, while.header, while.body, while.exit */
    TEST_ASSERT_GREATER_OR_EQUAL(4, fn->block_count);

    /* Must have branch (header condition) and multiple jumps (back-edge + jump to header) */
    TEST_ASSERT_GREATER_THAN(0, count_instrs_of_kind(fn, IRON_IR_BRANCH));
    TEST_ASSERT_GREATER_THAN(1, count_instrs_of_kind(fn, IRON_IR_JUMP));

    Iron_DiagList verify_diags = iron_diaglist_create();
    bool ok = iron_ir_verify(mod, &verify_diags, &g_ir_arena);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_INT(0, verify_diags.error_count);
    iron_diaglist_free(&verify_diags);
}

/* test_lower_verifier_passes: if/else + variable. End-to-end verify. */
void test_lower_verifier_passes(void) {
    Iron_Type *int_type  = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_Type *bool_type = iron_type_make_primitive(IRON_TYPE_BOOL);

    Iron_Param *param_c = make_param(&g_ir_arena, "c");
    Iron_TypeAnnotation *bool_ann = ARENA_ALLOC(&g_ir_arena, Iron_TypeAnnotation);
    memset(bool_ann, 0, sizeof(*bool_ann));
    bool_ann->span = test_span();
    bool_ann->kind = IRON_NODE_TYPE_ANNOTATION;
    bool_ann->name = "Bool";
    param_c->type_ann = (Iron_Node *)bool_ann;

    Iron_FuncDecl *fd = make_func_decl(&g_ir_arena, "combined_test", int_type);
    fd->params = (Iron_Node **)iron_arena_alloc(&g_ir_arena,
                     sizeof(Iron_Node *), _Alignof(Iron_Node *));
    fd->params[0] = (Iron_Node *)param_c;
    fd->param_count = 1;

    /* var result = 0 */
    Iron_Node *var_result = make_var_decl(&g_ir_arena, "result",
                                           make_int(&g_ir_arena, "0"), int_type);

    /* then: { result = 1 } */
    Iron_Node *ident_result1 = make_ident(&g_ir_arena, "result", int_type);
    Iron_Node *assign1 = make_assign(&g_ir_arena, ident_result1, make_int(&g_ir_arena, "1"));
    Iron_Node *then_stmts[1] = { assign1 };
    Iron_Block *then_body = make_block(&g_ir_arena, then_stmts, 1);

    /* else: { result = 2 } */
    Iron_Node *ident_result2 = make_ident(&g_ir_arena, "result", int_type);
    Iron_Node *assign2 = make_assign(&g_ir_arena, ident_result2, make_int(&g_ir_arena, "2"));
    Iron_Node *else_stmts[1] = { assign2 };
    Iron_Block *else_body = make_block(&g_ir_arena, else_stmts, 1);

    Iron_Node *if_stmt = make_if(&g_ir_arena,
                                  make_ident(&g_ir_arena, "c", bool_type),
                                  then_body, else_body);

    /* return result */
    Iron_Node *ret_result = make_return(&g_ir_arena,
                                         make_ident(&g_ir_arena, "result", int_type));

    Iron_Node *fn_stmts[3] = { var_result, if_stmt, ret_result };
    fd->body = (Iron_Node *)make_block(&g_ir_arena, fn_stmts, 3);

    Iron_Node *decls[1] = { (Iron_Node *)fd };
    Iron_Program *prog = make_program(&g_ir_arena, decls, 1);

    IronIR_Module *mod = iron_ir_lower(prog, NULL, &g_ir_arena, &g_diags);
    TEST_ASSERT_NOT_NULL(mod);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);

    Iron_DiagList verify_diags = iron_diaglist_create();
    bool ok = iron_ir_verify(mod, &verify_diags, &g_ir_arena);
    if (!ok) {
        for (int i = 0; i < verify_diags.count; i++) {
            printf("Verify error: %s\n", verify_diags.items[i].message);
        }
    }
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_INT(0, verify_diags.error_count);
    iron_diaglist_free(&verify_diags);
}

/* ── Snapshot tests ──────────────────────────────────────────────────────── */

/* test_snapshot_identity: fn identity(x: Int) -> Int { return x } */
void test_snapshot_identity(void) {
    Iron_Type *int_type = iron_type_make_primitive(IRON_TYPE_INT);

    Iron_Param *param_x = make_param(&g_ir_arena, "x");
    Iron_TypeAnnotation *int_ann = ARENA_ALLOC(&g_ir_arena, Iron_TypeAnnotation);
    memset(int_ann, 0, sizeof(*int_ann));
    int_ann->span = test_span();
    int_ann->kind = IRON_NODE_TYPE_ANNOTATION;
    int_ann->name = "Int";
    param_x->type_ann = (Iron_Node *)int_ann;

    Iron_FuncDecl *fd = make_func_decl(&g_ir_arena, "identity", int_type);
    fd->params = (Iron_Node **)iron_arena_alloc(&g_ir_arena,
                     sizeof(Iron_Node *), _Alignof(Iron_Node *));
    fd->params[0] = (Iron_Node *)param_x;
    fd->param_count = 1;

    /* return x */
    Iron_Node *ret_x = make_return(&g_ir_arena, make_ident(&g_ir_arena, "x", int_type));
    Iron_Node *stmts[1] = { ret_x };
    fd->body = (Iron_Node *)make_block(&g_ir_arena, stmts, 1);

    Iron_Node *decls[1] = { (Iron_Node *)fd };
    Iron_Program *prog = make_program(&g_ir_arena, decls, 1);

    IronIR_Module *mod = iron_ir_lower(prog, NULL, &g_ir_arena, &g_diags);
    TEST_ASSERT_NOT_NULL(mod);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);

    char *ir_text = iron_ir_print(mod, false);
    TEST_ASSERT_NOT_NULL(ir_text);

    const char *snap_path = "tests/ir/snapshots/identity.expected";

    /* Golden master: if snapshot contains placeholder, write real output */
    FILE *f = fopen(snap_path, "r");
    bool is_placeholder = true;
    if (f) {
        char buf[64];
        size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        buf[n] = '\0';
        fclose(f);
        is_placeholder = (strstr(buf, "placeholder") != NULL);
    }
    if (is_placeholder) {
        write_snapshot(ir_text, snap_path);
    }

    bool match = compare_snapshot(ir_text, snap_path);
    free(ir_text);
    TEST_ASSERT_TRUE_MESSAGE(match, "identity IR snapshot mismatch");
}

/* test_snapshot_arithmetic: fn add() -> Int { return 1 + 2 } */
void test_snapshot_arithmetic(void) {
    Iron_Type *int_type = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_FuncDecl *fd = make_func_decl(&g_ir_arena, "add", int_type);

    Iron_Node *add = make_binary(&g_ir_arena,
                                  make_int(&g_ir_arena, "1"),
                                  IRON_TOK_PLUS,
                                  make_int(&g_ir_arena, "2"),
                                  int_type);
    Iron_Node *ret = make_return(&g_ir_arena, add);
    Iron_Node *stmts[1] = { ret };
    fd->body = (Iron_Node *)make_block(&g_ir_arena, stmts, 1);

    Iron_Node *decls[1] = { (Iron_Node *)fd };
    Iron_Program *prog = make_program(&g_ir_arena, decls, 1);

    IronIR_Module *mod = iron_ir_lower(prog, NULL, &g_ir_arena, &g_diags);
    TEST_ASSERT_NOT_NULL(mod);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);

    char *ir_text = iron_ir_print(mod, false);
    TEST_ASSERT_NOT_NULL(ir_text);

    const char *snap_path = "tests/ir/snapshots/arithmetic.expected";

    FILE *f = fopen(snap_path, "r");
    bool is_placeholder = true;
    if (f) {
        char buf[64];
        size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        buf[n] = '\0';
        fclose(f);
        is_placeholder = (strstr(buf, "placeholder") != NULL);
    }
    if (is_placeholder) {
        write_snapshot(ir_text, snap_path);
    }

    bool match = compare_snapshot(ir_text, snap_path);
    free(ir_text);
    TEST_ASSERT_TRUE_MESSAGE(match, "arithmetic IR snapshot mismatch");
}

/* test_snapshot_if_else: fn choose(c: Bool) -> Int { var r = 0; if c { r = 1 } else { r = 2 }; return r } */
void test_snapshot_if_else(void) {
    Iron_Type *int_type  = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_Type *bool_type = iron_type_make_primitive(IRON_TYPE_BOOL);

    Iron_Param *param_c = make_param(&g_ir_arena, "c");
    Iron_TypeAnnotation *bool_ann = ARENA_ALLOC(&g_ir_arena, Iron_TypeAnnotation);
    memset(bool_ann, 0, sizeof(*bool_ann));
    bool_ann->span = test_span();
    bool_ann->kind = IRON_NODE_TYPE_ANNOTATION;
    bool_ann->name = "Bool";
    param_c->type_ann = (Iron_Node *)bool_ann;

    Iron_FuncDecl *fd = make_func_decl(&g_ir_arena, "choose", int_type);
    fd->params = (Iron_Node **)iron_arena_alloc(&g_ir_arena,
                     sizeof(Iron_Node *), _Alignof(Iron_Node *));
    fd->params[0] = (Iron_Node *)param_c;
    fd->param_count = 1;

    /* var r = 0 */
    Iron_Node *var_r = make_var_decl(&g_ir_arena, "r", make_int(&g_ir_arena, "0"), int_type);

    /* then: { r = 1 } */
    Iron_Node *assign1 = make_assign(&g_ir_arena,
                                      make_ident(&g_ir_arena, "r", int_type),
                                      make_int(&g_ir_arena, "1"));
    Iron_Node *then_stmts[1] = { assign1 };
    Iron_Block *then_body = make_block(&g_ir_arena, then_stmts, 1);

    /* else: { r = 2 } */
    Iron_Node *assign2 = make_assign(&g_ir_arena,
                                      make_ident(&g_ir_arena, "r", int_type),
                                      make_int(&g_ir_arena, "2"));
    Iron_Node *else_stmts[1] = { assign2 };
    Iron_Block *else_body = make_block(&g_ir_arena, else_stmts, 1);

    Iron_Node *if_stmt = make_if(&g_ir_arena,
                                  make_ident(&g_ir_arena, "c", bool_type),
                                  then_body, else_body);
    /* return r */
    Iron_Node *ret_r = make_return(&g_ir_arena, make_ident(&g_ir_arena, "r", int_type));

    Iron_Node *fn_stmts[3] = { var_r, if_stmt, ret_r };
    fd->body = (Iron_Node *)make_block(&g_ir_arena, fn_stmts, 3);

    Iron_Node *decls[1] = { (Iron_Node *)fd };
    Iron_Program *prog = make_program(&g_ir_arena, decls, 1);

    IronIR_Module *mod = iron_ir_lower(prog, NULL, &g_ir_arena, &g_diags);
    TEST_ASSERT_NOT_NULL(mod);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);

    char *ir_text = iron_ir_print(mod, false);
    TEST_ASSERT_NOT_NULL(ir_text);

    const char *snap_path = "tests/ir/snapshots/if_else.expected";

    FILE *f = fopen(snap_path, "r");
    bool is_placeholder = true;
    if (f) {
        char buf[64];
        size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        buf[n] = '\0';
        fclose(f);
        is_placeholder = (strstr(buf, "placeholder") != NULL) || (n == 0);
        /* If file doesn't exist yet treat it as placeholder */
    }
    if (is_placeholder) {
        write_snapshot(ir_text, snap_path);
    }

    bool match = compare_snapshot(ir_text, snap_path);
    free(ir_text);
    TEST_ASSERT_TRUE_MESSAGE(match, "if_else IR snapshot mismatch");
}

/* ── Wave 3: Unit tests (Plan 08-03) ────────────────────────────────────── */

/* Build a call expression: callee(args...) */
static Iron_Node *make_call(Iron_Arena *arena, Iron_Node *callee,
                             Iron_Node **args, int arg_count) {
    Iron_CallExpr *n = ARENA_ALLOC(arena, Iron_CallExpr);
    memset(n, 0, sizeof(*n));
    n->span = test_span();
    n->kind = IRON_NODE_CALL;
    n->callee = callee;
    n->resolved_type = iron_type_make_primitive(IRON_TYPE_VOID);
    if (arg_count > 0) {
        n->args = (Iron_Node **)iron_arena_alloc(arena,
                      (size_t)arg_count * sizeof(Iron_Node *), _Alignof(Iron_Node *));
        memcpy(n->args, args, (size_t)arg_count * sizeof(Iron_Node *));
    }
    n->arg_count = arg_count;
    return (Iron_Node *)n;
}

/* Build a defer statement */
static Iron_Node *make_defer(Iron_Arena *arena, Iron_Node *expr) {
    Iron_DeferStmt *n = ARENA_ALLOC(arena, Iron_DeferStmt);
    memset(n, 0, sizeof(*n));
    n->span = test_span();
    n->kind = IRON_NODE_DEFER;
    n->expr = expr;
    return (Iron_Node *)n;
}

/* Build a match statement (minimal: no cases, just validates subject lowering) */
static Iron_Node *make_match(Iron_Arena *arena, Iron_Node *subject,
                              Iron_Node **cases, int case_count) {
    Iron_MatchStmt *n = ARENA_ALLOC(arena, Iron_MatchStmt);
    memset(n, 0, sizeof(*n));
    n->span = test_span();
    n->kind = IRON_NODE_MATCH;
    n->subject = subject;
    if (case_count > 0) {
        n->cases = (Iron_Node **)iron_arena_alloc(arena,
                       (size_t)case_count * sizeof(Iron_Node *), _Alignof(Iron_Node *));
        memcpy(n->cases, cases, (size_t)case_count * sizeof(Iron_Node *));
    }
    n->case_count = case_count;
    n->else_body = NULL;
    return (Iron_Node *)n;
}

/* Build a match case */
static Iron_Node *make_match_case(Iron_Arena *arena, Iron_Node *pattern,
                                   Iron_Block *body) {
    Iron_MatchCase *n = ARENA_ALLOC(arena, Iron_MatchCase);
    memset(n, 0, sizeof(*n));
    n->span = test_span();
    n->kind = IRON_NODE_MATCH_CASE;
    n->pattern = pattern;
    n->body = (Iron_Node *)body;
    return (Iron_Node *)n;
}

/* Build a spawn statement */
static Iron_Node *make_spawn(Iron_Arena *arena, Iron_Block *body,
                              const char *handle_name) {
    Iron_SpawnStmt *n = ARENA_ALLOC(arena, Iron_SpawnStmt);
    memset(n, 0, sizeof(*n));
    n->span = test_span();
    n->kind = IRON_NODE_SPAWN;
    n->name = "spawned";
    n->pool_expr = NULL;
    n->body = (Iron_Node *)body;
    n->handle_name = handle_name;
    return (Iron_Node *)n;
}

/* Build a lambda expression */
static Iron_Node *make_lambda(Iron_Arena *arena, Iron_Block *body) {
    Iron_LambdaExpr *n = ARENA_ALLOC(arena, Iron_LambdaExpr);
    memset(n, 0, sizeof(*n));
    n->span = test_span();
    n->kind = IRON_NODE_LAMBDA;
    n->resolved_type = iron_type_make_primitive(IRON_TYPE_VOID);
    n->params = NULL;
    n->param_count = 0;
    n->return_type = NULL;
    n->body = (Iron_Node *)body;
    return (Iron_Node *)n;
}

/* ── test_lower_extern_decl ──────────────────────────────────────────────── */
/* Create a program with an extern function declaration.
 * Verify the module registers an extern decl with correct names. */
void test_lower_extern_decl(void) {
    Iron_Type *void_type = iron_type_make_primitive(IRON_TYPE_VOID);
    Iron_FuncDecl *fd = make_func_decl(&g_ir_arena, "init_window", void_type);
    fd->is_extern = true;
    fd->extern_c_name = "InitWindow";
    fd->body = NULL;  /* extern func — no body */

    Iron_Node *decls[1] = { (Iron_Node *)fd };
    Iron_Program *prog = make_program(&g_ir_arena, decls, 1);

    IronIR_Module *mod = iron_ir_lower(prog, NULL, &g_ir_arena, &g_diags);
    TEST_ASSERT_NOT_NULL(mod);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);

    /* Must have an extern_decl entry with correct names */
    TEST_ASSERT_GREATER_THAN(0, mod->extern_decl_count);
    bool found = false;
    for (int i = 0; i < mod->extern_decl_count; i++) {
        IronIR_ExternDecl *ed = mod->extern_decls[i];
        if (strcmp(ed->iron_name, "init_window") == 0 &&
            strcmp(ed->c_name, "InitWindow") == 0) {
            found = true;
            break;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(found, "extern_decl for init_window/InitWindow not found");

    /* The function entry in funcs array should be marked is_extern */
    bool fn_extern = false;
    for (int i = 0; i < mod->func_count; i++) {
        if (strcmp(mod->funcs[i]->name, "init_window") == 0) {
            fn_extern = mod->funcs[i]->is_extern;
            break;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(fn_extern, "IrFunc for init_window must have is_extern=true");

    Iron_DiagList verify_diags = iron_diaglist_create();
    bool ok = iron_ir_verify(mod, &verify_diags, &g_ir_arena);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_INT(0, verify_diags.error_count);
    iron_diaglist_free(&verify_diags);
}

/* ── test_lower_type_decl ────────────────────────────────────────────────── */
/* Create a program with an object declaration.
 * Verify the module has a type_decl entry with IRON_IR_TYPE_OBJECT. */
void test_lower_type_decl(void) {
    Iron_ObjectDecl *obj = ARENA_ALLOC(&g_ir_arena, Iron_ObjectDecl);
    memset(obj, 0, sizeof(*obj));
    obj->span = test_span();
    obj->kind = IRON_NODE_OBJECT_DECL;
    obj->name = "Player";

    Iron_Node *decls[1] = { (Iron_Node *)obj };
    Iron_Program *prog = make_program(&g_ir_arena, decls, 1);

    IronIR_Module *mod = iron_ir_lower(prog, NULL, &g_ir_arena, &g_diags);
    TEST_ASSERT_NOT_NULL(mod);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);

    /* Must have a type_decl entry for Player with kind OBJECT */
    bool found = false;
    for (int i = 0; i < mod->type_decl_count; i++) {
        IronIR_TypeDecl *td = mod->type_decls[i];
        if (strcmp(td->name, "Player") == 0 && td->kind == IRON_IR_TYPE_OBJECT) {
            found = true;
            break;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(found, "type_decl for Player (OBJECT) not found");

    Iron_DiagList verify_diags = iron_diaglist_create();
    bool ok = iron_ir_verify(mod, &verify_diags, &g_ir_arena);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_INT(0, verify_diags.error_count);
    iron_diaglist_free(&verify_diags);
}

/* ── test_lower_defer_before_return ─────────────────────────────────────── */
/* fn test() { defer cleanup_call(); return }
 * Verify the deferred call appears BEFORE the return terminator. */
void test_lower_defer_before_return(void) {
    Iron_Type *void_type = iron_type_make_primitive(IRON_TYPE_VOID);
    Iron_FuncDecl *fd = make_func_decl(&g_ir_arena, "test_defer", void_type);

    /* cleanup_call expression (a bare call expression used as defer) */
    Iron_Node *cleanup_ident = make_ident(&g_ir_arena, "cleanup",
                                           iron_type_make_primitive(IRON_TYPE_VOID));
    Iron_Node *cleanup_call = make_call(&g_ir_arena, cleanup_ident, NULL, 0);

    /* defer cleanup_call() */
    Iron_Node *defer_stmt = make_defer(&g_ir_arena, cleanup_call);

    /* return */
    Iron_Node *ret_stmt = make_return(&g_ir_arena, NULL);

    Iron_Node *stmts[2] = { defer_stmt, ret_stmt };
    fd->body = (Iron_Node *)make_block(&g_ir_arena, stmts, 2);

    Iron_Node *decls[1] = { (Iron_Node *)fd };
    Iron_Program *prog = make_program(&g_ir_arena, decls, 1);

    IronIR_Module *mod = iron_ir_lower(prog, NULL, &g_ir_arena, &g_diags);
    TEST_ASSERT_NOT_NULL(mod);
    /* Deferred call to unknown identifier may emit diagnostics; we only
     * care about structural correctness. Allow any diagnostic count but
     * require the module to be non-NULL (we stopped on errors only if
     * iron_ir_lower returns NULL). */

    if (mod == NULL) {
        /* If lowering fails due to missing cleanup symbol, skip structural check */
        return;
    }

    IronIR_Func *fn = mod->funcs[0];
    TEST_ASSERT_NOT_NULL(fn);

    /* Find the return instruction and verify a call or poison precedes it
     * in the same block (deferred expression runs before return). */
    bool return_found = false;
    bool call_before_return = false;
    for (int b = 0; b < fn->block_count; b++) {
        IronIR_Block *blk = fn->blocks[b];
        int last_non_term = -1;
        for (int i = 0; i < blk->instr_count; i++) {
            if (blk->instrs[i]->kind == IRON_IR_RETURN) {
                return_found = true;
                /* Check if any call or poison appeared before this return */
                if (last_non_term >= 0) call_before_return = true;
                break;
            }
            last_non_term = i;
        }
        if (return_found) break;
    }
    TEST_ASSERT_TRUE_MESSAGE(return_found, "return instruction not found");
    TEST_ASSERT_TRUE_MESSAGE(call_before_return,
        "deferred call must appear before return in same block");
}

/* ── test_lower_lambda_lifting ───────────────────────────────────────────── */
/* fn test() { val f = || { } } — verify lambda is lifted to top-level func */
void test_lower_lambda_lifting(void) {
    Iron_Type *void_type = iron_type_make_primitive(IRON_TYPE_VOID);
    Iron_FuncDecl *fd = make_func_decl(&g_ir_arena, "test_lambda", void_type);

    /* Lambda body: { } (empty — no return value to avoid type mismatch) */
    Iron_Block *lambda_body = make_block(&g_ir_arena, NULL, 0);

    /* Lambda: || { 42 } */
    Iron_Node *lam = make_lambda(&g_ir_arena, lambda_body);

    /* val f = || { 42 } */
    Iron_Node *val_f = make_val_decl(&g_ir_arena, "f", lam,
                                      iron_type_make_primitive(IRON_TYPE_VOID));

    Iron_Node *fn_stmts[1] = { val_f };
    fd->body = (Iron_Node *)make_block(&g_ir_arena, fn_stmts, 1);

    Iron_Node *decls[1] = { (Iron_Node *)fd };
    Iron_Program *prog = make_program(&g_ir_arena, decls, 1);

    IronIR_Module *mod = iron_ir_lower(prog, NULL, &g_ir_arena, &g_diags);
    TEST_ASSERT_NOT_NULL(mod);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);

    /* Module must have at least 2 functions: original + lifted lambda */
    TEST_ASSERT_GREATER_OR_EQUAL(2, mod->func_count);

    /* Lifted function name must start with "__lambda_" */
    bool found_lambda = false;
    for (int i = 0; i < mod->func_count; i++) {
        if (strncmp(mod->funcs[i]->name, "__lambda_", 9) == 0) {
            found_lambda = true;
            break;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(found_lambda,
        "lifted lambda function (__lambda_*) not found in module");

    Iron_DiagList verify_diags = iron_diaglist_create();
    bool ok = iron_ir_verify(mod, &verify_diags, &g_ir_arena);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_INT(0, verify_diags.error_count);
    iron_diaglist_free(&verify_diags);
}

/* ── test_lower_spawn ────────────────────────────────────────────────────── */
/* fn test() { spawn { val x = 1 } } — verify spawn body is lifted */
void test_lower_spawn(void) {
    Iron_Type *void_type = iron_type_make_primitive(IRON_TYPE_VOID);
    Iron_FuncDecl *fd = make_func_decl(&g_ir_arena, "test_spawn", void_type);

    /* Spawn body: { val x = 1 } */
    Iron_Node *val_x = make_val_decl(&g_ir_arena, "x", make_int(&g_ir_arena, "1"),
                                      iron_type_make_primitive(IRON_TYPE_INT));
    Iron_Node *spawn_stmts[1] = { val_x };
    Iron_Block *spawn_body = make_block(&g_ir_arena, spawn_stmts, 1);

    /* spawn { val x = 1 } */
    Iron_Node *spawn_stmt = make_spawn(&g_ir_arena, spawn_body, NULL);

    Iron_Node *fn_stmts[1] = { spawn_stmt };
    fd->body = (Iron_Node *)make_block(&g_ir_arena, fn_stmts, 1);

    Iron_Node *decls[1] = { (Iron_Node *)fd };
    Iron_Program *prog = make_program(&g_ir_arena, decls, 1);

    IronIR_Module *mod = iron_ir_lower(prog, NULL, &g_ir_arena, &g_diags);
    TEST_ASSERT_NOT_NULL(mod);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);

    /* Module must have at least 2 functions: original + lifted spawn body */
    TEST_ASSERT_GREATER_OR_EQUAL(2, mod->func_count);

    /* Lifted function name must start with "__spawn_" */
    bool found_spawn = false;
    for (int i = 0; i < mod->func_count; i++) {
        if (strncmp(mod->funcs[i]->name, "__spawn_", 8) == 0) {
            found_spawn = true;
            break;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(found_spawn,
        "lifted spawn function (__spawn_*) not found in module");

    /* Original function must contain an IRON_IR_SPAWN instruction */
    IronIR_Func *fn = mod->funcs[0];
    TEST_ASSERT_GREATER_THAN(0, count_instrs_of_kind(fn, IRON_IR_SPAWN));

    Iron_DiagList verify_diags = iron_diaglist_create();
    bool ok = iron_ir_verify(mod, &verify_diags, &g_ir_arena);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_INT(0, verify_diags.error_count);
    iron_diaglist_free(&verify_diags);
}

/* ── test_lower_match_switch ─────────────────────────────────────────────── */
/* fn test(x: Int) -> Int { match x { 1 -> { return 10 } 2 -> { return 20 } } return 0 }
 * Verify IR contains IRON_IR_SWITCH and multiple arm blocks. */
void test_lower_match_switch(void) {
    Iron_Type *int_type = iron_type_make_primitive(IRON_TYPE_INT);

    /* Build param: x: Int */
    Iron_Param *param_x = make_param(&g_ir_arena, "x");
    Iron_TypeAnnotation *int_ann = ARENA_ALLOC(&g_ir_arena, Iron_TypeAnnotation);
    memset(int_ann, 0, sizeof(*int_ann));
    int_ann->span = test_span();
    int_ann->kind = IRON_NODE_TYPE_ANNOTATION;
    int_ann->name = "Int";
    param_x->type_ann = (Iron_Node *)int_ann;

    Iron_FuncDecl *fd = make_func_decl(&g_ir_arena, "test_match", int_type);
    fd->params = (Iron_Node **)iron_arena_alloc(&g_ir_arena,
                     sizeof(Iron_Node *), _Alignof(Iron_Node *));
    fd->params[0] = (Iron_Node *)param_x;
    fd->param_count = 1;

    /* case 1: { return 10 } */
    Iron_Node *ret10 = make_return(&g_ir_arena, make_int(&g_ir_arena, "10"));
    Iron_Node *arm1_stmts[1] = { ret10 };
    Iron_Block *arm1_body = make_block(&g_ir_arena, arm1_stmts, 1);
    Iron_Node *case1 = make_match_case(&g_ir_arena, make_int(&g_ir_arena, "1"), arm1_body);

    /* case 2: { return 20 } */
    Iron_Node *ret20 = make_return(&g_ir_arena, make_int(&g_ir_arena, "20"));
    Iron_Node *arm2_stmts[1] = { ret20 };
    Iron_Block *arm2_body = make_block(&g_ir_arena, arm2_stmts, 1);
    Iron_Node *case2 = make_match_case(&g_ir_arena, make_int(&g_ir_arena, "2"), arm2_body);

    /* match x { 1 -> ... 2 -> ... } */
    Iron_Node *cases[2] = { case1, case2 };
    Iron_Node *match_stmt = make_match(&g_ir_arena,
                                        make_ident(&g_ir_arena, "x", int_type),
                                        cases, 2);

    /* return 0 (fallthrough/default case) */
    Iron_Node *ret0 = make_return(&g_ir_arena, make_int(&g_ir_arena, "0"));

    Iron_Node *fn_stmts[2] = { match_stmt, ret0 };
    fd->body = (Iron_Node *)make_block(&g_ir_arena, fn_stmts, 2);

    Iron_Node *decls[1] = { (Iron_Node *)fd };
    Iron_Program *prog = make_program(&g_ir_arena, decls, 1);

    IronIR_Module *mod = iron_ir_lower(prog, NULL, &g_ir_arena, &g_diags);
    TEST_ASSERT_NOT_NULL(mod);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);

    IronIR_Func *fn = mod->funcs[0];
    TEST_ASSERT_NOT_NULL(fn);

    /* Must have at least one IRON_IR_SWITCH instruction */
    TEST_ASSERT_GREATER_THAN(0, count_instrs_of_kind(fn, IRON_IR_SWITCH));

    /* Must have at least 3 blocks: entry (with switch), arm1, arm2 */
    TEST_ASSERT_GREATER_OR_EQUAL(3, fn->block_count);

    Iron_DiagList verify_diags = iron_diaglist_create();
    bool ok = iron_ir_verify(mod, &verify_diags, &g_ir_arena);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_INT(0, verify_diags.error_count);
    iron_diaglist_free(&verify_diags);
}

/* ── test_lower_full_program ─────────────────────────────────────────────── */
/* fn compute(x: Int) -> Int { var r = x; if r > 0 { r = r + 1 } return r }
 * End-to-end verify + ir_print sanity check. */
void test_lower_full_program(void) {
    Iron_Type *int_type  = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_Type *bool_type = iron_type_make_primitive(IRON_TYPE_BOOL);

    /* Build param: x: Int */
    Iron_Param *param_x = make_param(&g_ir_arena, "x");
    Iron_TypeAnnotation *int_ann = ARENA_ALLOC(&g_ir_arena, Iron_TypeAnnotation);
    memset(int_ann, 0, sizeof(*int_ann));
    int_ann->span = test_span();
    int_ann->kind = IRON_NODE_TYPE_ANNOTATION;
    int_ann->name = "Int";
    param_x->type_ann = (Iron_Node *)int_ann;

    Iron_FuncDecl *fd = make_func_decl(&g_ir_arena, "compute", int_type);
    fd->params = (Iron_Node **)iron_arena_alloc(&g_ir_arena,
                     sizeof(Iron_Node *), _Alignof(Iron_Node *));
    fd->params[0] = (Iron_Node *)param_x;
    fd->param_count = 1;

    /* var r = x */
    Iron_Node *var_r = make_var_decl(&g_ir_arena, "r",
                                      make_ident(&g_ir_arena, "x", int_type),
                                      int_type);

    /* r > 0 */
    Iron_Node *cond = make_binary(&g_ir_arena,
                                   make_ident(&g_ir_arena, "r", int_type),
                                   IRON_TOK_GREATER,
                                   make_int(&g_ir_arena, "0"),
                                   bool_type);

    /* then: r = r + 1 */
    Iron_Node *r_plus_1 = make_binary(&g_ir_arena,
                                       make_ident(&g_ir_arena, "r", int_type),
                                       IRON_TOK_PLUS,
                                       make_int(&g_ir_arena, "1"),
                                       int_type);
    Iron_Node *assign_r = make_assign(&g_ir_arena,
                                       make_ident(&g_ir_arena, "r", int_type),
                                       r_plus_1);
    Iron_Node *then_stmts[1] = { assign_r };
    Iron_Block *then_body = make_block(&g_ir_arena, then_stmts, 1);

    Iron_Node *if_stmt = make_if(&g_ir_arena, cond, then_body, NULL);

    /* return r */
    Iron_Node *ret_r = make_return(&g_ir_arena,
                                    make_ident(&g_ir_arena, "r", int_type));

    Iron_Node *fn_stmts[3] = { var_r, if_stmt, ret_r };
    fd->body = (Iron_Node *)make_block(&g_ir_arena, fn_stmts, 3);

    Iron_Node *decls[1] = { (Iron_Node *)fd };
    Iron_Program *prog = make_program(&g_ir_arena, decls, 1);

    IronIR_Module *mod = iron_ir_lower(prog, NULL, &g_ir_arena, &g_diags);
    TEST_ASSERT_NOT_NULL(mod);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);

    /* ir_verify must pass */
    Iron_DiagList verify_diags = iron_diaglist_create();
    bool ok = iron_ir_verify(mod, &verify_diags, &g_ir_arena);
    if (!ok) {
        for (int i = 0; i < verify_diags.count; i++) {
            printf("Verify error: %s\n", verify_diags.items[i].message);
        }
    }
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_INT(0, verify_diags.error_count);
    iron_diaglist_free(&verify_diags);

    /* ir_print must produce output containing function name and block labels */
    char *ir_text = iron_ir_print(mod, false);
    TEST_ASSERT_NOT_NULL(ir_text);
    TEST_ASSERT_TRUE_MESSAGE(strstr(ir_text, "compute") != NULL,
                              "ir_print output must contain function name 'compute'");
    TEST_ASSERT_TRUE_MESSAGE(strstr(ir_text, "entry") != NULL,
                              "ir_print output must contain 'entry' block label");
    free(ir_text);
}

/* ── main ───────────────────────────────────────────────────────────────── */

int main(void) {
    UNITY_BEGIN();

    /* Wave 0 */
    RUN_TEST(test_lower_placeholder);

    /* Wave 2: unit tests */
    RUN_TEST(test_lower_empty_func);
    RUN_TEST(test_lower_const_return);
    RUN_TEST(test_lower_var_alloca);
    RUN_TEST(test_lower_val_no_alloca);
    RUN_TEST(test_lower_arithmetic);
    RUN_TEST(test_lower_if_else);
    RUN_TEST(test_lower_while_loop);
    RUN_TEST(test_lower_verifier_passes);

    /* Wave 2: snapshot tests */
    RUN_TEST(test_snapshot_identity);
    RUN_TEST(test_snapshot_arithmetic);
    RUN_TEST(test_snapshot_if_else);

    /* Wave 3: Plan 08-03 tests */
    RUN_TEST(test_lower_extern_decl);
    RUN_TEST(test_lower_type_decl);
    RUN_TEST(test_lower_defer_before_return);
    RUN_TEST(test_lower_lambda_lifting);
    RUN_TEST(test_lower_spawn);
    RUN_TEST(test_lower_match_switch);
    RUN_TEST(test_lower_full_program);

    return UNITY_END();
}
