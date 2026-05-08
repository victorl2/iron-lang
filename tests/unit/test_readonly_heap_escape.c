/* Phase 22 Plan 01 Wave 0 RED -> GREEN: READ-05 readonly heap-escape tests.
 * Guards that IRON_ERR_READONLY_HEAP_ESCAPE (279) fires exactly when a
 * readonly method allocates heap memory, and NOT in non-readonly methods.
 * Multiple heap allocations in one body each emit a separate diagnostic.
 *
 * Iron syntax: readonly methods are declared inside object blocks as
 *   `readonly func f(...)`.
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

/* READ-05 case 1: readonly method allocating heap emits E0279. */
void test_readonly_heap_allocation_rejected(void) {
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
    TEST_ASSERT_TRUE(hint_contains(IRON_ERR_READONLY_HEAP_ESCAPE,
                                   "may not allocate heap"));
}

/* READ-05 case 2: NON-readonly method allocating heap emits ZERO E0279. */
void test_non_readonly_heap_allocation_allowed(void) {
    analyze_src(
        "object Point {\n"
        "    var x: Int\n"
        "    var y: Int\n"
        "}\n"
        "object X {\n"
        "    func f() -> Int {\n"
        "        val p = heap Point(1, 2)\n"
        "        return 0\n"
        "    }\n"
        "}\n");
    TEST_ASSERT_EQUAL_INT(0, count_code(IRON_ERR_READONLY_HEAP_ESCAPE));
}

/* READ-05 case 3: readonly method with no heap allocation emits ZERO E0279. */
void test_readonly_no_heap_clean(void) {
    analyze_src(
        "object X {\n"
        "    readonly func f() -> Int {\n"
        "        return 0\n"
        "    }\n"
        "}\n");
    TEST_ASSERT_EQUAL_INT(0, count_code(IRON_ERR_READONLY_HEAP_ESCAPE));
}

/* READ-05 case 4: two heap allocations in readonly body emit exactly 2 E0279. */
void test_readonly_two_heap_allocations_emit_two_diags(void) {
    analyze_src(
        "object Point {\n"
        "    var x: Int\n"
        "    var y: Int\n"
        "}\n"
        "object X {\n"
        "    readonly func f() -> Int {\n"
        "        val p = heap Point(1, 2)\n"
        "        val q = heap Point(3, 4)\n"
        "        return 0\n"
        "    }\n"
        "}\n");
    TEST_ASSERT_EQUAL_INT(2, count_code(IRON_ERR_READONLY_HEAP_ESCAPE));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_readonly_heap_allocation_rejected);
    RUN_TEST(test_non_readonly_heap_allocation_allowed);
    RUN_TEST(test_readonly_no_heap_clean);
    RUN_TEST(test_readonly_two_heap_allocations_emit_two_diags);
    return UNITY_END();
}
