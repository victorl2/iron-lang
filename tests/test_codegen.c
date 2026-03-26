/* test_codegen.c — Unity tests for the Iron C code generator.
 *
 * Tests cover:
 *   - val x = 42 generates const int64_t x = 42 inside Iron_main()
 *   - func add(a: Int, b: Int) -> Int generates correct C function
 *   - defer executes before early return and at scope exit
 *   - object Vec2 { var x: Float; var y: Float } generates correct struct
 *   - Object inheritance generates struct with _base embedding
 *   - Nullable var generates Iron_Optional_... type
 *   - Generated C has forward declarations for all structs
 *   - Generated C has function prototypes before implementations
 *   - All user symbols use Iron_ prefix
 *   - Emission order: includes -> forward_decls -> struct_bodies -> prototypes -> impls -> main
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

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static bool str_contains(const char *haystack, const char *needle) {
    if (!haystack || !needle) return false;
    return strstr(haystack, needle) != NULL;
}

/* Check that needle appears before needle2 in haystack */
static bool str_before(const char *haystack, const char *needle,
                         const char *needle2) {
    if (!haystack) return false;
    const char *p1 = strstr(haystack, needle);
    const char *p2 = strstr(haystack, needle2);
    if (!p1 || !p2) return false;
    return p1 < p2;
}

static bool has_error(int code) {
    for (int i = 0; i < g_diags.count; i++) {
        if (g_diags.items[i].code == code) return true;
    }
    return false;
}

/* ── Test: val x = 42 generates const int64_t x = 42 ───────────────────── */

void test_val_decl_generates_const(void) {
    const char *src = "func main() { val x = 42 }";
    const char *c = run_codegen(src);
    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_FALSE(has_error(0));
    /* Should contain const int64_t x = 42 */
    TEST_ASSERT_TRUE(str_contains(c, "const int64_t"));
    TEST_ASSERT_TRUE(str_contains(c, "x"));
    TEST_ASSERT_TRUE(str_contains(c, "42"));
    /* Function should be named Iron_main */
    TEST_ASSERT_TRUE(str_contains(c, "Iron_main"));
}

/* ── Test: func add(a: Int, b: Int) -> Int generates correct C function ──── */

void test_func_with_params_generates_c_function(void) {
    const char *src =
        "func add(a: Int, b: Int) -> Int {\n"
        "    return a + b\n"
        "}\n";
    const char *c = run_codegen(src);
    TEST_ASSERT_NOT_NULL(c);
    /* The mangled name must appear */
    TEST_ASSERT_TRUE(str_contains(c, "Iron_add"));
    /* Return type should be int64_t */
    TEST_ASSERT_TRUE(str_contains(c, "int64_t"));
    /* Prototype must appear before implementation */
    TEST_ASSERT_TRUE(str_before(c, "Iron_add(", "return"));
}

/* ── Test: main wrapper calls Iron_main ──────────────────────────────────── */

void test_main_wrapper_calls_iron_main(void) {
    const char *src = "func main() { val x = 1 }";
    const char *c = run_codegen(src);
    TEST_ASSERT_NOT_NULL(c);
    /* C main() must exist */
    TEST_ASSERT_TRUE(str_contains(c, "int main("));
    /* It must call Iron_main() */
    TEST_ASSERT_TRUE(str_contains(c, "Iron_main()"));
}

/* ── Test: object Vec2 generates typedef struct ──────────────────────────── */

void test_object_generates_struct(void) {
    const char *src =
        "object Vec2 {\n"
        "    var x: Float\n"
        "    var y: Float\n"
        "}\n"
        "func main() { val v = Vec2(1.0, 2.0) }\n";
    const char *c = run_codegen(src);
    TEST_ASSERT_NOT_NULL(c);
    /* Should have typedef struct Iron_Vec2 Iron_Vec2 forward decl */
    TEST_ASSERT_TRUE(str_contains(c, "Iron_Vec2"));
    /* Struct body with double fields */
    TEST_ASSERT_TRUE(str_contains(c, "double x"));
    TEST_ASSERT_TRUE(str_contains(c, "double y"));
}

/* ── Test: emission order is correct ─────────────────────────────────────── */

