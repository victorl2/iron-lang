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
    char       *output = iron_print_ast(ast, NULL, &arena);
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
    char      *output = iron_print_ast(ast, NULL, &arena);
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
    char      *output = iron_print_ast(ast, NULL, &arena);
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
    char      *output = iron_print_ast(ast, NULL, &arena);
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
    char      *output = iron_print_ast(ast, NULL, &arena);
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
    char      *output = iron_print_ast(ast1, NULL, &arena);
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
    char      *output = iron_print_ast(ast, NULL, &arena);
    TEST_ASSERT_NOT_NULL(output);
    /* The printed output should contain { and } from the interpolation */
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(output, "{"), "Missing '{' in interp string output");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(output, "}"), "Missing '}' in interp string output");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(output, "name"), "Missing 'name' in interp string output");
}

/* ── Phase 94 LIB-02: pub-stub generator tests ───────────────────────────── */

/* Helper: parse a source string and capture iron_print_pub_stubs output via
 * open_memstream. Returns a malloc'd buffer the caller must free. open_memstream
 * is available on macOS 10.13+ and Linux glibc 1.0+ (the only platforms ironc
 * supports for native dev builds). */
static char *capture_stub(const char *src, const char *pkg, const char *ver,
                          int *out_count) {
    Iron_Node *ast = parse(src);
    char  *buf  = NULL;
    size_t len  = 0;
    FILE  *out  = open_memstream(&buf, &len);
    int n = iron_print_pub_stubs((Iron_Program *)ast, out, pkg, ver);
    fclose(out);
    if (out_count) *out_count = n;
    return buf;
}

void test_v94_pub_stubs_empty_program(void) {
    int n = -1;
    char *out = capture_stub("", "mylib", "0.1.0", &n);
    TEST_ASSERT_NOT_NULL(out);
    TEST_ASSERT_EQUAL_INT(0, n);
    TEST_ASSERT_NOT_NULL(strstr(out, "iron-stub: auto-generated"));
    TEST_ASSERT_NULL(strstr(out, "pub func"));
    TEST_ASSERT_NULL(strstr(out, "pub object"));
    free(out);
}

void test_v94_pub_stubs_alphabetic_funcs(void) {
    const char *src =
        "pub func zebra() {}\n"
        "pub func alpha() {}\n"
        "pub func mango() {}\n";
    char *out = capture_stub(src, "mylib", "0.1.0", NULL);
    TEST_ASSERT_NOT_NULL(out);
    char *pa = strstr(out, "pub func alpha(");
    char *pm = strstr(out, "pub func mango(");
    char *pz = strstr(out, "pub func zebra(");
    TEST_ASSERT_NOT_NULL_MESSAGE(pa, "alpha missing");
    TEST_ASSERT_NOT_NULL_MESSAGE(pm, "mango missing");
    TEST_ASSERT_NOT_NULL_MESSAGE(pz, "zebra missing");
    TEST_ASSERT_TRUE_MESSAGE(pa < pm, "alpha must appear before mango");
    TEST_ASSERT_TRUE_MESSAGE(pm < pz, "mango must appear before zebra");
    free(out);
}

void test_v94_pub_stubs_skips_private_decls(void) {
    const char *src =
        "pub func public_one() {}\n"
        "func helper() {}\n";
    char *out = capture_stub(src, "mylib", "0.1.0", NULL);
    TEST_ASSERT_NOT_NULL(out);
    TEST_ASSERT_NOT_NULL(strstr(out, "public_one"));
    TEST_ASSERT_NULL_MESSAGE(strstr(out, "helper"),
                             "private 'helper' must NOT appear in stub");
    free(out);
}

void test_v94_pub_stubs_skips_patch_object(void) {
    /* Use Vec3 (custom user type) since patching String would trip the
     * receiver-tier gate (E0236) at parse time. The stub generator's
     * is_patch / is_patch_member filters operate purely on AST shape and
     * are unaffected by the choice of patched type. */
    const char *src =
        "pub object Vec3 {\n"
        "    var x: Int\n"
        "    var y: Int\n"
        "    pub init(a: Int, b: Int) {\n"
        "        self.x = a\n"
        "        self.y = b\n"
        "    }\n"
        "}\n"
        "pub patch object Vec3 {\n"
        "    pub func magnitude() -> Int { return 0 }\n"
        "}\n";
    int n = -1;
    char *out = capture_stub(src, "mylib", "0.1.0", &n);
    TEST_ASSERT_NOT_NULL(out);
    /* Stub must not contain `magnitude` (patch method) or any patch-object header. */
    TEST_ASSERT_NULL_MESSAGE(strstr(out, "magnitude"),
                             "patch method 'magnitude' leaked into stub");
    TEST_ASSERT_NULL_MESSAGE(strstr(out, "pub patch object"),
                             "patch header leaked into stub");
    /* The non-patch `pub object Vec3` MUST still appear (only the patch surface is suppressed). */
    TEST_ASSERT_NOT_NULL(strstr(out, "pub object Vec3"));
    free(out);
}

