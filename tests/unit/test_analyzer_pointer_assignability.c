/* Phase 20 Wave 0 (Plan 20-02a): TDD scaffold for PTR-12 — full-pipeline
 * re-validation of pointer assignability rules locked in Plan 20-01.
 *
 * Plan 20-01 established types_assignable rules at the unit-of-types level
 * (test_analyzer_ptr_distinct_types.c). Plan 20-02a re-verifies the same
 * rules surface correctly through the full lex→parse→analyze pipeline,
 * including arrays of pointers and the analyzer's `&`-resolves-to-*T path.
 *
 * GREEN by end of 20-02a Task 2 (depends on `&` resolving to *T which is
 * the IRON_NODE_UNARY-AMP analyzer-side change).
 */
#include "unity.h"
#include "analyzer/analyzer.h"
#include "diagnostics/diagnostics.h"
#include "util/arena.h"
#include "stb_ds.h"

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

static int total_errors(void) {
    int n = 0;
    for (int i = 0; i < arrlen(diags.items); i++) {
        if (diags.items[i].level == IRON_DIAG_ERROR) n++;
    }
    return n;
}

/* Case 1: `val p: *Point = &x` typechecks (basic shape). */
void test_full_pipeline_address_of_local_to_ptr(void) {
    analyze_src(
        "object Point {\n"
        "    var x: Int\n"
        "    init() { self.x = 0 }\n"
        "}\n"
        "func main() {\n"
        "    val pt = Point()\n"
        "    val p: *Point = &pt\n"
        "}\n");
    TEST_ASSERT_EQUAL_INT(0, total_errors());
}

/* Case 2: `val p: *Point = &y` where y is a different type emits an
 * error (cross-pointee rejection — PTR-12). */
void test_full_pipeline_cross_pointee_rejected(void) {
    analyze_src(
        "object Point {\n"
        "    var x: Int\n"
        "    init() { self.x = 0 }\n"
        "}\n"
        "object Player {\n"
        "    var hp: Int\n"
        "    init() { self.hp = 0 }\n"
        "}\n"
        "func main() {\n"
        "    val pl = Player()\n"
        "    val p: *Point = &pl\n"
        "}\n");
    TEST_ASSERT_GREATER_THAN_INT(0, total_errors());
}

/* Case 3: `val p: *Point = q` where q: *var Point typechecks (PTR-12
 * covariance: *var T -> *T). */
void test_full_pipeline_var_to_nonvar_covariance(void) {
    analyze_src(
        "object Point {\n"
        "    var x: Int\n"
        "    init() { self.x = 0 }\n"
        "}\n"
        "func reads(q: *var Point) {\n"
        "    val p: *Point = q\n"
        "}\n"
        "func main() {}\n");
    TEST_ASSERT_EQUAL_INT(0, total_errors());
}

/* Case 4: `val p: *var Point = q` where q: *Point emits an error
 * (PTR-12 invariance: *T -> *var T rejected). */
void test_full_pipeline_nonvar_to_var_rejected(void) {
    analyze_src(
        "object Point {\n"
        "    var x: Int\n"
        "    init() { self.x = 0 }\n"
        "}\n"
        "func reads(q: *Point) {\n"
        "    val p: *var Point = q\n"
        "}\n"
        "func main() {}\n");
    TEST_ASSERT_GREATER_THAN_INT(0, total_errors());
}

/* Case 5: Auto-address composition with PTR-12 — `func g(p: *Point)`
 * accepts an expression `&pt` where `pt: *var Point` would not be valid
 * (cross-pointee), but `pt: Point` IS valid via auto-address. This
 * verifies that the analyzer's `&`-resolves-to-*T path composes with the
 * existing types_assignable rules. (Array-of-pointers element-type
 * `[*Point]` syntax is currently rejected by the parser; that's tracked
 * outside this plan.) */
void test_full_pipeline_amp_to_param_ok(void) {
    analyze_src(
        "object Point {\n"
        "    var x: Int\n"
        "    init() { self.x = 0 }\n"
        "}\n"
        "func sink(q: *Point) {}\n"
        "func main() {\n"
        "    val pt = Point()\n"
        "    sink(&pt)\n"
        "}\n");
    TEST_ASSERT_EQUAL_INT(0, total_errors());
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_full_pipeline_address_of_local_to_ptr);
    RUN_TEST(test_full_pipeline_cross_pointee_rejected);
    RUN_TEST(test_full_pipeline_var_to_nonvar_covariance);
    RUN_TEST(test_full_pipeline_nonvar_to_var_rejected);
    RUN_TEST(test_full_pipeline_amp_to_param_ok);
    return UNITY_END();
}
