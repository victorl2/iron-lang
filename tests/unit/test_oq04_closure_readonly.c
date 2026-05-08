/* Phase 22 Plan 02 Wave 0 RED -> GREEN: OQ-04 closure body-purity + capture-purity.
 * Guards that:
 *   - closure inside a readonly method inherits readonly context (body-purity)
 *   - var-binding captures inside readonly closures are rejected with E0277
 *   - val-binding and *T captures are accepted (not mutable)
 *
 * Iron syntax: closures use `func(params) -> RetType { body }` inside method bodies.
 * Readonly context is inherited from the enclosing readonly method (OQ-04 rule).
 *
 * Pattern source: tests/unit/test_readonly_param_mutation.c */

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

/* OQ-04 case 1: closure inside a readonly method that performs I/O
 * emits E0278 IRON_ERR_READONLY_IO. The closure body inherits readonly context
 * from the enclosing readonly method via ctx->in_readonly_method propagation. */
void test_readonly_closure_body_io_rejected(void) {
    analyze_src(
        "object X {\n"
        "    readonly func f() -> Int {\n"
        "        val cb = func() -> Int {\n"
        "            println(\"x\")\n"
        "            return 0\n"
        "        }\n"
        "        return 0\n"
        "    }\n"
        "}\n");
    TEST_ASSERT_GREATER_THAN_INT(0, count_code(IRON_ERR_READONLY_IO));
}

/* OQ-04 case 2: closure inside a non-readonly method does NOT emit E0278.
 * Baseline: without readonly context, I/O is fine. */
void test_non_readonly_closure_body_io_allowed(void) {
    analyze_src(
        "object X {\n"
        "    func f() -> Int {\n"
        "        val cb = func() -> Int {\n"
        "            println(\"x\")\n"
        "            return 0\n"
        "        }\n"
        "        return 0\n"
        "    }\n"
        "}\n");
    TEST_ASSERT_EQUAL_INT(0, count_code(IRON_ERR_READONLY_IO));
}

/* OQ-04 case 3: readonly method with a lambda that captures a var binding
 * emits E0277 IRON_ERR_READONLY_PARAM_MUTATION from capture.c.
 * The var binding is mutable (is_mutable=true), so it is rejected. */
void test_readonly_method_var_capture_rejected(void) {
    analyze_src(
        "object X {\n"
        "    readonly func f() -> Int {\n"
        "        var x = 1\n"
        "        val cb = func() -> Int { return x }\n"
        "        return 0\n"
        "    }\n"
        "}\n");
    TEST_ASSERT_EQUAL_INT(1, count_code(IRON_ERR_READONLY_PARAM_MUTATION));
    TEST_ASSERT_TRUE(hint_contains(IRON_ERR_READONLY_PARAM_MUTATION,
                                   "closures in readonly"));
}

/* OQ-04 case 4: readonly method with a lambda that captures a val binding
 * emits ZERO E0277. val is not mutable (is_mutable=false), so it is accepted. */
void test_readonly_method_val_capture_accepted(void) {
    analyze_src(
        "object X {\n"
        "    readonly func f() -> Int {\n"
        "        val x = 1\n"
        "        val cb = func() -> Int { return x }\n"
        "        return 0\n"
        "    }\n"
        "}\n");
    TEST_ASSERT_EQUAL_INT(0, count_code(IRON_ERR_READONLY_PARAM_MUTATION));
}

/* OQ-04 case 5: non-readonly method with a lambda that captures a var binding
 * emits ZERO E0277. Without readonly context, var captures are legal. */
void test_non_readonly_method_var_capture_allowed(void) {
    analyze_src(
        "object X {\n"
        "    func f() -> Int {\n"
        "        var x = 1\n"
        "        val cb = func() -> Int { return x }\n"
        "        return 0\n"
        "    }\n"
        "}\n");
    TEST_ASSERT_EQUAL_INT(0, count_code(IRON_ERR_READONLY_PARAM_MUTATION));
}

/* OQ-04 case 6: readonly method with nested lambda capturing a var from outer
 * method scope emits E0277. The outer readonly context propagates into nested
 * lambdas (inner-out walk order). */
void test_readonly_method_nested_lambda_var_capture_rejected(void) {
    analyze_src(
        "object X {\n"
        "    readonly func f() -> Int {\n"
        "        var counter = 0\n"
        "        val cb = func() -> Int { return counter }\n"
        "        return 0\n"
        "    }\n"
        "}\n");
    TEST_ASSERT_EQUAL_INT(1, count_code(IRON_ERR_READONLY_PARAM_MUTATION));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_readonly_closure_body_io_rejected);
    RUN_TEST(test_non_readonly_closure_body_io_allowed);
    RUN_TEST(test_readonly_method_var_capture_rejected);
    RUN_TEST(test_readonly_method_val_capture_accepted);
    RUN_TEST(test_non_readonly_method_var_capture_allowed);
    RUN_TEST(test_readonly_method_nested_lambda_var_capture_rejected);
    return UNITY_END();
}
