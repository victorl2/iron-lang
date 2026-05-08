/* Phase 21 Wave 0 (Plan 02): TDD scaffold for POL-02 codegen migration —
 * IRON_LIR_HEAP_ALLOC emit_c.c output verification.
 *
 * 6 cases cover the primary migration + Pitfall 1 deref sites + defer-free:
 *   Case 1: heap allocation emits Iron_FatPtr + iron_heap_alloc (not malloc)
 *   Case 2: heap alloc + free emits iron_heap_free + PHASE-24 HOOK comment
 *   Case 3: heap alloc + &p ADDR_OF uses _vN.gen (not IronAllocHdr arithmetic)
 *   Case 4: heap alloc + p.x field access uses ((T *)_vN.addr)->x form
 *   Case 5: heap alloc + p.x = 7 store uses ((T *)_vN.addr)->x form (Pitfall 1 store arm)
 *   Case 6: defer free end-to-end via emit_defer_cleanup (DEFER-02)
 *
 * Authored RED first; assertions FAIL pre-migration (emit_c.c still emits
 * malloc for IRON_LIR_HEAP_ALLOC). Flips GREEN after Plan 21-02 Task 3
 * lands the emit_c.c migration.
 *
 * Pattern source: tests/unit/test_cov_compiler_printers.c (full pipeline
 * lex→parse→analyze→hir_lower→hir_to_lir→emit_c with output assertion). */

#include "unity.h"
#include "analyzer/analyzer.h"
#include "analyzer/scope.h"
#include "analyzer/types.h"
#include "diagnostics/diagnostics.h"
#include "hir/hir_lower.h"
#include "hir/hir_to_lir.h"
#include "lir/emit_c.h"
#include "lir/lir_optimize.h"
#include "lir/lir.h"
#include "util/arena.h"

#include <string.h>
#include <stdlib.h>

/* ── Module-level fixtures ────────────────────────────────────────────────── */

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

/* ── Full pipeline helper ─────────────────────────────────────────────────── */

/* Lex → parse → analyze → hir_lower → hir_to_lir → optimize → emit_c.
 * Returns arena-allocated C source string, or NULL on pipeline failure.
 * The caller must call iron_lir_optimize_info_free(&opt) after use.
 * mode: use IRON_ANALYSIS_MODE_CLI for normal tests; IRON_ANALYSIS_MODE_CLI_LENIENT
 * for tests that require mutable bindings (e.g. field-store tests with var fields). */
static const char *compile_to_c_mode(const char *src,
                                      IronLIR_OptimizeInfo *opt_out,
                                      IronLIR_Module **lir_out,
                                      IronAnalysisMode mode) {
    Iron_AnalyzeResult res = iron_analyze_buffer(
        src, strlen(src), "test.iron",
        mode,
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

    if (lir_out) *lir_out = lir;

    iron_lir_optimize(lir, opt_out, &g_out_arena, false, true);

    const char *c_src = iron_lir_emit_c(lir, &g_out_arena, &g_diags,
                                         opt_out, NULL, false, false);

    iron_hir_module_destroy(hir);
    return c_src;
}

static const char *compile_to_c(const char *src,
                                  IronLIR_OptimizeInfo *opt_out,
                                  IronLIR_Module **lir_out) {
    return compile_to_c_mode(src, opt_out, lir_out, IRON_ANALYSIS_MODE_CLI);
}

/* ── Case 1: heap allocation emits Iron_FatPtr + iron_heap_alloc ──────────── */

void test_heap_alloc_emits_fat_ptr_and_iron_heap_alloc(void) {
    static const char *src =
        "object Point {\n"
        "    val x: Int\n"
        "    val y: Int\n"
        "}\n"
        "func main() {\n"
        "    val p = heap Point(1, 2)\n"
        "}\n";

    IronLIR_OptimizeInfo opt;
    const char *c_src = compile_to_c(src, &opt, NULL);
    TEST_ASSERT_NOT_NULL_MESSAGE(c_src, "compile_to_c returned NULL");

    /* Post-migration: heap binding local must be Iron_FatPtr, not T * */
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(c_src, "Iron_FatPtr"),
        "Expected Iron_FatPtr in emitted C (heap local type)");

    /* Post-migration: iron_heap_alloc must be called */
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(c_src, "iron_heap_alloc(__FILE__, __LINE__, sizeof("),
        "Expected iron_heap_alloc(__FILE__, __LINE__, sizeof( in emitted C");

    /* Post-migration: malloc must NOT appear for the heap alloc */
    /* NOTE: malloc may appear elsewhere in runtime headers; check the function body.
     * We do a loose check: iron_heap_alloc must be present. */
    TEST_ASSERT_NULL_MESSAGE(strstr(c_src, "malloc(sizeof("),
        "Expected no malloc(sizeof( — heap alloc must use iron_heap_alloc");

    iron_lir_optimize_info_free(&opt);
}

