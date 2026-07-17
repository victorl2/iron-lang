/* Phase 23 Plan 23-02: VEC-03 bounded vector OOB panic codegen verification.
 *
 * Tests:
 *   1. iron_panic_bvec_oob declaration is reachable (symbol exists in runtime).
 *   2. Pushing to a [Int; <=4] generates iron_panic_bvec_oob in emitted C
 *      with the correct capacity bound (N=4).
 *   3. Index access on a [Int; <=4] generates a bounds-check against bv.len,
 *      NOT against N (CONTEXT decision Area 3: past-len semantics).
 *   4. Multiple pushes each emit a bounds-check (each push site independently
 *      checks).
 *
 * Pattern source: tests/unit/test_heap_alloc_codegen.c */

#include "unity.h"
#include "analyzer/analyzer.h"
#include "diagnostics/diagnostics.h"
#include "hir/hir_lower.h"
#include "hir/hir_to_lir.h"
#include "lir/emit_c.h"
#include "lir/lir_optimize.h"
#include "lir/lir.h"
#include "util/arena.h"
#include "runtime/iron_panic.h"  /* iron_panic_bvec_oob declaration */

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

/* Count occurrences of needle in haystack */
static int count_substr(const char *haystack, const char *needle) {
    int count = 0;
    const char *p = haystack;
    size_t nlen = strlen(needle);
    while ((p = strstr(p, needle)) != NULL) {
        count++;
        p += nlen;
    }
    return count;
}

/* ── Case 1: iron_panic_bvec_oob symbol is reachable ───────────────────────── */

void test_panic_bvec_oob_symbol_reachable(void) {
    /* If iron_panic_bvec_oob is not declared / defined, linking this test
     * fails.  We verify it's callable by emitting C that references it:
     * generate a bounded-vector push and check the symbol appears in output. */
    static const char *src =
        "func main() -> Int {\n"
        "    var bv: [Int; <=1]\n"
        "    bv.push(1)\n"
        "    return 0\n"
        "}\n";

    IronLIR_OptimizeInfo opt;
    const char *c_src = compile_to_c(src, &opt);
    TEST_ASSERT_NOT_NULL_MESSAGE(c_src, "compile_to_c returned NULL");

    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(c_src, "iron_panic_bvec_oob"),
        "Expected iron_panic_bvec_oob symbol reference in generated C");

    iron_lir_optimize_info_free(&opt);
}

/* ── Case 2: push bounds-check with correct capacity argument ───────────────── */

void test_push_bounds_check_uses_capacity_N(void) {
    /* [Int; <=4]: N = 4 */
    static const char *src =
        "func main() -> Int {\n"
        "    var bv: [Int; <=4]\n"
        "    bv.push(1)\n"
        "    return 0\n"
        "}\n";

    IronLIR_OptimizeInfo opt;
    const char *c_src = compile_to_c(src, &opt);
    TEST_ASSERT_NOT_NULL_MESSAGE(c_src, "compile_to_c returned NULL");

    /* Push bounds check: `>= 4)` must appear for capacity N=4 */
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(c_src, ">= 4)"),
        "Expected '>= 4)' bounds-check for [Int; <=4] push site");

    iron_lir_optimize_info_free(&opt);
}

/* ── Case 3: index bounds-check against len (not N) ────────────────────────── */

void test_index_bounds_check_uses_len_not_N(void) {
    static const char *src =
        "func main() -> Int {\n"
        "    var bv: [Int; <=4]\n"
        "    bv.push(42)\n"
        "    return bv[0]\n"
        "}\n";

    IronLIR_OptimizeInfo opt;
    const char *c_src = compile_to_c(src, &opt);
    TEST_ASSERT_NOT_NULL_MESSAGE(c_src, "compile_to_c returned NULL");

    /* Index bounds-check must reference .len (initialized region), not N */
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(c_src, ".len)"),
        "Expected '.len)' in index bounds-check (check against len, not N)");

    iron_lir_optimize_info_free(&opt);
}

/* ── Case 4: multiple pushes each emit a bounds-check ──────────────────────── */

void test_multiple_pushes_each_emit_bounds_check(void) {
    static const char *src =
        "func main() -> Int {\n"
        "    var bv: [Int; <=4]\n"
        "    bv.push(1)\n"
        "    bv.push(2)\n"
        "    bv.push(3)\n"
        "    return 0\n"
        "}\n";

    IronLIR_OptimizeInfo opt;
    const char *c_src = compile_to_c(src, &opt);
    TEST_ASSERT_NOT_NULL_MESSAGE(c_src, "compile_to_c returned NULL");

    /* Three pushes → three bounds-check calls */
    int n = count_substr(c_src, "iron_panic_bvec_oob");
    TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(3, n,
        "Expected at least 3 iron_panic_bvec_oob calls for 3 pushes");

    iron_lir_optimize_info_free(&opt);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_panic_bvec_oob_symbol_reachable);
    RUN_TEST(test_push_bounds_check_uses_capacity_N);
    RUN_TEST(test_index_bounds_check_uses_len_not_N);
    RUN_TEST(test_multiple_pushes_each_emit_bounds_check);
    return UNITY_END();
}
