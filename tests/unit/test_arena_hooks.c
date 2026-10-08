/* Arena cleanup hooks, and the analyzer structures that use them.
 *
 * Scopes and the interface registry keep stb_ds maps and arrays on the
 * heap; the arena frees them when it is freed. These tests run with leak
 * detection on in Linux sanitizer builds (see tests/unit/CMakeLists.txt),
 * so a structure that stops being released fails here even though the
 * suite as a whole runs with detect_leaks=0.
 *
 * AST node arrays are arena arrays (iron_arena_arr_new); the last test
 * analyzes a program that builds every kind of them and frees the arena. */
#include "unity.h"
#include "util/arena.h"
#include "analyzer/scope.h"
#include "analyzer/iface_collect.h"
#include "analyzer/analyzer.h"
#include "diagnostics/diagnostics.h"
#include "parser/ast.h"
#include "stb_ds.h"

#include <stdio.h>
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

static int order[8];
static int order_len;

static void record(void *ctx) { order[order_len++] = *(int *)ctx; }

void test_hooks_run_in_reverse_order_at_free(void) {
    static int one = 1, two = 2, three = 3;
    order_len = 0;
    Iron_Arena a = iron_arena_create(1024);
    TEST_ASSERT_TRUE(iron_arena_on_free(&a, record, &one));
    TEST_ASSERT_TRUE(iron_arena_on_free(&a, record, &two));
    TEST_ASSERT_TRUE(iron_arena_on_free(&a, record, &three));
    TEST_ASSERT_EQUAL_INT(0, order_len);
    iron_arena_free(&a);
    TEST_ASSERT_EQUAL_INT(3, order_len);
    TEST_ASSERT_EQUAL_INT(3, order[0]);
    TEST_ASSERT_EQUAL_INT(2, order[1]);
    TEST_ASSERT_EQUAL_INT(1, order[2]);
    TEST_ASSERT_EQUAL_INT(0, a.hook_count);
}

/* The hook's context may live in the arena: hooks run before the chunks
 * are released. */
static void read_arena_ctx(void *ctx) { order[order_len++] = *(int *)ctx; }

void test_hook_context_in_arena_is_still_valid(void) {
    order_len = 0;
    Iron_Arena a = iron_arena_create(1024);
    int *v = (int *)iron_arena_alloc(&a, sizeof(int), _Alignof(int));
    *v = 42;
    iron_arena_on_free(&a, read_arena_ctx, v);
    iron_arena_free(&a);
    TEST_ASSERT_EQUAL_INT(1, order_len);
    TEST_ASSERT_EQUAL_INT(42, order[0]);
}

/* An owned array that grows (and moves) after registration is freed at
 * its final address. */
void test_owned_array_freed_after_growth(void) {
    Iron_Arena a = iron_arena_create(1024);
    int **slot = (int **)iron_arena_alloc(&a, sizeof(int *), _Alignof(int *));
    *slot = NULL;
    iron_arena_own_arr(&a, (void **)slot);
    for (int i = 0; i < 10000; i++) arrput(*slot, i);
    TEST_ASSERT_EQUAL_INT(10000, (int)arrlen(*slot));
    iron_arena_free(&a);
}

/* An owned slot still NULL at free time is fine. */
void test_owned_null_array(void) {
    Iron_Arena a = iron_arena_create(1024);
    int **slot = (int **)iron_arena_alloc(&a, sizeof(int *), _Alignof(int *));
    *slot = NULL;
    iron_arena_own_arr(&a, (void **)slot);
    iron_arena_free(&a);
}

void test_scope_symbol_maps_released_with_arena(void) {
    Iron_Arena a = iron_arena_create(4096);
    Iron_Scope *global = iron_scope_create(&a, NULL, IRON_SCOPE_GLOBAL);
    TEST_ASSERT_NOT_NULL(global);
    Iron_Scope *inner = global;
    char name[32];
    for (int d = 0; d < 20; d++) {
        inner = iron_scope_create(&a, inner, IRON_SCOPE_BLOCK);
        for (int i = 0; i < 50; i++) {
            snprintf(name, sizeof name, "v%d_%d", d, i);
            const char *n = iron_arena_strdup(&a, name, strlen(name));
            Iron_Span sp;
            memset(&sp, 0, sizeof sp);
            Iron_Symbol *sym = iron_symbol_create(&a, n, IRON_SYM_VARIABLE, NULL, sp);
            TEST_ASSERT_TRUE(iron_scope_define(inner, &a, sym));
        }
    }
    TEST_ASSERT_NOT_NULL(iron_scope_lookup(inner, "v0_0"));
    iron_arena_free(&a);
}

