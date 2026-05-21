/* Phase 27 OQ-04 (Plan 27-03): closure capture weak rc retain/release balance.
 *
 * Mirror of test_oq03_closure_rc_retain_release.c with weak-rc opcodes
 * substituted for the strong-rc opcodes. Verifies the Plan 27-03
 * closure-capture wiring:
 *
 *   1. Compiling a program that closes over a `weak rc T` value produces
 *      generated C containing AT LEAST ONE iron_weak_rc_retain call
 *      (construct-time retain at IRON_HIR_EXPR_CLOSURE arm — Major #5
 *      hardening: catches the "0 retains slipped through" false-positive
 *      where count==0 trivially equals count==0).
 *
 *   2. The number of iron_weak_rc_release emissions equals the number of
 *      iron_weak_rc_retain emissions PLUS the number of iron_rc_downgrade
 *      emissions across the whole compilation unit (1:1 balance: every
 *      explicit retain pairs with a release, and each downgrade contributes
 *      +1 to weak_count with no paired retain — the asymmetric ground-truth
 *      analog of OQ-03's iron_rc_alloc).
 *
 *   3. The Phase 27 OQ-04 banner appears in the generated C (env-drop
 *      companion comment, shared with Phase 26 OQ-03 banner extension).
 *
 * Source code path proved by tests:
 *   - hir_to_lir.c IRON_HIR_EXPR_CLOSURE arm emits IRON_LIR_WEAK_RC_RETAIN
 *     per weak-rc-typed val capture BEFORE iron_lir_make_closure.
 *   - emit_c.c IRON_LIR_MAKE_CLOSURE arm emits <func_name>_env_drop
 *     companion that calls iron_weak_rc_release on each weak-rc-typed
 *     env field.
 *
 * Note on counting: iron_weak_rc_retain / iron_weak_rc_release /
 * iron_rc_downgrade are runtime symbols in src/runtime/iron_rc.c which IS
 * prepended into generated binaries by ironc build. Since the test uses
 * iron_lir_emit_c() directly (no link), the only sources of these
 * substrings are the codegen-side emissions, making count(substring) a
 * robust count of emit-site invocations.
 *
 * Pattern source: tests/unit/test_oq03_closure_rc_retain_release.c. */

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

static void dump_diags_if_any(void) {
    for (int i = 0; i < g_diags.count; i++) {
        Iron_Diagnostic *d = &g_diags.items[i];
        fprintf(stderr, "[diag %d] code=%d level=%d msg=%s\n",
                i, d->code, (int)d->level,
                d->message ? d->message : "(null)");
    }
}

static const char *compile_to_c(const char *src, IronLIR_OptimizeInfo *opt_out) {
    Iron_AnalyzeResult res = iron_analyze_buffer(
        src, strlen(src), "test_oq04.iron",
        IRON_ANALYSIS_MODE_CLI_LENIENT,
        &g_arena, &g_diags, NULL, 0);

    if (res.has_errors || g_diags.error_count > 0) {
        dump_diags_if_any();
        return NULL;
    }

    IronHIR_Module *hir = iron_hir_lower(res.program, res.global_scope,
                                          NULL, &g_diags);
    if (!hir || g_diags.error_count > 0) {
        dump_diags_if_any();
        return NULL;
    }

    IronLIR_Module *lir = iron_hir_to_lir(hir, res.program, res.global_scope,
                                            &g_lir_arena, &g_diags);
    if (!lir || g_diags.error_count > 0) {
        dump_diags_if_any();
        iron_hir_module_destroy(hir);
        return NULL;
    }

    iron_lir_optimize(lir, opt_out, &g_out_arena, false, true, false);

    const char *c_src = iron_lir_emit_c(lir, &g_out_arena, &g_diags,
                                         opt_out, NULL, false, false);
    iron_hir_module_destroy(hir);
    return c_src;
}

/* Count non-overlapping occurrences of `needle` in `haystack`. */
static int count_occurrences(const char *haystack, const char *needle) {
    if (!haystack || !needle || !*needle) return 0;
    int count = 0;
    size_t nlen = strlen(needle);
    const char *p = haystack;
    while ((p = strstr(p, needle)) != NULL) {
        count++;
        p += nlen;
    }
    return count;
}

/* Closure captures a weak rc Counter value, obtained via .downgrade() on
 * a strong rc Counter.
 *
 * Per-call accounting:
 *   - rc Counter allocation: 1 iron_rc_alloc + ground-truth refcount=1
 *   - downgrade(): 1 iron_rc_downgrade (bumps weak_count from 0 -> 1
 *     without a paired retain — the asymmetric weak-side ground truth)
 *   - closure construct: 1 explicit IRON_LIR_WEAK_RC_RETAIN at
 *     IRON_HIR_EXPR_CLOSURE arm (this plan)
 *   - env_drop companion: 1 iron_weak_rc_release per weak-rc env field
 *   - scope exit on outer weak binding: 1 IRON_LIR_WEAK_RC_RELEASE
 *
 * Construction-time retain (closure capture) is the load-bearing assertion:
 *   count(iron_weak_rc_retain) >= 1
 *   count(iron_weak_rc_release) == count(iron_weak_rc_retain) + count(iron_rc_downgrade)
 *
 * The minimum-count assertion catches the false-positive case where 0==0
 * trivially satisfies the balance equation (Major #5 hardening, inherited
 * from Phase 26 Plan 03 SUMMARY Deviation #1). */
