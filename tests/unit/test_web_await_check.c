/* test_web_await_check.c — Unity tests for the web_await_check analyzer pass.
 *
 * Tests cover WEB-RUNTIME-04: iron_web_await_check emits E0501 when `await`
 * is reachable from `main` on IRON_TARGET_WEB, and is a no-op on native.
 *
 * Cases:
 *   1. `await` directly in `main` on web target => E0501 emitted
 *   2. Same program on native target => no E0501
 *   3. Program with no `await` on web target => no E0501
 */

#include "unity.h"
#include "analyzer/web_await_check.h"
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

/* Build an Iron_AwaitExpr whose handle is an integer literal */
static Iron_AwaitExpr *make_await(Iron_Arena *a) {
    Iron_IntLit *handle = make_int_lit(a);

    Iron_AwaitExpr *ae = ARENA_ALLOC(a, Iron_AwaitExpr);
    ae->span          = ts(2, 5);
    ae->kind          = IRON_NODE_AWAIT;
    ae->resolved_type = NULL;
    ae->handle        = (Iron_Node *)handle;
    return ae;
}

/* Wrap stmts in a function with the given name and return an Iron_Program. */
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

/* Build a two-function program:
 *   func helper() { }   (no await)
 *   func main()   { }   (no await)
 */
static Iron_Program *make_no_await_prog(Iron_Arena *a) {
    /* helper(): empty body */
    Iron_Block *helper_body = ARENA_ALLOC(a, Iron_Block);
    helper_body->span       = ts(1, 18);
    helper_body->kind       = IRON_NODE_BLOCK;
    helper_body->stmts      = NULL;
    helper_body->stmt_count = 0;

    Iron_FuncDecl *helper_fn = ARENA_ALLOC(a, Iron_FuncDecl);
    helper_fn->span                 = ts(1, 1);
    helper_fn->kind                 = IRON_NODE_FUNC_DECL;
    helper_fn->name                 = "helper";
    helper_fn->params               = NULL;
    helper_fn->param_count          = 0;
    helper_fn->return_type          = NULL;
    helper_fn->body                 = (Iron_Node *)helper_body;
    helper_fn->is_private           = false;
    helper_fn->is_extern            = false;
    helper_fn->extern_c_name        = NULL;
    helper_fn->generic_params       = NULL;
    helper_fn->generic_param_count  = 0;
    helper_fn->resolved_return_type = NULL;
    helper_fn->is_fusible           = false;

    /* main(): empty body */
    Iron_Block *main_body = ARENA_ALLOC(a, Iron_Block);
    main_body->span       = ts(2, 14);
    main_body->kind       = IRON_NODE_BLOCK;
    main_body->stmts      = NULL;
    main_body->stmt_count = 0;

    Iron_FuncDecl *main_fn = ARENA_ALLOC(a, Iron_FuncDecl);
    main_fn->span                 = ts(2, 1);
    main_fn->kind                 = IRON_NODE_FUNC_DECL;
    main_fn->name                 = "main";
    main_fn->params               = NULL;
    main_fn->param_count          = 0;
    main_fn->return_type          = NULL;
    main_fn->body                 = (Iron_Node *)main_body;
    main_fn->is_private           = false;
    main_fn->is_extern            = false;
    main_fn->extern_c_name        = NULL;
    main_fn->generic_params       = NULL;
    main_fn->generic_param_count  = 0;
    main_fn->resolved_return_type = NULL;
    main_fn->is_fusible           = false;

    Iron_Node **decls = iron_arena_alloc(a, 2 * sizeof(Iron_Node *),
                                          _Alignof(Iron_Node *));
    decls[0] = (Iron_Node *)helper_fn;
    decls[1] = (Iron_Node *)main_fn;

    Iron_Program *prog = ARENA_ALLOC(a, Iron_Program);
    prog->span       = ts(1, 1);
    prog->kind       = IRON_NODE_PROGRAM;
    prog->decls      = decls;
    prog->decl_count = 2;
    return prog;
}

/* ── Test 1: `await` directly in main on web => E0501 ───────────────────── */

void test_await_in_main_on_web_errors(void) {
    /* Program:
     *   func main() { await 0 }
     *
     * Binding invariant: iron_web_await_check emits code 501 with target=WEB.
     */
    Iron_AwaitExpr *ae = make_await(&g_arena);

    Iron_Node **stmts = iron_arena_alloc(&g_arena, sizeof(Iron_Node *),
                                          _Alignof(Iron_Node *));
    stmts[0] = (Iron_Node *)ae;

    Iron_Program *prog = make_prog_with_func(&g_arena, "main", stmts, 1);

    iron_web_await_check(prog, &g_arena, &g_diags, IRON_TARGET_WEB, NULL);

    TEST_ASSERT_TRUE(g_diags.error_count >= 1);
    TEST_ASSERT_TRUE(has_code(501));
    TEST_ASSERT_TRUE(msg_contains("--target=web"));
}

/* ── Test 2: same program on native => no E0501 ─────────────────────────── */

void test_await_in_main_on_native_no_501(void) {
    /* Same fixture as test 1. Binding invariant: no E0501 on native.
     * (Earlier passes may emit other diagnostics — we only assert on 501.) */
    Iron_AwaitExpr *ae = make_await(&g_arena);

    Iron_Node **stmts = iron_arena_alloc(&g_arena, sizeof(Iron_Node *),
                                          _Alignof(Iron_Node *));
    stmts[0] = (Iron_Node *)ae;

    Iron_Program *prog = make_prog_with_func(&g_arena, "main", stmts, 1);

    iron_web_await_check(prog, &g_arena, &g_diags, IRON_TARGET_NATIVE, NULL);

    TEST_ASSERT_FALSE(has_code(501));
}

/* ── Test 3: no await on web => no E0501 ────────────────────────────────── */

void test_no_await_on_web_ok(void) {
    /* Program:
     *   func helper() { }
     *   func main()   { }
     *
     * No `await` anywhere. Binding invariant: no E0501 on web.
     */
    Iron_Program *prog = make_no_await_prog(&g_arena);

    iron_web_await_check(prog, &g_arena, &g_diags, IRON_TARGET_WEB, NULL);

    TEST_ASSERT_FALSE(has_code(501));
}

/* ── Main ────────────────────────────────────────────────────────────────── */

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_await_in_main_on_web_errors);
    RUN_TEST(test_await_in_main_on_native_no_501);
    RUN_TEST(test_no_await_on_web_ok);
    return UNITY_END();
}
