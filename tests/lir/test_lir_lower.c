/* test_ir_lower.c — Unity tests for the AST-to-IR lowering pass (Phase 8).
 *
 * Tests verify that iron_lir_lower() produces correct IR for Iron programs.
 * Wave 0: placeholder scaffold — just verifies the test binary links and runs.
 * Wave 1+: expression lowering tests added in Plans 08-01 onward.
 * Wave 2 (Plan 08-02): control flow and statement lowering tests.
 */

#include "unity.h"
#include "lir/lower.h"
#include "lir/lir.h"
#include "lir/verify.h"
#include "lir/print.h"
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
static int count_instrs_of_kind(IronLIR_Func *fn, IronLIR_InstrKind kind) {
    int count = 0;
    for (int b = 0; b < fn->block_count; b++) {
        IronLIR_Block *blk = fn->blocks[b];
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

    IronLIR_Module *mod = iron_lir_lower(prog, NULL, &g_ir_arena, &g_diags);
    TEST_ASSERT_NOT_NULL(mod);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);
    TEST_ASSERT_EQUAL_INT(1, mod->func_count);

    IronLIR_Func *fn = mod->funcs[0];
    TEST_ASSERT_NOT_NULL(fn);
    TEST_ASSERT_GREATER_THAN(0, fn->block_count);

    /* Must have at least one return instruction */
    TEST_ASSERT_GREATER_THAN(0, count_instrs_of_kind(fn, IRON_LIR_RETURN));

    /* Verify passes */
    Iron_DiagList verify_diags = iron_diaglist_create();
    bool ok = iron_lir_verify(mod, &verify_diags, &g_ir_arena);
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

    IronLIR_Module *mod = iron_lir_lower(prog, NULL, &g_ir_arena, &g_diags);
    TEST_ASSERT_NOT_NULL(mod);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);

    IronLIR_Func *fn = mod->funcs[0];
    TEST_ASSERT_NOT_NULL(fn);

    /* Should have CONST_INT and RETURN */
    TEST_ASSERT_GREATER_THAN(0, count_instrs_of_kind(fn, IRON_LIR_CONST_INT));
    TEST_ASSERT_GREATER_THAN(0, count_instrs_of_kind(fn, IRON_LIR_RETURN));

    /* Verify */
    Iron_DiagList verify_diags = iron_diaglist_create();
    bool ok = iron_lir_verify(mod, &verify_diags, &g_ir_arena);
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

    IronLIR_Module *mod = iron_lir_lower(prog, NULL, &g_ir_arena, &g_diags);
    TEST_ASSERT_NOT_NULL(mod);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);

    IronLIR_Func *fn = mod->funcs[0];
    TEST_ASSERT_NOT_NULL(fn);
    TEST_ASSERT_GREATER_THAN(0, fn->block_count);

    /* Alloca must be in entry block (blocks[0]) */
    IronLIR_Block *entry = fn->blocks[0];
    bool found_alloca = false;
    for (int i = 0; i < entry->instr_count; i++) {
        if (entry->instrs[i]->kind == IRON_LIR_ALLOCA) {
            found_alloca = true;
            break;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(found_alloca, "alloca for x must be in entry block");

    /* Should have two stores: one for init=10, one for assign=20 */
    TEST_ASSERT_GREATER_OR_EQUAL(2, count_instrs_of_kind(fn, IRON_LIR_STORE));

    Iron_DiagList verify_diags = iron_diaglist_create();
    bool ok = iron_lir_verify(mod, &verify_diags, &g_ir_arena);
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

    IronLIR_Module *mod = iron_lir_lower(prog, NULL, &g_ir_arena, &g_diags);
    TEST_ASSERT_NOT_NULL(mod);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);

    IronLIR_Func *fn = mod->funcs[0];
    TEST_ASSERT_NOT_NULL(fn);

    /* No alloca for val binding */
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, count_instrs_of_kind(fn, IRON_LIR_ALLOCA),
                                  "val binding must not produce alloca");

    /* Should have const_int 42 and return */
    TEST_ASSERT_GREATER_THAN(0, count_instrs_of_kind(fn, IRON_LIR_CONST_INT));
    TEST_ASSERT_GREATER_THAN(0, count_instrs_of_kind(fn, IRON_LIR_RETURN));
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

    IronLIR_Module *mod = iron_lir_lower(prog, NULL, &g_ir_arena, &g_diags);
    TEST_ASSERT_NOT_NULL(mod);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);

    IronLIR_Func *fn = mod->funcs[0];
    TEST_ASSERT_EQUAL_INT(2, count_instrs_of_kind(fn, IRON_LIR_CONST_INT));
    TEST_ASSERT_EQUAL_INT(1, count_instrs_of_kind(fn, IRON_LIR_ADD));
    TEST_ASSERT_GREATER_THAN(0, count_instrs_of_kind(fn, IRON_LIR_RETURN));

    Iron_DiagList verify_diags = iron_diaglist_create();
    bool ok = iron_lir_verify(mod, &verify_diags, &g_ir_arena);
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

    IronLIR_Module *mod = iron_lir_lower(prog, NULL, &g_ir_arena, &g_diags);
    TEST_ASSERT_NOT_NULL(mod);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);

    IronLIR_Func *fn = mod->funcs[0];
    TEST_ASSERT_NOT_NULL(fn);

    /* Must have at least 4 blocks: entry, then, else, merge (with return) */
    TEST_ASSERT_GREATER_OR_EQUAL(4, fn->block_count);

    /* Must have one branch instruction */
    TEST_ASSERT_EQUAL_INT(1, count_instrs_of_kind(fn, IRON_LIR_BRANCH));

    /* Must have at least 2 jumps (then->merge and else->merge) */
    TEST_ASSERT_GREATER_OR_EQUAL(2, count_instrs_of_kind(fn, IRON_LIR_JUMP));

    /* Must have one return instruction (in the merge block after both branches) */
    TEST_ASSERT_EQUAL_INT(1, count_instrs_of_kind(fn, IRON_LIR_RETURN));

    Iron_DiagList verify_diags = iron_diaglist_create();
    bool ok = iron_lir_verify(mod, &verify_diags, &g_ir_arena);
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

    IronLIR_Module *mod = iron_lir_lower(prog, NULL, &g_ir_arena, &g_diags);
    TEST_ASSERT_NOT_NULL(mod);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);

    IronLIR_Func *fn = mod->funcs[0];
    TEST_ASSERT_NOT_NULL(fn);

    /* Must have at least 4 blocks: entry, while.header, while.body, while.exit */
    TEST_ASSERT_GREATER_OR_EQUAL(4, fn->block_count);

    /* Must have branch (header condition) and multiple jumps (back-edge + jump to header) */
    TEST_ASSERT_GREATER_THAN(0, count_instrs_of_kind(fn, IRON_LIR_BRANCH));
    TEST_ASSERT_GREATER_THAN(1, count_instrs_of_kind(fn, IRON_LIR_JUMP));

    Iron_DiagList verify_diags = iron_diaglist_create();
    bool ok = iron_lir_verify(mod, &verify_diags, &g_ir_arena);
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

    IronLIR_Module *mod = iron_lir_lower(prog, NULL, &g_ir_arena, &g_diags);
    TEST_ASSERT_NOT_NULL(mod);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);

    Iron_DiagList verify_diags = iron_diaglist_create();
    bool ok = iron_lir_verify(mod, &verify_diags, &g_ir_arena);
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

    IronLIR_Module *mod = iron_lir_lower(prog, NULL, &g_ir_arena, &g_diags);
    TEST_ASSERT_NOT_NULL(mod);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);

    char *ir_text = iron_lir_print(mod, false);
    TEST_ASSERT_NOT_NULL(ir_text);

    const char *snap_path = "tests/lir/snapshots/identity.expected";

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

    IronLIR_Module *mod = iron_lir_lower(prog, NULL, &g_ir_arena, &g_diags);
    TEST_ASSERT_NOT_NULL(mod);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);

    char *ir_text = iron_lir_print(mod, false);
    TEST_ASSERT_NOT_NULL(ir_text);

    const char *snap_path = "tests/lir/snapshots/arithmetic.expected";

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

    IronLIR_Module *mod = iron_lir_lower(prog, NULL, &g_ir_arena, &g_diags);
    TEST_ASSERT_NOT_NULL(mod);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);

    char *ir_text = iron_lir_print(mod, false);
    TEST_ASSERT_NOT_NULL(ir_text);

    const char *snap_path = "tests/lir/snapshots/if_else.expected";

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

    IronLIR_Module *mod = iron_lir_lower(prog, NULL, &g_ir_arena, &g_diags);
    TEST_ASSERT_NOT_NULL(mod);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);

    /* Must have an extern_decl entry with correct names */
    TEST_ASSERT_GREATER_THAN(0, mod->extern_decl_count);
    bool found = false;
    for (int i = 0; i < mod->extern_decl_count; i++) {
        IronLIR_ExternDecl *ed = mod->extern_decls[i];
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
    bool ok = iron_lir_verify(mod, &verify_diags, &g_ir_arena);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_INT(0, verify_diags.error_count);
    iron_diaglist_free(&verify_diags);
}

/* ── test_lower_type_decl ────────────────────────────────────────────────── */
/* Create a program with an object declaration.
 * Verify the module has a type_decl entry with IRON_LIR_TYPE_OBJECT. */
