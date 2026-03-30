/* test_ir_print.c — Unity tests for the IR printer (TOOL-01).
 *
 * Each test builds a hand-constructed IR module and verifies that
 * iron_lir_print() produces the expected LLVM-style text fragments.
 *
 * Wave 2 (Plan 10-02): adds snapshot comparison tests using the
 * AST->HIR->LIR pipeline and compare_snapshot()/write_snapshot()
 * golden-master pattern.
 */

#include "unity.h"
#include "lir/lir.h"
#include "lir/print.h"
#include "hir/hir_lower.h"
#include "hir/hir_to_lir.h"
#include "lir/verify.h"
#include "parser/ast.h"
#include "analyzer/types.h"
#include "diagnostics/diagnostics.h"
#include "util/arena.h"
#include "lexer/lexer.h"

#include <string.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>

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

/* ── Helpers ──────────────────────────────────────────────────────────────── */

static Iron_Span zero_span(void) {
    return iron_span_make("test.iron", 1, 1, 1, 1);
}

static bool str_contains(const char *haystack, const char *needle) {
    if (!haystack || !needle) return false;
    return strstr(haystack, needle) != NULL;
}

/* Snapshot comparison: read expected file and strcmp against actual output. */
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

/* Write golden master snapshot. */
static void write_snapshot(const char *ir_output, const char *expected_path) {
    FILE *f = fopen(expected_path, "w");
    if (!f) { printf("Cannot write snapshot: %s\n", expected_path); return; }
    fputs(ir_output, f);
    fclose(f);
}

/* ── Minimal AST construction helpers (for snapshot tests) ────────────────── */

static Iron_Span test_span_p(void) {
    return iron_span_make("test.iron", 1, 1, 1, 1);
}

static Iron_Node *make_int_p(Iron_Arena *arena, const char *value) {
    Iron_IntLit *n = ARENA_ALLOC(arena, Iron_IntLit);
    memset(n, 0, sizeof(*n));
    n->span = test_span_p();
    n->kind = IRON_NODE_INT_LIT;
    n->value = value;
    n->resolved_type = iron_type_make_primitive(IRON_TYPE_INT);
    return (Iron_Node *)n;
}

static Iron_Node *make_ident_p(Iron_Arena *arena, const char *name, Iron_Type *type) {
    Iron_Ident *n = ARENA_ALLOC(arena, Iron_Ident);
    memset(n, 0, sizeof(*n));
    n->span = test_span_p();
    n->kind = IRON_NODE_IDENT;
    n->name = name;
    n->resolved_type = type;
    return (Iron_Node *)n;
}

static Iron_Node *make_binary_p(Iron_Arena *arena, Iron_Node *left, int op,
                                  Iron_Node *right, Iron_Type *result_type) {
    Iron_BinaryExpr *n = ARENA_ALLOC(arena, Iron_BinaryExpr);
    memset(n, 0, sizeof(*n));
    n->span = test_span_p();
    n->kind = IRON_NODE_BINARY;
    n->left = left;
    n->op = op;
    n->right = right;
    n->resolved_type = result_type;
    return (Iron_Node *)n;
}

static Iron_Node *make_return_p(Iron_Arena *arena, Iron_Node *value) {
    Iron_ReturnStmt *n = ARENA_ALLOC(arena, Iron_ReturnStmt);
    memset(n, 0, sizeof(*n));
    n->span = test_span_p();
    n->kind = IRON_NODE_RETURN;
    n->value = value;
    return (Iron_Node *)n;
}

static Iron_Node *make_var_decl_p(Iron_Arena *arena, const char *name,
                                    Iron_Node *init, Iron_Type *declared_type) {
    Iron_VarDecl *n = ARENA_ALLOC(arena, Iron_VarDecl);
    memset(n, 0, sizeof(*n));
    n->span = test_span_p();
    n->kind = IRON_NODE_VAR_DECL;
    n->name = name;
    n->init = init;
    n->declared_type = declared_type;
    return (Iron_Node *)n;
}

static Iron_Node *make_val_decl_p(Iron_Arena *arena, const char *name,
                                    Iron_Node *init, Iron_Type *declared_type) {
    Iron_ValDecl *n = ARENA_ALLOC(arena, Iron_ValDecl);
    memset(n, 0, sizeof(*n));
    n->span = test_span_p();
    n->kind = IRON_NODE_VAL_DECL;
    n->name = name;
    n->init = init;
    n->declared_type = declared_type;
    return (Iron_Node *)n;
}

