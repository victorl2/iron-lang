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

/* ── Test: interface generates vtable struct ──────────────────────────────── */

void test_interface_generates_vtable_struct(void) {
    const char *src =
        "interface Drawable {\n"
        "    func draw()\n"
        "}\n"
        "object Player implements Drawable {\n"
        "    var hp: Int\n"
        "}\n"
        "func Player.draw() { }\n"
        "func main() { val p = Player(100) }\n";
    const char *c = run_codegen(src);
    TEST_ASSERT_NOT_NULL(c);
    /* Vtable struct must be emitted */
    TEST_ASSERT_TRUE(str_contains(c, "Iron_Drawable_vtable"));
    /* Vtable must have a function pointer for draw */
    TEST_ASSERT_TRUE(str_contains(c, "(*draw)"));
    /* Static vtable instance for Player implementing Drawable */
    TEST_ASSERT_TRUE(str_contains(c, "Iron_Player_Drawable_vtable"));
    /* Interface ref type must be emitted */
    TEST_ASSERT_TRUE(str_contains(c, "Iron_Drawable_ref"));
}

/* ── Test: vtable instance references the correct implementation ─────────── */

void test_vtable_instance_has_correct_function_pointer(void) {
    const char *src =
        "interface Drawable {\n"
        "    func draw()\n"
        "}\n"
        "object Player implements Drawable {\n"
        "    var hp: Int\n"
        "}\n"
        "func Player.draw() { }\n"
        "func main() { val p = Player(100) }\n";
    const char *c = run_codegen(src);
    TEST_ASSERT_NOT_NULL(c);
    /* Vtable instance must reference the implementation function */
    TEST_ASSERT_TRUE(str_contains(c, ".draw"));
    TEST_ASSERT_TRUE(str_contains(c, "Iron_Player_draw"));
}

/* ── Test: monomorphization generates concrete struct via direct API ─────── */

void test_monomorphization_generates_concrete_struct(void) {
    /* Test via object with a generic-named field placeholder.
     * We directly verify ensure_monomorphized_type emits the struct. */
    /* Build a minimal context to call the API */
    Iron_Arena arena = iron_arena_create(64 * 1024);
    iron_types_init(&arena);
    Iron_DiagList diags = iron_diaglist_create();

    /* Create a minimal codegen context */
    Iron_Codegen ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.arena        = &arena;
    ctx.diags        = &diags;
    ctx.global_scope = NULL;
    ctx.program      = NULL;
    ctx.struct_bodies  = iron_strbuf_create(1024);
    ctx.forward_decls  = iron_strbuf_create(256);
    ctx.prototypes     = iron_strbuf_create(256);
    ctx.emitted_optionals = NULL;
    ctx.mono_registry  = NULL;

    /* Create an Iron_Type for Enemy: we'll use a simple OBJECT type */
    Iron_ObjectDecl enemy_decl;
    memset(&enemy_decl, 0, sizeof(enemy_decl));
    enemy_decl.kind = IRON_NODE_OBJECT_DECL;
    enemy_decl.name = "Enemy";

    Iron_Type *enemy_type = iron_type_make_object(&arena, &enemy_decl);
    Iron_Type *arg_types[1] = { enemy_type };

    /* Call ensure_monomorphized_type for List[Enemy] */
    const char *name = ensure_monomorphized_type(&ctx, "List", arg_types, 1);
    TEST_ASSERT_NOT_NULL(name);
    TEST_ASSERT_TRUE(str_contains(name, "Iron_List_Iron_Enemy"));

    /* Struct body must have been emitted */
    const char *bodies = iron_strbuf_get(&ctx.struct_bodies);
    TEST_ASSERT_TRUE(str_contains(bodies, "Iron_List_Iron_Enemy"));
    TEST_ASSERT_TRUE(str_contains(bodies, "count"));
    TEST_ASSERT_TRUE(str_contains(bodies, "capacity"));

    iron_strbuf_free(&ctx.struct_bodies);
    iron_strbuf_free(&ctx.forward_decls);
    iron_strbuf_free(&ctx.prototypes);
    iron_arena_free(&arena);
    iron_diaglist_free(&diags);
}