void test_v94_pub_stubs_groups_methods_under_owner(void) {
    const char *src =
        "pub object Foo {\n"
        "    var x: Int\n"
        "    pub init(v: Int) { self.x = v }\n"
        "    pub func bar() -> Int { return self.x }\n"
        "}\n";
    char *out = capture_stub(src, "mylib", "0.1.0", NULL);
    TEST_ASSERT_NOT_NULL(out);
    char *obj_open = strstr(out, "pub object Foo {");
    char *bar      = strstr(out, "pub func bar(");
    TEST_ASSERT_NOT_NULL(obj_open);
    TEST_ASSERT_NOT_NULL(bar);
    TEST_ASSERT_TRUE_MESSAGE(obj_open < bar,
                             "object header must appear before method");
    /* Method must appear inside the object's brace block, i.e. before the
     * matching closing brace of that block. */
    char *obj_close = strchr(obj_open, '}');
    TEST_ASSERT_NOT_NULL(obj_close);
    TEST_ASSERT_TRUE_MESSAGE(bar < obj_close,
                             "method must be nested inside object braces");
    free(out);
}

void test_v94_pub_stubs_round_trip(void) {
    /* The plan's RESEARCH "Critical constraint" line 241: the consumer's
     * parser will re-parse the concatenated stubs through the SAME parse
     * pipeline as user code, so the emitted Iron source MUST be valid Iron.
     * This test checks parse-only validity (typecheck-time errors such as
     * E0247 init-leaves-field-unassigned are part of Plan 03's consumer
     * flow and not in scope for the stub generator's parse contract). */
    const char *src =
        "pub func hello() -> String { return \"hi\" }\n"
        "pub enum State { ON, OFF }\n";
    char *out = capture_stub(src, "mylib", "0.1.0", NULL);
    TEST_ASSERT_NOT_NULL(out);

    /* Re-parse through the same pipeline. Use a fresh diag list so any
     * earlier diags from `parse(src)` don't leak into the assertion. */
    Iron_DiagList diags2 = iron_diaglist_create();
    Iron_Lexer    l2     = iron_lexer_create(out, "stub.iron", &arena, &diags2);
    Iron_Token   *toks2  = iron_lex_all(&l2);
    int           count2 = 0;
    while (toks2[count2].kind != IRON_TOK_EOF) count2++;
    count2++;
    Iron_Parser   p2   = iron_parser_create(toks2, count2, out, "stub.iron",
                                            &arena, &diags2);
    Iron_Node    *ast2 = iron_parse(&p2);
    TEST_ASSERT_NOT_NULL(ast2);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, diags2.error_count,
                                   "stub output must parse without errors");
    iron_diaglist_free(&diags2);
    free(out);
}

void test_v94_pub_stubs_strips_method_body(void) {
    const char *src = "pub func add(a: Int, b: Int) -> Int { return a + b }\n";
    char *out = capture_stub(src, "mylib", "0.1.0", NULL);
    TEST_ASSERT_NOT_NULL(out);
    TEST_ASSERT_NOT_NULL(strstr(out, "pub func add(a: Int, b: Int) -> Int"));
    TEST_ASSERT_NULL_MESSAGE(strstr(out, "return a + b"),
                             "method body must be stripped");
    free(out);
}

void test_v94_pub_stubs_header_format(void) {
    char *out = capture_stub("", "mylib", "0.1.0", NULL);
    TEST_ASSERT_NOT_NULL(out);
    TEST_ASSERT_NOT_NULL(strstr(out,
        "-- iron-stub: auto-generated from src/lib.iron of mylib v0.1.0. Do not edit."));
    free(out);
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
    /* Phase 94 LIB-02: pub-stub generator coverage. */
    RUN_TEST(test_v94_pub_stubs_empty_program);
    RUN_TEST(test_v94_pub_stubs_alphabetic_funcs);
    RUN_TEST(test_v94_pub_stubs_skips_private_decls);
    RUN_TEST(test_v94_pub_stubs_skips_patch_object);
    RUN_TEST(test_v94_pub_stubs_groups_methods_under_owner);
    RUN_TEST(test_v94_pub_stubs_round_trip);
    RUN_TEST(test_v94_pub_stubs_strips_method_body);
    RUN_TEST(test_v94_pub_stubs_header_format);
    return UNITY_END();
}