static Iron_Node *make_assign_p(Iron_Arena *arena, Iron_Node *target, Iron_Node *value) {
    Iron_AssignStmt *n = ARENA_ALLOC(arena, Iron_AssignStmt);
    memset(n, 0, sizeof(*n));
    n->span = test_span_p();
    n->kind = IRON_NODE_ASSIGN;
    n->target = target;
    n->value = value;
    n->op = IRON_TOK_ASSIGN;
    return (Iron_Node *)n;
}

static Iron_Block *make_block_p(Iron_Arena *arena, Iron_Node **stmts, int count) {
    Iron_Block *b = ARENA_ALLOC(arena, Iron_Block);
    memset(b, 0, sizeof(*b));
    b->span = test_span_p();
    b->kind = IRON_NODE_BLOCK;
    if (count > 0) {
        b->stmts = (Iron_Node **)iron_arena_alloc(arena,
                       (size_t)count * sizeof(Iron_Node *), _Alignof(Iron_Node *));
        memcpy(b->stmts, stmts, (size_t)count * sizeof(Iron_Node *));
    }
    b->stmt_count = count;
    return b;
}

static Iron_Node *make_if_p(Iron_Arena *arena, Iron_Node *condition,
                               Iron_Block *body, Iron_Block *else_body) {
    Iron_IfStmt *n = ARENA_ALLOC(arena, Iron_IfStmt);
    memset(n, 0, sizeof(*n));
    n->span = test_span_p();
    n->kind = IRON_NODE_IF;
    n->condition = condition;
    n->body = (Iron_Node *)body;
    n->else_body = else_body ? (Iron_Node *)else_body : NULL;
    n->elif_conds  = NULL;
    n->elif_bodies = NULL;
    n->elif_count  = 0;
    return (Iron_Node *)n;
}

static Iron_Node *make_while_p(Iron_Arena *arena, Iron_Node *condition, Iron_Block *body) {
    Iron_WhileStmt *n = ARENA_ALLOC(arena, Iron_WhileStmt);
    memset(n, 0, sizeof(*n));
    n->span = test_span_p();
    n->kind = IRON_NODE_WHILE;
    n->condition = condition;
    n->body = (Iron_Node *)body;
    return (Iron_Node *)n;
}

static Iron_Param *make_param_p(Iron_Arena *arena, const char *name) {
    Iron_Param *p = ARENA_ALLOC(arena, Iron_Param);
    memset(p, 0, sizeof(*p));
    p->span = test_span_p();
    p->kind = IRON_NODE_PARAM;
    p->name = name;
    return p;
}

static Iron_FuncDecl *make_func_decl_p(Iron_Arena *arena, const char *name,
                                          Iron_Type *return_type) {
    Iron_Block *body = ARENA_ALLOC(arena, Iron_Block);
    memset(body, 0, sizeof(*body));
    body->span = test_span_p();
    body->kind = IRON_NODE_BLOCK;
    body->stmts = NULL;
    body->stmt_count = 0;

    Iron_FuncDecl *fd = ARENA_ALLOC(arena, Iron_FuncDecl);
    memset(fd, 0, sizeof(*fd));
    fd->span = test_span_p();
    fd->kind = IRON_NODE_FUNC_DECL;
    fd->name = name;
    fd->params = NULL;
    fd->param_count = 0;
    fd->body = (Iron_Node *)body;
    fd->resolved_return_type = return_type;
    return fd;
}

static Iron_Program *make_program_p(Iron_Arena *arena, Iron_Node **decls, int count) {
    Iron_Program *prog = ARENA_ALLOC(arena, Iron_Program);
    memset(prog, 0, sizeof(*prog));
    prog->span = test_span_p();
    prog->kind = IRON_NODE_PROGRAM;
    if (count > 0) {
        prog->decls = (Iron_Node **)iron_arena_alloc(arena,
                          (size_t)count * sizeof(Iron_Node *), _Alignof(Iron_Node *));
        memcpy(prog->decls, decls, (size_t)count * sizeof(Iron_Node *));
    }
    prog->decl_count = count;
    return prog;
}

/* Try to snapshot-compare. If file is missing or placeholder, write golden master. */
static bool snapshot_test(const char *ir_text, const char *snap_path) {
    FILE *f = fopen(snap_path, "r");
    bool is_placeholder = true;
    if (f) {
        char buf[64];
        size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        buf[n] = '\0';
        fclose(f);
        is_placeholder = (n == 0) || (strstr(buf, "placeholder") != NULL);
    }
    if (is_placeholder) {
        write_snapshot(ir_text, snap_path);
    }
    return compare_snapshot(ir_text, snap_path);
}

/* Lower AST program to LIR using the HIR pipeline (AST->HIR->LIR). */
static IronLIR_Module *lower_prog_to_lir(Iron_Program *program,
                                          Iron_Arena *lir_arena,
                                          Iron_DiagList *diags) {
    IronHIR_Module *hir = iron_hir_lower(program, NULL, NULL, diags);
    if (!hir) return NULL;
    IronLIR_Module *lir = iron_hir_to_lir(hir, program, NULL, lir_arena, diags);
    iron_hir_module_destroy(hir);
    return lir;
}

