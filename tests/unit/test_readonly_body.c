/* Phase 22 Plan 01 Wave 0 RED -> GREEN: READ-09 §6 hint-string regression anchor.
 * Verifies that every readonly diagnostic (238/239/277/278/279) carries the
 * "6:" substring in its suggestion/hint field.
 * Also tests Pitfall 1 (double-emit prevention): a pure method I/O violation
 * must produce ONLY E0240 (IRON_ERR_PURE_IO) and ZERO E0278 (IRON_ERR_READONLY_IO).
 *
 * Iron syntax: readonly methods are declared inside object blocks as
 *   `readonly func f(...)`.  Pure methods use `pure func f(...)`.
 *
 * Cases 1-2 reference Phase 84 baseline codes (238/239) — already declared.
 * The hint-assertion WILL fail (RED) until Tasks 3+4 append hints to baselines.
 * Cases 3-5 reference new codes 277/278/279 — RED until Task 2 adds them.
 * Case 6 (Pitfall 1) GREEN once Tasks 2+3 land with !in_pure_method guard.
 *
 * Pattern source: tests/unit/test_free_leak_target_check.c */

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

static int count_code(int target) {
    int n = 0;
    for (int i = 0; i < arrlen(diags.items); i++) {
        if (diags.items[i].code == target) n++;
    }
    return n;
}

static bool hint_contains(int target, const char *needle) {
    for (int i = 0; i < arrlen(diags.items); i++) {
        if (diags.items[i].code == target &&
            diags.items[i].suggestion && strstr(diags.items[i].suggestion, needle))
            return true;
    }
    return false;
}

/* Case 1 (READ-09 for READ-01): readonly method writing self.field produces
 * E0238 IRON_ERR_READONLY_WRITE_SELF with suggestion containing "6:". */
void test_readonly_write_self_has_section_six_hint(void) {
    analyze_src(
        "object X {\n"
        "    var v: Int\n"
        "    readonly func f() -> Int {\n"
        "        self.v = 1\n"
        "        return 0\n"
        "    }\n"
        "}\n");
    TEST_ASSERT_EQUAL_INT(1, count_code(IRON_ERR_READONLY_WRITE_SELF));
    TEST_ASSERT_TRUE(hint_contains(IRON_ERR_READONLY_WRITE_SELF, "6:"));
}

/* Case 2 (READ-09 for READ-03): readonly method calling non-readonly method
 * produces E0239 IRON_ERR_READONLY_CALLS_MUTATING with hint containing "6:". */
void test_readonly_calls_mutating_has_section_six_hint(void) {
    analyze_src(
        "object X {\n"
        "    var v: Int\n"
        "    func mutate() {\n"
        "        self.v = 1\n"
        "    }\n"
        "    readonly func f() -> Int {\n"
        "        self.mutate()\n"
        "        return 0\n"
        "    }\n"
        "}\n");
    TEST_ASSERT_GREATER_THAN_INT(0, count_code(IRON_ERR_READONLY_CALLS_MUTATING));
    TEST_ASSERT_TRUE(hint_contains(IRON_ERR_READONLY_CALLS_MUTATING, "6:"));
}

/* Case 3 (READ-09 for READ-02): readonly method writing a param produces
 * E0277 IRON_ERR_READONLY_PARAM_MUTATION with hint containing "6:". */
void test_readonly_param_mutation_has_section_six_hint(void) {
    analyze_src(
        "object X {\n"
        "    readonly func f(p: Int) -> Int {\n"
        "        p = 99\n"
        "        return p\n"
        "    }\n"
        "}\n");
    TEST_ASSERT_EQUAL_INT(1, count_code(IRON_ERR_READONLY_PARAM_MUTATION));
    TEST_ASSERT_TRUE(hint_contains(IRON_ERR_READONLY_PARAM_MUTATION, "6:"));
}

/* Case 4 (READ-09 for READ-04): readonly method calling println produces
 * E0278 IRON_ERR_READONLY_IO with hint containing "6:". */
void test_readonly_io_has_section_six_hint(void) {
    analyze_src(
        "object X {\n"
        "    readonly func f() -> Int {\n"
        "        println(\"x\")\n"
        "        return 0\n"
        "    }\n"
        "}\n");
    TEST_ASSERT_GREATER_THAN_INT(0, count_code(IRON_ERR_READONLY_IO));
    TEST_ASSERT_TRUE(hint_contains(IRON_ERR_READONLY_IO, "6:"));
}

/* Case 5 (READ-09 for READ-05): readonly method allocating heap produces
 * E0279 IRON_ERR_READONLY_HEAP_ESCAPE with hint containing "6:". */
void test_readonly_heap_escape_has_section_six_hint(void) {
    analyze_src(
        "object Point {\n"
        "    var x: Int\n"
        "    var y: Int\n"
        "}\n"
        "object X {\n"
        "    readonly func f() -> Int {\n"
        "        val p = heap Point(1, 2)\n"
        "        return 0\n"
        "    }\n"
        "}\n");
    TEST_ASSERT_EQUAL_INT(1, count_code(IRON_ERR_READONLY_HEAP_ESCAPE));
    TEST_ASSERT_TRUE(hint_contains(IRON_ERR_READONLY_HEAP_ESCAPE, "6:"));
}

/* Case 6 (Pitfall 1 — double-emit prevention): PURE method calling println
 * must produce EXACTLY ONE E0240 (IRON_ERR_PURE_IO) and ZERO E0278
 * (IRON_ERR_READONLY_IO).  The !ctx->in_pure_method guard prevents the
 * readonly-tier check from double-firing inside a pure method body.
 *
 * ctx->in_readonly_method is true for BOTH pure and readonly methods;
 * without the Pitfall 1 guard both codes would fire. */
void test_pure_method_io_no_double_emit(void) {
    analyze_src(
        "object X {\n"
        "    pure func f() -> Int {\n"
        "        println(\"x\")\n"
        "        return 0\n"
        "    }\n"
        "}\n");
    TEST_ASSERT_EQUAL_INT(1, count_code(IRON_ERR_PURE_IO));
    TEST_ASSERT_EQUAL_INT(0, count_code(IRON_ERR_READONLY_IO));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_readonly_write_self_has_section_six_hint);
    RUN_TEST(test_readonly_calls_mutating_has_section_six_hint);
    RUN_TEST(test_readonly_param_mutation_has_section_six_hint);
    RUN_TEST(test_readonly_io_has_section_six_hint);
    RUN_TEST(test_readonly_heap_escape_has_section_six_hint);
    RUN_TEST(test_pure_method_io_no_double_emit);
    return UNITY_END();
}
