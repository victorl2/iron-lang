/* test_pipeline.c — End-to-end pipeline tests for the Iron compiler.
 *
 * Tests exercise the full pipeline: source string -> lex -> parse -> analyze -> codegen.
 * Uses iron_analyze() for the semantic passes (unified entry point).
 *
 * Tests cover:
 *   - pipeline_hello: func main prints hello world
 *   - pipeline_variables: val/var with inference and immutability
 *   - pipeline_arithmetic: val x = 1 + 2 * 3
 *   - pipeline_if: if/elif/else generates correct C
 *   - pipeline_while: while loop generates correct C
 *   - pipeline_function: func with params and return
 *   - pipeline_object: object with fields generates struct
 *   - pipeline_method: method declaration generates C function with self
 *   - pipeline_val_error: val reassignment -> E0203
 *   - pipeline_undefined_error: undefined variable -> E0200
 */

#include "unity.h"
#include "analyzer/analyzer.h"
#include "codegen/codegen.h"
#include "parser/parser.h"
#include "parser/ast.h"
#include "lexer/lexer.h"
#include "diagnostics/diagnostics.h"
#include "util/arena.h"

#include <string.h>
#include <stdbool.h>
#include <stdio.h>

/* ── Module-level fixtures ───────────────────────────────────────────────── */

static Iron_Arena    g_arena;
static Iron_DiagList g_diags;

void setUp(void) {
    g_arena = iron_arena_create(1024 * 1024);
    g_diags = iron_diaglist_create();
}

void tearDown(void) {
    iron_arena_free(&g_arena);
    iron_diaglist_free(&g_diags);
}

/* ── Full pipeline helper: source -> generated C (or NULL on error) ──────── */

static const char *compile_iron(const char *source, Iron_DiagList *diags) {
    Iron_Arena arena = iron_arena_create(256 * 1024);
    *diags = iron_diaglist_create();

    Iron_Lexer   lexer  = iron_lexer_create(source, "test.iron", &arena, diags);
    Iron_Token  *tokens = iron_lex_all(&lexer);
    int          count  = 0;
    while (tokens[count].kind != IRON_TOK_EOF) count++;
    count++;  /* include EOF */

    Iron_Parser  p       = iron_parser_create(tokens, count, source,
                                               "test.iron", &arena, diags);
    Iron_Node   *root    = iron_parse(&p);
    Iron_Program *program = (Iron_Program *)root;

    if (!program || diags->error_count > 0) {
        iron_arena_free(&arena);
        return NULL;
    }

    Iron_AnalyzeResult result = iron_analyze(program, &arena, diags);
    if (result.has_errors) {
        iron_arena_free(&arena);
        return NULL;
    }

    const char *c_code = iron_codegen(program, result.global_scope, &arena, diags);

    /* Note: arena is intentionally not freed here so caller can use the string.
     * In real usage, caller would copy or keep arena alive. */
    (void)arena;  /* Suppress unused warning — arena lives through process */
    return c_code;
}

/* Simplified helper that uses global arena/diags for cases where we want
 * to inspect diags after calling. */
static const char *run_pipeline(const char *src) {
    Iron_Lexer   l      = iron_lexer_create(src, "test.iron", &g_arena, &g_diags);
    Iron_Token  *tokens = iron_lex_all(&l);
    int          count  = 0;
    while (tokens[count].kind != IRON_TOK_EOF) count++;
    count++;

    Iron_Parser  p    = iron_parser_create(tokens, count, src, "test.iron",
                                            &g_arena, &g_diags);
    Iron_Node   *root = iron_parse(&p);
    Iron_Program *prog = (Iron_Program *)root;

    if (!prog || g_diags.error_count > 0) return NULL;

    Iron_AnalyzeResult result = iron_analyze(prog, &g_arena, &g_diags);
    if (result.has_errors) return NULL;

    return iron_codegen(prog, result.global_scope, &g_arena, &g_diags);
}

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static bool has_diag_code(Iron_DiagList *diags, int code) {
    for (int i = 0; i < diags->count; i++) {
        if (diags->items[i].code == code) return true;
    }
    return false;
}

/* ── Test: hello world pipeline ──────────────────────────────────────────── */