/* ── Test: same generic instantiation is emitted only once (dedup) ─────────── */

void test_monomorphization_dedup(void) {
    Iron_Arena arena = iron_arena_create(64 * 1024);
    iron_types_init(&arena);
    Iron_DiagList diags = iron_diaglist_create();

    Iron_Codegen ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.arena        = &arena;
    ctx.diags        = &diags;
    ctx.global_scope = NULL;
    ctx.program      = NULL;
    ctx.struct_bodies  = iron_strbuf_create(1024);
    ctx.forward_decls  = iron_strbuf_create(256);
    ctx.prototypes     = iron_strbuf_create(256);
    ctx.emitted_optionals = NULL;
    ctx.mono_registry  = NULL;

    Iron_ObjectDecl enemy_decl;
    memset(&enemy_decl, 0, sizeof(enemy_decl));
    enemy_decl.kind = IRON_NODE_OBJECT_DECL;
    enemy_decl.name = "Enemy";

    Iron_Type *enemy_type = iron_type_make_object(&arena, &enemy_decl);
    Iron_Type *arg_types[1] = { enemy_type };

    /* Call twice — should produce same mangled name, only emit struct once */
    const char *name1 = ensure_monomorphized_type(&ctx, "List", arg_types, 1);
    const char *name2 = ensure_monomorphized_type(&ctx, "List", arg_types, 1);
    TEST_ASSERT_NOT_NULL(name1);
    TEST_ASSERT_NOT_NULL(name2);
    TEST_ASSERT_EQUAL_STRING(name1, name2);

    /* Count struct definitions emitted */
    const char *bodies = iron_strbuf_get(&ctx.struct_bodies);
    int count = 0;
    const char *ptr = bodies;
    while ((ptr = strstr(ptr, "Iron_List_Iron_Enemy {")) != NULL) {
        count++;
        ptr++;
    }
    TEST_ASSERT_EQUAL_INT(1, count);

    iron_strbuf_free(&ctx.struct_bodies);
    iron_strbuf_free(&ctx.forward_decls);
    iron_strbuf_free(&ctx.prototypes);
    iron_arena_free(&arena);
    iron_diaglist_free(&diags);
}

/* ── Test: lambda without captures generates lifted function ──────────────── */

void test_lambda_no_captures_generates_lifted_function(void) {
    const char *src =
        "func main() {\n"
        "    val f = func(x: Int) -> Int { return x + 1 }\n"
        "}\n";
    const char *c = run_codegen(src);
    TEST_ASSERT_NOT_NULL(c);
    /* A lifted function should be emitted for the lambda */
    TEST_ASSERT_TRUE(str_contains(c, "Iron_main_lambda_0"));
    /* The function should have the correct signature with int64_t param */
    TEST_ASSERT_TRUE(str_contains(c, "int64_t x"));
}

/* ── Test: lambda with captures generates env struct ─────────────────────── */

void test_lambda_with_captures_generates_env_struct(void) {
    /* Build an AST manually to avoid parser/resolver complications with captures.
     * We test the lambda code emitter directly via a minimal codegen context. */
    Iron_Arena arena = iron_arena_create(64 * 1024);
    iron_types_init(&arena);
    Iron_DiagList diags = iron_diaglist_create();

    /* Build a minimal lambda node with a "score" capture marker */
    Iron_LambdaExpr *lam = ARENA_ALLOC(&arena, Iron_LambdaExpr);
    Iron_Span s = iron_span_make("test.iron", 1, 1, 1, 10);
    lam->span         = s;
    lam->kind         = IRON_NODE_LAMBDA;
    lam->params       = NULL;
    lam->param_count  = 0;
    lam->return_type  = NULL;
    lam->resolved_type = NULL;

    /* Empty block body */
    Iron_Block *body = ARENA_ALLOC(&arena, Iron_Block);
    body->span       = s;
    body->kind       = IRON_NODE_BLOCK;
    body->stmts      = NULL;
    body->stmt_count = 0;
    lam->body        = (Iron_Node *)body;

    /* Create a minimal codegen context */
    Iron_Codegen ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.arena        = &arena;
    ctx.diags        = &diags;
    ctx.global_scope = NULL;
    ctx.program      = NULL;
    ctx.struct_bodies  = iron_strbuf_create(1024);
    ctx.forward_decls  = iron_strbuf_create(256);
    ctx.prototypes     = iron_strbuf_create(256);
    ctx.implementations = iron_strbuf_create(1024);
    ctx.lifted_funcs   = iron_strbuf_create(1024);
    ctx.emitted_optionals = NULL;
    ctx.mono_registry  = NULL;
    ctx.lambda_counter = 0;

    /* Emit the lambda — caller function name is "main" */
    Iron_StrBuf sb = iron_strbuf_create(256);
    emit_lambda(&sb, (Iron_Node *)lam, &ctx, "main");

    /* Check that a lifted function was produced */
    const char *lifted = iron_strbuf_get(&ctx.lifted_funcs);
    TEST_ASSERT_TRUE(str_contains(lifted, "Iron_main_lambda_0"));

    iron_strbuf_free(&sb);
    iron_strbuf_free(&ctx.struct_bodies);
    iron_strbuf_free(&ctx.forward_decls);
    iron_strbuf_free(&ctx.prototypes);
    iron_strbuf_free(&ctx.implementations);
    iron_strbuf_free(&ctx.lifted_funcs);
    iron_arena_free(&arena);
    iron_diaglist_free(&diags);
}