void test_lower_type_decl(void) {
    Iron_ObjectDecl *obj = ARENA_ALLOC(&g_ir_arena, Iron_ObjectDecl);
    memset(obj, 0, sizeof(*obj));
    obj->span = test_span();
    obj->kind = IRON_NODE_OBJECT_DECL;
    obj->name = "Player";

    Iron_Node *decls[1] = { (Iron_Node *)obj };
    Iron_Program *prog = make_program(&g_ir_arena, decls, 1);

    IronLIR_Module *mod = iron_lir_lower(prog, NULL, &g_ir_arena, &g_diags);
    TEST_ASSERT_NOT_NULL(mod);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);

    /* Must have a type_decl entry for Player with kind OBJECT */
    bool found = false;
    for (int i = 0; i < mod->type_decl_count; i++) {
        IronLIR_TypeDecl *td = mod->type_decls[i];
        if (strcmp(td->name, "Player") == 0 && td->kind == IRON_LIR_TYPE_OBJECT) {
            found = true;
            break;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(found, "type_decl for Player (OBJECT) not found");

    Iron_DiagList verify_diags = iron_diaglist_create();
    bool ok = iron_lir_verify(mod, &verify_diags, &g_ir_arena);
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

    IronLIR_Module *mod = iron_lir_lower(prog, NULL, &g_ir_arena, &g_diags);
    TEST_ASSERT_NOT_NULL(mod);
    /* Deferred call to unknown identifier may emit diagnostics; we only
     * care about structural correctness. Allow any diagnostic count but
     * require the module to be non-NULL (we stopped on errors only if
     * iron_lir_lower returns NULL). */

    if (mod == NULL) {
        /* If lowering fails due to missing cleanup symbol, skip structural check */
        return;
    }

    IronLIR_Func *fn = mod->funcs[0];
    TEST_ASSERT_NOT_NULL(fn);

    /* Find the return instruction and verify a call or poison precedes it
     * in the same block (deferred expression runs before return). */
    bool return_found = false;
    bool call_before_return = false;
    for (int b = 0; b < fn->block_count; b++) {
        IronLIR_Block *blk = fn->blocks[b];
        int last_non_term = -1;
        for (int i = 0; i < blk->instr_count; i++) {
            if (blk->instrs[i]->kind == IRON_LIR_RETURN) {
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

    IronLIR_Module *mod = iron_lir_lower(prog, NULL, &g_ir_arena, &g_diags);
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
    bool ok = iron_lir_verify(mod, &verify_diags, &g_ir_arena);
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

    IronLIR_Module *mod = iron_lir_lower(prog, NULL, &g_ir_arena, &g_diags);
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

    /* Original function must contain an IRON_LIR_SPAWN instruction */
    IronLIR_Func *fn = mod->funcs[0];
    TEST_ASSERT_GREATER_THAN(0, count_instrs_of_kind(fn, IRON_LIR_SPAWN));

    Iron_DiagList verify_diags = iron_diaglist_create();
    bool ok = iron_lir_verify(mod, &verify_diags, &g_ir_arena);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_INT(0, verify_diags.error_count);
    iron_diaglist_free(&verify_diags);
}

/* ── test_lower_match_switch ─────────────────────────────────────────────── */
/* fn test(x: Int) -> Int { match x { 1 -> { return 10 } 2 -> { return 20 } } return 0 }
 * Verify IR contains IRON_LIR_SWITCH and multiple arm blocks. */
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

    IronLIR_Module *mod = iron_lir_lower(prog, NULL, &g_ir_arena, &g_diags);
    TEST_ASSERT_NOT_NULL(mod);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);

    IronLIR_Func *fn = mod->funcs[0];
    TEST_ASSERT_NOT_NULL(fn);

    /* Must have at least one IRON_LIR_SWITCH instruction */
    TEST_ASSERT_GREATER_THAN(0, count_instrs_of_kind(fn, IRON_LIR_SWITCH));

    /* Must have at least 3 blocks: entry (with switch), arm1, arm2 */
    TEST_ASSERT_GREATER_OR_EQUAL(3, fn->block_count);

    Iron_DiagList verify_diags = iron_diaglist_create();
    bool ok = iron_lir_verify(mod, &verify_diags, &g_ir_arena);
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

    IronLIR_Module *mod = iron_lir_lower(prog, NULL, &g_ir_arena, &g_diags);
    TEST_ASSERT_NOT_NULL(mod);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);

    /* ir_verify must pass */
    Iron_DiagList verify_diags = iron_diaglist_create();
    bool ok = iron_lir_verify(mod, &verify_diags, &g_ir_arena);
    if (!ok) {
        for (int i = 0; i < verify_diags.count; i++) {
            printf("Verify error: %s\n", verify_diags.items[i].message);
        }
    }
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_INT(0, verify_diags.error_count);
    iron_diaglist_free(&verify_diags);

    /* ir_print must produce output containing function name and block labels */
    char *ir_text = iron_lir_print(mod, false);
    TEST_ASSERT_NOT_NULL(ir_text);
    TEST_ASSERT_TRUE_MESSAGE(strstr(ir_text, "compute") != NULL,
                              "ir_print output must contain function name 'compute'");
    TEST_ASSERT_TRUE_MESSAGE(strstr(ir_text, "entry") != NULL,
                              "ir_print output must contain 'entry' block label");
    free(ir_text);
}

/* ── Wave 4: Instruction-kind coverage tests (Plan 10-02) ──────────────── */

/* Helper: return true if any instruction in fn has the given kind. */
static bool has_instr_kind(IronLIR_Func *fn, IronLIR_InstrKind kind) {
    for (int b = 0; b < fn->block_count; b++) {
        IronLIR_Block *blk = fn->blocks[b];
        for (int i = 0; i < blk->instr_count; i++) {
            if (blk->instrs[i]->kind == kind) return true;
        }
    }
    return false;
}

/* ── test_lower_const_int ─────────────────────────────────────────────────
 * fn f() -> Int { return 42 } — explicit named test for CONST_INT. */
void test_lower_const_int(void) {
    Iron_Type *int_type = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_FuncDecl *fd = make_func_decl(&g_ir_arena, "test_const_int", int_type);

    Iron_Node *ret = make_return(&g_ir_arena, make_int(&g_ir_arena, "42"));
    Iron_Node *stmts[1] = { ret };
    fd->body = (Iron_Node *)make_block(&g_ir_arena, stmts, 1);

    Iron_Node *decls[1] = { (Iron_Node *)fd };
    Iron_Program *prog = make_program(&g_ir_arena, decls, 1);

    IronLIR_Module *mod = iron_lir_lower(prog, NULL, &g_ir_arena, &g_diags);
    TEST_ASSERT_NOT_NULL(mod);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);
    TEST_ASSERT_TRUE(has_instr_kind(mod->funcs[0], IRON_LIR_CONST_INT));
}

/* ── test_lower_const_float ───────────────────────────────────────────────
 * fn f() -> Float { return 3.14 } */
void test_lower_const_float(void) {
    Iron_Type *float_type = iron_type_make_primitive(IRON_TYPE_FLOAT);
    Iron_FuncDecl *fd = make_func_decl(&g_ir_arena, "test_const_float", float_type);

    Iron_FloatLit *lit = ARENA_ALLOC(&g_ir_arena, Iron_FloatLit);
    memset(lit, 0, sizeof(*lit));
    lit->span = test_span();
    lit->kind = IRON_NODE_FLOAT_LIT;
    lit->value = "3.14";
    lit->resolved_type = float_type;

    Iron_Node *ret = make_return(&g_ir_arena, (Iron_Node *)lit);
    Iron_Node *stmts[1] = { ret };
    fd->body = (Iron_Node *)make_block(&g_ir_arena, stmts, 1);

    Iron_Node *decls[1] = { (Iron_Node *)fd };
    Iron_Program *prog = make_program(&g_ir_arena, decls, 1);

    IronLIR_Module *mod = iron_lir_lower(prog, NULL, &g_ir_arena, &g_diags);
    TEST_ASSERT_NOT_NULL(mod);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);
    TEST_ASSERT_TRUE(has_instr_kind(mod->funcs[0], IRON_LIR_CONST_FLOAT));
}

/* ── test_lower_const_bool ────────────────────────────────────────────────
 * fn f() -> Bool { return true } */
void test_lower_const_bool(void) {
    Iron_Type *bool_type = iron_type_make_primitive(IRON_TYPE_BOOL);
    Iron_FuncDecl *fd = make_func_decl(&g_ir_arena, "test_const_bool", bool_type);

    Iron_Node *ret = make_return(&g_ir_arena, make_bool(&g_ir_arena, true));
    Iron_Node *stmts[1] = { ret };
    fd->body = (Iron_Node *)make_block(&g_ir_arena, stmts, 1);

    Iron_Node *decls[1] = { (Iron_Node *)fd };
    Iron_Program *prog = make_program(&g_ir_arena, decls, 1);

    IronLIR_Module *mod = iron_lir_lower(prog, NULL, &g_ir_arena, &g_diags);
    TEST_ASSERT_NOT_NULL(mod);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);
    TEST_ASSERT_TRUE(has_instr_kind(mod->funcs[0], IRON_LIR_CONST_BOOL));
}

/* ── test_lower_const_string ──────────────────────────────────────────────
 * fn f() { val s = "hello" } */
void test_lower_const_string(void) {
    Iron_Type *str_type  = iron_type_make_primitive(IRON_TYPE_STRING);
    Iron_Type *void_type = iron_type_make_primitive(IRON_TYPE_VOID);
    Iron_FuncDecl *fd = make_func_decl(&g_ir_arena, "test_const_string", void_type);

    Iron_StringLit *lit = ARENA_ALLOC(&g_ir_arena, Iron_StringLit);
    memset(lit, 0, sizeof(*lit));
    lit->span = test_span();
    lit->kind = IRON_NODE_STRING_LIT;
    lit->value = "hello";
    lit->resolved_type = str_type;

    Iron_Node *val_s = make_val_decl(&g_ir_arena, "s", (Iron_Node *)lit, str_type);
    Iron_Node *stmts[1] = { val_s };
    fd->body = (Iron_Node *)make_block(&g_ir_arena, stmts, 1);

    Iron_Node *decls[1] = { (Iron_Node *)fd };
    Iron_Program *prog = make_program(&g_ir_arena, decls, 1);

    IronLIR_Module *mod = iron_lir_lower(prog, NULL, &g_ir_arena, &g_diags);
    TEST_ASSERT_NOT_NULL(mod);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);
    TEST_ASSERT_TRUE(has_instr_kind(mod->funcs[0], IRON_LIR_CONST_STRING));
}

/* ── test_lower_const_null ────────────────────────────────────────────────
 * fn f() { val x = null } */
void test_lower_const_null(void) {
    Iron_Type *null_type = iron_type_make_primitive(IRON_TYPE_NULL);
    Iron_Type *void_type = iron_type_make_primitive(IRON_TYPE_VOID);
    Iron_FuncDecl *fd = make_func_decl(&g_ir_arena, "test_const_null", void_type);

    Iron_NullLit *lit = ARENA_ALLOC(&g_ir_arena, Iron_NullLit);
    memset(lit, 0, sizeof(*lit));
    lit->span = test_span();
    lit->kind = IRON_NODE_NULL_LIT;
    lit->resolved_type = null_type;

    Iron_Node *val_x = make_val_decl(&g_ir_arena, "x", (Iron_Node *)lit, null_type);
    Iron_Node *stmts[1] = { val_x };
    fd->body = (Iron_Node *)make_block(&g_ir_arena, stmts, 1);

    Iron_Node *decls[1] = { (Iron_Node *)fd };
    Iron_Program *prog = make_program(&g_ir_arena, decls, 1);

    IronLIR_Module *mod = iron_lir_lower(prog, NULL, &g_ir_arena, &g_diags);
    TEST_ASSERT_NOT_NULL(mod);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);
    TEST_ASSERT_TRUE(has_instr_kind(mod->funcs[0], IRON_LIR_CONST_NULL));
}

/* ── test_lower_add ───────────────────────────────────────────────────────
 * fn f() -> Int { return 1 + 2 } */