void test_pipeline_hello(void) {
    const char *src =
        "func main() {\n"
        "    println(\"hello\")\n"
        "}\n";
    const char *c = run_pipeline(src);
    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);
    /* Iron_ prefix applied to main */
    TEST_ASSERT_NOT_NULL(strstr(c, "Iron_main"));
    /* printf used for println */
    TEST_ASSERT_NOT_NULL(strstr(c, "printf"));
    /* String content present */
    TEST_ASSERT_NOT_NULL(strstr(c, "hello"));
}

/* ── Test: val/var variable declarations ─────────────────────────────────── */

void test_pipeline_variables(void) {
    const char *src =
        "func main() {\n"
        "    val x = 42\n"
        "    var y = 10\n"
        "}\n";
    const char *c = run_pipeline(src);
    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);
    /* val generates const */
    TEST_ASSERT_NOT_NULL(strstr(c, "const int64_t"));
    /* var generates mutable */
    TEST_ASSERT_NOT_NULL(strstr(c, "int64_t"));
    /* Iron_ prefix on main */
    TEST_ASSERT_NOT_NULL(strstr(c, "Iron_main"));
}

/* ── Test: arithmetic expression ─────────────────────────────────────────── */

void test_pipeline_arithmetic(void) {
    const char *src =
        "func main() {\n"
        "    val x = 1 + 2 * 3\n"
        "}\n";
    const char *c = run_pipeline(src);
    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);
    /* Expression components must appear */
    TEST_ASSERT_NOT_NULL(strstr(c, "1"));
    TEST_ASSERT_NOT_NULL(strstr(c, "2"));
    TEST_ASSERT_NOT_NULL(strstr(c, "3"));
    TEST_ASSERT_NOT_NULL(strstr(c, "+"));
    TEST_ASSERT_NOT_NULL(strstr(c, "*"));
}

/* ── Test: if/elif/else generates correct C ──────────────────────────────── */

void test_pipeline_if(void) {
    const char *src =
        "func main() {\n"
        "    val x = 5\n"
        "    if x > 3 {\n"
        "        val a = 1\n"
        "    } elif x > 1 {\n"
        "        val b = 2\n"
        "    } else {\n"
        "        val c = 3\n"
        "    }\n"
        "}\n";
    const char *c = run_pipeline(src);
    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);
    TEST_ASSERT_NOT_NULL(strstr(c, "if ("));
    TEST_ASSERT_NOT_NULL(strstr(c, "else if ("));
    TEST_ASSERT_NOT_NULL(strstr(c, "else {"));
}

/* ── Test: while loop generates correct C ────────────────────────────────── */

void test_pipeline_while(void) {
    const char *src =
        "func main() {\n"
        "    var i = 0\n"
        "    while i < 10 {\n"
        "        i = i + 1\n"
        "    }\n"
        "}\n";
    const char *c = run_pipeline(src);
    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);
    TEST_ASSERT_NOT_NULL(strstr(c, "while ("));
}

/* ── Test: function with params and return ───────────────────────────────── */

void test_pipeline_function(void) {
    const char *src =
        "func add(a: Int, b: Int) -> Int {\n"
        "    return a + b\n"
        "}\n"
        "func main() {\n"
        "    val result = add(10, 20)\n"
        "}\n";
    const char *c = run_pipeline(src);
    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);
    /* Mangled function name */
    TEST_ASSERT_NOT_NULL(strstr(c, "Iron_add"));
    /* Return type */
    TEST_ASSERT_NOT_NULL(strstr(c, "int64_t"));
    /* Return statement */
    TEST_ASSERT_NOT_NULL(strstr(c, "return"));
}

/* ── Test: object with fields generates struct ───────────────────────────── */

void test_pipeline_object(void) {
    const char *src =
        "object Vec2 {\n"
        "    var x: Float\n"
        "    var y: Float\n"
        "}\n"
        "func main() {\n"
        "    val v = Vec2(1.0, 2.0)\n"
        "}\n";
    const char *c = run_pipeline(src);
    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);
    /* Struct typedef */
    TEST_ASSERT_NOT_NULL(strstr(c, "typedef struct Iron_Vec2"));
    /* Float fields map to double */
    TEST_ASSERT_NOT_NULL(strstr(c, "double x"));
    TEST_ASSERT_NOT_NULL(strstr(c, "double y"));
    TEST_ASSERT_NOT_NULL(strstr(c, "Iron_main"));
}

/* ── Test: method declaration generates C function with self ─────────────── */