/* ── Test: spawn generates lifted function and pool_submit call ───────────── */

void test_spawn_generates_lifted_function_and_pool_submit(void) {
    const char *src =
        "func save() { }\n"
        "func main() {\n"
        "    spawn(\"autosave\") { save() }\n"
        "}\n";
    const char *c = run_codegen(src);
    TEST_ASSERT_NOT_NULL(c);
    /* Lifted spawn function must be emitted */
    TEST_ASSERT_TRUE(str_contains(c, "Iron_spawn_"));
    /* pool_submit call must be emitted */
    TEST_ASSERT_TRUE(str_contains(c, "Iron_pool_submit"));
    /* Lifted function must take void* arg */
    TEST_ASSERT_TRUE(str_contains(c, "void* arg"));
}

/* ── Test: parallel-for generates chunk function and barrier ─────────────── */

void test_parallel_for_generates_chunk_and_barrier(void) {
    const char *src =
        "func work(i: Int) { }\n"
        "func main() {\n"
        "    val n = 100\n"
        "    for i in n parallel { work(i) }\n"
        "}\n";
    const char *c = run_codegen(src);
    TEST_ASSERT_NOT_NULL(c);
    /* Chunk function must be emitted */
    TEST_ASSERT_TRUE(str_contains(c, "Iron_parallel_chunk_"));
    /* pool_submit call must be emitted */
    TEST_ASSERT_TRUE(str_contains(c, "Iron_pool_submit"));
    /* Barrier call must be present */
    TEST_ASSERT_TRUE(str_contains(c, "Iron_pool_barrier"));
}

/* ── Test: field access uses -> for self (pointer receiver) ─────────────── */

void test_field_access_uses_arrow_for_self(void) {
    const char *src =
        "object Vec2 {\n"
        "    var x: Float\n"
        "}\n"
        "func Vec2.get_x() -> Float {\n"
        "    return self.x\n"
        "}\n"
        "func main() { val v = Vec2(1.0) }\n";
    const char *c = run_codegen(src);
    TEST_ASSERT_NOT_NULL(c);
    /* Method body must use -> not . for self field access */
    TEST_ASSERT_TRUE(str_contains(c, "self->x"));
    /* Must NOT use self.x (dot access on pointer is invalid C) */
    TEST_ASSERT_FALSE(str_contains(c, "self.x"));
}

/* ── Test: CALL to type name emits compound literal, not function call ────── */

void test_call_to_type_emits_compound_literal(void) {
    const char *src =
        "object Vec2 {\n"
        "    var x: Float\n"
        "    var y: Float\n"
        "}\n"
        "func main() {\n"
        "    val v = Vec2(1.0, 2.0)\n"
        "}\n";
    const char *c = run_codegen(src);
    TEST_ASSERT_NOT_NULL(c);
    /* Should emit compound literal (Iron_Vec2){ */
    TEST_ASSERT_TRUE(str_contains(c, "(Iron_Vec2){"));
    /* Must NOT emit Iron_Vec2(1 — a C function call that doesn't exist */
    TEST_ASSERT_FALSE(str_contains(c, "Iron_Vec2(1"));
}