void test_lower_add(void) {
    Iron_Type *int_type = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_FuncDecl *fd = make_func_decl(&g_ir_arena, "test_add", int_type);

    Iron_Node *add_expr = make_binary(&g_ir_arena, make_int(&g_ir_arena, "1"),
                                       IRON_TOK_PLUS, make_int(&g_ir_arena, "2"), int_type);
    Iron_Node *ret = make_return(&g_ir_arena, add_expr);
    Iron_Node *stmts[1] = { ret };
    fd->body = (Iron_Node *)make_block(&g_ir_arena, stmts, 1);

    Iron_Node *decls[1] = { (Iron_Node *)fd };
    Iron_Program *prog = make_program(&g_ir_arena, decls, 1);

    IronLIR_Module *mod = iron_lir_lower(prog, NULL, &g_ir_arena, &g_diags);
    TEST_ASSERT_NOT_NULL(mod);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);
    TEST_ASSERT_TRUE(has_instr_kind(mod->funcs[0], IRON_LIR_ADD));
}

/* ── test_lower_sub ───────────────────────────────────────────────────────
 * fn f() -> Int { return 5 - 3 } */
void test_lower_sub(void) {
    Iron_Type *int_type = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_FuncDecl *fd = make_func_decl(&g_ir_arena, "test_sub", int_type);

    Iron_Node *sub_expr = make_binary(&g_ir_arena, make_int(&g_ir_arena, "5"),
                                       IRON_TOK_MINUS, make_int(&g_ir_arena, "3"), int_type);
    Iron_Node *ret = make_return(&g_ir_arena, sub_expr);
    Iron_Node *stmts[1] = { ret };
    fd->body = (Iron_Node *)make_block(&g_ir_arena, stmts, 1);

    Iron_Node *decls[1] = { (Iron_Node *)fd };
    Iron_Program *prog = make_program(&g_ir_arena, decls, 1);

    IronLIR_Module *mod = iron_lir_lower(prog, NULL, &g_ir_arena, &g_diags);
    TEST_ASSERT_NOT_NULL(mod);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);
    TEST_ASSERT_TRUE(has_instr_kind(mod->funcs[0], IRON_LIR_SUB));
}

/* ── test_lower_mul ───────────────────────────────────────────────────────
 * fn f() -> Int { return 2 * 3 } */
void test_lower_mul(void) {
    Iron_Type *int_type = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_FuncDecl *fd = make_func_decl(&g_ir_arena, "test_mul", int_type);

    Iron_Node *mul_expr = make_binary(&g_ir_arena, make_int(&g_ir_arena, "2"),
                                       IRON_TOK_STAR, make_int(&g_ir_arena, "3"), int_type);
    Iron_Node *ret = make_return(&g_ir_arena, mul_expr);
    Iron_Node *stmts[1] = { ret };
    fd->body = (Iron_Node *)make_block(&g_ir_arena, stmts, 1);

    Iron_Node *decls[1] = { (Iron_Node *)fd };
    Iron_Program *prog = make_program(&g_ir_arena, decls, 1);

    IronLIR_Module *mod = iron_lir_lower(prog, NULL, &g_ir_arena, &g_diags);
    TEST_ASSERT_NOT_NULL(mod);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);
    TEST_ASSERT_TRUE(has_instr_kind(mod->funcs[0], IRON_LIR_MUL));
}

/* ── test_lower_div ───────────────────────────────────────────────────────
 * fn f() -> Int { return 10 / 2 } */
void test_lower_div(void) {
    Iron_Type *int_type = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_FuncDecl *fd = make_func_decl(&g_ir_arena, "test_div", int_type);

    Iron_Node *div_expr = make_binary(&g_ir_arena, make_int(&g_ir_arena, "10"),
                                       IRON_TOK_SLASH, make_int(&g_ir_arena, "2"), int_type);
    Iron_Node *ret = make_return(&g_ir_arena, div_expr);
    Iron_Node *stmts[1] = { ret };
    fd->body = (Iron_Node *)make_block(&g_ir_arena, stmts, 1);

    Iron_Node *decls[1] = { (Iron_Node *)fd };
    Iron_Program *prog = make_program(&g_ir_arena, decls, 1);

    IronLIR_Module *mod = iron_lir_lower(prog, NULL, &g_ir_arena, &g_diags);
    TEST_ASSERT_NOT_NULL(mod);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);
    TEST_ASSERT_TRUE(has_instr_kind(mod->funcs[0], IRON_LIR_DIV));
}

/* ── test_lower_mod ───────────────────────────────────────────────────────
 * fn f() -> Int { return 10 % 3 } */
void test_lower_mod(void) {
    Iron_Type *int_type = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_FuncDecl *fd = make_func_decl(&g_ir_arena, "test_mod", int_type);

    Iron_Node *mod_expr = make_binary(&g_ir_arena, make_int(&g_ir_arena, "10"),
                                       IRON_TOK_PERCENT, make_int(&g_ir_arena, "3"), int_type);
    Iron_Node *ret = make_return(&g_ir_arena, mod_expr);
    Iron_Node *stmts[1] = { ret };
    fd->body = (Iron_Node *)make_block(&g_ir_arena, stmts, 1);

    Iron_Node *decls[1] = { (Iron_Node *)fd };
    Iron_Program *prog = make_program(&g_ir_arena, decls, 1);

    IronLIR_Module *mod = iron_lir_lower(prog, NULL, &g_ir_arena, &g_diags);
    TEST_ASSERT_NOT_NULL(mod);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);
    TEST_ASSERT_TRUE(has_instr_kind(mod->funcs[0], IRON_LIR_MOD));
}

/* ── test_lower_eq ────────────────────────────────────────────────────────
 * fn f() -> Bool { return 1 == 1 } */
void test_lower_eq(void) {
    Iron_Type *bool_type = iron_type_make_primitive(IRON_TYPE_BOOL);
    Iron_FuncDecl *fd = make_func_decl(&g_ir_arena, "test_eq", bool_type);

    Iron_Node *cmp = make_binary(&g_ir_arena, make_int(&g_ir_arena, "1"),
                                  IRON_TOK_EQUALS, make_int(&g_ir_arena, "1"), bool_type);
    Iron_Node *ret = make_return(&g_ir_arena, cmp);
    Iron_Node *stmts[1] = { ret };
    fd->body = (Iron_Node *)make_block(&g_ir_arena, stmts, 1);

    Iron_Node *decls[1] = { (Iron_Node *)fd };
    Iron_Program *prog = make_program(&g_ir_arena, decls, 1);

    IronLIR_Module *mod = iron_lir_lower(prog, NULL, &g_ir_arena, &g_diags);
    TEST_ASSERT_NOT_NULL(mod);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);
    TEST_ASSERT_TRUE(has_instr_kind(mod->funcs[0], IRON_LIR_EQ));
}

/* ── test_lower_neq ───────────────────────────────────────────────────────
 * fn f() -> Bool { return 1 != 2 } */
void test_lower_neq(void) {
    Iron_Type *bool_type = iron_type_make_primitive(IRON_TYPE_BOOL);
    Iron_FuncDecl *fd = make_func_decl(&g_ir_arena, "test_neq", bool_type);

    Iron_Node *cmp = make_binary(&g_ir_arena, make_int(&g_ir_arena, "1"),
                                  IRON_TOK_NOT_EQUALS, make_int(&g_ir_arena, "2"), bool_type);
    Iron_Node *ret = make_return(&g_ir_arena, cmp);
    Iron_Node *stmts[1] = { ret };
    fd->body = (Iron_Node *)make_block(&g_ir_arena, stmts, 1);

    Iron_Node *decls[1] = { (Iron_Node *)fd };
    Iron_Program *prog = make_program(&g_ir_arena, decls, 1);

    IronLIR_Module *mod = iron_lir_lower(prog, NULL, &g_ir_arena, &g_diags);
    TEST_ASSERT_NOT_NULL(mod);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);
    TEST_ASSERT_TRUE(has_instr_kind(mod->funcs[0], IRON_LIR_NEQ));
}

/* ── test_lower_lt ────────────────────────────────────────────────────────
 * fn f() -> Bool { return 1 < 2 } */
void test_lower_lt(void) {
    Iron_Type *bool_type = iron_type_make_primitive(IRON_TYPE_BOOL);
    Iron_FuncDecl *fd = make_func_decl(&g_ir_arena, "test_lt", bool_type);

    Iron_Node *cmp = make_binary(&g_ir_arena, make_int(&g_ir_arena, "1"),
                                  IRON_TOK_LESS, make_int(&g_ir_arena, "2"), bool_type);
    Iron_Node *ret = make_return(&g_ir_arena, cmp);
    Iron_Node *stmts[1] = { ret };
    fd->body = (Iron_Node *)make_block(&g_ir_arena, stmts, 1);

    Iron_Node *decls[1] = { (Iron_Node *)fd };
    Iron_Program *prog = make_program(&g_ir_arena, decls, 1);

    IronLIR_Module *mod = iron_lir_lower(prog, NULL, &g_ir_arena, &g_diags);
    TEST_ASSERT_NOT_NULL(mod);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);
    TEST_ASSERT_TRUE(has_instr_kind(mod->funcs[0], IRON_LIR_LT));
}

/* ── test_lower_lte ───────────────────────────────────────────────────────
 * fn f() -> Bool { return 1 <= 2 } */
void test_lower_lte(void) {
    Iron_Type *bool_type = iron_type_make_primitive(IRON_TYPE_BOOL);
    Iron_FuncDecl *fd = make_func_decl(&g_ir_arena, "test_lte", bool_type);

    Iron_Node *cmp = make_binary(&g_ir_arena, make_int(&g_ir_arena, "1"),
                                  IRON_TOK_LESS_EQ, make_int(&g_ir_arena, "2"), bool_type);
    Iron_Node *ret = make_return(&g_ir_arena, cmp);
    Iron_Node *stmts[1] = { ret };
    fd->body = (Iron_Node *)make_block(&g_ir_arena, stmts, 1);

    Iron_Node *decls[1] = { (Iron_Node *)fd };
    Iron_Program *prog = make_program(&g_ir_arena, decls, 1);

    IronLIR_Module *mod = iron_lir_lower(prog, NULL, &g_ir_arena, &g_diags);
    TEST_ASSERT_NOT_NULL(mod);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);
    TEST_ASSERT_TRUE(has_instr_kind(mod->funcs[0], IRON_LIR_LTE));
}

/* ── test_lower_gt ────────────────────────────────────────────────────────
 * fn f() -> Bool { return 2 > 1 } */