void test_iface_registry_released_with_arena(void) {
    Iron_Arena a = iron_arena_create(4096);
    Iron_InterfaceDecl *iface = ARENA_ALLOC(&a, Iron_InterfaceDecl);
    memset(iface, 0, sizeof *iface);
    iface->kind = IRON_NODE_INTERFACE_DECL;
    iface->name = "Shape";

    enum { N = 30 };
    Iron_Node **decls = (Iron_Node **)iron_arena_alloc(
        &a, (N + 1) * sizeof(Iron_Node *), _Alignof(Iron_Node *));
    decls[0] = (Iron_Node *)iface;
    static const char *impls[] = { "Shape" };
    char name[16];
    for (int i = 0; i < N; i++) {
        Iron_ObjectDecl *obj = ARENA_ALLOC(&a, Iron_ObjectDecl);
        memset(obj, 0, sizeof *obj);
        obj->kind = IRON_NODE_OBJECT_DECL;
        snprintf(name, sizeof name, "T%02d", N - i);
        obj->name = iron_arena_strdup(&a, name, strlen(name));
        obj->implements_names = impls;
        obj->implements_count = 1;
        decls[i + 1] = (Iron_Node *)obj;
    }
    Iron_Program *prog = ARENA_ALLOC(&a, Iron_Program);
    memset(prog, 0, sizeof *prog);
    prog->kind = IRON_NODE_PROGRAM;
    prog->decls = decls;
    prog->decl_count = N + 1;

    Iron_IfaceRegistry reg = iron_iface_collect(prog, &a);
    ptrdiff_t idx = shgeti(reg.map, "Shape");
    TEST_ASSERT_TRUE(idx >= 0);
    TEST_ASSERT_EQUAL_INT(N, reg.map[idx].value.impl_count);
    TEST_ASSERT_EQUAL_STRING("T01", reg.map[idx].value.impls[0].type_name);
    iron_arena_free(&a);
}


/* Arena arrays read correctly through arrlen and grow by copying. */
void test_arena_array_header_and_push(void) {
    Iron_Arena a = iron_arena_create(1024);
    TEST_ASSERT_NULL(iron_arena_arr_new(&a, 0, sizeof(int)));

    int *stb = NULL;
    for (int i = 0; i < 100; i++) arrput(stb, i);
    int *arr = (int *)iron_arena_arr_adopt(&a, stb, sizeof(int));
    TEST_ASSERT_EQUAL_INT(100, (int)arrlen(arr));
    TEST_ASSERT_EQUAL_INT(100, (int)arrcap(arr));
    TEST_ASSERT_EQUAL_INT(99, arr[99]);

    int *empty = NULL;
    TEST_ASSERT_NULL(iron_arena_arr_adopt(&a, empty, sizeof(int)));

    const char **names = NULL;
    int count = 0;
    static const char *words[] = { "a", "b", "c" };
    for (int i = 0; i < 3; i++) {
        IRON_ARENA_ARR_PUSH(&a, names, count, words[i]);
        count++;
    }
    TEST_ASSERT_EQUAL_INT(3, (int)arrlen(names));
    TEST_ASSERT_EQUAL_STRING("a", names[0]);
    TEST_ASSERT_EQUAL_STRING("c", names[2]);
    iron_arena_free(&a);
}

/* Parsing and analyzing a program that uses every kind of node array
 * (block statements, call and method arguments, parameters, generic
 * parameters and arguments, func type parameters, tuples, interpolation
 * parts, match cases and patterns, enum variants and payloads, object
 * fields, implements lists, interface signatures, elif chains, list
 * literals) leaves nothing behind once the arena is freed. The program
 * compiles and runs cleanly, so every analyzer pass sees all of it. */
