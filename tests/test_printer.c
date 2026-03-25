#include "unity.h"
#include "parser/parser.h"
#include "parser/ast.h"
#include "parser/printer.h"
#include "lexer/lexer.h"
#include "util/arena.h"
#include "diagnostics/diagnostics.h"
#include "stb_ds.h"

#include <string.h>
#include <stdlib.h>

/* ── Module-level fixtures ───────────────────────────────────────────────── */

static Iron_Arena    arena;
static Iron_DiagList diags;

void setUp(void) {
    arena = iron_arena_create(262144);
    diags = iron_diaglist_create();
}

void tearDown(void) {
    iron_arena_free(&arena);
    iron_diaglist_free(&diags);
}

/* ── Parse helper ────────────────────────────────────────────────────────── */

static Iron_Node *parse(const char *src) {
    Iron_Lexer   l      = iron_lexer_create(src, "test.iron", &arena, &diags);
    Iron_Token  *tokens = iron_lex_all(&l);
    int          count  = 0;
    while (tokens[count].kind != IRON_TOK_EOF) count++;
    count++;  /* include EOF */
    Iron_Parser  p = iron_parser_create(tokens, count, src, "test.iron", &arena, &diags);
    return iron_parse(&p);
}

/* ── Pretty-printer tests ─────────────────────────────────────────────────── */

/* parse "val x = 10", print, verify output contains "val x = 10" */
void test_print_val_decl(void) {
    Iron_Node  *ast    = parse("val x = 10");
    char       *output = iron_print_ast(ast, &arena);
    TEST_ASSERT_NOT_NULL(output);
    TEST_ASSERT_NOT_NULL(strstr(output, "val x = 10"));
}

/* parse func, print, verify output contains "func add(" */
void test_print_func_decl(void) {
    const char *src =
        "func add(a: Int, b: Int) -> Int {\n"
        "  return a\n"
        "}\n";
    Iron_Node *ast    = parse(src);
    char      *output = iron_print_ast(ast, &arena);
    TEST_ASSERT_NOT_NULL(output);
    TEST_ASSERT_NOT_NULL(strstr(output, "func add("));
}

/* parse object, print, verify output contains "object Player" */
void test_print_object_decl(void) {
    const char *src =
        "object Player {\n"
        "  var hp: Int\n"
        "  val name: String\n"
        "}\n";
    Iron_Node *ast    = parse(src);
    char      *output = iron_print_ast(ast, &arena);
    TEST_ASSERT_NOT_NULL(output);
    TEST_ASSERT_NOT_NULL(strstr(output, "object Player"));
}

/* parse if/elif/else, print, verify all branches present */
void test_print_if_elif_else(void) {
    const char *src =
        "func check(x: Int) {\n"
        "  if x > 0 {\n"
        "    val a = 1\n"
        "  } elif x < 0 {\n"
        "    val b = 2\n"
        "  } else {\n"
        "    val c = 3\n"
        "  }\n"
        "}\n";
    Iron_Node *ast    = parse(src);
    char      *output = iron_print_ast(ast, &arena);
    TEST_ASSERT_NOT_NULL(output);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(output, "if"), "Missing 'if' in output");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(output, "elif"), "Missing 'elif' in output");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(output, "else"), "Missing 'else' in output");
}

/* parse for..parallel, print, verify "parallel" in output */
void test_print_for_parallel(void) {
    const char *src =
        "func work(items: [Int]) {\n"
        "  for item in items parallel {\n"
        "    val x = 1\n"
        "  }\n"
        "}\n";
    Iron_Node *ast    = parse(src);
    char      *output = iron_print_ast(ast, &arena);
    TEST_ASSERT_NOT_NULL(output);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(output, "parallel"), "Missing 'parallel' in output");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(output, "for"), "Missing 'for' in output");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(output, "in"), "Missing 'in' in output");
}

/* Round-trip hello: parse hello.iron, print, parse again, verify AST structure */
void test_roundtrip_hello(void) {
    const char *src =
        "func main() {\n"
        "  println(\"Hello, Iron!\")\n"
        "}\n";
    Iron_Node *ast1   = parse(src);
    char      *output = iron_print_ast(ast1, &arena);
    TEST_ASSERT_NOT_NULL(output);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(output, "func main"), "Round-trip missing 'func main'");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(output, "println"), "Round-trip missing 'println'");

    /* Parse the printed output again */
    Iron_DiagList diags2 = iron_diaglist_create();
    Iron_Lexer    l2     = iron_lexer_create(output, "roundtrip.iron", &arena, &diags2);
    Iron_Token   *toks2  = iron_lex_all(&l2);
    int           count2 = 0;
    while (toks2[count2].kind != IRON_TOK_EOF) count2++;
    count2++;
    Iron_Parser   p2   = iron_parser_create(toks2, count2, output, "roundtrip.iron",
                                             &arena, &diags2);
    Iron_Node    *ast2 = iron_parse(&p2);

    TEST_ASSERT_NOT_NULL(ast2);
    TEST_ASSERT_EQUAL_INT(IRON_NODE_PROGRAM, ast2->kind);
    Iron_Program *pr2 = (Iron_Program *)ast2;
    TEST_ASSERT_EQUAL_INT(1, pr2->decl_count);
    TEST_ASSERT_EQUAL_INT(IRON_NODE_FUNC_DECL, pr2->decls[0]->kind);
    Iron_FuncDecl *f2 = (Iron_FuncDecl *)pr2->decls[0];
    TEST_ASSERT_EQUAL_STRING("main", f2->name);
    TEST_ASSERT_EQUAL_INT(0, diags2.error_count);
    iron_diaglist_free(&diags2);
}

/* parse string with interpolation, print, verify curly braces in output */
void test_print_interp_string(void) {
    const char *src =
        "func greet(name: String) {\n"
        "  val msg = \"Hello {name}!\"\n"
        "}\n";
    Iron_Node *ast    = parse(src);
    char      *output = iron_print_ast(ast, &arena);
    TEST_ASSERT_NOT_NULL(output);
    /* The printed output should contain { and } from the interpolation */
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(output, "{"), "Missing '{' in interp string output");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(output, "}"), "Missing '}' in interp string output");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(output, "name"), "Missing 'name' in interp string output");
}

/* ── Main ────────────────────────────────────────────────────────────────── */

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_print_val_decl);
    RUN_TEST(test_print_func_decl);
    RUN_TEST(test_print_object_decl);
    RUN_TEST(test_print_if_elif_else);
    RUN_TEST(test_print_for_parallel);
    RUN_TEST(test_roundtrip_hello);
    RUN_TEST(test_print_interp_string);
    return UNITY_END();
}