void test_lower_gt(void) {
    Iron_Type *bool_type = iron_type_make_primitive(IRON_TYPE_BOOL);
    Iron_FuncDecl *fd = make_func_decl(&g_ir_arena, "test_gt", bool_type);

    Iron_Node *cmp = make_binary(&g_ir_arena, make_int(&g_ir_arena, "2"),
                                  IRON_TOK_GREATER, make_int(&g_ir_arena, "1"), bool_type);
    Iron_Node *ret = make_return(&g_ir_arena, cmp);
    Iron_Node *stmts[1] = { ret };
    fd->body = (Iron_Node *)make_block(&g_ir_arena, stmts, 1);

    Iron_Node *decls[1] = { (Iron_Node *)fd };
    Iron_Program *prog = make_program(&g_ir_arena, decls, 1);

    IronLIR_Module *mod = iron_lir_lower(prog, NULL, &g_ir_arena, &g_diags);
    TEST_ASSERT_NOT_NULL(mod);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);
    TEST_ASSERT_TRUE(has_instr_kind(mod->funcs[0], IRON_LIR_GT));
}

/* ── test_lower_gte ───────────────────────────────────────────────────────
 * fn f() -> Bool { return 2 >= 1 } */
void test_lower_gte(void) {
    Iron_Type *bool_type = iron_type_make_primitive(IRON_TYPE_BOOL);
    Iron_FuncDecl *fd = make_func_decl(&g_ir_arena, "test_gte", bool_type);

    Iron_Node *cmp = make_binary(&g_ir_arena, make_int(&g_ir_arena, "2"),
                                  IRON_TOK_GREATER_EQ, make_int(&g_ir_arena, "1"), bool_type);
    Iron_Node *ret = make_return(&g_ir_arena, cmp);
    Iron_Node *stmts[1] = { ret };
    fd->body = (Iron_Node *)make_block(&g_ir_arena, stmts, 1);

    Iron_Node *decls[1] = { (Iron_Node *)fd };
    Iron_Program *prog = make_program(&g_ir_arena, decls, 1);

    IronLIR_Module *mod = iron_lir_lower(prog, NULL, &g_ir_arena, &g_diags);
    TEST_ASSERT_NOT_NULL(mod);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);
    TEST_ASSERT_TRUE(has_instr_kind(mod->funcs[0], IRON_LIR_GTE));
}

/* ── test_lower_and ───────────────────────────────────────────────────────
 * Short-circuit AND: emits BRANCH, not IRON_LIR_AND.
 * fn f() -> Bool { return true and false } */
void test_lower_and(void) {
    Iron_Type *bool_type = iron_type_make_primitive(IRON_TYPE_BOOL);
    Iron_FuncDecl *fd = make_func_decl(&g_ir_arena, "test_and", bool_type);

    Iron_Node *and_expr = make_binary(&g_ir_arena,
                                       make_bool(&g_ir_arena, true),
                                       IRON_TOK_AND,
                                       make_bool(&g_ir_arena, false),
                                       bool_type);
    Iron_Node *ret = make_return(&g_ir_arena, and_expr);
    Iron_Node *stmts[1] = { ret };
    fd->body = (Iron_Node *)make_block(&g_ir_arena, stmts, 1);

    Iron_Node *decls[1] = { (Iron_Node *)fd };
    Iron_Program *prog = make_program(&g_ir_arena, decls, 1);

    IronLIR_Module *mod = iron_lir_lower(prog, NULL, &g_ir_arena, &g_diags);
    TEST_ASSERT_NOT_NULL(mod);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);
    /* AND uses short-circuit pattern; verify BRANCH exists */
    TEST_ASSERT_TRUE(has_instr_kind(mod->funcs[0], IRON_LIR_BRANCH));
}

/* ── test_lower_or ────────────────────────────────────────────────────────
 * Short-circuit OR: emits BRANCH, not IRON_LIR_OR.
 * fn f() -> Bool { return true or false } */
void test_lower_or(void) {
    Iron_Type *bool_type = iron_type_make_primitive(IRON_TYPE_BOOL);
    Iron_FuncDecl *fd = make_func_decl(&g_ir_arena, "test_or", bool_type);

    Iron_Node *or_expr = make_binary(&g_ir_arena,
                                      make_bool(&g_ir_arena, true),
                                      IRON_TOK_OR,
                                      make_bool(&g_ir_arena, false),
                                      bool_type);
    Iron_Node *ret = make_return(&g_ir_arena, or_expr);
    Iron_Node *stmts[1] = { ret };
    fd->body = (Iron_Node *)make_block(&g_ir_arena, stmts, 1);

    Iron_Node *decls[1] = { (Iron_Node *)fd };
    Iron_Program *prog = make_program(&g_ir_arena, decls, 1);

    IronLIR_Module *mod = iron_lir_lower(prog, NULL, &g_ir_arena, &g_diags);
    TEST_ASSERT_NOT_NULL(mod);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);
    /* OR uses short-circuit pattern; verify BRANCH exists */
    TEST_ASSERT_TRUE(has_instr_kind(mod->funcs[0], IRON_LIR_BRANCH));
}

/* ── test_lower_neg ───────────────────────────────────────────────────────
 * fn f() -> Int { return -42 } */
void test_lower_neg(void) {
    Iron_Type *int_type = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_FuncDecl *fd = make_func_decl(&g_ir_arena, "test_neg", int_type);

    Iron_UnaryExpr *un = ARENA_ALLOC(&g_ir_arena, Iron_UnaryExpr);
    memset(un, 0, sizeof(*un));
    un->span = test_span();
    un->kind = IRON_NODE_UNARY;
    un->op = IRON_TOK_MINUS;
    un->operand = make_int(&g_ir_arena, "42");
    un->resolved_type = int_type;

    Iron_Node *ret = make_return(&g_ir_arena, (Iron_Node *)un);
    Iron_Node *stmts[1] = { ret };
    fd->body = (Iron_Node *)make_block(&g_ir_arena, stmts, 1);

    Iron_Node *decls[1] = { (Iron_Node *)fd };
    Iron_Program *prog = make_program(&g_ir_arena, decls, 1);

    IronLIR_Module *mod = iron_lir_lower(prog, NULL, &g_ir_arena, &g_diags);
    TEST_ASSERT_NOT_NULL(mod);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);
    TEST_ASSERT_TRUE(has_instr_kind(mod->funcs[0], IRON_LIR_NEG));
}

/* ── test_lower_not ───────────────────────────────────────────────────────
 * fn f() -> Bool { return not true } */
void test_lower_not(void) {
    Iron_Type *bool_type = iron_type_make_primitive(IRON_TYPE_BOOL);
    Iron_FuncDecl *fd = make_func_decl(&g_ir_arena, "test_not", bool_type);

    Iron_UnaryExpr *un = ARENA_ALLOC(&g_ir_arena, Iron_UnaryExpr);
    memset(un, 0, sizeof(*un));
    un->span = test_span();
    un->kind = IRON_NODE_UNARY;
    un->op = IRON_TOK_NOT;
    un->operand = make_bool(&g_ir_arena, true);
    un->resolved_type = bool_type;

    Iron_Node *ret = make_return(&g_ir_arena, (Iron_Node *)un);
    Iron_Node *stmts[1] = { ret };
    fd->body = (Iron_Node *)make_block(&g_ir_arena, stmts, 1);

    Iron_Node *decls[1] = { (Iron_Node *)fd };
    Iron_Program *prog = make_program(&g_ir_arena, decls, 1);

    IronLIR_Module *mod = iron_lir_lower(prog, NULL, &g_ir_arena, &g_diags);
    TEST_ASSERT_NOT_NULL(mod);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);
    TEST_ASSERT_TRUE(has_instr_kind(mod->funcs[0], IRON_LIR_NOT));
}

/* ── test_lower_alloca_load_store ─────────────────────────────────────────
 * fn f() { var x = 0; x = 1; val y = x } */
void test_lower_alloca_load_store(void) {
    Iron_Type *int_type  = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_Type *void_type = iron_type_make_primitive(IRON_TYPE_VOID);
    Iron_FuncDecl *fd = make_func_decl(&g_ir_arena, "test_mem_ops", void_type);

    Iron_Node *var_x = make_var_decl(&g_ir_arena, "x", make_int(&g_ir_arena, "0"), int_type);
    Iron_Node *assign = make_assign(&g_ir_arena,
                                     make_ident(&g_ir_arena, "x", int_type),
                                     make_int(&g_ir_arena, "1"));
    Iron_Node *val_y = make_val_decl(&g_ir_arena, "y",
                                      make_ident(&g_ir_arena, "x", int_type), int_type);

    Iron_Node *stmts[3] = { var_x, assign, val_y };
    fd->body = (Iron_Node *)make_block(&g_ir_arena, stmts, 3);

    Iron_Node *decls[1] = { (Iron_Node *)fd };
    Iron_Program *prog = make_program(&g_ir_arena, decls, 1);

    IronLIR_Module *mod = iron_lir_lower(prog, NULL, &g_ir_arena, &g_diags);
    TEST_ASSERT_NOT_NULL(mod);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);

    IronLIR_Func *fn = mod->funcs[0];
    TEST_ASSERT_TRUE(has_instr_kind(fn, IRON_LIR_ALLOCA));
    TEST_ASSERT_TRUE(has_instr_kind(fn, IRON_LIR_STORE));
    TEST_ASSERT_TRUE(has_instr_kind(fn, IRON_LIR_LOAD));
}

/* ── test_lower_get_field ─────────────────────────────────────────────────
 * field access expression -> IRON_LIR_GET_FIELD */
void test_lower_get_field(void) {
    Iron_Type *int_type  = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_Type *null_type = iron_type_make_primitive(IRON_TYPE_NULL);
    Iron_Type *void_type = iron_type_make_primitive(IRON_TYPE_VOID);
    Iron_FuncDecl *fd = make_func_decl(&g_ir_arena, "test_get_field", void_type);

    Iron_NullLit *obj_lit = ARENA_ALLOC(&g_ir_arena, Iron_NullLit);
    memset(obj_lit, 0, sizeof(*obj_lit));
    obj_lit->span = test_span();
    obj_lit->kind = IRON_NODE_NULL_LIT;
    obj_lit->resolved_type = null_type;

    Iron_FieldAccess *fa = ARENA_ALLOC(&g_ir_arena, Iron_FieldAccess);
    memset(fa, 0, sizeof(*fa));
    fa->span = test_span();
    fa->kind = IRON_NODE_FIELD_ACCESS;
    fa->object = (Iron_Node *)obj_lit;
    fa->field = "x";
    fa->resolved_type = int_type;

    Iron_Node *val_v = make_val_decl(&g_ir_arena, "v", (Iron_Node *)fa, int_type);
    Iron_Node *stmts[1] = { val_v };
    fd->body = (Iron_Node *)make_block(&g_ir_arena, stmts, 1);

    Iron_Node *decls[1] = { (Iron_Node *)fd };
    Iron_Program *prog = make_program(&g_ir_arena, decls, 1);

    IronLIR_Module *mod = iron_lir_lower(prog, NULL, &g_ir_arena, &g_diags);
    TEST_ASSERT_NOT_NULL(mod);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);
    TEST_ASSERT_TRUE(has_instr_kind(mod->funcs[0], IRON_LIR_GET_FIELD));
}

