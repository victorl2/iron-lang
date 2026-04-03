/* test_init_check.c -- Unity tests for the Iron definite assignment analysis pass.
 *
 * Tests cover:
 *   - var without init used before assignment => E0314
 *   - var with init used after init => no E0314
 *   - var assigned then used => no E0314
 *   - val always has init => no E0314
 *   - function param always initialized => no E0314
 */

#include "unity.h"
#include "analyzer/resolve.h"
#include "analyzer/typecheck.h"
#include "analyzer/init_check.h"
#include "analyzer/scope.h"
#include "analyzer/types.h"
#include "parser/ast.h"
#include "parser/parser.h"
#include "lexer/lexer.h"
#include "diagnostics/diagnostics.h"
#include "util/arena.h"

#include <string.h>
#include <stdbool.h>

/* ── Module-level fixtures ───────────────────────────────────────────────── */

static Iron_Arena    g_arena;
static Iron_DiagList g_diags;

void setUp(void) {
    g_arena = iron_arena_create(1024 * 512);
    g_diags = iron_diaglist_create();
    iron_types_init(&g_arena);
}

void tearDown(void) {
    iron_arena_free(&g_arena);
    iron_diaglist_free(&g_diags);
}

/* ── Parse + resolve + typecheck + init_check helper ────────────────────── */

static void run_init_check(const char *src) {
    Iron_Lexer   l      = iron_lexer_create(src, "test.iron", &g_arena, &g_diags);
    Iron_Token  *tokens = iron_lex_all(&l);
    int          count  = 0;
    while (tokens[count].kind != IRON_TOK_EOF) count++;
    count++;
    Iron_Parser  p    = iron_parser_create(tokens, count, src, "test.iron",
                                           &g_arena, &g_diags);
    Iron_Node   *root = iron_parse(&p);
    Iron_Program *prog = (Iron_Program *)root;
    Iron_Scope   *global = iron_resolve(prog, &g_arena, &g_diags);
    iron_typecheck(prog, global, &g_arena, &g_diags);
    iron_init_check(prog, global, &g_arena, &g_diags);
}

static bool has_error(int code) {
    for (int i = 0; i < g_diags.count; i++) {
        if (g_diags.items[i].code == code) return true;
    }
    return false;
}

/* ── Tests ──────────────────────────────────────────────────────────────── */

/* Test 1: var without init used before assignment => E0314 */
void test_var_used_before_init(void) {
    run_init_check("fn main() {\n  var x: Int\n  val y = x\n}");
    TEST_ASSERT_TRUE(has_error(IRON_ERR_POSSIBLY_UNINITIALIZED));
}

/* Test 2: var with init used after init => no E0314 */
void test_var_with_init_no_error(void) {
    run_init_check("fn main() {\n  var x: Int = 5\n  val y = x\n}");
    TEST_ASSERT_FALSE(has_error(IRON_ERR_POSSIBLY_UNINITIALIZED));
}

/* Test 3: var assigned then used => no E0314 */
void test_var_assigned_then_used_no_error(void) {
    run_init_check("fn main() {\n  var x: Int\n  x = 5\n  val y = x\n}");
    TEST_ASSERT_FALSE(has_error(IRON_ERR_POSSIBLY_UNINITIALIZED));
}

/* Test 4: val always has init => no E0314 */
void test_val_always_initialized(void) {
    run_init_check("fn main() {\n  val x = 10\n  val y = x\n}");
    TEST_ASSERT_FALSE(has_error(IRON_ERR_POSSIBLY_UNINITIALIZED));
}

/* Test 5: function param always initialized => no E0314 */
void test_param_always_initialized(void) {
    run_init_check("fn foo(x: Int) {\n  val y = x\n}");
    TEST_ASSERT_FALSE(has_error(IRON_ERR_POSSIBLY_UNINITIALIZED));
}

/* ── Runner ─────────────────────────────────────────────────────────────── */

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_var_used_before_init);
    RUN_TEST(test_var_with_init_no_error);
    RUN_TEST(test_var_assigned_then_used_no_error);
    RUN_TEST(test_val_always_initialized);
    RUN_TEST(test_param_always_initialized);
    return UNITY_END();
}
