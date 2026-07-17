/* Phase 32 DEFER-01: `defer` accepts ANY statement — code 276
 * (IRON_ERR_DEFER_FORM_UNSUPPORTED) is RETIRED (see diagnostics.h) and must
 * never be emitted again, for any defer form.
 *
 * History: Phase 21 (DEFER-02) restricted defer to `defer free <ident>` and
 * this file asserted E0276 fired for every other form. Phase 32 generalized
 * defer (parser.c IRON_TOK_DEFER arm parses a full statement or block) but
 * this test was left asserting the OLD contract, so it went permanently RED.
 * It now locks the Phase 32 contract from both directions:
 *   1. every previously-rejected form parses + lowers with ZERO E0276, and
 *   2. the legal Phase 21 form (`defer free <ident>`) still emits ZERO E0276.
 * Other diagnostics (e.g. free-target validation) are intentionally NOT
 * asserted here — this file only guards the retirement of code 276.
 *
 * Pipeline pattern unchanged: iron_analyze_buffer -> iron_hir_lower, then
 * inspect the combined diag list. */

#include "unity.h"
#include "analyzer/analyzer.h"
#include "hir/hir_lower.h"
#include "hir/hir.h"
#include "diagnostics/diagnostics.h"
#include "util/arena.h"
#include "stb_ds.h"

#include <string.h>

static Iron_Arena    arena;
static Iron_DiagList diags;

void setUp(void) {
    arena = iron_arena_create(262144);
    diags = iron_diaglist_create();
}

void tearDown(void) {
    iron_diaglist_free(&diags);
    iron_arena_free(&arena);
}

/* Run full pipeline: parse + analyze + hir_lower.
 * Accumulates diagnostics from both phases into diags. */
static void run_pipeline(const char *src) {
    Iron_AnalyzeResult r = iron_analyze_buffer(
        src, strlen(src), "test.iron",
        IRON_ANALYSIS_MODE_CLI,
        &arena, &diags, NULL,
        0);
    if (!r.program || !r.global_scope) return;
    IronHIR_Module *mod = iron_hir_lower(r.program, r.global_scope,
                                          NULL, &diags);
    if (mod) iron_hir_module_destroy(mod);
}

static int count_with_code(int target) {
    int n = 0;
    for (int i = 0; i < arrlen(diags.items); i++) {
        if (diags.items[i].code == target) n++;
    }
    return n;
}

/* DEFER-01: `defer println("x")` — function-call statement is legal. */
void test_defer_function_call_accepted(void) {
    run_pipeline(
        "func main() {\n"
        "    defer println(\"cleanup\")\n"
        "}\n");
    TEST_ASSERT_EQUAL_INT(0,
        count_with_code(IRON_ERR_DEFER_FORM_UNSUPPORTED));
}

/* DEFER-01: `defer compute()` — generic call statement is legal. */
void test_defer_compute_call_accepted(void) {
    run_pipeline(
        "func compute() {}\n"
        "func main() {\n"
        "    defer compute()\n"
        "}\n");
    TEST_ASSERT_EQUAL_INT(0,
        count_with_code(IRON_ERR_DEFER_FORM_UNSUPPORTED));
}

/* DEFER-01: `defer free p.field` — free of a field target never emits the
 * retired 276 (free-target validity is a separate check, not asserted). */
void test_defer_free_field_no_retired_code(void) {
    run_pipeline(
        "object Container { var inner: Int }\n"
        "func main() {\n"
        "    val c = heap Container(0)\n"
        "    defer free c.inner\n"
        "}\n");
    TEST_ASSERT_EQUAL_INT(0,
        count_with_code(IRON_ERR_DEFER_FORM_UNSUPPORTED));
}

/* DEFER-01: `defer free arr[i]` — index target never emits the retired 276. */
void test_defer_free_index_no_retired_code(void) {
    run_pipeline(
        "func main() {\n"
        "    val arr = [heap 42]\n"
        "    val i = 0\n"
        "    defer free arr[i]\n"
        "}\n");
    TEST_ASSERT_EQUAL_INT(0,
        count_with_code(IRON_ERR_DEFER_FORM_UNSUPPORTED));
}

/* Legal since Phase 21: `defer free p` with a bare identifier — zero E0276. */
void test_defer_free_ident_legal(void) {
    run_pipeline(
        "object Point { var x: Int; var y: Int\n"
        "    init() { self.x = 0; self.y = 0 }\n"
        "}\n"
        "func main() {\n"
        "    val p = heap Point()\n"
        "    defer free p\n"
        "}\n");
    TEST_ASSERT_EQUAL_INT(0,
        count_with_code(IRON_ERR_DEFER_FORM_UNSUPPORTED));
}

/* DEFER-01: `defer x = 1` — assignment statement is legal. */
void test_defer_assignment_accepted(void) {
    run_pipeline(
        "func main() {\n"
        "    var x = 0\n"
        "    defer x = 1\n"
        "}\n");
    TEST_ASSERT_EQUAL_INT(0,
        count_with_code(IRON_ERR_DEFER_FORM_UNSUPPORTED));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_defer_function_call_accepted);
    RUN_TEST(test_defer_compute_call_accepted);
    RUN_TEST(test_defer_free_field_no_retired_code);
    RUN_TEST(test_defer_free_index_no_retired_code);
    RUN_TEST(test_defer_free_ident_legal);
    RUN_TEST(test_defer_assignment_accepted);
    return UNITY_END();
}
