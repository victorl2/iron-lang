/* Phase 21 Wave 0 (Plan 01): TDD scaffold for DEFER-02 — only
 * `defer free <ident>` is supported in v3.0-alpha.1; all other defer forms
 * emit IRON_ERR_DEFER_FORM_UNSUPPORTED=276.
 *
 * The structural check fires in hir_lower.c (IRON_NODE_DEFER arm) — AFTER
 * the parse+analyze pipeline — so each test runs:
 *   iron_analyze_buffer  -> Iron_AnalyzeResult (with program + global_scope)
 *   iron_hir_lower       -> IronHIR_Module (or NULL on error)
 * and then inspects the combined diag list for E0276.
 *
 * Authored RED first; flips GREEN once Plan 21-01 Task 4 inserts the
 * structural check at the TOP of the IRON_NODE_DEFER arm in hir_lower.c.
 *
 * Pattern source: tests/unit/test_analyzer_parm_var_slot.c +
 *                 tests/hir/test_hir_lower.c (hir_lower invocation) */

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
    /* Run hir_lower — this is where IRON_ERR_DEFER_FORM_UNSUPPORTED fires */
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

static bool msg_contains(int target, const char *needle) {
    for (int i = 0; i < arrlen(diags.items); i++) {
        if (diags.items[i].code == target) {
            if (diags.items[i].message && strstr(diags.items[i].message, needle))
                return true;
            if (diags.items[i].suggestion && strstr(diags.items[i].suggestion, needle))
                return true;
        }
    }
    return false;
}

/* DEFER-02: `defer println("x")` — function call is not `free <ident>`. */
void test_defer_function_call_rejected(void) {
    run_pipeline(
        "func main() {\n"
        "    defer println(\"cleanup\")\n"
        "}\n");
    TEST_ASSERT_GREATER_THAN_INT(0,
        count_with_code(IRON_ERR_DEFER_FORM_UNSUPPORTED));
    TEST_ASSERT_TRUE(msg_contains(IRON_ERR_DEFER_FORM_UNSUPPORTED,
        "only `defer free <binding>` is supported"));
    TEST_ASSERT_TRUE(msg_contains(IRON_ERR_DEFER_FORM_UNSUPPORTED,
        "Phase 32"));
}

/* DEFER-02: `defer compute()` — generic function call rejected. */
void test_defer_compute_call_rejected(void) {
    run_pipeline(
        "func compute() {}\n"
        "func main() {\n"
        "    defer compute()\n"
        "}\n");
    TEST_ASSERT_GREATER_THAN_INT(0,
        count_with_code(IRON_ERR_DEFER_FORM_UNSUPPORTED));
}

/* DEFER-02: `defer free p.field` — inner free target is field access, not ident. */
void test_defer_free_field_rejected(void) {
    run_pipeline(
        "object Container { var inner: Int }\n"
        "func main() {\n"
        "    val c = heap Container(0)\n"
        "    defer free c.inner\n"
        "}\n");
    TEST_ASSERT_GREATER_THAN_INT(0,
        count_with_code(IRON_ERR_DEFER_FORM_UNSUPPORTED));
}

/* DEFER-02: `defer free arr[i]` — inner free target is index expr, not ident. */
void test_defer_free_index_rejected(void) {
    run_pipeline(
        "func main() {\n"
        "    val arr = [heap 42]\n"
        "    val i = 0\n"
        "    defer free arr[i]\n"
        "}\n");
    TEST_ASSERT_GREATER_THAN_INT(0,
        count_with_code(IRON_ERR_DEFER_FORM_UNSUPPORTED));
}

/* Legal form: `defer free p` where p is a bare identifier — must emit ZERO E0276.
 * The defer lowers through the existing IRON_HIR_STMT_DEFER path. */
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

/* DEFER-02: `defer x = 1` — assignment in defer body rejected. */
void test_defer_assignment_rejected(void) {
    run_pipeline(
        "func main() {\n"
        "    var x = 0\n"
        "    defer x = 1\n"
        "}\n");
    TEST_ASSERT_GREATER_THAN_INT(0,
        count_with_code(IRON_ERR_DEFER_FORM_UNSUPPORTED));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_defer_function_call_rejected);
    RUN_TEST(test_defer_compute_call_rejected);
    RUN_TEST(test_defer_free_field_rejected);
    RUN_TEST(test_defer_free_index_rejected);
    RUN_TEST(test_defer_free_ident_legal);
    RUN_TEST(test_defer_assignment_rejected);
    return UNITY_END();
}