/* ── Tests: hand-constructed IR ──────────────────────────────────────────── */

void test_print_empty_module(void) {
    Iron_Arena ir_arena = iron_arena_create(4096);
    iron_types_init(&ir_arena);

    IronLIR_Module *mod = iron_lir_module_create(&ir_arena, "test");
    char *out = iron_lir_print(mod, false);

    TEST_ASSERT_NOT_NULL(out);
    TEST_ASSERT_TRUE(str_contains(out, "; Module: test"));

    free(out);
    iron_lir_module_destroy(mod);
    iron_arena_free(&ir_arena);
}

void test_print_const_int(void) {
    Iron_Arena ir_arena = iron_arena_create(4096);
    iron_types_init(&ir_arena);

    IronLIR_Module *mod = iron_lir_module_create(&ir_arena, "test");
    Iron_Type *int_type = iron_type_make_primitive(IRON_TYPE_INT);
    IronLIR_Func *fn = iron_lir_func_create(mod, "Iron_main", NULL, 0, int_type);
    IronLIR_Block *entry = iron_lir_block_create(fn, "entry");
    Iron_Span span = zero_span();

    IronLIR_Instr *c = iron_lir_const_int(fn, entry, 42, int_type, span);
    iron_lir_return(fn, entry, c->id, false, int_type, span);

    char *out = iron_lir_print(mod, false);

    TEST_ASSERT_NOT_NULL(out);
    TEST_ASSERT_TRUE(str_contains(out, "func @Iron_main"));
    TEST_ASSERT_TRUE(str_contains(out, "entry:"));
    TEST_ASSERT_TRUE(str_contains(out, "%1 = const_int 42 : Int"));
    TEST_ASSERT_TRUE(str_contains(out, "ret %1"));

    free(out);
    iron_lir_module_destroy(mod);
    iron_arena_free(&ir_arena);
}

void test_print_binop(void) {
    Iron_Arena ir_arena = iron_arena_create(4096);
    iron_types_init(&ir_arena);

    IronLIR_Module *mod = iron_lir_module_create(&ir_arena, "test");
    Iron_Type *int_type = iron_type_make_primitive(IRON_TYPE_INT);
    IronLIR_Func *fn = iron_lir_func_create(mod, "fn", NULL, 0, int_type);
    IronLIR_Block *entry = iron_lir_block_create(fn, "entry");
    Iron_Span span = zero_span();

    IronLIR_Instr *c1 = iron_lir_const_int(fn, entry, 10, int_type, span);
    IronLIR_Instr *c2 = iron_lir_const_int(fn, entry, 20, int_type, span);
    IronLIR_Instr *add = iron_lir_binop(fn, entry, IRON_LIR_ADD, c1->id, c2->id, int_type, span);
    iron_lir_return(fn, entry, add->id, false, int_type, span);

    char *out = iron_lir_print(mod, false);

    TEST_ASSERT_NOT_NULL(out);
    TEST_ASSERT_TRUE(str_contains(out, "%3 = add %1, %2 : Int"));

    free(out);
    iron_lir_module_destroy(mod);
    iron_arena_free(&ir_arena);
}

void test_print_alloca_load_store(void) {
    Iron_Arena ir_arena = iron_arena_create(4096);
    iron_types_init(&ir_arena);

    IronLIR_Module *mod = iron_lir_module_create(&ir_arena, "test");
    Iron_Type *int_type = iron_type_make_primitive(IRON_TYPE_INT);
    IronLIR_Func *fn = iron_lir_func_create(mod, "fn", NULL, 0, NULL);
    IronLIR_Block *entry = iron_lir_block_create(fn, "entry");
    Iron_Span span = zero_span();

    IronLIR_Instr *ptr = iron_lir_alloca(fn, entry, int_type, NULL, span);
    IronLIR_Instr *val = iron_lir_const_int(fn, entry, 42, int_type, span);
    iron_lir_store(fn, entry, ptr->id, val->id, span);
    iron_lir_load(fn, entry, ptr->id, int_type, span);
    iron_lir_return(fn, entry, IRON_LIR_VALUE_INVALID, true, NULL, span);

    char *out = iron_lir_print(mod, false);

    TEST_ASSERT_NOT_NULL(out);
    TEST_ASSERT_TRUE(str_contains(out, "%1 = alloca Int"));
    TEST_ASSERT_TRUE(str_contains(out, "store %1, %2"));
    TEST_ASSERT_TRUE(str_contains(out, "%3 = load %1 : Int"));

    free(out);
    iron_lir_module_destroy(mod);
    iron_arena_free(&ir_arena);
}