/* ── Case 2: heap alloc + free emits iron_heap_free + PHASE-24 HOOK ──────── */

void test_heap_free_emits_iron_heap_free_and_phase24_hook(void) {
    static const char *src =
        "object Point {\n"
        "    val x: Int\n"
        "    val y: Int\n"
        "}\n"
        "func main() {\n"
        "    val p = heap Point(1, 2)\n"
        "    free p\n"
        "}\n";

    IronLIR_OptimizeInfo opt;
    const char *c_src = compile_to_c(src, &opt, NULL);
    TEST_ASSERT_NOT_NULL_MESSAGE(c_src, "compile_to_c returned NULL");

    /* Post-migration: iron_heap_free must be called */
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(c_src, "iron_heap_free("),
        "Expected iron_heap_free( in emitted C (IRON_LIR_FREE migration)");

    /* Post-migration: PHASE-24 HOOK comment must be present */
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(c_src, "PHASE-24 HOOK"),
        "Expected PHASE-24 HOOK comment at IRON_LIR_FREE site");

    /* Post-migration: raw free() must NOT appear for the binding free */
    /* The PHASE-24 HOOK comment uses the word "free" but let's check
     * for bare "free(" without "iron_heap_free" prefix. */
    /* Loose check: iron_heap_free is present, raw "free(" without iron prefix absent.
     * We search for "free(" and verify it's preceded by "iron_heap_". */
    const char *raw_free = strstr(c_src, "free(");
    while (raw_free) {
        /* Check that it's preceded by "iron_heap_" */
        if (raw_free >= c_src + 10 &&
            strncmp(raw_free - 10, "iron_heap_", 10) == 0) {
            /* This is iron_heap_free( — OK */
        } else {
            /* Raw free() found — fail */
            TEST_FAIL_MESSAGE("Found raw free( in emitted C — should use iron_heap_free");
        }
        raw_free = strstr(raw_free + 1, "free(");
    }

    iron_lir_optimize_info_free(&opt);
}

/* ── Case 3: heap alloc + &p ADDR_OF uses _vN.gen directly ───────────────── */

void test_heap_addr_of_uses_direct_gen_field(void) {
    static const char *src =
        "object Point {\n"
        "    val x: Int\n"
        "    val y: Int\n"
        "}\n"
        "func main() {\n"
        "    val p = heap Point(1, 2)\n"
        "    val ptr: *Point = &p\n"
        "}\n";

    IronLIR_OptimizeInfo opt;
    const char *c_src = compile_to_c(src, &opt, NULL);
    TEST_ASSERT_NOT_NULL_MESSAGE(c_src, "compile_to_c returned NULL");

    /* Post-migration: gen recovery must use _vN.gen directly (not IronAllocHdr arithmetic).
     * The heap binding IS the Iron_FatPtr so .gen is already the IronAllocHdr gen. */
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(c_src, ".gen"),
        "Expected .gen field read in emitted C (post-migration direct gen recovery)");

    /* Post-migration: IronAllocHdr arithmetic must NOT be present for heap-source ADDR_OF */
    TEST_ASSERT_NULL_MESSAGE(strstr(c_src, "sizeof(IronAllocHdr)"),
        "Expected no IronAllocHdr arithmetic — heap gen must be read from _vN.gen directly");

    iron_lir_optimize_info_free(&opt);
}

/* ── Case 4: heap alloc + field access uses ((T *)_vN.addr)->field ────────── */

