/* test_container_drop.c — Phase 33 Wave 0 anchor (STDLIB-02 element dtors).
 *
 * Asserts the container element-destructor codegen contract: when a List[T]'s
 * element type T has a user `drop` / `copy` block, the monomorphized list
 * `_free` iterates and calls `Iron_<Elem>_drop` per element (before
 * free(items)), and `_clone` calls `Iron_<Elem>_copy` per element (instead of
 * bulk memcpy).  Primitive / trivial element types KEEP the fast
 * `free(items)` / `memcpy` path — no spurious per-element calls (Pitfall 5).
 *
 * Pattern source: tests/unit/test_drop_static_dispatch.c (in-process pipeline
 * lex -> parse -> analyze -> hir -> lir -> optimize -> emit_c, then grep the
 * generated C string for the per-element loop).  Wave 4 (Plan 33-04) lands the
 * element-destructor-aware monomorphization in emit_structs.c.
 */
#include "unity.h"
#include "analyzer/analyzer.h"
#include "diagnostics/diagnostics.h"
#include "hir/hir_lower.h"
#include "hir/hir_to_lir.h"
#include "lir/emit_c.h"
#include "lir/lir_optimize.h"
#include "lir/lir.h"
#include "util/arena.h"

#include <string.h>

static Iron_Arena    g_arena;
static Iron_Arena    g_lir_arena;
static Iron_Arena    g_out_arena;
static Iron_DiagList g_diags;

void setUp(void) {
    g_arena     = iron_arena_create(2 * 1024 * 1024);
    g_lir_arena = iron_arena_create(2 * 1024 * 1024);
    g_out_arena = iron_arena_create(2 * 1024 * 1024);
    g_diags     = iron_diaglist_create();
    iron_types_init(&g_arena);
}

void tearDown(void) {
    iron_arena_free(&g_out_arena);
    iron_arena_free(&g_lir_arena);
    iron_arena_free(&g_arena);
    iron_diaglist_free(&g_diags);
}

/* Full pipeline -> arena-allocated C source string, or NULL on failure. */
static const char *compile_to_c(const char *src, IronLIR_OptimizeInfo *opt_out) {
    Iron_AnalyzeResult res = iron_analyze_buffer(
        src, strlen(src), "test.iron",
        IRON_ANALYSIS_MODE_CLI_LENIENT,
        &g_arena, &g_diags, NULL, 0);
    if (res.has_errors || g_diags.error_count > 0) return NULL;

    IronHIR_Module *hir = iron_hir_lower(res.program, res.global_scope,
                                          NULL, &g_diags);
    if (!hir || g_diags.error_count > 0) return NULL;

    IronLIR_Module *lir = iron_hir_to_lir(hir, res.program, res.global_scope,
                                            &g_lir_arena, &g_diags);
    if (!lir || g_diags.error_count > 0) {
        iron_hir_module_destroy(hir);
        return NULL;
    }

    iron_lir_optimize(lir, opt_out, &g_out_arena, false, true, false);
    const char *c_src = iron_lir_emit_c(lir, &g_out_arena, &g_diags,
                                         opt_out, NULL, false, false);
    iron_hir_module_destroy(hir);
    return c_src;
}

/* Managed element: Tracked has BOTH a drop and a copy block, so a
 * List[Tracked] must emit per-element Iron_Tracked_drop / Iron_Tracked_copy
 * loops in _free / _clone. */
static const char *kManagedSrc =
    "object Tracked {\n"
    "    val id: Int\n"
    "    drop {\n"
    "        var d = 0\n"
    "    }\n"
    "    copy {\n"
    "        var c = 0\n"
    "    }\n"
    "}\n"
    "func main() {\n"
    "    var xs: [Tracked] = []\n"
    "    xs.push(Tracked(1))\n"
    "}\n";

/* Trivial element: List[Int] must keep the fast free(items)/memcpy path with
 * NO per-element destructor loop. */
static const char *kTrivialSrc =
    "func main() {\n"
    "    var xs: [Int] = []\n"
    "    xs.push(1)\n"
    "    xs.push(2)\n"
    "}\n";

/* Case 1: List[Tracked] _free emits a per-element Iron_Tracked_drop loop. */
static void test_list_drop_invokes_element_destructor(void) {
    IronLIR_OptimizeInfo opt;
    const char *c = compile_to_c(kManagedSrc, &opt);
    TEST_ASSERT_NOT_NULL_MESSAGE(c, "compile_to_c returned NULL (managed)");

    /* The element-destructor-aware _free body and its per-element call. */
    TEST_ASSERT_NOT_NULL_MESSAGE(
        strstr(c, "per-element destructor _free for Iron_Tracked"),
        "Expected element-destructor-aware _free for Iron_Tracked");
    TEST_ASSERT_NOT_NULL_MESSAGE(
        strstr(c, "Iron_Tracked_drop(&self->items["),
        "Expected per-element Iron_Tracked_drop(&self->items[..]) loop in _free");
    iron_lir_optimize_info_free(&opt);
}

/* Case 2: List[Tracked] _clone deep-copies each element via Iron_Tracked_copy. */
static void test_list_copy_invokes_element_copy(void) {
    IronLIR_OptimizeInfo opt;
    const char *c = compile_to_c(kManagedSrc, &opt);
    TEST_ASSERT_NOT_NULL_MESSAGE(c, "compile_to_c returned NULL (managed)");

    TEST_ASSERT_NOT_NULL_MESSAGE(
        strstr(c, "element-copy _clone for Iron_Tracked"),
        "Expected element-copy-aware _clone for Iron_Tracked");
    TEST_ASSERT_NOT_NULL_MESSAGE(
        strstr(c, "Iron_Tracked_copy(&dst.items["),
        "Expected per-element Iron_Tracked_copy(&dst.items[..]) loop in _clone");
    iron_lir_optimize_info_free(&opt);
}

/* Case 3 (Pitfall 5): List[Int] keeps the fast path — NO per-element drop loop,
 * NO element-destructor-aware _free comment. */
static void test_list_int_keeps_fast_path(void) {
    IronLIR_OptimizeInfo opt;
    const char *c = compile_to_c(kTrivialSrc, &opt);
    TEST_ASSERT_NOT_NULL_MESSAGE(c, "compile_to_c returned NULL (trivial)");

    TEST_ASSERT_NULL_MESSAGE(
        strstr(c, "per-element destructor _free"),
        "List[Int] must NOT emit an element-destructor-aware _free (fast path)");
    /* No primitive *_drop element loop of the gated form. */
    TEST_ASSERT_NULL_MESSAGE(
        strstr(c, "_drop(&self->items["),
        "List[Int] must NOT emit a per-element drop loop (Pitfall 5)");
    iron_lir_optimize_info_free(&opt);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_list_drop_invokes_element_destructor);
    RUN_TEST(test_list_copy_invokes_element_copy);
    RUN_TEST(test_list_int_keeps_fast_path);
    return UNITY_END();
}