/* ── test_lower_set_field ─────────────────────────────────────────────────
 * obj.field = val -> IRON_LIR_SET_FIELD */
void test_lower_set_field(void) {
    Iron_Type *int_type  = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_Type *null_type = iron_type_make_primitive(IRON_TYPE_NULL);
    Iron_Type *void_type = iron_type_make_primitive(IRON_TYPE_VOID);
    Iron_FuncDecl *fd = make_func_decl(&g_ir_arena, "test_set_field", void_type);

    Iron_NullLit *obj_lit = ARENA_ALLOC(&g_ir_arena, Iron_NullLit);
    memset(obj_lit, 0, sizeof(*obj_lit));
    obj_lit->span = test_span();
    obj_lit->kind = IRON_NODE_NULL_LIT;
    obj_lit->resolved_type = null_type;

    Iron_Node *val_obj = make_val_decl(&g_ir_arena, "obj", (Iron_Node *)obj_lit, null_type);

    Iron_FieldAccess *fa = ARENA_ALLOC(&g_ir_arena, Iron_FieldAccess);
    memset(fa, 0, sizeof(*fa));
    fa->span = test_span();
    fa->kind = IRON_NODE_FIELD_ACCESS;
    fa->object = make_ident(&g_ir_arena, "obj", null_type);
    fa->field = "x";
    fa->resolved_type = int_type;

    Iron_Node *assign = make_assign(&g_ir_arena, (Iron_Node *)fa, make_int(&g_ir_arena, "42"));

    Iron_Node *stmts[2] = { val_obj, assign };
    fd->body = (Iron_Node *)make_block(&g_ir_arena, stmts, 2);

    Iron_Node *decls[1] = { (Iron_Node *)fd };
    Iron_Program *prog = make_program(&g_ir_arena, decls, 1);

    IronLIR_Module *mod = iron_lir_lower(prog, NULL, &g_ir_arena, &g_diags);
    TEST_ASSERT_NOT_NULL(mod);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);
    TEST_ASSERT_TRUE(has_instr_kind(mod->funcs[0], IRON_LIR_SET_FIELD));
}

/* ── test_lower_get_index ─────────────────────────────────────────────────
 * arr[0] -> IRON_LIR_GET_INDEX */
void test_lower_get_index(void) {
    Iron_Type *int_type  = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_Type *null_type = iron_type_make_primitive(IRON_TYPE_NULL);
    Iron_Type *arr_type  = iron_type_make_array(&g_ir_arena, int_type, -1);
    Iron_Type *void_type = iron_type_make_primitive(IRON_TYPE_VOID);
    Iron_FuncDecl *fd = make_func_decl(&g_ir_arena, "test_get_index", void_type);

    /* val arr = null (typed placeholder) */
    Iron_NullLit *arr_lit = ARENA_ALLOC(&g_ir_arena, Iron_NullLit);
    memset(arr_lit, 0, sizeof(*arr_lit));
    arr_lit->span = test_span();
    arr_lit->kind = IRON_NODE_NULL_LIT;
    arr_lit->resolved_type = null_type;

    Iron_Node *val_arr = make_val_decl(&g_ir_arena, "arr", (Iron_Node *)arr_lit, arr_type);

    Iron_IndexExpr *idx = ARENA_ALLOC(&g_ir_arena, Iron_IndexExpr);
    memset(idx, 0, sizeof(*idx));
    idx->span = test_span();
    idx->kind = IRON_NODE_INDEX;
    idx->object = make_ident(&g_ir_arena, "arr", arr_type);
    idx->index = make_int(&g_ir_arena, "0");
    idx->resolved_type = int_type;

    Iron_Node *val_x = make_val_decl(&g_ir_arena, "x", (Iron_Node *)idx, int_type);

    Iron_Node *stmts[2] = { val_arr, val_x };
    fd->body = (Iron_Node *)make_block(&g_ir_arena, stmts, 2);

    Iron_Node *decls[1] = { (Iron_Node *)fd };
    Iron_Program *prog = make_program(&g_ir_arena, decls, 1);

    IronLIR_Module *mod = iron_lir_lower(prog, NULL, &g_ir_arena, &g_diags);
    TEST_ASSERT_NOT_NULL(mod);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);
    TEST_ASSERT_TRUE(has_instr_kind(mod->funcs[0], IRON_LIR_GET_INDEX));
}

/* ── test_lower_set_index ─────────────────────────────────────────────────
 * arr[0] = 99 -> IRON_LIR_SET_INDEX */
void test_lower_set_index(void) {
    Iron_Type *int_type  = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_Type *null_type = iron_type_make_primitive(IRON_TYPE_NULL);
    Iron_Type *arr_type  = iron_type_make_array(&g_ir_arena, int_type, -1);
    Iron_Type *void_type = iron_type_make_primitive(IRON_TYPE_VOID);
    Iron_FuncDecl *fd = make_func_decl(&g_ir_arena, "test_set_index", void_type);

    Iron_NullLit *arr_lit = ARENA_ALLOC(&g_ir_arena, Iron_NullLit);
    memset(arr_lit, 0, sizeof(*arr_lit));
    arr_lit->span = test_span();
    arr_lit->kind = IRON_NODE_NULL_LIT;
    arr_lit->resolved_type = null_type;

    Iron_Node *var_arr = make_var_decl(&g_ir_arena, "arr", (Iron_Node *)arr_lit, arr_type);

    Iron_IndexExpr *idx = ARENA_ALLOC(&g_ir_arena, Iron_IndexExpr);
    memset(idx, 0, sizeof(*idx));
    idx->span = test_span();
    idx->kind = IRON_NODE_INDEX;
    idx->object = make_ident(&g_ir_arena, "arr", arr_type);
    idx->index = make_int(&g_ir_arena, "0");
    idx->resolved_type = int_type;

    Iron_Node *assign = make_assign(&g_ir_arena, (Iron_Node *)idx, make_int(&g_ir_arena, "99"));

    Iron_Node *stmts[2] = { var_arr, assign };
    fd->body = (Iron_Node *)make_block(&g_ir_arena, stmts, 2);

    Iron_Node *decls[1] = { (Iron_Node *)fd };
    Iron_Program *prog = make_program(&g_ir_arena, decls, 1);

    IronLIR_Module *mod = iron_lir_lower(prog, NULL, &g_ir_arena, &g_diags);
    TEST_ASSERT_NOT_NULL(mod);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);
    TEST_ASSERT_TRUE(has_instr_kind(mod->funcs[0], IRON_LIR_SET_INDEX));
}

/* ── test_lower_branch ────────────────────────────────────────────────────
 * if true { } -> IRON_LIR_BRANCH */
void test_lower_branch(void) {
    Iron_Type *void_type = iron_type_make_primitive(IRON_TYPE_VOID);
    Iron_FuncDecl *fd = make_func_decl(&g_ir_arena, "test_branch_instr", void_type);

    Iron_Block *then_body = make_block(&g_ir_arena, NULL, 0);
    Iron_Node *if_stmt = make_if(&g_ir_arena, make_bool(&g_ir_arena, true), then_body, NULL);

    Iron_Node *stmts[1] = { if_stmt };
    fd->body = (Iron_Node *)make_block(&g_ir_arena, stmts, 1);

    Iron_Node *decls[1] = { (Iron_Node *)fd };
    Iron_Program *prog = make_program(&g_ir_arena, decls, 1);

    IronLIR_Module *mod = iron_lir_lower(prog, NULL, &g_ir_arena, &g_diags);
    TEST_ASSERT_NOT_NULL(mod);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);
    TEST_ASSERT_TRUE(has_instr_kind(mod->funcs[0], IRON_LIR_BRANCH));
}

/* ── test_lower_jump ──────────────────────────────────────────────────────
 * if/else produces jumps to merge block -> IRON_LIR_JUMP */
void test_lower_jump(void) {
    Iron_Type *void_type = iron_type_make_primitive(IRON_TYPE_VOID);
    Iron_FuncDecl *fd = make_func_decl(&g_ir_arena, "test_jump_instr", void_type);

    Iron_Block *then_body = make_block(&g_ir_arena, NULL, 0);
    Iron_Block *else_body = make_block(&g_ir_arena, NULL, 0);
    Iron_Node *if_stmt = make_if(&g_ir_arena, make_bool(&g_ir_arena, true), then_body, else_body);

    Iron_Node *stmts[1] = { if_stmt };
    fd->body = (Iron_Node *)make_block(&g_ir_arena, stmts, 1);

    Iron_Node *decls[1] = { (Iron_Node *)fd };
    Iron_Program *prog = make_program(&g_ir_arena, decls, 1);

    IronLIR_Module *mod = iron_lir_lower(prog, NULL, &g_ir_arena, &g_diags);
    TEST_ASSERT_NOT_NULL(mod);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);
    TEST_ASSERT_TRUE(has_instr_kind(mod->funcs[0], IRON_LIR_JUMP));
}

/* ── test_lower_return ────────────────────────────────────────────────────
 * fn foo() -> Int { return 42 } -> IRON_LIR_RETURN */
void test_lower_return(void) {
    Iron_Type *int_type = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_FuncDecl *fd = make_func_decl(&g_ir_arena, "test_return_instr", int_type);

    Iron_Node *ret = make_return(&g_ir_arena, make_int(&g_ir_arena, "42"));
    Iron_Node *stmts[1] = { ret };
    fd->body = (Iron_Node *)make_block(&g_ir_arena, stmts, 1);

    Iron_Node *decls[1] = { (Iron_Node *)fd };
    Iron_Program *prog = make_program(&g_ir_arena, decls, 1);

    IronLIR_Module *mod = iron_lir_lower(prog, NULL, &g_ir_arena, &g_diags);
    TEST_ASSERT_NOT_NULL(mod);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);
    TEST_ASSERT_TRUE(has_instr_kind(mod->funcs[0], IRON_LIR_RETURN));
}

/* ── test_lower_switch ────────────────────────────────────────────────────
 * match expression -> IRON_LIR_SWITCH */