void test_heap_field_access_uses_addr_cast_deref(void) {
    static const char *src =
        "object Point {\n"
        "    val x: Int\n"
        "    val y: Int\n"
        "}\n"
        "func main() {\n"
        "    val p = heap Point(1, 2)\n"
        "    val q = p.x\n"
        "}\n";

    IronLIR_OptimizeInfo opt;
    const char *c_src = compile_to_c(src, &opt, NULL);
    TEST_ASSERT_NOT_NULL_MESSAGE(c_src, "compile_to_c returned NULL");

    /* Post-migration: field access on a heap binding must use .addr cast-and-deref.
     * Pre-migration: _vN->x (direct dereference of T* local)
     * Post-migration: ((Iron_Point *)_vN.addr)->x */
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(c_src, ".addr)"),
        "Expected .addr) cast-and-deref in emitted C for heap field access");

    iron_lir_optimize_info_free(&opt);
}

/* ── Case 5: heap alloc + store uses ((T *)_vN.addr)->field form ─────────── */
/* Pitfall 1 store-site coverage (RESEARCH Pitfall 1 store arm) */

void test_heap_field_store_uses_addr_cast_deref(void) {
    /* var fields + var binding required for field mutation.
     * IRON_ANALYSIS_MODE_CLI_LENIENT disables v3_strict_mode (which would
     * require an explicit init block for objects with mutable fields). */
    static const char *src =
        "object Point {\n"
        "    var x: Int\n"
        "    var y: Int\n"
        "}\n"
        "func main() {\n"
        "    var p = heap Point(1, 2)\n"
        "    p.x = 7\n"
        "}\n";

    IronLIR_OptimizeInfo opt;
    const char *c_src = compile_to_c_mode(src, &opt, NULL, IRON_ANALYSIS_MODE_CLI_LENIENT);
    TEST_ASSERT_NOT_NULL_MESSAGE(c_src, "compile_to_c returned NULL");

    /* Post-migration: store to heap field must use .addr cast-and-deref form.
     * Pre-migration: _vN->x = 7
     * Post-migration: ((Iron_Point *)_vN.addr)->x = 7  OR  *((Iron_Point *)_vN.addr) = ... */
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(c_src, ".addr)"),
        "Expected .addr) cast-and-deref for heap field store (Pitfall 1 store arm)");

    iron_lir_optimize_info_free(&opt);
}

/* ── Case 6: defer free end-to-end via emit_defer_cleanup ────────────────── */
/* DEFER-02 ergonomic idiom end-to-end (hir_to_lir:1730 emit_defer_cleanup) */

void test_defer_free_emits_iron_heap_free_in_epilogue(void) {
    static const char *src =
        "object Point {\n"
        "    val x: Int\n"
        "    val y: Int\n"
        "}\n"
        "func main() {\n"
        "    val p = heap Point(1, 2)\n"
        "    defer free p\n"
        "}\n";

    IronLIR_OptimizeInfo opt;
    const char *c_src = compile_to_c(src, &opt, NULL);
    TEST_ASSERT_NOT_NULL_MESSAGE(c_src, "compile_to_c returned NULL");

    /* Post-migration: emit_defer_cleanup LIFO machinery emits iron_heap_free
     * before IRON_LIR_RETURN. The call must appear in the emitted C. */
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(c_src, "iron_heap_free("),
        "Expected iron_heap_free( in emitted C via defer-free epilogue (DEFER-02)");

    /* Post-migration: iron_heap_alloc must also be present (alloc side) */
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(c_src, "iron_heap_alloc("),
        "Expected iron_heap_alloc( in emitted C (alloc side of defer free)");

    iron_lir_optimize_info_free(&opt);
}

/* ── main ─────────────────────────────────────────────────────────────────── */

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_heap_alloc_emits_fat_ptr_and_iron_heap_alloc);
    RUN_TEST(test_heap_free_emits_iron_heap_free_and_phase24_hook);
    RUN_TEST(test_heap_addr_of_uses_direct_gen_field);
    RUN_TEST(test_heap_field_access_uses_addr_cast_deref);
    RUN_TEST(test_heap_field_store_uses_addr_cast_deref);
    RUN_TEST(test_defer_free_emits_iron_heap_free_in_epilogue);
    return UNITY_END();
}