void test_pipeline_method(void) {
    const char *src =
        "object Counter {\n"
        "    var count: Int\n"
        "}\n"
        "func Counter.get() -> Int {\n"
        "    return self.count\n"
        "}\n"
        "func main() {\n"
        "    val c = Counter(0)\n"
        "}\n";
    const char *c = run_pipeline(src);
    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);
    /* Mangled method name */
    TEST_ASSERT_NOT_NULL(strstr(c, "Iron_Counter_get"));
    /* self parameter */
    TEST_ASSERT_NOT_NULL(strstr(c, "self"));
}

/* ── Test: val reassignment emits E0203 ──────────────────────────────────── */

void test_pipeline_val_error(void) {
    const char *src =
        "func main() {\n"
        "    val x = 10\n"
        "    x = 20\n"  /* Error: val is immutable */
        "}\n";
    Iron_DiagList diags;
    const char *c = compile_iron(src, &diags);
    TEST_ASSERT_NULL(c);
    TEST_ASSERT_GREATER_THAN(0, diags.error_count);
    TEST_ASSERT_TRUE(has_diag_code(&diags, IRON_ERR_VAL_REASSIGN));  /* E0203 */
    iron_diaglist_free(&diags);
}

/* ── Test: undefined variable emits E0200 ────────────────────────────────── */

void test_pipeline_undefined_error(void) {
    const char *src =
        "func main() {\n"
        "    val y = undefined_var + 1\n"  /* Error: undefined_var not declared */
        "}\n";
    Iron_DiagList diags;
    const char *c = compile_iron(src, &diags);
    TEST_ASSERT_NULL(c);
    TEST_ASSERT_GREATER_THAN(0, diags.error_count);
    TEST_ASSERT_TRUE(has_diag_code(&diags, IRON_ERR_UNDEFINED_VAR));  /* E0200 */
    iron_diaglist_free(&diags);
}

/* ── Test: iron_analyze returns correct result struct ────────────────────── */

void test_pipeline_analyze_result_success(void) {
    const char *src = "func main() { val x = 1 }";
    Iron_Lexer   l      = iron_lexer_create(src, "test.iron", &g_arena, &g_diags);
    Iron_Token  *tokens = iron_lex_all(&l);
    int          count  = 0;
    while (tokens[count].kind != IRON_TOK_EOF) count++;
    count++;
    Iron_Parser  p    = iron_parser_create(tokens, count, src, "test.iron",
                                            &g_arena, &g_diags);
    Iron_Node   *root = iron_parse(&p);
    Iron_Program *prog = (Iron_Program *)root;

    Iron_AnalyzeResult result = iron_analyze(prog, &g_arena, &g_diags);
    TEST_ASSERT_FALSE(result.has_errors);
    TEST_ASSERT_NOT_NULL(result.global_scope);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);
}

void test_pipeline_analyze_result_failure(void) {
    const char *src =
        "func main() {\n"
        "    val x = 10\n"
        "    x = 20\n"
        "}\n";
    Iron_Lexer   l      = iron_lexer_create(src, "test.iron", &g_arena, &g_diags);
    Iron_Token  *tokens = iron_lex_all(&l);
    int          count  = 0;
    while (tokens[count].kind != IRON_TOK_EOF) count++;
    count++;
    Iron_Parser  p    = iron_parser_create(tokens, count, src, "test.iron",
                                            &g_arena, &g_diags);
    Iron_Node   *root = iron_parse(&p);
    Iron_Program *prog = (Iron_Program *)root;

    Iron_AnalyzeResult result = iron_analyze(prog, &g_arena, &g_diags);
    TEST_ASSERT_TRUE(result.has_errors);
    TEST_ASSERT_GREATER_THAN(0, g_diags.error_count);
}

/* ── Test runner ─────────────────────────────────────────────────────────── */

int main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_pipeline_hello);
    RUN_TEST(test_pipeline_variables);
    RUN_TEST(test_pipeline_arithmetic);
    RUN_TEST(test_pipeline_if);
    RUN_TEST(test_pipeline_while);
    RUN_TEST(test_pipeline_function);
    RUN_TEST(test_pipeline_object);
    RUN_TEST(test_pipeline_method);
    RUN_TEST(test_pipeline_val_error);
    RUN_TEST(test_pipeline_undefined_error);
    RUN_TEST(test_pipeline_analyze_result_success);
    RUN_TEST(test_pipeline_analyze_result_failure);

    return UNITY_END();
}
