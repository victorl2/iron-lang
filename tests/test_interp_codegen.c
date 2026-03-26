/* test_interp_codegen.c — Unity tests verifying string interpolation codegen.
 *
 * Tests cover:
 *   - "value is {x}" with Int x emits snprintf and %lld
 *   - "pi is {y}" with Float y emits %g
 *   - "flag is {b}" with Bool b emits "true" : "false" ternary
 *   - "hello {name}" with String name emits iron_string_cstr
 *   - iron_string_from_literal (not iron_string_from_cstr) used for result
 */

#include "unity.h"
#include "codegen/codegen.h"
#include "analyzer/resolve.h"
#include "analyzer/typecheck.h"
#include "analyzer/escape.h"
#include "analyzer/scope.h"
#include "analyzer/types.h"
#include "parser/ast.h"
#include "parser/parser.h"
#include "lexer/lexer.h"
#include "diagnostics/diagnostics.h"
#include "util/arena.h"

#include <string.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

/* ── Module-level fixtures ───────────────────────────────────────────────── */

static Iron_Arena    g_arena;
static Iron_DiagList g_diags;

void setUp(void) {
    g_arena = iron_arena_create(1024 * 1024);
    g_diags = iron_diaglist_create();
    iron_types_init(&g_arena);
}

void tearDown(void) {
    iron_arena_free(&g_arena);
    iron_diaglist_free(&g_diags);
}

/* ── Full pipeline helper: source -> generated C string ──────────────────── */

static const char *run_codegen(const char *src) {
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
    iron_escape_analyze(prog, global, &g_arena, &g_diags);
    return iron_codegen(prog, global, &g_arena, &g_diags);
}

/* ── Test: Int interpolation emits snprintf and %lld ─────────────────────── */

void test_interp_int(void) {
    const char *src =
        "func main() {\n"
        "    val x = 42\n"
        "    val s = \"value is {x}\"\n"
        "    println(s)\n"
        "}\n";
    const char *c = run_codegen(src);
    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(c, "snprintf"),
        "should emit snprintf for int interpolation");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(c, "%lld"),
        "should emit %lld format specifier for Int interpolation");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(c, "_ibuf"),
        "should emit _ibuf stack buffer for interpolation");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(c, "iron_string_from_literal"),
        "should wrap result with iron_string_from_literal");
}

/* ── Test: Float interpolation emits %g ─────────────────────────────────── */

void test_interp_float(void) {
    const char *src =
        "func main() {\n"
        "    val y = 3.14\n"
        "    val s = \"pi is {y}\"\n"
        "    println(s)\n"
        "}\n";
    const char *c = run_codegen(src);
    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(c, "snprintf"),
        "should emit snprintf for float interpolation");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(c, "%g"),
        "should emit %g format specifier for Float interpolation");
}

/* ── Test: Bool interpolation emits ternary "true" : "false" ─────────────── */

void test_interp_bool(void) {
    const char *src =
        "func main() {\n"
        "    val b = true\n"
        "    val s = \"flag is {b}\"\n"
        "    println(s)\n"
        "}\n";
    const char *c = run_codegen(src);
    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(c, "snprintf"),
        "should emit snprintf for bool interpolation");
    /* Bool uses %s with ternary "true" : "false" */
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(c, "\"true\""),
        "should emit \"true\" string for Bool ternary");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(c, "\"false\""),
        "should emit \"false\" string for Bool ternary");
}

/* ── Test: String interpolation emits iron_string_cstr ───────────────────── */

void test_interp_string(void) {
    const char *src =
        "func main() {\n"
        "    val name = \"world\"\n"
        "    val s = \"hello {name}\"\n"
        "    println(s)\n"
        "}\n";
    const char *c = run_codegen(src);
    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(c, "snprintf"),
        "should emit snprintf for string interpolation");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(c, "iron_string_cstr"),
        "should emit iron_string_cstr for String-typed interpolation arg");
}

/* ── Test: result wrapped with iron_string_from_literal (not from_cstr) ──── */

void test_interp_from_literal(void) {
    const char *src =
        "func main() {\n"
        "    val x = 1\n"
        "    val s = \"x is {x}\"\n"
        "    println(s)\n"
        "}\n";
    const char *c = run_codegen(src);
    TEST_ASSERT_NOT_NULL(c);
    /* Must use iron_string_from_literal for the interpolation result */
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(c, "iron_string_from_literal"),
        "should wrap interpolation result with iron_string_from_literal");
    /* Confirm the interpolation path does NOT use iron_string_from_cstr
     * for the interpolation result (it may appear for other literals) */
    const char *lit_pos = strstr(c, "iron_string_from_literal(_ibuf");
    if (lit_pos == NULL) {
        /* Also accept heap path pattern */
        lit_pos = strstr(c, "iron_string_from_literal(_ihp");
    }
    TEST_ASSERT_NOT_NULL_MESSAGE(lit_pos,
        "interpolation result should use iron_string_from_literal with buffer");
}

/* ── Test: no-expression interpolation (pure literal) still works ─────────── */

void test_interp_no_exprs(void) {
    /* Edge case: interpolation string with no actual {expr} parts */
    const char *src =
        "func main() {\n"
        "    val s = \"hello world\"\n"
        "    println(s)\n"
        "}\n";
    const char *c = run_codegen(src);
    TEST_ASSERT_NOT_NULL(c);
    /* Regular string lit — should use iron_string_from_literal */
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(c, "iron_string_from_literal"),
        "plain string literal should use iron_string_from_literal");
}

/* ── main ─────────────────────────────────────────────────────────────────── */

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_interp_int);
    RUN_TEST(test_interp_float);
    RUN_TEST(test_interp_bool);
    RUN_TEST(test_interp_string);
    RUN_TEST(test_interp_from_literal);
    RUN_TEST(test_interp_no_exprs);
    return UNITY_END();
}