void test_emission_order(void) {
    const char *src =
        "object Foo {\n"
        "    var x: Int\n"
        "}\n"
        "func bar() -> Int { return 1 }\n"
        "func main() { val f = Foo(42) }\n";
    const char *c = run_codegen(src);
    TEST_ASSERT_NOT_NULL(c);

    /* 1. includes appear first */
    TEST_ASSERT_TRUE(str_contains(c, "#include <stdint.h>"));
    /* 2. forward decls before struct bodies */
    TEST_ASSERT_TRUE(str_before(c, "typedef struct Iron_Foo Iron_Foo",
                                   "struct Iron_Foo {"));
    /* 3. prototypes before implementations */
    /* The prototype section (";") comes before the implementation ("{...}") */
    /* Check that "Iron_bar();\n" prototype appears before "Iron_bar() {\n" body */
    TEST_ASSERT_TRUE(str_before(c, "Iron_bar();", "Iron_bar() {"));
    /* 4. implementations before int main() wrapper */
    TEST_ASSERT_TRUE(str_before(c, "Iron_main() {", "int main("));
}

/* ── Test: all user symbols have Iron_ prefix ─────────────────────────────── */

void test_iron_prefix_applied(void) {
    const char *src =
        "object Player {\n"
        "    var hp: Int\n"
        "}\n"
        "func main() { val p = Player(100) }\n";
    const char *c = run_codegen(src);
    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_TRUE(str_contains(c, "Iron_Player"));
    TEST_ASSERT_TRUE(str_contains(c, "Iron_main"));
    /* Raw "Player" should not appear without the Iron_ prefix
     * (in struct/typedef context at least) */
    /* The typedef should say typedef struct Iron_Player Iron_Player */
    TEST_ASSERT_TRUE(str_contains(c, "typedef struct Iron_Player Iron_Player"));
}

/* ── Test: defer executes before return ──────────────────────────────────── */

void test_defer_executes_before_return(void) {
    /* We use a var and defer reassignment as a proxy for "defer ran" */
    const char *src =
        "func foo() {\n"
        "    val x = 1\n"
        "    defer bar()\n"
        "    return\n"
        "}\n"
        "func bar() { }\n"
        "func main() { foo() }\n";
    const char *c = run_codegen(src);
    TEST_ASSERT_NOT_NULL(c);
    /* Iron_bar() call must appear BEFORE the return statement in Iron_foo impl */
    const char *foo_impl = strstr(c, "Iron_foo() {");
    TEST_ASSERT_NOT_NULL(foo_impl);
    const char *bar_call = strstr(foo_impl, "Iron_bar()");
    const char *ret_stmt = strstr(foo_impl, "return;");
    TEST_ASSERT_NOT_NULL(bar_call);
    TEST_ASSERT_NOT_NULL(ret_stmt);
    TEST_ASSERT_TRUE(bar_call < ret_stmt);
}

/* ── Test: defer executes at scope exit (end of function) ─────────────────── */

void test_defer_executes_at_scope_exit(void) {
    const char *src =
        "func foo() {\n"
        "    val x = 1\n"
        "    defer bar()\n"
        "    work()\n"
        "}\n"
        "func bar() { }\n"
        "func work() { }\n"
        "func main() { foo() }\n";
    const char *c = run_codegen(src);
    TEST_ASSERT_NOT_NULL(c);
    /* Find the implementation body of Iron_foo (after prototypes section).
     * The implementation uses "Iron_foo() {" pattern. */
    const char *foo_impl = strstr(c, "Iron_foo() {");
    TEST_ASSERT_NOT_NULL(foo_impl);
    /* work() call should be before bar() (deferred) within the body */
    const char *work_call = strstr(foo_impl, "Iron_work()");
    const char *bar_call  = strstr(foo_impl, "Iron_bar()");
    TEST_ASSERT_NOT_NULL(work_call);
    TEST_ASSERT_NOT_NULL(bar_call);
    TEST_ASSERT_TRUE(work_call < bar_call);
}

/* ── Test: inheritance generates _base embedding ──────────────────────────── */

void test_inheritance_uses_base_embedding(void) {
    const char *src =
        "object Entity {\n"
        "    var hp: Int\n"
        "}\n"
        "object Player extends Entity {\n"
        "    var name: String\n"
        "}\n"
        "func make_entity() -> Entity { return Entity(100) }\n"
        "func main() { val e = make_entity() }\n";
    const char *c = run_codegen(src);
    TEST_ASSERT_NOT_NULL(c);
    /* Player struct should embed _base */
    TEST_ASSERT_TRUE(str_contains(c, "_base"));
    /* Iron_Entity should be defined */
    TEST_ASSERT_TRUE(str_contains(c, "Iron_Entity"));
    /* Iron_Player should be defined */
    TEST_ASSERT_TRUE(str_contains(c, "Iron_Player"));
    /* Entity must be emitted before Player (topo sort) */
    TEST_ASSERT_TRUE(str_before(c, "struct Iron_Entity", "struct Iron_Player"));
}

/* ── Test: nullable type generates Iron_Optional struct ───────────────────── */

