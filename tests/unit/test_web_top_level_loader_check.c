/* test_web_top_level_loader_check.c — Unity tests for web_top_level_loader_check.
 *
 * Tests cover WEB-ASSET-03: iron_web_top_level_loader_check emits E0502 when
 * a raylib resource-loader function (LoadTexture, LoadSound, LoadFont,
 * LoadModel) is called at module level on IRON_TARGET_WEB, and is a no-op on
 * native targets and for calls inside function bodies.
 *
 * Cases:
 *   1. top-level `LoadTexture("x.png")` on web => E0502 emitted
 *   2. same fixture on native target => no E0502
 *   3. `LoadTexture("x.png")` INSIDE func main() on web => no E0502
 *   4. top-level LoadTexture + LoadSound + LoadFont + LoadModel on web =>
 *      >= 4 errors, messages contain each function name
 *
 * Note: as of Iron v1.1.0-alpha the language has no module-level executable
 * statements.  All executable code lives inside function bodies.  These tests
 * build synthetic AST fixtures that the parser never produces in practice; the
 * check is infrastructure for when module-level expressions are added.
 * Test cases 1, 2, and 4 use a top-level VAL_DECL whose init is the
 * forbidden call — the closest approximation to a module-level call using the
 * existing AST node set.
 */

#include "unity.h"
#include "analyzer/web_top_level_loader_check.h"
#include "parser/ast.h"
#include "diagnostics/diagnostics.h"
#include "util/arena.h"
#include "cli/build.h"

#include <string.h>
#include <stdbool.h>

/* ── Module-level fixtures ───────────────────────────────────────────────── */

static Iron_Arena    g_arena;
static Iron_DiagList g_diags;

void setUp(void) {
    g_arena = iron_arena_create(1024 * 512);
    g_diags = iron_diaglist_create();
}

void tearDown(void) {
    iron_arena_free(&g_arena);
    iron_diaglist_free(&g_diags);
}

static bool has_code(int code) {
    for (int i = 0; i < g_diags.count; i++) {
        if (g_diags.items[i].code == code) return true;
    }
    return false;
}

static bool msg_contains(const char *substr) {
    for (int i = 0; i < g_diags.count; i++) {
        if (g_diags.items[i].message &&
            strstr(g_diags.items[i].message, substr)) return true;
    }
    return false;
}

/* ── Span helper ─────────────────────────────────────────────────────────── */

static Iron_Span ts(int l, int c) {
    return iron_span_make("test.iron", (uint32_t)l, (uint32_t)c,
                          (uint32_t)l, (uint32_t)(c + 1));
}

/* ── AST builder helpers ─────────────────────────────────────────────────── */

/* Build an Iron_IntLit with value "0" */
static Iron_IntLit *make_int_lit(Iron_Arena *a) {
    Iron_IntLit *lit = ARENA_ALLOC(a, Iron_IntLit);
    lit->span          = ts(2, 15);
    lit->kind          = IRON_NODE_INT_LIT;
    lit->resolved_type = NULL;
    lit->value         = "0";
    return lit;
}

/* Build a call expression: `<callee_name>(0)` */
static Iron_CallExpr *make_call(Iron_Arena *a, const char *callee_name,
                                int line) {
    Iron_Ident *callee = ARENA_ALLOC(a, Iron_Ident);
    callee->span          = ts(line, 1);
    callee->kind          = IRON_NODE_IDENT;
    callee->resolved_type = NULL;
    callee->name          = callee_name;
    callee->resolved_sym  = NULL;
    callee->constraint_name = NULL;

    Iron_Node **args = iron_arena_alloc(a, sizeof(Iron_Node *),
                                        _Alignof(Iron_Node *));
    args[0] = (Iron_Node *)make_int_lit(a);

    Iron_CallExpr *ce = ARENA_ALLOC(a, Iron_CallExpr);
    ce->span             = ts(line, 1);
    ce->kind             = IRON_NODE_CALL;
    ce->resolved_type    = NULL;
    ce->callee           = (Iron_Node *)callee;
    ce->args             = args;
    ce->arg_count        = 1;
    ce->is_primitive_cast = false;
    return ce;
}

/* Build a top-level VAL_DECL whose init is the given node.
 * This simulates the closest thing to a module-level call expression. */