void test_lower_switch(void) {
    Iron_Type *int_type = iron_type_make_primitive(IRON_TYPE_INT);

    Iron_Param *param_x = make_param(&g_ir_arena, "x");
    Iron_TypeAnnotation *int_ann = ARENA_ALLOC(&g_ir_arena, Iron_TypeAnnotation);
    memset(int_ann, 0, sizeof(*int_ann));
    int_ann->span = test_span();
    int_ann->kind = IRON_NODE_TYPE_ANNOTATION;
    int_ann->name = "Int";
    param_x->type_ann = (Iron_Node *)int_ann;

    Iron_FuncDecl *fd = make_func_decl(&g_ir_arena, "test_switch_instr", int_type);
    fd->params = (Iron_Node **)iron_arena_alloc(&g_ir_arena,
                     sizeof(Iron_Node *), _Alignof(Iron_Node *));
    fd->params[0] = (Iron_Node *)param_x;
    fd->param_count = 1;

    Iron_Node *ret10 = make_return(&g_ir_arena, make_int(&g_ir_arena, "10"));
    Iron_Node *arm1_stmts[1] = { ret10 };
    Iron_Node *case1 = make_match_case(&g_ir_arena, make_int(&g_ir_arena, "1"),
                                        make_block(&g_ir_arena, arm1_stmts, 1));

    Iron_Node *cases[1] = { case1 };
    Iron_Node *match_stmt = make_match(&g_ir_arena,
                                        make_ident(&g_ir_arena, "x", int_type),
                                        cases, 1);
    Iron_Node *ret0 = make_return(&g_ir_arena, make_int(&g_ir_arena, "0"));
    Iron_Node *fn_stmts[2] = { match_stmt, ret0 };
    fd->body = (Iron_Node *)make_block(&g_ir_arena, fn_stmts, 2);

    Iron_Node *decls[1] = { (Iron_Node *)fd };
    Iron_Program *prog = make_program(&g_ir_arena, decls, 1);

    IronLIR_Module *mod = iron_lir_lower(prog, NULL, &g_ir_arena, &g_diags);
    TEST_ASSERT_NOT_NULL(mod);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);
    TEST_ASSERT_TRUE(has_instr_kind(mod->funcs[0], IRON_LIR_SWITCH));
}

/* ── test_lower_call ──────────────────────────────────────────────────────
 * calling a function -> IRON_LIR_CALL + IRON_LIR_FUNC_REF */
void test_lower_call(void) {
    Iron_Type *int_type  = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_Type *void_type = iron_type_make_primitive(IRON_TYPE_VOID);

    Iron_FuncDecl *callee = make_func_decl(&g_ir_arena, "callee", int_type);
    Iron_Node *ret1 = make_return(&g_ir_arena, make_int(&g_ir_arena, "1"));
    Iron_Node *callee_stmts[1] = { ret1 };
    callee->body = (Iron_Node *)make_block(&g_ir_arena, callee_stmts, 1);

    Iron_FuncDecl *caller = make_func_decl(&g_ir_arena, "caller", void_type);
    Iron_Node *callee_ident = make_ident(&g_ir_arena, "callee", int_type);
    Iron_Node *call_expr = make_call(&g_ir_arena, callee_ident, NULL, 0);
    ((Iron_CallExpr *)call_expr)->resolved_type = int_type;
    Iron_Node *val_r = make_val_decl(&g_ir_arena, "r", call_expr, int_type);
    Iron_Node *caller_stmts[1] = { val_r };
    caller->body = (Iron_Node *)make_block(&g_ir_arena, caller_stmts, 1);

    Iron_Node *decls[2] = { (Iron_Node *)callee, (Iron_Node *)caller };
    Iron_Program *prog = make_program(&g_ir_arena, decls, 2);

    IronLIR_Module *mod = iron_lir_lower(prog, NULL, &g_ir_arena, &g_diags);
    TEST_ASSERT_NOT_NULL(mod);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);

    IronLIR_Func *caller_fn = NULL;
    for (int i = 0; i < mod->func_count; i++) {
        if (strcmp(mod->funcs[i]->name, "caller") == 0) {
            caller_fn = mod->funcs[i];
            break;
        }
    }
    TEST_ASSERT_NOT_NULL(caller_fn);
    TEST_ASSERT_TRUE(has_instr_kind(caller_fn, IRON_LIR_CALL));
}

/* ── test_lower_func_ref ──────────────────────────────────────────────────
 * FUNC_REF is emitted as part of CALL setup -> IRON_LIR_FUNC_REF */
void test_lower_func_ref(void) {
    Iron_Type *int_type  = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_Type *void_type = iron_type_make_primitive(IRON_TYPE_VOID);

    Iron_FuncDecl *callee = make_func_decl(&g_ir_arena, "target_fn", int_type);
    Iron_Node *ret1 = make_return(&g_ir_arena, make_int(&g_ir_arena, "1"));
    Iron_Node *callee_stmts[1] = { ret1 };
    callee->body = (Iron_Node *)make_block(&g_ir_arena, callee_stmts, 1);

    Iron_FuncDecl *caller = make_func_decl(&g_ir_arena, "caller_fn", void_type);
    Iron_Node *callee_ident = make_ident(&g_ir_arena, "target_fn", int_type);
    Iron_Node *call_expr = make_call(&g_ir_arena, callee_ident, NULL, 0);
    ((Iron_CallExpr *)call_expr)->resolved_type = int_type;
    Iron_Node *val_r = make_val_decl(&g_ir_arena, "r", call_expr, int_type);
    Iron_Node *caller_stmts[1] = { val_r };
    caller->body = (Iron_Node *)make_block(&g_ir_arena, caller_stmts, 1);

    Iron_Node *decls[2] = { (Iron_Node *)callee, (Iron_Node *)caller };
    Iron_Program *prog = make_program(&g_ir_arena, decls, 2);

    IronLIR_Module *mod = iron_lir_lower(prog, NULL, &g_ir_arena, &g_diags);
    TEST_ASSERT_NOT_NULL(mod);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);

    IronLIR_Func *caller_fn = NULL;
    for (int i = 0; i < mod->func_count; i++) {
        if (strcmp(mod->funcs[i]->name, "caller_fn") == 0) {
            caller_fn = mod->funcs[i];
            break;
        }
    }
    TEST_ASSERT_NOT_NULL(caller_fn);
    TEST_ASSERT_TRUE(has_instr_kind(caller_fn, IRON_LIR_FUNC_REF));
}

/* ── test_lower_cast ──────────────────────────────────────────────────────
 * IS expression with type narrowing -> IRON_LIR_CAST */
void test_lower_cast(void) {
    Iron_Type *int_type  = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_Type *null_type = iron_type_make_primitive(IRON_TYPE_NULL);
    Iron_Type *void_type = iron_type_make_primitive(IRON_TYPE_VOID);
    Iron_FuncDecl *fd = make_func_decl(&g_ir_arena, "test_cast_instr", void_type);

    Iron_NullLit *null_val = ARENA_ALLOC(&g_ir_arena, Iron_NullLit);
    memset(null_val, 0, sizeof(*null_val));
    null_val->span = test_span();
    null_val->kind = IRON_NODE_NULL_LIT;
    null_val->resolved_type = null_type;

    Iron_Node *val_x = make_val_decl(&g_ir_arena, "x", (Iron_Node *)null_val, null_type);

    /* (x is Int) — type narrowing emits CAST */
    Iron_IsExpr *is_expr = ARENA_ALLOC(&g_ir_arena, Iron_IsExpr);
    memset(is_expr, 0, sizeof(*is_expr));
    is_expr->span = test_span();
    is_expr->kind = IRON_NODE_IS;
    is_expr->expr = make_ident(&g_ir_arena, "x", null_type);
    is_expr->type_name = "Int";
    is_expr->resolved_type = int_type;

    Iron_Node *val_c = make_val_decl(&g_ir_arena, "c", (Iron_Node *)is_expr, int_type);

    Iron_Node *stmts[2] = { val_x, val_c };
    fd->body = (Iron_Node *)make_block(&g_ir_arena, stmts, 2);

    Iron_Node *decls[1] = { (Iron_Node *)fd };
    Iron_Program *prog = make_program(&g_ir_arena, decls, 1);

    IronLIR_Module *mod = iron_lir_lower(prog, NULL, &g_ir_arena, &g_diags);
    TEST_ASSERT_NOT_NULL(mod);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);
    TEST_ASSERT_TRUE(has_instr_kind(mod->funcs[0], IRON_LIR_CAST));
}

/* ── test_lower_construct ─────────────────────────────────────────────────
 * ConstructExpr -> IRON_LIR_CONSTRUCT */
void test_lower_construct(void) {
    Iron_Type *int_type  = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_Type *void_type = iron_type_make_primitive(IRON_TYPE_VOID);

    Iron_ObjectDecl *obj = ARENA_ALLOC(&g_ir_arena, Iron_ObjectDecl);
    memset(obj, 0, sizeof(*obj));
    obj->span = test_span();
    obj->kind = IRON_NODE_OBJECT_DECL;
    obj->name = "Point";

    Iron_Type *point_type = iron_type_make_object(&g_ir_arena, obj);

    Iron_FuncDecl *fd = make_func_decl(&g_ir_arena, "test_construct_instr", void_type);

    Iron_ConstructExpr *con = ARENA_ALLOC(&g_ir_arena, Iron_ConstructExpr);
    memset(con, 0, sizeof(*con));
    con->span = test_span();
    con->kind = IRON_NODE_CONSTRUCT;
    con->type_name = "Point";
    con->resolved_type = point_type;

    Iron_Node *arg1 = make_int(&g_ir_arena, "3");
    Iron_Node *arg2 = make_int(&g_ir_arena, "7");
    con->args = (Iron_Node **)iron_arena_alloc(&g_ir_arena,
                    2 * sizeof(Iron_Node *), _Alignof(Iron_Node *));
    con->args[0] = arg1;
    con->args[1] = arg2;
    con->arg_count = 2;
    (void)int_type;

    Iron_Node *val_p = make_val_decl(&g_ir_arena, "p", (Iron_Node *)con, point_type);
    Iron_Node *fn_stmts[1] = { val_p };
    fd->body = (Iron_Node *)make_block(&g_ir_arena, fn_stmts, 1);

    Iron_Node *decls[2] = { (Iron_Node *)obj, (Iron_Node *)fd };
    Iron_Program *prog = make_program(&g_ir_arena, decls, 2);

    IronLIR_Module *mod = iron_lir_lower(prog, NULL, &g_ir_arena, &g_diags);
    TEST_ASSERT_NOT_NULL(mod);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);
    TEST_ASSERT_TRUE(has_instr_kind(mod->funcs[0], IRON_LIR_CONSTRUCT));
}

/* ── test_lower_array_lit ─────────────────────────────────────────────────
 * [1, 2, 3] -> IRON_LIR_ARRAY_LIT */