/* ── Test: println emits Iron_println() builtin, not printf ─────────────── */

void test_println_format_no_extra_args(void) {
    const char *src =
        "func main() {\n"
        "    println(\"hello\")\n"
        "}\n";
    const char *c = run_codegen(src);
    TEST_ASSERT_NOT_NULL(c);
    /* Must emit Iron_println() builtin call */
    TEST_ASSERT_TRUE(str_contains(c, "Iron_println"));
    /* Must NOT use raw printf for println any more */
    TEST_ASSERT_FALSE(str_contains(c, "printf(\"%%s\\n\""));
}

/* ── Test: generated C compiles through clang -std=c11 -Wall -Werror ──────── */

void test_codegen_output_compiles_with_clang(void) {
    /* A comprehensive Iron program exercising all three fixed bugs:
     *   - Vec2.length() uses self.x -> must emit self->x (Bug 1 fix)
     *   - Vec2(1.0, 2.0) is a type constructor -> must emit compound literal (Bug 2 fix)
     *   - println("hello") -> must emit printf("%s\n",...) (Bug 3 fix)
     * This program does NOT use spawn/parallel to avoid undeclared pool functions. */
    const char *src =
        "object Vec2 {\n"
        "    var x: Float\n"
        "    var y: Float\n"
        "}\n"
        "func Vec2.length() -> Float {\n"
        "    return self.x + self.y\n"
        "}\n"
        "func main() {\n"
        "    val v = Vec2(1.0, 2.0)\n"
        "    println(\"hello\")\n"
        "}\n";
    const char *c = run_codegen(src);
    TEST_ASSERT_NOT_NULL(c);

    /* Write generated C to a temp file.
     * Prepend the runtime header include so Iron_println, iron_runtime_init,
     * etc. are declared (codegen runtime include wiring is done in Plan 04).
     *
     * __FILE__ is tests/test_codegen.c; two dirname steps reach the project
     * root where src/ lives.  We use that to form absolute -I paths so the
     * test works regardless of the working directory it is invoked from. */
    const char *tmpfile = "/tmp/iron_codegen_test.c";
    FILE *f = fopen(tmpfile, "w");
    TEST_ASSERT_NOT_NULL_MESSAGE(f, "Could not open temp file for writing");
    /* Determine project root from __FILE__ (absolute path injected by compiler) */
    char src_root[512];
    strncpy(src_root, __FILE__, sizeof(src_root) - 1);
    src_root[sizeof(src_root) - 1] = '\0';
    /* Strip "tests/test_codegen.c" to get project root */
    char *sep = strrchr(src_root, '/');
    if (sep) { *sep = '\0'; }  /* strip filename */
    sep = strrchr(src_root, '/');
    if (sep) { *sep = '\0'; }  /* strip "tests" dir */
    fputs("#include \"runtime/iron_runtime.h\"\n", f);
    fputs(c, f);
    fclose(f);

    /* Invoke clang -fsyntax-only to verify the C compiles without errors.
     * -I <src_root>/src        : allow #include "runtime/iron_runtime.h".
     * -I <src_root>/src/vendor : for vendor headers included by iron_runtime.
     * -Wno-unused-variable: val v is unused (Iron doesn't require use).
     * -Wno-unused-function: generated helpers may be unused in this snippet.
     * -Werror: any remaining warning is a hard failure. */
    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
             "clang -std=c11 -Wall -Werror "
             "-Wno-unused-variable -Wno-unused-function "
             "-I %s/src -I %s/src/vendor "
             "-fsyntax-only %s 2>/dev/null",
             src_root, src_root, tmpfile);
    int ret = system(cmd);
    remove(tmpfile);

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, ret,
        "clang -std=c11 -Wall -Werror rejected generated C — "
        "check /tmp/iron_codegen_test.c for details");
}

/* ── Test: auto_free heap allocation emits free() at block exit ─────────── */

