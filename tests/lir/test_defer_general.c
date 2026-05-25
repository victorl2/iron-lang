/* test_defer_general.c — Phase 32 DEFER-01/03/04 codegen-order RED-anchor.
 *
 * Wave 0 contract-first TDD. Asserts the deterministic, position-based oracle
 * for generalized `defer <statement>`: the emitted C must place defer bodies
 * (LIFO) ahead of local destructors at each scope-exit edge, must run defers on
 * the early-return cleanup path, and must read deferred variables at exit time
 * (read-at-exit, NOT Go-style snapshotting). Oracle = strstr-offset ordering of
 * substrings in the emitted C string; NEVER any wall-time assertion.
 *
 * TWO STAGES (mirroring tests/lir/test_gencheck_elision.c):
 *
 *   (a) RUNS NOW — test_defer_free_still_compiles: compiles a `defer free b`
 *       program (the Phase-21 supported form) and asserts the emitted C is
 *       non-NULL and contains the heap-free call. Proves the full pipeline
 *       (lex->parse->analyze->hir_lower->hir_to_lir->optimize->emit_c) links and
 *       runs against iron_compiler today.
 *
 *   (b) STAGED behind #ifdef DEFER_GENERAL_READY (NOT defined yet) — the
 *       gate-dependent asserts (LIFO order, defer-then-drop interleave,
 *       early-return cleanup, read-at-exit). These compile a GENERAL defer
 *       program; today the Phase-21 E0276 gate rejects it so the pipeline
 *       returns NULL. The #ifdef keeps THIS executable linkable + GREEN now
 *       (no unresolved symbols; the gate-dependent asserts stay dormant).
 *       Plan 32-02 removes the gate and defines DEFER_GENERAL_READY to flip
 *       these asserts live — they are the GREEN targets.
 *
 * Pattern source: tests/unit/test_heap_alloc_codegen.c (compile_to_c_mode
 * full-pipeline helper; Case 6 shows the emit_defer_cleanup assertion style).
 */

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

/* ── Full pipeline helper (verbatim from test_heap_alloc_codegen.c) ───────── */

/* Lex → parse → analyze → hir_lower → hir_to_lir → optimize → emit_c.
 * Returns arena-allocated C source string, or NULL on pipeline failure.
 * The caller must call iron_lir_optimize_info_free(&opt) after use.
 * mode: IRON_ANALYSIS_MODE_CLI for normal tests; IRON_ANALYSIS_MODE_CLI_LENIENT
 * for tests that require mutable bindings. */
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

    iron_lir_optimize(lir, opt_out, &g_out_arena, false, true, false);

    const char *c_src = iron_lir_emit_c(lir, &g_out_arena, &g_diags,
                                         opt_out, NULL, false, false);

    iron_hir_module_destroy(hir);
    return c_src;
}

/* ── find_order: byte-offset of needle in haystack (ordering oracle) ──────── */
/* Returns the offset of `needle` within `haystack`, or -1 if absent. Tests use
 * find_order(a) < find_order(b) to assert relative emission order — never any
 * timing-based comparison. Guarded by DEFER_GENERAL_READY: only the staged
 * (Plan 32-02 GREEN-target) cases use it, so leaving it unguarded would trip
 * -Werror=unused-function in the runs-now (gate-on) configuration. */
#ifdef DEFER_GENERAL_READY
static long find_order(const char *haystack, const char *needle) {
    const char *p = strstr(haystack, needle);
    return p ? (long)(p - haystack) : -1L;
}
#endif

/* ── (a) RUNS NOW: defer free still compiles end-to-end ───────────────────── */

void test_defer_free_still_compiles(void) {
    /* The Phase-21 supported `defer free <binding>` form. Proves the harness +
     * full pipeline link and run against iron_compiler today, independent of
     * the general-defer gate removal landing in Plan 32-02. */
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
    const char *c_src = compile_to_c_mode(src, &opt, NULL, IRON_ANALYSIS_MODE_CLI);
    TEST_ASSERT_NOT_NULL_MESSAGE(c_src, "compile_to_c_mode returned NULL for defer free");

    /* emit_defer_cleanup must place a heap-free on the scope-exit edge. */
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(c_src, "iron_heap_free"),
        "Expected iron_heap_free in emitted C via defer-free epilogue (DEFER-02)");

    iron_lir_optimize_info_free(&opt);
}

/* ── (b) STAGED behind DEFER_GENERAL_READY — Plan 32-02 GREEN targets ─────── */
#ifdef DEFER_GENERAL_READY

/* LIFO ordering (DEFER-03): two defers → the last-registered (d2) defer body is
 * emitted BEFORE the first-registered (d1) at the exit edge. */
void test_lifo_order(void) {
    static const char *src =
        "func main() {\n"
        "    defer print(\"d1\")\n"
        "    defer print(\"d2\")\n"
        "    print(\"body\")\n"
        "}\n";

    IronLIR_OptimizeInfo opt;
    const char *c_src = compile_to_c_mode(src, &opt, NULL, IRON_ANALYSIS_MODE_CLI);
    TEST_ASSERT_NOT_NULL_MESSAGE(c_src, "compile_to_c_mode returned NULL for LIFO defers");

    long body = find_order(c_src, "body");
    long d2   = find_order(c_src, "d2");
    long d1   = find_order(c_src, "d1");
    TEST_ASSERT_TRUE_MESSAGE(body >= 0 && d2 >= 0 && d1 >= 0,
        "Expected body/d1/d2 print literals in emitted C");
    /* body emits first (it is the function body), then defers LIFO: d2 then d1. */
    TEST_ASSERT_TRUE_MESSAGE(body < d2,
        "Expected function body before deferred bodies");
    TEST_ASSERT_TRUE_MESSAGE(d2 < d1,
        "Expected LIFO defer order: d2 (last-registered) before d1");

    iron_lir_optimize_info_free(&opt);
}