static const char *k_all_node_arrays =
    "interface Describer {\n"
    "    readonly func describe() -> String\n"
    "    readonly func shout() -> String {\n"
    "        return \"{self.describe()}!\"\n"
    "    }\n"
    "}\n"
    "\n"
    "interface Named {\n"
    "    readonly func name() -> String\n"
    "}\n"
    "\n"
    "object Cat impl Describer {\n"
    "    val name: String\n"
    "    val lives: Int\n"
    "\n"
    "    readonly func describe() -> String {\n"
    "        return \"cat {self.name} with {self.lives} lives\"\n"
    "    }\n"
    "}\n"
    "\n"
    "patch object Cat impl Named {\n"
    "    readonly func name() -> String {\n"
    "        return self.name\n"
    "    }\n"
    "}\n"
    "\n"
    "object Holder[T] {\n"
    "    val item: T\n"
    "}\n"
    "\n"
    "enum Shape {\n"
    "    Circle(Float),\n"
    "    Rect(Float, Float),\n"
    "    Empty,\n"
    "}\n"
    "\n"
    "enum Outcome[T, E] {\n"
    "    Ok(T),\n"
    "    Err(E),\n"
    "}\n"
    "\n"
    "func first[T](xs: [T], fallback: T) -> T {\n"
    "    if len(xs) > 0 {\n"
    "        return xs[0]\n"
    "    }\n"
    "    return fallback\n"
    "}\n"
    "\n"
    "func area(s: Shape) -> Float {\n"
    "    match s {\n"
    "        Shape.Circle(r) -> return 3.0 * r * r\n"
    "        Shape.Rect(w, h) -> {\n"
    "            return w * h\n"
    "        }\n"
    "        Shape.Empty -> return 0.0\n"
    "    }\n"
    "    return 0.0\n"
    "}\n"
    "\n"
    "func divmod(a: Int, b: Int) -> (Int, Int) {\n"
    "    return (a / b, a % b)\n"
    "}\n"
    "\n"
    "func check(n: Int) -> Outcome[Int, String] {\n"
    "    if n > 10 {\n"
    "        return Outcome.Ok(n)\n"
    "    } elif n > 5 {\n"
    "        return Outcome.Ok(n * 2)\n"
    "    } elif n > 0 {\n"
    "        return Outcome.Err(\"small {n}\")\n"
    "    }\n"
    "    return Outcome.Err(\"not positive\")\n"
    "}\n"
    "\n"
    "func main() {\n"
    "    val c = Cat(\"Tom\", 9)\n"
    "    val d: Describer = c\n"
    "    println(\"{d.describe()} {d.shout()} {c.name()}\")\n"
    "    val b = Holder[Int](3)\n"
    "    val ops: [func(Int) -> Int] = [func(x: Int) -> Int { return x + b.item }]\n"
    "    val total = first([1, 2, 3], 0) + ops[0](4)\n"
    "    val (q, r) = divmod(17, 5)\n"
    "    val shared: rc [Int] = rc [q, r]\n"
    "    val nums = [1, 2, 3]\n"
    "    for v in nums parallel {\n"
    "        println(\"{v}\")\n"
    "    }\n"
    "    match check(total) {\n"
    "        Outcome.Ok(v) -> println(\"ok {v} {len(shared)}\")\n"
    "        Outcome.Err(e) -> println(\"error: {e}\")\n"
    "    }\n"
    "    println(\"area={area(Shape.Rect(2.0, 3.0))}\")\n"
    "    assert(total == 8)\n"
    "}\n";

void test_analysis_node_arrays_released_with_arena(void) {
    for (int round = 0; round < 2; round++) {
        Iron_Arena a = iron_arena_create(64 * 1024);
        Iron_DiagList diags = iron_diaglist_create();
        IronAnalysisMode mode = round == 0 ? IRON_ANALYSIS_MODE_CLI
                                           : IRON_ANALYSIS_MODE_LSP;
        Iron_AnalyzeResult r = iron_analyze_buffer(
            k_all_node_arrays, strlen(k_all_node_arrays), "node_arrays.iron",
            mode, &a, &diags, NULL, 0);
        if (diags.error_count > 0) {
            for (int i = 0; i < diags.count; i++)
                printf("diag: %s\n", diags.items[i].message);
        }
        TEST_ASSERT_EQUAL_INT(0, diags.error_count);
        TEST_ASSERT_NOT_NULL(r.global_scope);
        iron_diaglist_free(&diags);
        iron_arena_free(&a);
    }
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_hooks_run_in_reverse_order_at_free);
    RUN_TEST(test_hook_context_in_arena_is_still_valid);
    RUN_TEST(test_owned_array_freed_after_growth);
    RUN_TEST(test_owned_null_array);
    RUN_TEST(test_scope_symbol_maps_released_with_arena);
    RUN_TEST(test_iface_registry_released_with_arena);
    RUN_TEST(test_arena_array_header_and_push);
    RUN_TEST(test_analysis_node_arrays_released_with_arena);
    return UNITY_END();
}