void test_print_branch(void) {
    Iron_Arena ir_arena = iron_arena_create(4096);
    iron_types_init(&ir_arena);

    IronLIR_Module *mod = iron_lir_module_create(&ir_arena, "test");
    Iron_Type *bool_type = iron_type_make_primitive(IRON_TYPE_BOOL);
    IronLIR_Func *fn = iron_lir_func_create(mod, "fn", NULL, 0, NULL);
    IronLIR_Block *entry  = iron_lir_block_create(fn, "entry");
    IronLIR_Block *then_b = iron_lir_block_create(fn, "then");
    IronLIR_Block *else_b = iron_lir_block_create(fn, "else");
    Iron_Span span = zero_span();

    IronLIR_Instr *cond = iron_lir_const_bool(fn, entry, true, bool_type, span);
    iron_lir_branch(fn, entry, cond->id, then_b->id, else_b->id, span);

    iron_lir_return(fn, then_b, IRON_LIR_VALUE_INVALID, true, NULL, span);
    iron_lir_return(fn, else_b, IRON_LIR_VALUE_INVALID, true, NULL, span);

    char *out = iron_lir_print(mod, false);

    TEST_ASSERT_NOT_NULL(out);
    TEST_ASSERT_TRUE(str_contains(out, "branch %1, then, else"));

    free(out);
    iron_lir_module_destroy(mod);
    iron_arena_free(&ir_arena);
}

void test_print_annotations_on(void) {
    Iron_Arena ir_arena = iron_arena_create(4096);
    iron_types_init(&ir_arena);

    IronLIR_Module *mod = iron_lir_module_create(&ir_arena, "test");
    Iron_Type *int_type = iron_type_make_primitive(IRON_TYPE_INT);
    IronLIR_Func *fn = iron_lir_func_create(mod, "fn", NULL, 0, NULL);
    IronLIR_Block *entry = iron_lir_block_create(fn, "entry");
    Iron_Span span = zero_span();

    IronLIR_Instr *val = iron_lir_const_int(fn, entry, 1, int_type, span);
    /* heap_alloc with auto_free=true */
    iron_lir_heap_alloc(fn, entry, val->id, true, false, int_type, span);
    iron_lir_return(fn, entry, IRON_LIR_VALUE_INVALID, true, NULL, span);

    char *out = iron_lir_print(mod, true);

    TEST_ASSERT_NOT_NULL(out);
    TEST_ASSERT_TRUE(str_contains(out, "; auto_free"));

    free(out);
    iron_lir_module_destroy(mod);
    iron_arena_free(&ir_arena);
}

void test_print_annotations_off(void) {
    Iron_Arena ir_arena = iron_arena_create(4096);
    iron_types_init(&ir_arena);

    IronLIR_Module *mod = iron_lir_module_create(&ir_arena, "test");
    Iron_Type *int_type = iron_type_make_primitive(IRON_TYPE_INT);
    IronLIR_Func *fn = iron_lir_func_create(mod, "fn", NULL, 0, NULL);
    IronLIR_Block *entry = iron_lir_block_create(fn, "entry");
    Iron_Span span = zero_span();

    IronLIR_Instr *val = iron_lir_const_int(fn, entry, 1, int_type, span);
    iron_lir_heap_alloc(fn, entry, val->id, true, false, int_type, span);
    iron_lir_return(fn, entry, IRON_LIR_VALUE_INVALID, true, NULL, span);

    /* With annotations OFF, "; auto_free" should NOT appear */
    char *out = iron_lir_print(mod, false);

    TEST_ASSERT_NOT_NULL(out);
    TEST_ASSERT_FALSE(str_contains(out, "; auto_free"));

    free(out);
    iron_lir_module_destroy(mod);
    iron_arena_free(&ir_arena);
}

void test_print_function_params(void) {
    Iron_Arena ir_arena = iron_arena_create(4096);
    iron_types_init(&ir_arena);

    IronLIR_Module *mod = iron_lir_module_create(&ir_arena, "test");
    Iron_Type *int_type   = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_Type *float_type = iron_type_make_primitive(IRON_TYPE_FLOAT);

    IronLIR_Param params[2] = {
        { "x", int_type   },
        { "y", float_type }
    };

    IronLIR_Func *fn = iron_lir_func_create(mod, "add", params, 2, int_type);
    IronLIR_Block *entry = iron_lir_block_create(fn, "entry");
    Iron_Span span = zero_span();

    IronLIR_Instr *c = iron_lir_const_int(fn, entry, 0, int_type, span);
    iron_lir_return(fn, entry, c->id, false, int_type, span);

    char *out = iron_lir_print(mod, false);

    TEST_ASSERT_NOT_NULL(out);
    TEST_ASSERT_TRUE(str_contains(out, "func @add("));
    TEST_ASSERT_TRUE(str_contains(out, "x: Int"));
    TEST_ASSERT_TRUE(str_contains(out, "y: Float"));

    free(out);
    iron_lir_module_destroy(mod);
    iron_arena_free(&ir_arena);
}

