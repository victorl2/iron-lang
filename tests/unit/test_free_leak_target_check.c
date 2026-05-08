/* Phase 21 Wave 0 (Plan 01): TDD scaffold for POL-04 + POL-05 — `free` and
 * `leak` targets must be bare identifiers (binding names), not expressions.
 *
 * POL-04: `free p.field`, `free arr[i]`, `free obj.method()` all emit
 *         IRON_ERR_FREE_NOT_BINDING=274.
 * POL-05: `leak p.field`, `leak arr[i]` emit IRON_ERR_LEAK_NOT_BINDING=275.
 *
 * Authored RED first; flips GREEN once Plan 21-01 Task 3 extends
 * src/analyzer/typecheck.c IRON_NODE_FREE + IRON_NODE_LEAK arms.
 *
 * Pattern source: tests/unit/test_analyzer_parm_var_slot.c */

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

/* POL-04: `free c.inner` — field access is not a binding name. */
void test_free_field_access_rejected(void) {
    analyze_src(
        "object Container { var inner: Int }\n"
        "func main() {\n"
        "    val c = heap Container(0)\n"
        "    free c.inner\n"
        "}\n");
    TEST_ASSERT_GREATER_THAN_INT(0,
        count_with_code(IRON_ERR_FREE_NOT_BINDING));
    TEST_ASSERT_TRUE(msg_contains(IRON_ERR_FREE_NOT_BINDING,
        "must be a binding name"));
}

/* POL-04: `free arr[i]` — index expression is not a binding name. */
void test_free_index_expr_rejected(void) {
    analyze_src(
        "func main() {\n"
        "    val arr = [heap 42, heap 43]\n"
        "    val i = 0\n"
        "    free arr[i]\n"
        "}\n");
    TEST_ASSERT_GREATER_THAN_INT(0,
        count_with_code(IRON_ERR_FREE_NOT_BINDING));
}

/* POL-04: `free obj.method()` — method call is not a binding name. */
void test_free_method_call_rejected(void) {
    analyze_src(
        "object Box { var v: Int\n"
        "    func get() -> Int { return self.v }\n"
        "}\n"
        "func main() {\n"
        "    val b = heap Box(0)\n"
        "    free b.get()\n"
        "}\n");
    TEST_ASSERT_GREATER_THAN_INT(0,
        count_with_code(IRON_ERR_FREE_NOT_BINDING));
}

/* Legal form: `free p` where p is an identifier must emit ZERO E0274.
 * (escape.c E0212 may fire if p is not heap-backed; that is a separate check.) */
void test_free_identifier_target_legal(void) {
    analyze_src(
        "object Point { var x: Int; var y: Int\n"
        "    init() { self.x = 0; self.y = 0 }\n"
        "}\n"
        "func main() {\n"
        "    val p = heap Point()\n"
        "    free p\n"
        "}\n");
    TEST_ASSERT_EQUAL_INT(0,
        count_with_code(IRON_ERR_FREE_NOT_BINDING));
}

/* POL-05: `leak c.inner` — field access is not a binding name. */
void test_leak_field_access_rejected(void) {
    analyze_src(
        "object Container { var inner: Int }\n"
        "func main() {\n"
        "    val c = heap Container(0)\n"
        "    leak c.inner\n"
        "}\n");
    TEST_ASSERT_GREATER_THAN_INT(0,
        count_with_code(IRON_ERR_LEAK_NOT_BINDING));
    TEST_ASSERT_TRUE(msg_contains(IRON_ERR_LEAK_NOT_BINDING,
        "must be a binding name"));
}

/* Legal form: `leak p` where p is an identifier must emit ZERO E0275. */
void test_leak_identifier_target_legal(void) {
    analyze_src(
        "object Point { var x: Int; var y: Int\n"
        "    init() { self.x = 0; self.y = 0 }\n"
        "}\n"
        "func main() {\n"
        "    val p = heap Point()\n"
        "    leak p\n"
        "}\n");
    TEST_ASSERT_EQUAL_INT(0,
        count_with_code(IRON_ERR_LEAK_NOT_BINDING));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_free_field_access_rejected);
    RUN_TEST(test_free_index_expr_rejected);
    RUN_TEST(test_free_method_call_rejected);
    RUN_TEST(test_free_identifier_target_legal);
    RUN_TEST(test_leak_field_access_rejected);
    RUN_TEST(test_leak_identifier_target_legal);
    return UNITY_END();
}
