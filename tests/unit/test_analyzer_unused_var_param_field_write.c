/* Phase 18 Wave 0: VAL-06 false-positive regression test (Pitfall 3 lock).
 *
 * Phase 17 introduced IRON_WARN_UNUSED_VAR_PARAM=614 with mutation-only
 * tracking (binding reassignment via IRON_NODE_ASSIGN with IDENT target).
 * That definition false-positives on var parameters that ONLY do field
 * writes (e.g., `func f(var p: Point) { p.x = 5 }`) — the param IS being
 * used for mutation, but the writes target FIELD_ACCESS not IDENT.
 *
 * Plan 18-01 extends src/analyzer/unused_var.c scan_for_writes
 * IRON_NODE_ASSIGN branch to also mark FIELD_ACCESS-rooted IDENTs as
 * "written". This file pins that behaviour:
 *
 *   - test_var_param_field_write_does_not_warn: `func f(var p: Point) { p.x = 5 }`
 *     produces zero IRON_WARN_UNUSED_VAR_PARAM
 *   - test_var_param_truly_unused_still_warns: `func f(var p: Int) { }` still
 *     warns (regression boundary — the fix must not eliminate the warning)
 *
 * IMPORTANT: VAL-05 (val locals) is INTENTIONALLY NOT extended. Field-write
 * does not "use" a val local for VAL-05 purposes (binding never reassigned).
 * Only var params (PARM-02 mental model: "param exists for mutation") gain
 * the field-write-counts-as-use semantics. */
#include "unity.h"
#include "analyzer/analyzer.h"
#include "diagnostics/diagnostics.h"
#include "util/arena.h"
#include "stb_ds.h"

#include <stdatomic.h>
#include <string.h>

static Iron_Arena    arena;
static Iron_DiagList diags;

void setUp(void) {
    arena = iron_arena_create(131072);
    diags = iron_diaglist_create();
}

void tearDown(void) {
    iron_diaglist_free(&diags);
    iron_arena_free(&arena);
}

static void analyze_src(const char *src) {
    Iron_AnalyzeResult r = iron_analyze_buffer(
        src, strlen(src), "test.iron",
        IRON_ANALYSIS_MODE_CLI,
        &arena, &diags, NULL,
        0);
    (void)r;
}

static int count_with_code(int target) {
    int n = 0;
    for (int i = 0; i < arrlen(diags.items); i++) {
        if (diags.items[i].code == target) n++;
    }
    return n;
}

/* Pitfall 3 lock: var param doing only field-writes is USED for mutation;
 * IRON_WARN_UNUSED_VAR_PARAM must NOT fire. */
void test_var_param_field_write_does_not_warn(void) {
    analyze_src(
        "object Pt {\n"
        "    var x: Int\n"
        "    init(v: Int) { self.x = v }\n"
        "}\n"
        "func f(var p: Pt) {\n"
        "    p.x = 5\n"
        "}\n"
        "func main() {\n"
        "    f(Pt(0))\n"
        "}\n");
    TEST_ASSERT_EQUAL_INT(0, count_with_code(IRON_WARN_UNUSED_VAR_PARAM));
}

/* Regression boundary: the fix must not eliminate the warning entirely.
 * A truly-unused var param (no field writes, no rebinds) still warns. */
void test_var_param_truly_unused_still_warns(void) {
    analyze_src(
        "func f(var p: Int) {\n"
        "}\n"
        "func main() {\n"
        "    f(0)\n"
        "}\n");
    TEST_ASSERT_GREATER_THAN_INT(0, count_with_code(IRON_WARN_UNUSED_VAR_PARAM));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_var_param_field_write_does_not_warn);
    RUN_TEST(test_var_param_truly_unused_still_warns);
    return UNITY_END();
}
