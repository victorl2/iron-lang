/* Phase 22 Plan 01 Wave 0 RED -> GREEN: READ-02 readonly param-mutation tests.
 * Guards that IRON_ERR_READONLY_PARAM_MUTATION (277) fires exactly when a
 * readonly method assigns to a parameter, and NOT in non-readonly methods.
 *
 * Iron syntax: readonly methods are declared inside object blocks as
 *   `readonly func f(...)`.  The `func X.f() readonly` external form
 *   does not exist in v3.
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

/* Check that the suggestion string of any diag with `target` code contains needle. */
static bool hint_contains(int target, const char *needle) {
    for (int i = 0; i < arrlen(diags.items); i++) {
        if (diags.items[i].code == target &&
            diags.items[i].suggestion && strstr(diags.items[i].suggestion, needle))
            return true;
    }
    return false;
}

/* READ-02 case 1: readonly method assigning a val-param emits E0277. */
void test_readonly_val_param_assignment_rejected(void) {
    analyze_src(
        "object X {\n"
        "    readonly func f(p: Int) -> Int {\n"
        "        p = 99\n"
        "        return p\n"
        "    }\n"
        "}\n");
    TEST_ASSERT_EQUAL_INT(1, count_code(IRON_ERR_READONLY_PARAM_MUTATION));
    TEST_ASSERT_TRUE(hint_contains(IRON_ERR_READONLY_PARAM_MUTATION, "6:"));
    TEST_ASSERT_TRUE(hint_contains(IRON_ERR_READONLY_PARAM_MUTATION,
                                   "may not assign to any parameter"));
}

/* READ-02 case 3: NON-readonly method with same param-assign body emits
 * ZERO E0277.  The `in_readonly_method` guard must be false. */
void test_non_readonly_param_assignment_allowed(void) {
    analyze_src(
        "object X {\n"
        "    func f(p: Int) -> Int {\n"
        "        p = 99\n"
        "        return p\n"
        "    }\n"
        "}\n");
    TEST_ASSERT_EQUAL_INT(0, count_code(IRON_ERR_READONLY_PARAM_MUTATION));
}

/* READ-02 case 4: readonly method with no param assignments emits ZERO E0277. */
void test_readonly_no_param_assign_clean(void) {
    analyze_src(
        "object X {\n"
        "    readonly func f() -> Int {\n"
        "        return 0\n"
        "    }\n"
        "}\n");
    TEST_ASSERT_EQUAL_INT(0, count_code(IRON_ERR_READONLY_PARAM_MUTATION));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_readonly_val_param_assignment_rejected);
    RUN_TEST(test_non_readonly_param_assignment_allowed);
    RUN_TEST(test_readonly_no_param_assign_clean);
    return UNITY_END();
}