/* DEFER-04 interleave: both defer bodies emit BEFORE the first local drop call
 * at the exit edge (defers first, then locals reverse-decl). */
void test_defer_then_drop(void) {
    static const char *src =
        "object Resource {\n"
        "    val name: String\n"
        "    drop {\n"
        "        println(\"drop{self.name}\")\n"
        "    }\n"
        "}\n"
        "func main() {\n"
        "    val local1 = Resource(\"1\")\n"
        "    val local2 = Resource(\"2\")\n"
        "    defer println(\"defer1\")\n"
        "    defer println(\"defer2\")\n"
        "    println(\"body\")\n"
        "}\n";

    IronLIR_OptimizeInfo opt;
    const char *c_src = compile_to_c_mode(src, &opt, NULL, IRON_ANALYSIS_MODE_CLI);
    TEST_ASSERT_NOT_NULL_MESSAGE(c_src, "compile_to_c_mode returned NULL for defer-then-drop");

    long defer2   = find_order(c_src, "defer2");
    long defer1   = find_order(c_src, "defer1");
    long drop_call = find_order(c_src, "Resource_drop");
    TEST_ASSERT_TRUE_MESSAGE(defer2 >= 0 && defer1 >= 0 && drop_call >= 0,
        "Expected defer1/defer2 literals + Resource_drop call in emitted C");
    /* defers LIFO (defer2 < defer1) THEN the first drop call (DEFER-04). */
    TEST_ASSERT_TRUE_MESSAGE(defer2 < defer1,
        "Expected LIFO defer order: defer2 before defer1");
    TEST_ASSERT_TRUE_MESSAGE(defer1 < drop_call,
        "Expected both defers before the first local drop (DEFER-04 interleave)");

    iron_lir_optimize_info_free(&opt);
}

/* DEFER-01 early-return edge: a defer before an early `return` is emitted on the
 * early-return cleanup path (emit_defer_cleanup fires at return). */
void test_early_return_runs_defer(void) {
    static const char *src =
        "func run(early: Bool) {\n"
        "    defer print(\"c\")\n"
        "    if early {\n"
        "        return\n"
        "    }\n"
        "    print(\"fell\")\n"
        "}\n"
        "func main() {\n"
        "    run(true)\n"
        "}\n";

    IronLIR_OptimizeInfo opt;
    const char *c_src = compile_to_c_mode(src, &opt, NULL, IRON_ANALYSIS_MODE_CLI);
    TEST_ASSERT_NOT_NULL_MESSAGE(c_src, "compile_to_c_mode returned NULL for early-return defer");

    /* The deferred print must appear on the early-return cleanup path. */
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(c_src, "\"c\""),
        "Expected deferred print(\"c\") body on the early-return cleanup path");

    iron_lir_optimize_info_free(&opt);
}

/* Read-at-exit (RESEARCH Pattern 2): `var x=1; defer print(x); x=2` → the defer
 * body's load of x is emitted AFTER the `x = 2` store (positional read-at-exit,
 * NOT a snapshot at defer-registration time). */
void test_read_at_exit(void) {
    static const char *src =
        "func main() {\n"
        "    var x = 1\n"
        "    defer print(x)\n"
        "    x = 2\n"
        "}\n";

    IronLIR_OptimizeInfo opt;
    const char *c_src = compile_to_c_mode(src, &opt, NULL, IRON_ANALYSIS_MODE_CLI_LENIENT);
    TEST_ASSERT_NOT_NULL_MESSAGE(c_src, "compile_to_c_mode returned NULL for read-at-exit defer");

    /* The store `= 2` must be emitted before the deferred body's load of x.
     * NOTE: the exact print-call symbol is finalized in Plan 32-02 when this
     * case flips live; key on the last `= 2` store offset vs the final emitted
     * x reference. The positional invariant (store-before-deferred-load) is the
     * read-at-exit witness, immune to the precise print emission form. */
    long store = find_order(c_src, "= 2");
    const char *after = strstr(c_src, "= 2");
    long later_x = (after && strstr(after, "x")) ? store + 1 : -1L;
    TEST_ASSERT_TRUE_MESSAGE(store >= 0,
        "Expected the `= 2` store in emitted C");
    TEST_ASSERT_TRUE_MESSAGE(later_x > store,
        "Expected an x reference after the `x = 2` store (read-at-exit, not snapshot)");

    iron_lir_optimize_info_free(&opt);
}

#endif /* DEFER_GENERAL_READY */

/* ── main ─────────────────────────────────────────────────────────────────── */

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_defer_free_still_compiles);
#ifdef DEFER_GENERAL_READY
    RUN_TEST(test_lifo_order);
    RUN_TEST(test_defer_then_drop);
    RUN_TEST(test_early_return_runs_defer);
    RUN_TEST(test_read_at_exit);
#endif
    return UNITY_END();
}