/* ── Wave 2: Snapshot tests (Plan 10-02) ─────────────────────────────────── */

/* test_snapshot_control_flow: if/else + while loop */
void test_snapshot_control_flow(void) {
    Iron_Type *int_type  = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_Type *bool_type = iron_type_make_primitive(IRON_TYPE_BOOL);
    Iron_Type *void_type = iron_type_make_primitive(IRON_TYPE_VOID);
    Iron_FuncDecl *fd = make_func_decl_p(&g_ir_arena, "main", void_type);

    /* var x = 10 */
    Iron_Node *var_x = make_var_decl_p(&g_ir_arena, "x",
                                        make_int_p(&g_ir_arena, "10"), int_type);

    /* if x > 5 { } else { } */
    Iron_Node *cond = make_binary_p(&g_ir_arena,
                                     make_ident_p(&g_ir_arena, "x", int_type),
                                     IRON_TOK_GREATER,
                                     make_int_p(&g_ir_arena, "5"),
                                     bool_type);
    Iron_Block *then_body = make_block_p(&g_ir_arena, NULL, 0);
    Iron_Block *else_body = make_block_p(&g_ir_arena, NULL, 0);
    Iron_Node *if_stmt = make_if_p(&g_ir_arena, cond, then_body, else_body);

    /* while x > 0 { x = x - 1 } */
    Iron_Node *while_cond = make_binary_p(&g_ir_arena,
                                           make_ident_p(&g_ir_arena, "x", int_type),
                                           IRON_TOK_GREATER,
                                           make_int_p(&g_ir_arena, "0"),
                                           bool_type);
    Iron_Node *x_minus_1 = make_binary_p(&g_ir_arena,
                                           make_ident_p(&g_ir_arena, "x", int_type),
                                           IRON_TOK_MINUS,
                                           make_int_p(&g_ir_arena, "1"),
                                           int_type);
    Iron_Node *assign_x = make_assign_p(&g_ir_arena,
                                         make_ident_p(&g_ir_arena, "x", int_type),
                                         x_minus_1);
    Iron_Node *body_stmts[1] = { assign_x };
    Iron_Block *while_body = make_block_p(&g_ir_arena, body_stmts, 1);
    Iron_Node *while_stmt = make_while_p(&g_ir_arena, while_cond, while_body);

    Iron_Node *fn_stmts[3] = { var_x, if_stmt, while_stmt };
    fd->body = (Iron_Node *)make_block_p(&g_ir_arena, fn_stmts, 3);

    Iron_Node *decls[1] = { (Iron_Node *)fd };
    Iron_Program *prog = make_program_p(&g_ir_arena, decls, 1);

    IronLIR_Module *mod = lower_prog_to_lir(prog, &g_ir_arena, &g_diags);
    TEST_ASSERT_NOT_NULL(mod);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);

    char *ir_text = iron_lir_print(mod, false);
    TEST_ASSERT_NOT_NULL(ir_text);

    bool match = snapshot_test(ir_text, "tests/lir/snapshots/control_flow.expected");
    free(ir_text);
    TEST_ASSERT_TRUE_MESSAGE(match, "control_flow IR snapshot mismatch");
    (void)void_type;
}

