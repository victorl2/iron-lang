/* Phase 23 Plan 23-02: VEC-01 bounded vector codegen output verification.
 *
 * Tests:
 *   1. ALLOCA for var bv: [Int; <=4] emits Iron_BVec_int64_t_4 typedef +
 *      zero-init `= {0}` (Pitfall 7 guard).
 *   2. bv.push(x) emits inline bounds-check against bv.len (NOT N) then
 *      bv.data[bv.len] = x; bv.len += 1.
 *   3. bv[0] emits inline bounds-check against bv.len then bv.data[0].
 *   4. bv.len emits (int64_t)bv.len field access (no runtime call).
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

/* Full pipeline: lex -> parse -> analyze -> hir_lower -> hir_to_lir -> optimize -> emit_c.
 * Returns arena-allocated C source string, or NULL on pipeline failure. */
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

/* ── Case 1: typedef synthesis + zero-init ──────────────────────────────────── */

void test_bvec_typedef_and_zero_init(void) {
    static const char *src =
        "func main() -> Int {\n"
        "    var bv: [Int; <=4]\n"
        "    return 0\n"
        "}\n";

    IronLIR_OptimizeInfo opt;
    const char *c_src = compile_to_c(src, &opt);
    TEST_ASSERT_NOT_NULL_MESSAGE(c_src, "compile_to_c returned NULL — check for diagnostics");

    /* VEC-01: typedef must be synthesized */
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(c_src, "Iron_BVec_int64_t_4"),
        "Expected Iron_BVec_int64_t_4 typedef in generated C");

    /* Pitfall 7: zero-init designated initializer must be present */
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(c_src, "= {0}"),
        "Expected = {0} zero-init for bounded vector ALLOCA (Pitfall 7)");

    iron_lir_optimize_info_free(&opt);
}

/* ── Case 2: push emits inline bounds-check ─────────────────────────────────── */

void test_bvec_push_emits_bounds_check(void) {
    static const char *src =
        "func main() -> Int {\n"
        "    var bv: [Int; <=4]\n"
        "    bv.push(10)\n"
        "    bv.push(20)\n"
        "    return 0\n"
        "}\n";

    IronLIR_OptimizeInfo opt;
    const char *c_src = compile_to_c(src, &opt);
    TEST_ASSERT_NOT_NULL_MESSAGE(c_src, "compile_to_c returned NULL — check for diagnostics");

    /* Push site: inline bounds-check calling iron_panic_bvec_oob */
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(c_src, "iron_panic_bvec_oob"),
        "Expected iron_panic_bvec_oob in generated C (push bounds-check)");

    /* Push site: data write through .data field */
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(c_src, ".data["),
        "Expected .data[ in generated C (push write to data field)");

    /* Push site: len increment */
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(c_src, ".len += 1"),
        "Expected .len += 1 in generated C (push len bump)");

    iron_lir_optimize_info_free(&opt);
}

/* ── Case 3: index emits inline bounds-check against len ───────────────────── */

void test_bvec_index_emits_bounds_check_against_len(void) {
    static const char *src =
        "func main() -> Int {\n"
        "    var bv: [Int; <=4]\n"
        "    bv.push(99)\n"
        "    return bv[0]\n"
        "}\n";

    IronLIR_OptimizeInfo opt;
    const char *c_src = compile_to_c(src, &opt);
    TEST_ASSERT_NOT_NULL_MESSAGE(c_src, "compile_to_c returned NULL — check for diagnostics");

    /* Index site: bounds check against bv.len (not N) */
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(c_src, "iron_panic_bvec_oob"),
        "Expected iron_panic_bvec_oob at index site in generated C");

    /* Index site: .data[] access */
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(c_src, ".data["),
        "Expected .data[ in generated C for bounded vector index read");

    iron_lir_optimize_info_free(&opt);
}

/* ── Case 4: Iron_BVec struct is a struct-level type in generated C ─────────── */

void test_bvec_struct_has_len_and_data_fields(void) {
    static const char *src =
        "func main() -> Int {\n"
        "    var bv: [Int; <=4]\n"
        "    bv.push(1)\n"
        "    return 0\n"
        "}\n";

    IronLIR_OptimizeInfo opt;
    const char *c_src = compile_to_c(src, &opt);
    TEST_ASSERT_NOT_NULL_MESSAGE(c_src, "compile_to_c returned NULL");

    /* Typedef contains uint32_t len and data[] fields */
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(c_src, "uint32_t len"),
        "Expected uint32_t len field in Iron_BVec struct");

    iron_lir_optimize_info_free(&opt);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_bvec_typedef_and_zero_init);
    RUN_TEST(test_bvec_push_emits_bounds_check);
    RUN_TEST(test_bvec_index_emits_bounds_check_against_len);
    RUN_TEST(test_bvec_struct_has_len_and_data_fields);
    return UNITY_END();
}