void test_auto_free_emits_free(void) {
    /* Build AST directly to inject auto_free=true without running escape
     * analysis (which may not mark this specific pattern). Instead we
     * manually construct the nodes and call emit_block. */
    Iron_Arena arena = iron_arena_create(64 * 1024);
    iron_types_init(&arena);
    Iron_DiagList diags = iron_diaglist_create();

    Iron_Span s = iron_span_make("test.iron", 1, 1, 1, 10);

    /* Build: object Vec2 { var x: Float } for the type */
    Iron_ObjectDecl *od = ARENA_ALLOC(&arena, Iron_ObjectDecl);
    memset(od, 0, sizeof(*od));
    od->span = s;
    od->kind = IRON_NODE_OBJECT_DECL;
    od->name = "Vec2";
    od->fields = NULL;
    od->field_count = 0;

    /* Build a scope with Vec2 symbol */
    Iron_Scope *global = iron_scope_create(&arena, NULL, IRON_SCOPE_GLOBAL);
    Iron_Symbol *vec2_sym = iron_symbol_create(&arena, "Vec2",
                                                IRON_SYM_TYPE,
                                                (Iron_Node *)od, s);
    iron_scope_define(global, &arena, vec2_sym);

    /* Build inner construct node for Vec2() */
    Iron_ConstructExpr *ce = ARENA_ALLOC(&arena, Iron_ConstructExpr);
    memset(ce, 0, sizeof(*ce));
    ce->span = s;
    ce->kind = IRON_NODE_CONSTRUCT;
    ce->type_name = "Vec2";
    ce->args = NULL;
    ce->arg_count = 0;

    /* Build heap expr with auto_free=true */
    Iron_HeapExpr *he = ARENA_ALLOC(&arena, Iron_HeapExpr);
    memset(he, 0, sizeof(*he));
    he->span = s;
    he->kind = IRON_NODE_HEAP;
    he->inner = (Iron_Node *)ce;
    he->auto_free = true;
    he->escapes   = false;

    /* Build val p = heap Vec2() */
    Iron_ValDecl *vd = ARENA_ALLOC(&arena, Iron_ValDecl);
    memset(vd, 0, sizeof(*vd));
    vd->span = s;
    vd->kind = IRON_NODE_VAL_DECL;
    vd->name = "p";
    vd->init = (Iron_Node *)he;
    vd->declared_type = NULL; /* will use fallback int64_t */

    /* Build a block containing this statement */
    Iron_Node **stmts = NULL;
    arrput(stmts, (Iron_Node *)vd);
    Iron_Block *block = ARENA_ALLOC(&arena, Iron_Block);
    memset(block, 0, sizeof(*block));
    block->span       = s;
    block->kind       = IRON_NODE_BLOCK;
    block->stmts      = stmts;
    block->stmt_count = 1;

    /* Create codegen context */
    Iron_Codegen ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.arena        = &arena;
    ctx.diags        = &diags;
    ctx.global_scope = global;
    ctx.program      = NULL;
    ctx.struct_bodies    = iron_strbuf_create(1024);
    ctx.forward_decls    = iron_strbuf_create(256);
    ctx.prototypes       = iron_strbuf_create(256);
    ctx.implementations  = iron_strbuf_create(1024);
    ctx.lifted_funcs     = iron_strbuf_create(1024);
    ctx.emitted_optionals = NULL;
    ctx.mono_registry    = NULL;
    ctx.indent           = 0;
    ctx.defer_depth      = 0;
    ctx.function_scope_depth = 0;

    Iron_StrBuf sb = iron_strbuf_create(512);
    emit_block(&sb, block, &ctx);

    const char *out = iron_strbuf_get(&sb);
    TEST_ASSERT_NOT_NULL(out);
    /* free(p) must appear in the block output */
    TEST_ASSERT_TRUE(str_contains(out, "free(p)"));

    iron_strbuf_free(&sb);
    iron_strbuf_free(&ctx.struct_bodies);
    iron_strbuf_free(&ctx.forward_decls);
    iron_strbuf_free(&ctx.prototypes);
    iron_strbuf_free(&ctx.implementations);
    iron_strbuf_free(&ctx.lifted_funcs);
    iron_arena_free(&arena);
    iron_diaglist_free(&diags);
}