static const char *kSrc =
    "object Counter {\n"
    "    val tag: Int\n"
    "}\n"
    "func consume(w: weak rc Counter) -> Int { return 0 }\n"
    "func main() -> Int {\n"
    "    val original = rc Counter(99)\n"
    "    val observer = original.downgrade()\n"
    "    val show = func() -> Int { return consume(observer) }\n"
    "    return show()\n"
    "}\n";

/* ── Case 1: count(weak_rc_retain) >= 1 ─────────────────────────────── */
void test_oq04_weak_rc_retain_count_at_least_one(void) {
    IronLIR_OptimizeInfo opt;
    const char *c_src = compile_to_c(kSrc, &opt);
    TEST_ASSERT_NOT_NULL_MESSAGE(c_src,
        "compile_to_c returned NULL -- check diagnostics");

    int retain_count = count_occurrences(c_src, "iron_weak_rc_retain(");
    /* Major #5 hardening (inherited from Phase 26): minimum-count assertion
     * catches the "0 retains slipped through" false-positive case where
     * 0 == 0 would be trivially-true under just the equality check. */
    TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(1, retain_count,
        "Expected count(iron_weak_rc_retain) >= 1 (closure construct retain)");

    iron_lir_optimize_info_free(&opt);
}

/* ── Case 2: count(weak_release) == count(weak_retain) + count(downgrade) ─ */
/*
 * Counting model (OQ-04 1:1 invariant — mirror of OQ-03 with downgrade
 * replacing rc_alloc as the asymmetric source for weak_count):
 *
 *   N_downgrade   = count of iron_rc_downgrade( emits (each contributes
 *                   weak_count += 1 WITHOUT an explicit
 *                   iron_weak_rc_retain emit -- downgrade is the
 *                   ground-truth +1 on the weak side)
 *   N_retain      = count of iron_weak_rc_retain( emits (explicit weak_count
 *                   bumps: closure capture, store alias of weak binding,
 *                   call-arg copy of weak rc)
 *   N_release     = count of iron_weak_rc_release( emits (every scope-exit
 *                   release on an IRON_TYPE_WEAK_RC binding, plus env_drop
 *                   releases per captured weak-rc field)
 *
 *   Invariant: N_release == N_retain + N_downgrade
 *
 * Plan 27-03 Major #5 hardening assertion: the closure-attributable
 * weak retains balance with closure-attributable weak releases. We count
 * iron_rc_downgrade( as N_downgrade to subtract from N_release. */
void test_oq04_weak_rc_balance_invariant(void) {
    IronLIR_OptimizeInfo opt;
    const char *c_src = compile_to_c(kSrc, &opt);
    TEST_ASSERT_NOT_NULL_MESSAGE(c_src,
        "compile_to_c returned NULL -- check diagnostics");

    int retain_count    = count_occurrences(c_src, "iron_weak_rc_retain(");
    int release_count   = count_occurrences(c_src, "iron_weak_rc_release(");
    int downgrade_count = count_occurrences(c_src, "iron_rc_downgrade(");

    /* count(weak_release) == count(weak_retain) + count(downgrade) — every
     * weak reference dropped, no leaks, no double-frees. Each downgrade
     * contributes one ground-truth +1 to weak_count which must be
     * decremented exactly once (the scope-exit release of the originally
     * downgraded weak binding). Every explicit weak retain emit pairs with
     * one weak release emit. */
    char msg[320];
    snprintf(msg, sizeof(msg),
        "count(iron_weak_rc_release)=%d != count(iron_weak_rc_retain)=%d + "
        "count(iron_rc_downgrade)=%d "
        "(OQ-04 1:1 closure balance violated; expected releases=%d)",
        release_count, retain_count, downgrade_count,
        retain_count + downgrade_count);
    TEST_ASSERT_EQUAL_INT_MESSAGE(retain_count + downgrade_count,
                                  release_count, msg);

    iron_lir_optimize_info_free(&opt);
}

/* ── Case 3: env_drop companion + Phase 27 banner present ───────────── */
void test_oq04_env_drop_companion_emitted(void) {
    IronLIR_OptimizeInfo opt;
    const char *c_src = compile_to_c(kSrc, &opt);
    TEST_ASSERT_NOT_NULL_MESSAGE(c_src,
        "compile_to_c returned NULL -- check diagnostics");

    /* Plan 27-03 banner present in env-drop comment (extends Phase 26
     * banner to mention Phase 27 OQ-04). */
    TEST_ASSERT_NOT_NULL_MESSAGE(
        strstr(c_src, "Phase 27 OQ-04"),
        "env-drop companion must carry Phase 27 OQ-04 banner");

    /* Companion function symbol present */
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(c_src, "_env_drop(void *env_void)"),
        "<func_name>_env_drop(void *env_void) companion must be synthesized");

    /* env-drop body releases at least one weak-rc capture field */
    TEST_ASSERT_NOT_NULL_MESSAGE(
        strstr(c_src, "iron_weak_rc_release((void *)_env->"),
        "env-drop body must call iron_weak_rc_release on weak-rc env fields");

    iron_lir_optimize_info_free(&opt);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_oq04_weak_rc_retain_count_at_least_one);
    RUN_TEST(test_oq04_weak_rc_balance_invariant);
    RUN_TEST(test_oq04_env_drop_companion_emitted);
    return UNITY_END();
}