static Iron_ValDecl *make_top_level_val_decl(Iron_Arena *a, Iron_Node *init,
                                              int line) {
    Iron_ValDecl *vd = ARENA_ALLOC(a, Iron_ValDecl);
    vd->span           = ts(line, 1);
    vd->kind           = IRON_NODE_VAL_DECL;
    vd->name           = "tex";
    vd->type_ann       = NULL;
    vd->init           = init;
    vd->declared_type  = NULL;
    vd->binding_names  = NULL;
    vd->binding_count  = 0;
    return vd;
}

/* Build a program whose only top-level decl is a VAL_DECL initialised by the
 * given call expression. */
static Iron_Program *make_prog_with_top_level_call(Iron_Arena *a,
                                                    Iron_CallExpr *call) {
    Iron_ValDecl *vd = make_top_level_val_decl(a, (Iron_Node *)call, 1);

    Iron_Node **decls = iron_arena_alloc(a, sizeof(Iron_Node *),
                                          _Alignof(Iron_Node *));
    decls[0] = (Iron_Node *)vd;

    Iron_Program *prog = ARENA_ALLOC(a, Iron_Program);
    prog->span       = ts(1, 1);
    prog->kind       = IRON_NODE_PROGRAM;
    prog->decls      = decls;
    prog->decl_count = 1;
    return prog;
}

/* Wrap stmts in a function with the given name and return an Iron_Program.
 * Used for the "call inside function body" test case. */
static Iron_Program *make_prog_with_func(Iron_Arena *a, const char *fn_name,
                                          Iron_Node **stmts, int stmt_count) {
    Iron_Block *body = ARENA_ALLOC(a, Iron_Block);
    body->span       = ts(1, 14);
    body->kind       = IRON_NODE_BLOCK;
    body->stmts      = stmts;
    body->stmt_count = stmt_count;

    Iron_FuncDecl *fn = ARENA_ALLOC(a, Iron_FuncDecl);
    fn->span                 = ts(1, 1);
    fn->kind                 = IRON_NODE_FUNC_DECL;
    fn->name                 = fn_name;
    fn->params               = NULL;
    fn->param_count          = 0;
    fn->return_type          = NULL;
    fn->body                 = (Iron_Node *)body;
    fn->is_private           = false;
    fn->is_extern            = false;
    fn->extern_c_name        = NULL;
    fn->generic_params       = NULL;
    fn->generic_param_count  = 0;
    fn->resolved_return_type = NULL;
    fn->is_fusible           = false;

    Iron_Node **decls = iron_arena_alloc(a, sizeof(Iron_Node *),
                                          _Alignof(Iron_Node *));
    decls[0] = (Iron_Node *)fn;

    Iron_Program *prog = ARENA_ALLOC(a, Iron_Program);
    prog->span       = ts(1, 1);
    prog->kind       = IRON_NODE_PROGRAM;
    prog->decls      = decls;
    prog->decl_count = 1;
    return prog;
}

/* ── Test 1: top-level LoadTexture on web => E0502 ───────────────────────── */

void test_top_level_LoadTexture_on_web_errors(void) {
    /* Program (synthetic):
     *   val tex = LoadTexture(0)   -- module-level VAL_DECL
     *
     * Binding invariant: iron_web_top_level_loader_check emits code 502,
     * message contains "top-level" and "LoadTexture", on IRON_TARGET_WEB.
     */
    Iron_CallExpr *call = make_call(&g_arena, "LoadTexture", 1);
    Iron_Program  *prog = make_prog_with_top_level_call(&g_arena, call);

    iron_web_top_level_loader_check(prog, &g_arena, &g_diags, IRON_TARGET_WEB, NULL);

    TEST_ASSERT_TRUE(g_diags.error_count >= 1);
    TEST_ASSERT_TRUE(has_code(502));
    TEST_ASSERT_TRUE(msg_contains("top-level"));
    TEST_ASSERT_TRUE(msg_contains("LoadTexture"));
    TEST_ASSERT_TRUE(msg_contains("--target=web"));
}

/* ── Test 2: same fixture on native => no E0502 ─────────────────────────── */

