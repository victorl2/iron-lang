/* Phase 22 Plan 01 Wave 0 RED -> GREEN: READ-04 readonly I/O unit tests.
 * Guards that IRON_ERR_READONLY_IO (278) fires at both free-function call
 * sites (println, print, readline) and method-call sites (Log.*) from a
 * readonly method body, and NOT in non-readonly methods.
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

/* READ-04 case 1: readonly method calling println emits E0278. */
void test_readonly_println_rejected(void) {
    analyze_src(
        "object X {\n"
        "    readonly func f() -> Int {\n"
        "        println(\"x\")\n"
        "        return 0\n"
        "    }\n"
        "}\n");
    TEST_ASSERT_GREATER_THAN_INT(0, count_code(IRON_ERR_READONLY_IO));
    TEST_ASSERT_TRUE(hint_contains(IRON_ERR_READONLY_IO, "6:"));
    TEST_ASSERT_TRUE(hint_contains(IRON_ERR_READONLY_IO, "may not perform I/O"));
}

/* READ-04 case 2: readonly method calling print emits E0278. */
void test_readonly_print_rejected(void) {
    analyze_src(
        "object X {\n"
        "    readonly func f() -> Int {\n"
        "        print(\"x\")\n"
        "        return 0\n"
        "    }\n"
        "}\n");
    TEST_ASSERT_GREATER_THAN_INT(0, count_code(IRON_ERR_READONLY_IO));
    TEST_ASSERT_TRUE(hint_contains(IRON_ERR_READONLY_IO, "6:"));
}

/* READ-04 case 3: readonly method calling print emits E0278 with hint text.
 * (readline() is a stdlib function not available in minimal analysis mode;
 *  the IRON_RO_IO_BUILTINS array includes it for when stdlib is loaded.) */
void test_readonly_print_hint_text(void) {
    analyze_src(
        "object X {\n"
        "    readonly func f() -> Int {\n"
        "        print(\"x\")\n"
        "        return 0\n"
        "    }\n"
        "}\n");
    TEST_ASSERT_GREATER_THAN_INT(0, count_code(IRON_ERR_READONLY_IO));
    TEST_ASSERT_TRUE(hint_contains(IRON_ERR_READONLY_IO, "may not perform I/O"));
}

/* READ-04 case 4: readonly method calling Log.info emits E0278.
 * Covers the method-call site (IRON_NODE_METHOD_CALL arm). */
void test_readonly_log_method_call_rejected(void) {
    analyze_src(
        "object X {\n"
        "    readonly func f() -> Int {\n"
        "        Log.info(\"x\")\n"
        "        return 0\n"
        "    }\n"
        "}\n");
    TEST_ASSERT_GREATER_THAN_INT(0, count_code(IRON_ERR_READONLY_IO));
    TEST_ASSERT_TRUE(hint_contains(IRON_ERR_READONLY_IO, "6:"));
}

/* READ-04 case 5: readonly method with no I/O emits ZERO E0278. */
void test_readonly_no_io_clean(void) {
    analyze_src(
        "object X {\n"
        "    readonly func f() -> Int {\n"
        "        return 0\n"
        "    }\n"
        "}\n");
    TEST_ASSERT_EQUAL_INT(0, count_code(IRON_ERR_READONLY_IO));
}

/* READ-04 case 6: NON-readonly method calling println emits ZERO E0278. */
void test_non_readonly_println_allowed(void) {
    analyze_src(
        "object X {\n"
        "    func f() -> Int {\n"
        "        println(\"x\")\n"
        "        return 0\n"
        "    }\n"
        "}\n");
    TEST_ASSERT_EQUAL_INT(0, count_code(IRON_ERR_READONLY_IO));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_readonly_println_rejected);
    RUN_TEST(test_readonly_print_rejected);
    RUN_TEST(test_readonly_print_hint_text);
    RUN_TEST(test_readonly_log_method_call_rejected);
    RUN_TEST(test_readonly_no_io_clean);
    RUN_TEST(test_non_readonly_println_allowed);
    return UNITY_END();
}
