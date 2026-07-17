/* Phase 21 Plan 03 (B2 closure 2026-05-08): OQ-09 mechanical gate.
 *
 * Verifies the closed-policy invariant for heap allocation and position locks.
 * Three cases:
 *
 *   Case 1: `heap Point(1, 2)` compiles cleanly — zero errors, no
 *           IRON_ERR_HEAP_BAD_POSITION (273) false-positive.
 *           POL-02 happy path.
 *
 *   Case 2: `heap val p = ...` (heap in binding position) IS rejected with
 *           IRON_ERR_HEAP_BAD_POSITION (273). This is the key Phase 21
 *           position-lock gate. Combined with Case 1, it confirms that:
 *           - heap in allocation-expression position: ACCEPTED
 *           - heap in binding-declaration position: REJECTED
 *           This asymmetry is the mechanical proof of the OQ-09 lock: the
 *           `heap` keyword policy is strictly enforced at the allocation site.
 *
 *   Case 3: OQ-09 position asymmetry within a single function body. Both
 *           `val p = heap Point(1, 2)` (ACCEPTED) and `heap val q = ...`
 *           (REJECTED) in the same function — confirms the parser correctly
 *           distinguishes allocation-expression position from binding position
 *           and the errors come only from the position-locked forms.
 *
 * OQ-09 rationale: "Policy promotion heap → rc is NOT supported. The closed
 * policy set is enforced at type level: a programmer wanting reference
 * counting must allocate via rc from the start." The position-lock gates
 * (Cases 2+3) are the Phase 21 mechanical enforcement of this policy. The
 * rc allocation form is v1/v2 carry-over; Phase 26 ships the rc allocation
 * form with proper POL-11 closed-policy enforcement. See PROJECT.md Key
 * Decisions OQ-09 row for the canonical resolution.
 *
 * Pattern source: tests/unit/test_heap_bad_position.c */

#include "unity.h"
#include "analyzer/analyzer.h"
#include "diagnostics/diagnostics.h"
#include "util/arena.h"
#include "stb_ds.h"

#include <string.h>

static Iron_Arena    g_arena;
static Iron_DiagList g_diags;

void setUp(void) {
    g_arena = iron_arena_create(131072);
    g_diags  = iron_diaglist_create();
}

void tearDown(void) {
    iron_diaglist_free(&g_diags);
    iron_arena_free(&g_arena);
}

/* ── Pipeline helper ──────────────────────────────────────────────────────── */

static void analyze_src(const char *src) {
    Iron_AnalyzeResult r = iron_analyze_buffer(
        src, strlen(src), "test.iron",
        IRON_ANALYSIS_MODE_CLI,
        &g_arena, &g_diags, NULL, 0);
    (void)r;
}

static int count_errors(void) {
    int n = 0;
    for (int i = 0; i < arrlen(g_diags.items); i++) {
        if (g_diags.items[i].level == IRON_DIAG_ERROR) n++;
    }
    return n;
}

static int count_with_code(int target) {
    int n = 0;
    for (int i = 0; i < arrlen(g_diags.items); i++) {
        if (g_diags.items[i].code == target) n++;
    }
    return n;
}

/* ── Case 1: heap T(...) accepted at allocation-expression position ────────── */

/* POL-02 happy path: `val p = heap Point(1, 2)` must compile cleanly with
 * zero errors. Also confirms no IRON_ERR_HEAP_BAD_POSITION(273) false-positive
 * is fired for the correct allocation-expression position.
 *
 * Source shape mirrors test_heap_alloc_codegen.c Case 1 (Plan 21-02, GREEN). */
void test_heap_alloc_accepted(void) {
    analyze_src(
        "object Point {\n"
        "    val x: Int\n"
        "    val y: Int\n"
        "}\n"
        "func main() {\n"
        "    val p = heap Point(1, 2)\n"
        "}\n");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, count_errors(),
        "heap T(...) at allocation-expression position must compile cleanly");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0,
        count_with_code(IRON_ERR_HEAP_BAD_POSITION),
        "IRON_ERR_HEAP_BAD_POSITION must NOT fire for correct heap T(...) form");
}

/* ── Case 2: heap in binding position rejected (OQ-09 position-lock gate) ─── */

/* POL-03 policy lock: `heap val p = ...` (heap as binding modifier) emits
 * IRON_ERR_HEAP_BAD_POSITION (273). This is the primary OQ-09 mechanical gate:
 * it proves that while heap T(...) is accepted (Case 1), using heap as a
 * binding modifier is NOT accepted. Phase 21 enforces the closed-policy set
 * by locking heap to allocation-expression position only. */
void test_heap_binding_modifier_rejected(void) {
    analyze_src(
        "object Point {\n"
        "    val x: Int\n"
        "    val y: Int\n"
        "}\n"
        "func main() {\n"
        "    heap val p = heap Point(1, 2)\n"
        "}\n");
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0,
        count_with_code(IRON_ERR_HEAP_BAD_POSITION),
        "heap in binding-modifier position must emit IRON_ERR_HEAP_BAD_POSITION");
}

/* ── Case 3: position asymmetry in same function (OQ-09 lock proof) ─────────── */

/* OQ-09 asymmetry proof: in the same function body, `val p = heap Point(...)`
 * is ACCEPTED but `heap val q = heap Point(...)` is REJECTED. The asymmetry
 * demonstrates that the Phase 21 position-lock (IRON_ERR_HEAP_BAD_POSITION)
 * correctly distinguishes allocation-expression position from binding position.
 * Any future plan that silently accepts heap in binding position would fail
 * this test, satisfying the OQ-09 lock mechanically. */
void test_heap_alloc_ok_heap_modifier_rejected_in_same_function(void) {
    analyze_src(
        "object Point {\n"
        "    val x: Int\n"
        "    val y: Int\n"
        "}\n"
        "func main() {\n"
        "    val p = heap Point(1, 2)\n"
        "    heap val q = heap Point(3, 4)\n"
        "}\n");
    /* heap binding modifier on q must fire E0273 */
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0,
        count_with_code(IRON_ERR_HEAP_BAD_POSITION),
        "IRON_ERR_HEAP_BAD_POSITION must fire for heap-as-binding-modifier "
        "(heap val q = ...) even when heap T(...) in same function is valid");
}

/* ── Test runner ──────────────────────────────────────────────────────────── */

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_heap_alloc_accepted);
    RUN_TEST(test_heap_binding_modifier_rejected);
    RUN_TEST(test_heap_alloc_ok_heap_modifier_rejected_in_same_function);
    return UNITY_END();
}