void test_nullable_generates_optional_struct(void) {
    /* Test that nullable field types in objects generate Optional structs */
    const char *src =
        "object Enemy {\n"
        "    var hp: Int\n"
        "}\n"
        "object Level {\n"
        "    var boss: Enemy?\n"
        "}\n"
        "func main() { val x = 1 }\n";
    const char *c = run_codegen(src);
    TEST_ASSERT_NOT_NULL(c);
    /* Optional struct should be emitted */
    TEST_ASSERT_TRUE(str_contains(c, "Iron_Optional_"));
    /* has_value field should be present */
    TEST_ASSERT_TRUE(str_contains(c, "has_value"));
}

/* ── Test: enum generates typedef enum ───────────────────────────────────── */

void test_enum_generates_typedef_enum(void) {
    const char *src =
        "enum Direction { North, South, East, West }\n"
        "func main() { val d = Direction.North }\n";
    const char *c = run_codegen(src);
    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_TRUE(str_contains(c, "typedef enum"));
    TEST_ASSERT_TRUE(str_contains(c, "Iron_Direction"));
    TEST_ASSERT_TRUE(str_contains(c, "Iron_Direction_North"));
}

/* ── Test: generated C has standard includes ─────────────────────────────── */

void test_generated_c_has_standard_includes(void) {
    const char *src = "func main() { val x = 1 }";
    const char *c = run_codegen(src);
    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_TRUE(str_contains(c, "#include <stdint.h>"));
    TEST_ASSERT_TRUE(str_contains(c, "#include <stdbool.h>"));
    TEST_ASSERT_TRUE(str_contains(c, "#include <stdio.h>"));
}

/* ── Test: var decl generates mutable C variable ─────────────────────────── */

void test_var_decl_generates_mutable(void) {
    const char *src = "func main() { var x = 10 }";
    const char *c = run_codegen(src);
    TEST_ASSERT_NOT_NULL(c);
    /* var should NOT generate const */
    TEST_ASSERT_TRUE(str_contains(c, "int64_t x"));
    /* Make sure it's not const x */
    const char *cx = strstr(c, "x");
    (void)cx;
    /* The var line should not have "const int64_t x" */
    const char *var_line = strstr(c, "int64_t x");
    TEST_ASSERT_NOT_NULL(var_line);
    /* Check that "const" doesn't immediately precede it */
    if (var_line > c + 6) {
        /* Look for const int64_t x pattern — it should NOT be there for var */
        TEST_ASSERT_FALSE(strstr(c, "const int64_t x") != NULL);
    }
}

/* ── Test: while loop generates C while loop ─────────────────────────────── */

void test_while_loop_generates_c_while(void) {
    const char *src =
        "func main() {\n"
        "    var i = 0\n"
        "    while i < 10 {\n"
        "        i += 1\n"
        "    }\n"
        "}\n";
    const char *c = run_codegen(src);
    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_TRUE(str_contains(c, "while ("));
    TEST_ASSERT_TRUE(str_contains(c, "i < "));
}

/* ── Test: binary arithmetic generates C operators ───────────────────────── */

void test_binary_arithmetic(void) {
    const char *src =
        "func add(a: Int, b: Int) -> Int { return a + b }\n"
        "func main() { val r = add(1, 2) }\n";
    const char *c = run_codegen(src);
    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_TRUE(str_contains(c, "(a + b)"));
}

/* ── Test: if statement generates C if statement ─────────────────────────── */

void test_if_generates_c_if(void) {
    const char *src =
        "func main() {\n"
        "    val x = 5\n"
        "    if x > 3 {\n"
        "        val y = 1\n"
        "    }\n"
        "}\n";
    const char *c = run_codegen(src);
    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_TRUE(str_contains(c, "if ("));
}

/* ── Main ─────────────────────────────────────────────────────────────────── */

int main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_val_decl_generates_const);
    RUN_TEST(test_func_with_params_generates_c_function);
    RUN_TEST(test_main_wrapper_calls_iron_main);
    RUN_TEST(test_object_generates_struct);
    RUN_TEST(test_emission_order);
    RUN_TEST(test_iron_prefix_applied);
    RUN_TEST(test_defer_executes_before_return);
    RUN_TEST(test_defer_executes_at_scope_exit);
    RUN_TEST(test_inheritance_uses_base_embedding);
    RUN_TEST(test_nullable_generates_optional_struct);
    RUN_TEST(test_enum_generates_typedef_enum);
    RUN_TEST(test_generated_c_has_standard_includes);
    RUN_TEST(test_var_decl_generates_mutable);
    RUN_TEST(test_while_loop_generates_c_while);
    RUN_TEST(test_binary_arithmetic);
    RUN_TEST(test_if_generates_c_if);

    return UNITY_END();
}