/* test_snapshot_function_call: add(a,b) + main() calling add */
void test_snapshot_function_call(void) {
    Iron_Type *int_type  = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_Type *void_type = iron_type_make_primitive(IRON_TYPE_VOID);

    /* func add(a: Int, b: Int) -> Int { return a + b } */
    Iron_Param *param_a = make_param_p(&g_ir_arena, "a");
    Iron_TypeAnnotation *int_ann_a = ARENA_ALLOC(&g_ir_arena, Iron_TypeAnnotation);
    memset(int_ann_a, 0, sizeof(*int_ann_a));
    int_ann_a->span = test_span_p();
    int_ann_a->kind = IRON_NODE_TYPE_ANNOTATION;
    int_ann_a->name = "Int";
    param_a->type_ann = (Iron_Node *)int_ann_a;

    Iron_Param *param_b = make_param_p(&g_ir_arena, "b");
    Iron_TypeAnnotation *int_ann_b = ARENA_ALLOC(&g_ir_arena, Iron_TypeAnnotation);
    memset(int_ann_b, 0, sizeof(*int_ann_b));
    int_ann_b->span = test_span_p();
    int_ann_b->kind = IRON_NODE_TYPE_ANNOTATION;
    int_ann_b->name = "Int";
    param_b->type_ann = (Iron_Node *)int_ann_b;

    Iron_FuncDecl *add_fn = make_func_decl_p(&g_ir_arena, "add", int_type);
    add_fn->params = (Iron_Node **)iron_arena_alloc(&g_ir_arena,
                         2 * sizeof(Iron_Node *), _Alignof(Iron_Node *));
    add_fn->params[0] = (Iron_Node *)param_a;
    add_fn->params[1] = (Iron_Node *)param_b;
    add_fn->param_count = 2;

    Iron_Node *sum = make_binary_p(&g_ir_arena,
                                    make_ident_p(&g_ir_arena, "a", int_type),
                                    IRON_TOK_PLUS,
                                    make_ident_p(&g_ir_arena, "b", int_type),
                                    int_type);
    Iron_Node *ret_sum = make_return_p(&g_ir_arena, sum);
    Iron_Node *add_stmts[1] = { ret_sum };
    add_fn->body = (Iron_Node *)make_block_p(&g_ir_arena, add_stmts, 1);

    /* func main() { val result = add(3, 4) } */
    Iron_FuncDecl *main_fn = make_func_decl_p(&g_ir_arena, "main", void_type);

    Iron_CallExpr *call = ARENA_ALLOC(&g_ir_arena, Iron_CallExpr);
    memset(call, 0, sizeof(*call));
    call->span = test_span_p();
    call->kind = IRON_NODE_CALL;
    call->callee = make_ident_p(&g_ir_arena, "add", int_type);
    call->resolved_type = int_type;
    call->args = (Iron_Node **)iron_arena_alloc(&g_ir_arena,
                     2 * sizeof(Iron_Node *), _Alignof(Iron_Node *));
    call->args[0] = make_int_p(&g_ir_arena, "3");
    call->args[1] = make_int_p(&g_ir_arena, "4");
    call->arg_count = 2;

    Iron_Node *val_result = make_val_decl_p(&g_ir_arena, "result",
                                              (Iron_Node *)call, int_type);
    Iron_Node *main_stmts[1] = { val_result };
    main_fn->body = (Iron_Node *)make_block_p(&g_ir_arena, main_stmts, 1);

    Iron_Node *decls[2] = { (Iron_Node *)add_fn, (Iron_Node *)main_fn };
    Iron_Program *prog = make_program_p(&g_ir_arena, decls, 2);

    IronLIR_Module *mod = lower_prog_to_lir(prog, &g_ir_arena, &g_diags);
    TEST_ASSERT_NOT_NULL(mod);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);

    char *ir_text = iron_lir_print(mod, false);
    TEST_ASSERT_NOT_NULL(ir_text);

    bool match = snapshot_test(ir_text, "tests/lir/snapshots/function_call.expected");
    free(ir_text);
    TEST_ASSERT_TRUE_MESSAGE(match, "function_call IR snapshot mismatch");
    (void)void_type;
}

/* test_snapshot_memory_ops: mutable counter with increments */
void test_snapshot_memory_ops(void) {
    Iron_Type *int_type  = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_Type *void_type = iron_type_make_primitive(IRON_TYPE_VOID);
    Iron_FuncDecl *fd = make_func_decl_p(&g_ir_arena, "main", void_type);

    /* var counter = 0 */
    Iron_Node *var_counter = make_var_decl_p(&g_ir_arena, "counter",
                                               make_int_p(&g_ir_arena, "0"), int_type);

    /* counter = counter + 1 */
    Iron_Node *counter_p1 = make_binary_p(&g_ir_arena,
                                           make_ident_p(&g_ir_arena, "counter", int_type),
                                           IRON_TOK_PLUS,
                                           make_int_p(&g_ir_arena, "1"),
                                           int_type);
    Iron_Node *assign1 = make_assign_p(&g_ir_arena,
                                        make_ident_p(&g_ir_arena, "counter", int_type),
                                        counter_p1);

    /* counter = counter + 1 (again) */
    Iron_Node *counter_p2 = make_binary_p(&g_ir_arena,
                                           make_ident_p(&g_ir_arena, "counter", int_type),
                                           IRON_TOK_PLUS,
                                           make_int_p(&g_ir_arena, "1"),
                                           int_type);
    Iron_Node *assign2 = make_assign_p(&g_ir_arena,
                                        make_ident_p(&g_ir_arena, "counter", int_type),
                                        counter_p2);

    Iron_Node *stmts[3] = { var_counter, assign1, assign2 };
    fd->body = (Iron_Node *)make_block_p(&g_ir_arena, stmts, 3);

    Iron_Node *decls[1] = { (Iron_Node *)fd };
    Iron_Program *prog = make_program_p(&g_ir_arena, decls, 1);

    IronLIR_Module *mod = lower_prog_to_lir(prog, &g_ir_arena, &g_diags);
    TEST_ASSERT_NOT_NULL(mod);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);

    char *ir_text = iron_lir_print(mod, false);
    TEST_ASSERT_NOT_NULL(ir_text);

    bool match = snapshot_test(ir_text, "tests/lir/snapshots/memory_ops.expected");
    free(ir_text);
    TEST_ASSERT_TRUE_MESSAGE(match, "memory_ops IR snapshot mismatch");
    (void)void_type;
}