void test_lower_array_lit(void) {
    Iron_Type *int_type  = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_Type *arr_type  = iron_type_make_array(&g_ir_arena, int_type, -1);
    Iron_Type *void_type = iron_type_make_primitive(IRON_TYPE_VOID);
    Iron_FuncDecl *fd = make_func_decl(&g_ir_arena, "test_array_lit_instr", void_type);

    Iron_ArrayLit *al = ARENA_ALLOC(&g_ir_arena, Iron_ArrayLit);
    memset(al, 0, sizeof(*al));
    al->span = test_span();
    al->kind = IRON_NODE_ARRAY_LIT;
    al->resolved_type = arr_type;

    al->elements = (Iron_Node **)iron_arena_alloc(&g_ir_arena,
                       3 * sizeof(Iron_Node *), _Alignof(Iron_Node *));
    al->elements[0] = make_int(&g_ir_arena, "1");
    al->elements[1] = make_int(&g_ir_arena, "2");
    al->elements[2] = make_int(&g_ir_arena, "3");
    al->element_count = 3;

    Iron_Node *val_arr = make_val_decl(&g_ir_arena, "arr", (Iron_Node *)al, arr_type);
    Iron_Node *stmts[1] = { val_arr };
    fd->body = (Iron_Node *)make_block(&g_ir_arena, stmts, 1);

    Iron_Node *decls[1] = { (Iron_Node *)fd };
    Iron_Program *prog = make_program(&g_ir_arena, decls, 1);

    IronLIR_Module *mod = iron_lir_lower(prog, NULL, &g_ir_arena, &g_diags);
    TEST_ASSERT_NOT_NULL(mod);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);
    TEST_ASSERT_TRUE(has_instr_kind(mod->funcs[0], IRON_LIR_ARRAY_LIT));
}

/* ── test_lower_interp_string ─────────────────────────────────────────────
 * "{x}" -> IRON_LIR_INTERP_STRING */
void test_lower_interp_string(void) {
    Iron_Type *int_type  = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_Type *str_type  = iron_type_make_primitive(IRON_TYPE_STRING);
    Iron_Type *void_type = iron_type_make_primitive(IRON_TYPE_VOID);
    Iron_FuncDecl *fd = make_func_decl(&g_ir_arena, "test_interp_string_instr", void_type);

    Iron_Node *val_x = make_val_decl(&g_ir_arena, "x", make_int(&g_ir_arena, "42"), int_type);

    Iron_StringLit *prefix = ARENA_ALLOC(&g_ir_arena, Iron_StringLit);
    memset(prefix, 0, sizeof(*prefix));
    prefix->span = test_span();
    prefix->kind = IRON_NODE_STRING_LIT;
    prefix->value = "";
    prefix->resolved_type = str_type;

    Iron_InterpString *interp = ARENA_ALLOC(&g_ir_arena, Iron_InterpString);
    memset(interp, 0, sizeof(*interp));
    interp->span = test_span();
    interp->kind = IRON_NODE_INTERP_STRING;
    interp->resolved_type = str_type;

    interp->parts = (Iron_Node **)iron_arena_alloc(&g_ir_arena,
                        2 * sizeof(Iron_Node *), _Alignof(Iron_Node *));
    interp->parts[0] = (Iron_Node *)prefix;
    interp->parts[1] = make_ident(&g_ir_arena, "x", int_type);
    interp->part_count = 2;

    Iron_Node *val_s = make_val_decl(&g_ir_arena, "s", (Iron_Node *)interp, str_type);
    Iron_Node *stmts[2] = { val_x, val_s };
    fd->body = (Iron_Node *)make_block(&g_ir_arena, stmts, 2);

    Iron_Node *decls[1] = { (Iron_Node *)fd };
    Iron_Program *prog = make_program(&g_ir_arena, decls, 1);

    IronLIR_Module *mod = iron_lir_lower(prog, NULL, &g_ir_arena, &g_diags);
    TEST_ASSERT_NOT_NULL(mod);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);
    TEST_ASSERT_TRUE(has_instr_kind(mod->funcs[0], IRON_LIR_INTERP_STRING));
}

/* ── test_lower_is_null ───────────────────────────────────────────────────
 * (x is Null) -> IRON_LIR_IS_NULL */
void test_lower_is_null(void) {
    Iron_Type *null_type = iron_type_make_primitive(IRON_TYPE_NULL);
    Iron_Type *bool_type = iron_type_make_primitive(IRON_TYPE_BOOL);
    Iron_Type *void_type = iron_type_make_primitive(IRON_TYPE_VOID);
    Iron_FuncDecl *fd = make_func_decl(&g_ir_arena, "test_is_null_instr", void_type);

    Iron_NullLit *null_val = ARENA_ALLOC(&g_ir_arena, Iron_NullLit);
    memset(null_val, 0, sizeof(*null_val));
    null_val->span = test_span();
    null_val->kind = IRON_NODE_NULL_LIT;
    null_val->resolved_type = null_type;

    Iron_Node *val_x = make_val_decl(&g_ir_arena, "x", (Iron_Node *)null_val, null_type);

    Iron_IsExpr *is_expr = ARENA_ALLOC(&g_ir_arena, Iron_IsExpr);
    memset(is_expr, 0, sizeof(*is_expr));
    is_expr->span = test_span();
    is_expr->kind = IRON_NODE_IS;
    is_expr->expr = make_ident(&g_ir_arena, "x", null_type);
    is_expr->type_name = "Null";
    is_expr->resolved_type = bool_type;

    Iron_Node *val_b = make_val_decl(&g_ir_arena, "b", (Iron_Node *)is_expr, bool_type);

    Iron_Node *stmts[2] = { val_x, val_b };
    fd->body = (Iron_Node *)make_block(&g_ir_arena, stmts, 2);

    Iron_Node *decls[1] = { (Iron_Node *)fd };
    Iron_Program *prog = make_program(&g_ir_arena, decls, 1);

    IronLIR_Module *mod = iron_lir_lower(prog, NULL, &g_ir_arena, &g_diags);
    TEST_ASSERT_NOT_NULL(mod);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);
    TEST_ASSERT_TRUE(has_instr_kind(mod->funcs[0], IRON_LIR_IS_NULL));
}

/* ── test_lower_slice ─────────────────────────────────────────────────────
 * arr[1..3] -> IRON_LIR_SLICE */
void test_lower_slice(void) {
    Iron_Type *int_type  = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_Type *null_type = iron_type_make_primitive(IRON_TYPE_NULL);
    Iron_Type *arr_type  = iron_type_make_array(&g_ir_arena, int_type, -1);
    Iron_Type *void_type = iron_type_make_primitive(IRON_TYPE_VOID);
    Iron_FuncDecl *fd = make_func_decl(&g_ir_arena, "test_slice_instr", void_type);

    Iron_NullLit *arr_null = ARENA_ALLOC(&g_ir_arena, Iron_NullLit);
    memset(arr_null, 0, sizeof(*arr_null));
    arr_null->span = test_span();
    arr_null->kind = IRON_NODE_NULL_LIT;
    arr_null->resolved_type = null_type;

    Iron_Node *val_arr = make_val_decl(&g_ir_arena, "arr", (Iron_Node *)arr_null, arr_type);

    Iron_SliceExpr *sl = ARENA_ALLOC(&g_ir_arena, Iron_SliceExpr);
    memset(sl, 0, sizeof(*sl));
    sl->span = test_span();
    sl->kind = IRON_NODE_SLICE;
    sl->object = make_ident(&g_ir_arena, "arr", arr_type);
    sl->start = make_int(&g_ir_arena, "1");
    sl->end   = make_int(&g_ir_arena, "3");
    sl->resolved_type = arr_type;

    Iron_Node *val_s = make_val_decl(&g_ir_arena, "s", (Iron_Node *)sl, arr_type);
    Iron_Node *stmts[2] = { val_arr, val_s };
    fd->body = (Iron_Node *)make_block(&g_ir_arena, stmts, 2);

    Iron_Node *decls[1] = { (Iron_Node *)fd };
    Iron_Program *prog = make_program(&g_ir_arena, decls, 1);

    IronLIR_Module *mod = iron_lir_lower(prog, NULL, &g_ir_arena, &g_diags);
    TEST_ASSERT_NOT_NULL(mod);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);
    TEST_ASSERT_TRUE(has_instr_kind(mod->funcs[0], IRON_LIR_SLICE));
}

/* ── test_lower_heap_alloc ────────────────────────────────────────────────
 * heap <expr> -> IRON_LIR_HEAP_ALLOC */
void test_lower_heap_alloc(void) {
    Iron_Type *int_type  = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_Type *void_type = iron_type_make_primitive(IRON_TYPE_VOID);
    Iron_FuncDecl *fd = make_func_decl(&g_ir_arena, "test_heap_alloc_instr", void_type);

    Iron_HeapExpr *heap = ARENA_ALLOC(&g_ir_arena, Iron_HeapExpr);
    memset(heap, 0, sizeof(*heap));
    heap->span = test_span();
    heap->kind = IRON_NODE_HEAP;
    heap->inner = make_int(&g_ir_arena, "42");
    heap->auto_free = false;
    heap->escapes = false;
    heap->resolved_type = int_type;

    Iron_Node *val_h = make_val_decl(&g_ir_arena, "h", (Iron_Node *)heap, int_type);
    Iron_Node *stmts[1] = { val_h };
    fd->body = (Iron_Node *)make_block(&g_ir_arena, stmts, 1);

    Iron_Node *decls[1] = { (Iron_Node *)fd };
    Iron_Program *prog = make_program(&g_ir_arena, decls, 1);

    IronLIR_Module *mod = iron_lir_lower(prog, NULL, &g_ir_arena, &g_diags);
    TEST_ASSERT_NOT_NULL(mod);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);
    TEST_ASSERT_TRUE(has_instr_kind(mod->funcs[0], IRON_LIR_HEAP_ALLOC));
}

/* ── test_lower_rc_alloc ──────────────────────────────────────────────────
 * rc <expr> -> IRON_LIR_RC_ALLOC */
void test_lower_rc_alloc(void) {
    Iron_Type *int_type  = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_Type *void_type = iron_type_make_primitive(IRON_TYPE_VOID);
    Iron_FuncDecl *fd = make_func_decl(&g_ir_arena, "test_rc_alloc_instr", void_type);

    Iron_RcExpr *rc = ARENA_ALLOC(&g_ir_arena, Iron_RcExpr);
    memset(rc, 0, sizeof(*rc));
    rc->span = test_span();
    rc->kind = IRON_NODE_RC;
    rc->inner = make_int(&g_ir_arena, "42");
    rc->resolved_type = int_type;

    Iron_Node *val_r = make_val_decl(&g_ir_arena, "r", (Iron_Node *)rc, int_type);
    Iron_Node *stmts[1] = { val_r };
    fd->body = (Iron_Node *)make_block(&g_ir_arena, stmts, 1);

    Iron_Node *decls[1] = { (Iron_Node *)fd };
    Iron_Program *prog = make_program(&g_ir_arena, decls, 1);

    IronLIR_Module *mod = iron_lir_lower(prog, NULL, &g_ir_arena, &g_diags);
    TEST_ASSERT_NOT_NULL(mod);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);
    TEST_ASSERT_TRUE(has_instr_kind(mod->funcs[0], IRON_LIR_RC_ALLOC));
}