void test_top_level_LoadTexture_on_native_ok(void) {
    /* Same synthetic program as test 1.
     * Binding invariant: no E0502 on IRON_TARGET_NATIVE — the guard is a
     * zero-cost early return for native builds. */
    Iron_CallExpr *call = make_call(&g_arena, "LoadTexture", 1);
    Iron_Program  *prog = make_prog_with_top_level_call(&g_arena, call);

    iron_web_top_level_loader_check(prog, &g_arena, &g_diags, IRON_TARGET_NATIVE, NULL);

    TEST_ASSERT_FALSE(has_code(502));
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);
}

/* ── Test 3: LoadTexture inside func main() on web => no E0502 ──────────── */

void test_in_function_LoadTexture_on_web_ok(void) {
    /* Program:
     *   func main() { LoadTexture(0) }
     *
     * The call is inside a function body (IRON_NODE_FUNC_DECL), so the pass
     * must skip it entirely.  Binding invariant: no E0502 on IRON_TARGET_WEB
     * when the call is function-scoped.
     */
    Iron_CallExpr *call = make_call(&g_arena, "LoadTexture", 2);

    Iron_Node **stmts = iron_arena_alloc(&g_arena, sizeof(Iron_Node *),
                                          _Alignof(Iron_Node *));
    stmts[0] = (Iron_Node *)call;

    Iron_Program *prog = make_prog_with_func(&g_arena, "main", stmts, 1);

    iron_web_top_level_loader_check(prog, &g_arena, &g_diags, IRON_TARGET_WEB, NULL);

    TEST_ASSERT_FALSE(has_code(502));
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);
}

/* ── Test 4: all four loaders at top-level on web => >= 4 errors ─────────── */

void test_all_four_loaders_at_top_level_on_web_errors(void) {
    /* Program (synthetic):
     *   val a = LoadTexture(0)
     *   val b = LoadSound(0)
     *   val c = LoadFont(0)
     *   val d = LoadModel(0)
     *
     * Binding invariant: >= 4 E0502 errors, messages contain each function
     * name.
     */
    Iron_CallExpr *c1 = make_call(&g_arena, "LoadTexture", 1);
    Iron_CallExpr *c2 = make_call(&g_arena, "LoadSound",   2);
    Iron_CallExpr *c3 = make_call(&g_arena, "LoadFont",    3);
    Iron_CallExpr *c4 = make_call(&g_arena, "LoadModel",   4);

    Iron_ValDecl *vd1 = make_top_level_val_decl(&g_arena, (Iron_Node *)c1, 1);
    Iron_ValDecl *vd2 = make_top_level_val_decl(&g_arena, (Iron_Node *)c2, 2);
    Iron_ValDecl *vd3 = make_top_level_val_decl(&g_arena, (Iron_Node *)c3, 3);
    Iron_ValDecl *vd4 = make_top_level_val_decl(&g_arena, (Iron_Node *)c4, 4);

    Iron_Node **decls = iron_arena_alloc(&g_arena, 4 * sizeof(Iron_Node *),
                                          _Alignof(Iron_Node *));
    decls[0] = (Iron_Node *)vd1;
    decls[1] = (Iron_Node *)vd2;
    decls[2] = (Iron_Node *)vd3;
    decls[3] = (Iron_Node *)vd4;

    Iron_Program *prog = ARENA_ALLOC(&g_arena, Iron_Program);
    prog->span       = ts(1, 1);
    prog->kind       = IRON_NODE_PROGRAM;
    prog->decls      = decls;
    prog->decl_count = 4;

    iron_web_top_level_loader_check(prog, &g_arena, &g_diags, IRON_TARGET_WEB, NULL);

    TEST_ASSERT_TRUE(g_diags.error_count >= 4);
    TEST_ASSERT_TRUE(msg_contains("LoadTexture"));
    TEST_ASSERT_TRUE(msg_contains("LoadSound"));
    TEST_ASSERT_TRUE(msg_contains("LoadFont"));
    TEST_ASSERT_TRUE(msg_contains("LoadModel"));
}

/* ── Main ────────────────────────────────────────────────────────────────── */

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_top_level_LoadTexture_on_web_errors);
    RUN_TEST(test_top_level_LoadTexture_on_native_ok);
    RUN_TEST(test_in_function_LoadTexture_on_web_ok);
    RUN_TEST(test_all_four_loaders_at_top_level_on_web_errors);
    return UNITY_END();
}