/* test_snapshot_object_construct: Point object + construction + field access */
void test_snapshot_object_construct(void) {
    Iron_Type *int_type  = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_Type *void_type = iron_type_make_primitive(IRON_TYPE_VOID);

    Iron_ObjectDecl *obj = ARENA_ALLOC(&g_ir_arena, Iron_ObjectDecl);
    memset(obj, 0, sizeof(*obj));
    obj->span = test_span_p();
    obj->kind = IRON_NODE_OBJECT_DECL;
    obj->name = "Point";

    Iron_Type *point_type = iron_type_make_object(&g_ir_arena, obj);

    Iron_FuncDecl *fd = make_func_decl_p(&g_ir_arena, "main", void_type);

    /* val p = Point(3, 7) */
    Iron_ConstructExpr *con = ARENA_ALLOC(&g_ir_arena, Iron_ConstructExpr);
    memset(con, 0, sizeof(*con));
    con->span = test_span_p();
    con->kind = IRON_NODE_CONSTRUCT;
    con->type_name = "Point";
    con->resolved_type = point_type;
    con->args = (Iron_Node **)iron_arena_alloc(&g_ir_arena,
                    2 * sizeof(Iron_Node *), _Alignof(Iron_Node *));
    con->args[0] = make_int_p(&g_ir_arena, "3");
    con->args[1] = make_int_p(&g_ir_arena, "7");
    con->arg_count = 2;

    Iron_Node *val_p = make_val_decl_p(&g_ir_arena, "p", (Iron_Node *)con, point_type);

    /* val px = p.x */
    Iron_FieldAccess *fa = ARENA_ALLOC(&g_ir_arena, Iron_FieldAccess);
    memset(fa, 0, sizeof(*fa));
    fa->span = test_span_p();
    fa->kind = IRON_NODE_FIELD_ACCESS;
    fa->object = make_ident_p(&g_ir_arena, "p", point_type);
    fa->field = "x";
    fa->resolved_type = int_type;

    Iron_Node *val_px = make_val_decl_p(&g_ir_arena, "px", (Iron_Node *)fa, int_type);

    Iron_Node *fn_stmts[2] = { val_p, val_px };
    fd->body = (Iron_Node *)make_block_p(&g_ir_arena, fn_stmts, 2);

    Iron_Node *decls[2] = { (Iron_Node *)obj, (Iron_Node *)fd };
    Iron_Program *prog = make_program_p(&g_ir_arena, decls, 2);

    IronLIR_Module *mod = lower_prog_to_lir(prog, &g_ir_arena, &g_diags);
    TEST_ASSERT_NOT_NULL(mod);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);

    char *ir_text = iron_lir_print(mod, false);
    TEST_ASSERT_NOT_NULL(ir_text);

    bool match = snapshot_test(ir_text, "tests/lir/snapshots/object_construct.expected");
    free(ir_text);
    TEST_ASSERT_TRUE_MESSAGE(match, "object_construct IR snapshot mismatch");
    (void)void_type;
}