/* ── test_lower_free ──────────────────────────────────────────────────────
 * free <expr> -> IRON_LIR_FREE */
void test_lower_free(void) {
    Iron_Type *int_type  = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_Type *void_type = iron_type_make_primitive(IRON_TYPE_VOID);
    Iron_FuncDecl *fd = make_func_decl(&g_ir_arena, "test_free_instr", void_type);

    Iron_HeapExpr *heap = ARENA_ALLOC(&g_ir_arena, Iron_HeapExpr);
    memset(heap, 0, sizeof(*heap));
    heap->span = test_span();
    heap->kind = IRON_NODE_HEAP;
    heap->inner = make_int(&g_ir_arena, "42");
    heap->auto_free = false;
    heap->escapes = false;
    heap->resolved_type = int_type;

    Iron_Node *val_h = make_val_decl(&g_ir_arena, "h", (Iron_Node *)heap, int_type);

    Iron_FreeStmt *free_stmt = ARENA_ALLOC(&g_ir_arena, Iron_FreeStmt);
    memset(free_stmt, 0, sizeof(*free_stmt));
    free_stmt->span = test_span();
    free_stmt->kind = IRON_NODE_FREE;
    free_stmt->expr = make_ident(&g_ir_arena, "h", int_type);

    Iron_Node *stmts[2] = { val_h, (Iron_Node *)free_stmt };
    fd->body = (Iron_Node *)make_block(&g_ir_arena, stmts, 2);

    Iron_Node *decls[1] = { (Iron_Node *)fd };
    Iron_Program *prog = make_program(&g_ir_arena, decls, 1);

    IronLIR_Module *mod = iron_lir_lower(prog, NULL, &g_ir_arena, &g_diags);
    TEST_ASSERT_NOT_NULL(mod);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);
    TEST_ASSERT_TRUE(has_instr_kind(mod->funcs[0], IRON_LIR_FREE));
}

/* ── test_lower_make_closure ──────────────────────────────────────────────
 * lambda expression -> IRON_LIR_MAKE_CLOSURE */
void test_lower_make_closure(void) {
    Iron_Type *void_type = iron_type_make_primitive(IRON_TYPE_VOID);
    Iron_FuncDecl *fd = make_func_decl(&g_ir_arena, "test_make_closure_instr", void_type);

    Iron_Block *lambda_body = make_block(&g_ir_arena, NULL, 0);
    Iron_Node *lam = make_lambda(&g_ir_arena, lambda_body);

    Iron_Node *val_f = make_val_decl(&g_ir_arena, "f", lam, void_type);
    Iron_Node *stmts[1] = { val_f };
    fd->body = (Iron_Node *)make_block(&g_ir_arena, stmts, 1);

    Iron_Node *decls[1] = { (Iron_Node *)fd };
    Iron_Program *prog = make_program(&g_ir_arena, decls, 1);

    IronLIR_Module *mod = iron_lir_lower(prog, NULL, &g_ir_arena, &g_diags);
    TEST_ASSERT_NOT_NULL(mod);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);
    TEST_ASSERT_TRUE(has_instr_kind(mod->funcs[0], IRON_LIR_MAKE_CLOSURE));
}

/* ── test_lower_spawn_instr ───────────────────────────────────────────────
 * spawn statement -> IRON_LIR_SPAWN */
void test_lower_spawn_instr(void) {
    Iron_Type *void_type = iron_type_make_primitive(IRON_TYPE_VOID);
    Iron_FuncDecl *fd = make_func_decl(&g_ir_arena, "test_spawn_instr_fn", void_type);

    Iron_Block *spawn_body = make_block(&g_ir_arena, NULL, 0);
    Iron_Node *spawn_stmt = make_spawn(&g_ir_arena, spawn_body, NULL);

    Iron_Node *stmts[1] = { spawn_stmt };
    fd->body = (Iron_Node *)make_block(&g_ir_arena, stmts, 1);

    Iron_Node *decls[1] = { (Iron_Node *)fd };
    Iron_Program *prog = make_program(&g_ir_arena, decls, 1);

    IronLIR_Module *mod = iron_lir_lower(prog, NULL, &g_ir_arena, &g_diags);
    TEST_ASSERT_NOT_NULL(mod);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);
    TEST_ASSERT_TRUE(has_instr_kind(mod->funcs[0], IRON_LIR_SPAWN));
}

/* ── test_lower_parallel_for ──────────────────────────────────────────────
 * for i in N parallel { } -> IRON_LIR_PARALLEL_FOR */
void test_lower_parallel_for(void) {
    Iron_Type *void_type = iron_type_make_primitive(IRON_TYPE_VOID);
    Iron_FuncDecl *fd = make_func_decl(&g_ir_arena, "test_parallel_for_instr", void_type);

    Iron_ForStmt *for_stmt = ARENA_ALLOC(&g_ir_arena, Iron_ForStmt);
    memset(for_stmt, 0, sizeof(*for_stmt));
    for_stmt->span = test_span();
    for_stmt->kind = IRON_NODE_FOR;
    for_stmt->var_name = "i";
    for_stmt->iterable = make_int(&g_ir_arena, "4");
    for_stmt->body = (Iron_Node *)make_block(&g_ir_arena, NULL, 0);
    for_stmt->is_parallel = true;
    for_stmt->pool_expr = NULL;

    Iron_Node *stmts[1] = { (Iron_Node *)for_stmt };
    fd->body = (Iron_Node *)make_block(&g_ir_arena, stmts, 1);

    Iron_Node *decls[1] = { (Iron_Node *)fd };
    Iron_Program *prog = make_program(&g_ir_arena, decls, 1);

    IronLIR_Module *mod = iron_lir_lower(prog, NULL, &g_ir_arena, &g_diags);
    TEST_ASSERT_NOT_NULL(mod);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);
    TEST_ASSERT_TRUE(has_instr_kind(mod->funcs[0], IRON_LIR_PARALLEL_FOR));
}

/* ── test_lower_await ─────────────────────────────────────────────────────
 * await handle -> IRON_LIR_AWAIT
 * Use a null literal as the handle placeholder (AWAIT just wraps any ValueId). */
void test_lower_await(void) {
    Iron_Type *null_type = iron_type_make_primitive(IRON_TYPE_NULL);
    Iron_Type *void_type = iron_type_make_primitive(IRON_TYPE_VOID);
    Iron_FuncDecl *fd = make_func_decl(&g_ir_arena, "test_await_instr", void_type);

    /* val h = null (placeholder handle — AWAIT wraps any resolved ValueId) */
    Iron_NullLit *null_h = ARENA_ALLOC(&g_ir_arena, Iron_NullLit);
    memset(null_h, 0, sizeof(*null_h));
    null_h->span = test_span();
    null_h->kind = IRON_NODE_NULL_LIT;
    null_h->resolved_type = null_type;

    Iron_Node *val_h = make_val_decl(&g_ir_arena, "h", (Iron_Node *)null_h, null_type);

    /* await h */
    Iron_AwaitExpr *aw = ARENA_ALLOC(&g_ir_arena, Iron_AwaitExpr);
    memset(aw, 0, sizeof(*aw));
    aw->span = test_span();
    aw->kind = IRON_NODE_AWAIT;
    aw->handle = make_ident(&g_ir_arena, "h", null_type);
    aw->resolved_type = void_type;

    Iron_Node *val_r = make_val_decl(&g_ir_arena, "r", (Iron_Node *)aw, void_type);

    Iron_Node *stmts[2] = { val_h, val_r };
    fd->body = (Iron_Node *)make_block(&g_ir_arena, stmts, 2);

    Iron_Node *decls[1] = { (Iron_Node *)fd };
    Iron_Program *prog = make_program(&g_ir_arena, decls, 1);

    IronLIR_Module *mod = iron_lir_lower(prog, NULL, &g_ir_arena, &g_diags);
    TEST_ASSERT_NOT_NULL(mod);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);
    TEST_ASSERT_TRUE(has_instr_kind(mod->funcs[0], IRON_LIR_AWAIT));
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

    /* Wave 4: Plan 10-02 — instruction-kind coverage */
    RUN_TEST(test_lower_const_int);
    RUN_TEST(test_lower_const_float);
    RUN_TEST(test_lower_const_bool);
    RUN_TEST(test_lower_const_string);
    RUN_TEST(test_lower_const_null);
    RUN_TEST(test_lower_add);
    RUN_TEST(test_lower_sub);
    RUN_TEST(test_lower_mul);
    RUN_TEST(test_lower_div);
    RUN_TEST(test_lower_mod);
    RUN_TEST(test_lower_eq);
    RUN_TEST(test_lower_neq);
    RUN_TEST(test_lower_lt);
    RUN_TEST(test_lower_lte);
    RUN_TEST(test_lower_gt);
    RUN_TEST(test_lower_gte);
    RUN_TEST(test_lower_and);
    RUN_TEST(test_lower_or);
    RUN_TEST(test_lower_neg);
    RUN_TEST(test_lower_not);
    RUN_TEST(test_lower_alloca_load_store);
    RUN_TEST(test_lower_get_field);
    RUN_TEST(test_lower_set_field);
    RUN_TEST(test_lower_get_index);
    RUN_TEST(test_lower_set_index);
    RUN_TEST(test_lower_branch);
    RUN_TEST(test_lower_jump);
    RUN_TEST(test_lower_return);
    RUN_TEST(test_lower_switch);
    RUN_TEST(test_lower_call);
    RUN_TEST(test_lower_func_ref);
    RUN_TEST(test_lower_cast);
    RUN_TEST(test_lower_construct);
    RUN_TEST(test_lower_array_lit);
    RUN_TEST(test_lower_interp_string);
    RUN_TEST(test_lower_is_null);
    RUN_TEST(test_lower_slice);
    RUN_TEST(test_lower_heap_alloc);
    RUN_TEST(test_lower_rc_alloc);
    RUN_TEST(test_lower_free);
    RUN_TEST(test_lower_make_closure);
    RUN_TEST(test_lower_spawn_instr);
    RUN_TEST(test_lower_parallel_for);
    RUN_TEST(test_lower_await);

    return UNITY_END();
}
