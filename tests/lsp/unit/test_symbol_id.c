/* test_symbol_id -- Phase 3 Plan 01 Task 04 (NAV-16).
 *
 * Covers:
 *   1. Triple determinism across a re-analyze into a fresh arena
 *      (Pitfall 1).
 *   2. Method name_path is dotted module.Owner.method.
 *   3. Stdlib sentinel prefix yields module stem correctly.
 *   4. Hash equality on equal triples; FNV-1a constants pinned.
 */
#include "unity.h"

#include "lsp/facade/nav/symbol_id.h"

#include "analyzer/analyzer.h"
#include "analyzer/scope.h"
#include "diagnostics/diagnostics.h"
#include "parser/ast.h"
#include "util/arena.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

/* ── Test 01: determinism across re-analyze ──────────────────────────── */
static void test_triple_deterministic_across_reanalyze(void) {
    const char *src = "func greet(name: String) -> String { return name }\n";
    const char *path = "/tmp/util.iron";

    Iron_Arena a1 = iron_arena_create(16 * 1024);
    Iron_DiagList d1 = iron_diaglist_create();
    Iron_AnalyzeResult r1 = iron_analyze_buffer(src, strlen(src), path,
                                                 IRON_ANALYSIS_MODE_CLI,
                                                 &a1, &d1, NULL);
    TEST_ASSERT_NOT_NULL(r1.global_scope);
    Iron_Symbol *s1 = iron_scope_lookup_local(r1.global_scope, "greet");
    TEST_ASSERT_NOT_NULL(s1);
    IronLsp_SymbolId id1 = ilsp_symbol_id_derive(s1, path, r1.program, &a1);

    Iron_Arena a2 = iron_arena_create(16 * 1024);
    Iron_DiagList d2 = iron_diaglist_create();
    Iron_AnalyzeResult r2 = iron_analyze_buffer(src, strlen(src), path,
                                                 IRON_ANALYSIS_MODE_CLI,
                                                 &a2, &d2, NULL);
    Iron_Symbol *s2 = iron_scope_lookup_local(r2.global_scope, "greet");
    TEST_ASSERT_NOT_NULL(s2);
    IronLsp_SymbolId id2 = ilsp_symbol_id_derive(s2, path, r2.program, &a2);

    TEST_ASSERT_TRUE_MESSAGE(ilsp_symbol_id_equal(id1, id2),
                              "triples must be equal across re-analyze");
    TEST_ASSERT_EQUAL_UINT64(id1.hash, id2.hash);
    TEST_ASSERT_EQUAL_STRING(id1.name_path, id2.name_path);
    TEST_ASSERT_EQUAL_STRING("util.greet", id1.name_path);

    iron_diaglist_free(&d1);
    iron_diaglist_free(&d2);
    iron_arena_free(&a1);
    iron_arena_free(&a2);
}

/* ── Test 02: method name_path is dotted (module.Owner.method) ───────── */
static void test_method_name_path_is_dotted(void) {
    const char *src =
        "object Foo {\n"
        "    val x: Int\n"
        "}\n"
        "func Foo.bar() {}\n";
    const char *path = "/tmp/mod.iron";

    Iron_Arena arena = iron_arena_create(16 * 1024);
    Iron_DiagList diags = iron_diaglist_create();
    Iron_AnalyzeResult r = iron_analyze_buffer(src, strlen(src), path,
                                                IRON_ANALYSIS_MODE_CLI,
                                                &arena, &diags, NULL);
    TEST_ASSERT_NOT_NULL(r.global_scope);
    /* Look up `Foo` to confirm the object symbol exists; the method
     * symbol may live in a child scope attached to Foo, so we also
     * derive an id by constructing a sym from the program AST
     * directly. */
    Iron_Symbol *foo = iron_scope_lookup_local(r.global_scope, "Foo");
    TEST_ASSERT_NOT_NULL(foo);
    IronLsp_SymbolId foo_id = ilsp_symbol_id_derive(foo, path, r.program, &arena);
    TEST_ASSERT_EQUAL_STRING("mod.Foo", foo_id.name_path);

    /* Find the method decl in the program to synthesize a stand-in
     * method symbol. This does not exercise the resolver's method
     * registration (that may vary), but it exercises the name_path
     * walker on a method decl node directly. */
    Iron_Node *method_node = NULL;
    for (int i = 0; i < r.program->decl_count; i++) {
        if (r.program->decls[i] &&
            r.program->decls[i]->kind == IRON_NODE_METHOD_DECL) {
            method_node = r.program->decls[i];
            break;
        }
    }
    TEST_ASSERT_NOT_NULL_MESSAGE(method_node, "expected Foo.bar method");
    Iron_MethodDecl *md = (Iron_MethodDecl *)method_node;
    Iron_Symbol syn = {0};
    syn.name = md->method_name;
    syn.sym_kind = IRON_SYM_METHOD;
    syn.decl_node = method_node;
    syn.span = method_node->span;
    IronLsp_SymbolId bar_id = ilsp_symbol_id_derive(&syn, path, r.program, &arena);
    TEST_ASSERT_EQUAL_STRING("mod.Foo.bar", bar_id.name_path);
    TEST_ASSERT_EQUAL_INT(IRON_SYM_METHOD, bar_id.kind);

    iron_diaglist_free(&diags);
    iron_arena_free(&arena);
}

/* ── Test 03: stdlib sentinel prefix yields correct module stem ──────── */
static void test_stdlib_sentinel_prefix(void) {
    Iron_Arena arena = iron_arena_create(2048);
    const char *stem = ilsp_symbol_id_module_stem("stdlib://math", &arena);
    TEST_ASSERT_EQUAL_STRING("math", stem);

    const char *dep = ilsp_symbol_id_module_stem("dep://pkg/rel/util.iron",
                                                   &arena);
    TEST_ASSERT_EQUAL_STRING("util", dep);

    const char *abs_path = ilsp_symbol_id_module_stem("/tmp/mymodule.iron",
                                                        &arena);
    TEST_ASSERT_EQUAL_STRING("mymodule", abs_path);

    iron_arena_free(&arena);
}

/* ── Test 04: FNV-1a pinned constants yield deterministic hash ───────── */
static void test_fnv1a_pinned_constants(void) {
    /* Known reference vector: FNV-1a of empty input returns the offset
     * basis. FNV-1a of "a" (0x61) returns basis XOR 0x61, then * prime. */
    TEST_ASSERT_EQUAL_UINT64((uint64_t)0xcbf29ce484222325ULL,
                              ilsp_symbol_id_fnv1a64("", 0));

    uint64_t expected_a = (uint64_t)0xcbf29ce484222325ULL;
    expected_a ^= (uint64_t)'a';
    expected_a *= (uint64_t)0x100000001b3ULL;
    TEST_ASSERT_EQUAL_UINT64(expected_a, ilsp_symbol_id_fnv1a64("a", 1));

    /* Equal triples -> equal hash. */
    IronLsp_SymbolId x = { .canonical_path = "/a.iron", .name_path = "a.foo",
                            .kind = IRON_SYM_FUNCTION, .hash = 12345 };
    IronLsp_SymbolId y = { .canonical_path = "/a.iron", .name_path = "a.foo",
                            .kind = IRON_SYM_FUNCTION, .hash = 12345 };
    TEST_ASSERT_TRUE(ilsp_symbol_id_equal(x, y));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_triple_deterministic_across_reanalyze);
    RUN_TEST(test_method_name_path_is_dotted);
    RUN_TEST(test_stdlib_sentinel_prefix);
    RUN_TEST(test_fnv1a_pinned_constants);
    return UNITY_END();
}