/* test_snapshot_string_interp: interpolated string with two parts */
void test_snapshot_string_interp(void) {
    Iron_Type *int_type  = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_Type *str_type  = iron_type_make_primitive(IRON_TYPE_STRING);
    Iron_Type *void_type = iron_type_make_primitive(IRON_TYPE_VOID);
    Iron_FuncDecl *fd = make_func_decl_p(&g_ir_arena, "main", void_type);

    /* val age = 42 */
    Iron_Node *val_age = make_val_decl_p(&g_ir_arena, "age",
                                          make_int_p(&g_ir_arena, "42"), int_type);

    /* "age is {age}" */
    Iron_StringLit *prefix = ARENA_ALLOC(&g_ir_arena, Iron_StringLit);
    memset(prefix, 0, sizeof(*prefix));
    prefix->span = test_span_p();
    prefix->kind = IRON_NODE_STRING_LIT;
    prefix->value = "age is ";
    prefix->resolved_type = str_type;

    Iron_InterpString *interp = ARENA_ALLOC(&g_ir_arena, Iron_InterpString);
    memset(interp, 0, sizeof(*interp));
    interp->span = test_span_p();
    interp->kind = IRON_NODE_INTERP_STRING;
    interp->resolved_type = str_type;
    interp->parts = (Iron_Node **)iron_arena_alloc(&g_ir_arena,
                        2 * sizeof(Iron_Node *), _Alignof(Iron_Node *));
    interp->parts[0] = (Iron_Node *)prefix;
    interp->parts[1] = make_ident_p(&g_ir_arena, "age", int_type);
    interp->part_count = 2;

    Iron_Node *val_s = make_val_decl_p(&g_ir_arena, "s", (Iron_Node *)interp, str_type);
    Iron_Node *stmts[2] = { val_age, val_s };
    fd->body = (Iron_Node *)make_block_p(&g_ir_arena, stmts, 2);

    Iron_Node *decls[1] = { (Iron_Node *)fd };
    Iron_Program *prog = make_program_p(&g_ir_arena, decls, 1);

    IronLIR_Module *mod = lower_prog_to_lir(prog, &g_ir_arena, &g_diags);
    TEST_ASSERT_NOT_NULL(mod);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);

    char *ir_text = iron_lir_print(mod, false);
    TEST_ASSERT_NOT_NULL(ir_text);

    bool match = snapshot_test(ir_text, "tests/lir/snapshots/string_interp.expected");
    free(ir_text);
    TEST_ASSERT_TRUE_MESSAGE(match, "string_interp IR snapshot mismatch");
    (void)void_type;
}

/* test_snapshot_mutable_var: swap two mutable variables */
void test_snapshot_mutable_var(void) {
    Iron_Type *int_type  = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_Type *void_type = iron_type_make_primitive(IRON_TYPE_VOID);
    Iron_FuncDecl *fd = make_func_decl_p(&g_ir_arena, "main", void_type);

    /* var a = 1; var b = 2 */
    Iron_Node *var_a = make_var_decl_p(&g_ir_arena, "a",
                                        make_int_p(&g_ir_arena, "1"), int_type);
    Iron_Node *var_b = make_var_decl_p(&g_ir_arena, "b",
                                        make_int_p(&g_ir_arena, "2"), int_type);
    /* val tmp = a */
    Iron_Node *val_tmp = make_val_decl_p(&g_ir_arena, "tmp",
                                          make_ident_p(&g_ir_arena, "a", int_type), int_type);
    /* a = b */
    Iron_Node *assign_a = make_assign_p(&g_ir_arena,
                                         make_ident_p(&g_ir_arena, "a", int_type),
                                         make_ident_p(&g_ir_arena, "b", int_type));
    /* b = tmp */
    Iron_Node *assign_b = make_assign_p(&g_ir_arena,
                                         make_ident_p(&g_ir_arena, "b", int_type),
                                         make_ident_p(&g_ir_arena, "tmp", int_type));

    Iron_Node *stmts[5] = { var_a, var_b, val_tmp, assign_a, assign_b };
    fd->body = (Iron_Node *)make_block_p(&g_ir_arena, stmts, 5);

    Iron_Node *decls[1] = { (Iron_Node *)fd };
    Iron_Program *prog = make_program_p(&g_ir_arena, decls, 1);

    IronLIR_Module *mod = lower_prog_to_lir(prog, &g_ir_arena, &g_diags);
    TEST_ASSERT_NOT_NULL(mod);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);

    char *ir_text = iron_lir_print(mod, false);
    TEST_ASSERT_NOT_NULL(ir_text);

    bool match = snapshot_test(ir_text, "tests/lir/snapshots/mutable_var.expected");
    free(ir_text);
    TEST_ASSERT_TRUE_MESSAGE(match, "mutable_var IR snapshot mismatch");
    (void)void_type;
}

/* ── Main ─────────────────────────────────────────────────────────────────── */

int main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_print_empty_module);
    RUN_TEST(test_print_const_int);
    RUN_TEST(test_print_binop);
    RUN_TEST(test_print_alloca_load_store);
    RUN_TEST(test_print_branch);
    RUN_TEST(test_print_annotations_on);
    RUN_TEST(test_print_annotations_off);
    RUN_TEST(test_print_function_params);

    /* Wave 2: snapshot tests (Plan 10-02) */
    RUN_TEST(test_snapshot_control_flow);
    RUN_TEST(test_snapshot_function_call);
    RUN_TEST(test_snapshot_memory_ops);
    RUN_TEST(test_snapshot_object_construct);
    RUN_TEST(test_snapshot_string_interp);
    RUN_TEST(test_snapshot_mutable_var);

    return UNITY_END();
}