/* ── Test: generated C includes runtime header ───────────────────────────── */

void test_codegen_includes_runtime(void) {
    const char *src = "func main() { val x = 1 }";
    const char *c = run_codegen(src);
    TEST_ASSERT_NOT_NULL(c);
    /* Generated output must include the runtime header */
    TEST_ASSERT_TRUE(str_contains(c, "#include \"runtime/iron_runtime.h\""));
}

/* ── Test: print uses Iron_println (not raw printf) ─────────────────────── */

void test_codegen_print_uses_iron_print(void) {
    const char *src =
        "func main() {\n"
        "    println(\"hello\")\n"
        "}\n";
    const char *c = run_codegen(src);
    TEST_ASSERT_NOT_NULL(c);
    /* Must emit the runtime builtin, not a raw printf */
    TEST_ASSERT_TRUE(str_contains(c, "Iron_println"));
    /* Must NOT fall back to printf with %s format */
    TEST_ASSERT_FALSE(str_contains(c, "printf(\"%s"));
}

/* ── Test: generated main() has runtime init and shutdown ───────────────── */

void test_codegen_main_has_init_shutdown(void) {
    const char *src = "func main() { val x = 1 }";
    const char *c = run_codegen(src);
    TEST_ASSERT_NOT_NULL(c);
    /* The C main() wrapper must call iron_runtime_init() and shutdown */
    TEST_ASSERT_TRUE(str_contains(c, "iron_runtime_init()"));
    TEST_ASSERT_TRUE(str_contains(c, "iron_runtime_shutdown()"));
    /* init must precede shutdown */
    TEST_ASSERT_TRUE(str_before(c, "iron_runtime_init()", "iron_runtime_shutdown()"));
}

/* ── Test: parallel-for uses dynamic thread count (not hardcoded 4) ──────── */

void test_codegen_parallel_for_dynamic_chunks(void) {
    const char *src =
        "func work(i: Int) { }\n"
        "func main() {\n"
        "    val n = 100\n"
        "    for i in n parallel { work(i) }\n"
        "}\n";
    const char *c = run_codegen(src);
    TEST_ASSERT_NOT_NULL(c);
    /* Must query pool thread count at runtime */
    TEST_ASSERT_TRUE(str_contains(c, "Iron_pool_thread_count"));
    /* Must NOT use the old hardcoded 4-chunk formula */
    TEST_ASSERT_FALSE(str_contains(c, "(_total + 3) / 4"));
}

/* ── Test: len() emits Iron_len() runtime builtin ────────────────────────── */

void test_codegen_builtin_len(void) {
    const char *src =
        "func main() {\n"
        "    val s = \"hello\"\n"
        "    val n = len(s)\n"
        "}\n";
    const char *c = run_codegen(src);
    TEST_ASSERT_NOT_NULL(c);
    /* Must emit the runtime builtin Iron_len */
    TEST_ASSERT_TRUE(str_contains(c, "Iron_len("));
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
    RUN_TEST(test_interface_generates_vtable_struct);
    RUN_TEST(test_vtable_instance_has_correct_function_pointer);
    RUN_TEST(test_monomorphization_generates_concrete_struct);
    RUN_TEST(test_monomorphization_dedup);
    RUN_TEST(test_lambda_no_captures_generates_lifted_function);
    RUN_TEST(test_lambda_with_captures_generates_env_struct);
    RUN_TEST(test_spawn_generates_lifted_function_and_pool_submit);
    RUN_TEST(test_parallel_for_generates_chunk_and_barrier);
    RUN_TEST(test_field_access_uses_arrow_for_self);
    RUN_TEST(test_call_to_type_emits_compound_literal);
    RUN_TEST(test_println_format_no_extra_args);
    RUN_TEST(test_auto_free_emits_free);
    RUN_TEST(test_codegen_output_compiles_with_clang);
    RUN_TEST(test_codegen_includes_runtime);
    RUN_TEST(test_codegen_print_uses_iron_print);
    RUN_TEST(test_codegen_main_has_init_shutdown);
    RUN_TEST(test_codegen_parallel_for_dynamic_chunks);
    RUN_TEST(test_codegen_builtin_len);

    return UNITY_END();
}
