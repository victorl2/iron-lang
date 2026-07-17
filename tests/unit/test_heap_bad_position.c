/* Phase 21 Wave 0 (Plan 01): TDD scaffold for POL-03 — `heap` keyword in
 * illegal position. Covers three position-lock sites added in Plan 21-01
 * Task 2:
 *   Site 1: type-annotation position  (val p: heap T = ...)
 *   Site 2: binding-declaration position (`heap val p = ...`)
 *   Site 3: parameter-qualifier position  (func f(heap p: T) {})
 *
 * Authored RED first; flips GREEN once Plan 21-01 Task 2 lands
 * IRON_ERR_HEAP_BAD_POSITION=273 in src/diagnostics/diagnostics.h
 * AND inserts the three position-lock guards in src/parser/parser.c.
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

/* Lex + parse + analyze via the production buffer entry point. */
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

/* POL-03 Site 1: `heap` in type-annotation position.
 * `val p: heap Point = heap Point(1, 2)` must emit E0273 with hint
 * "in type annotation". */
void test_heap_in_type_annotation_rejected(void) {
    analyze_src(
        "object Point { var x: Int; var y: Int }\n"
        "func main() {\n"
        "    val p: heap Point = heap Point(1, 2)\n"
        "}\n");
    TEST_ASSERT_GREATER_THAN_INT(0,
        count_with_code(IRON_ERR_HEAP_BAD_POSITION));
    TEST_ASSERT_TRUE(msg_contains(IRON_ERR_HEAP_BAD_POSITION,
        "in type annotation"));
}

/* POL-03 Site 2a: `heap val p = expr` (heap as binding modifier on val).
 * Must emit E0273 with hint "in binding declaration". */
void test_heap_as_val_binding_modifier_rejected(void) {
    analyze_src(
        "func main() {\n"
        "    heap val p = 42\n"
        "}\n");
    TEST_ASSERT_GREATER_THAN_INT(0,
        count_with_code(IRON_ERR_HEAP_BAD_POSITION));
    TEST_ASSERT_TRUE(msg_contains(IRON_ERR_HEAP_BAD_POSITION,
        "in binding declaration"));
}

/* POL-03 Site 2b: `heap var p = expr` (heap as binding modifier on var).
 * Must emit E0273 with hint "in binding declaration". */
void test_heap_as_var_binding_modifier_rejected(void) {
    analyze_src(
        "func main() {\n"
        "    heap var p = 42\n"
        "}\n");
    TEST_ASSERT_GREATER_THAN_INT(0,
        count_with_code(IRON_ERR_HEAP_BAD_POSITION));
    TEST_ASSERT_TRUE(msg_contains(IRON_ERR_HEAP_BAD_POSITION,
        "in binding declaration"));
}

/* POL-03 Site 3: `heap` as a parameter qualifier.
 * `func f(heap p: Int) {}` must emit E0273 with hint "in parameter declaration". */
void test_heap_as_param_qualifier_rejected(void) {
    analyze_src(
        "func f(heap p: Int) -> Int {\n"
        "    return p\n"
        "}\n"
        "func main() {\n"
        "    f(42)\n"
        "}\n");
    TEST_ASSERT_GREATER_THAN_INT(0,
        count_with_code(IRON_ERR_HEAP_BAD_POSITION));
    TEST_ASSERT_TRUE(msg_contains(IRON_ERR_HEAP_BAD_POSITION,
        "in parameter declaration"));
}

/* Legal form: `heap T(...)` at expression position must emit ZERO E0273.
 * The IRON_TOK_HEAP case at iron_parse_primary is untouched by Plan 21-01. */
void test_heap_expr_position_legal(void) {
    analyze_src(
        "object Point { var x: Int; var y: Int\n"
        "    init(x0: Int, y0: Int) { self.x = x0; self.y = y0 }\n"
        "}\n"
        "func main() {\n"
        "    val p = heap Point(1, 2)\n"
        "}\n");
    TEST_ASSERT_EQUAL_INT(0,
        count_with_code(IRON_ERR_HEAP_BAD_POSITION));
}

/* Recursive type annotation: `?heap T` — the `heap` guard fires on the inner
 * annotation parse (called recursively from the `?` handler). Must emit at
 * least one E0273 because `heap` is illegal in any type-annotation context. */
void test_heap_inside_nullable_type_annotation_rejected(void) {
    analyze_src(
        "object T { var x: Int }\n"
        "func main() {\n"
        "    val p: ?heap T = null\n"
        "}\n");
    TEST_ASSERT_GREATER_THAN_INT(0,
        count_with_code(IRON_ERR_HEAP_BAD_POSITION));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_heap_in_type_annotation_rejected);
    RUN_TEST(test_heap_as_val_binding_modifier_rejected);
    RUN_TEST(test_heap_as_var_binding_modifier_rejected);
    RUN_TEST(test_heap_as_param_qualifier_rejected);
    RUN_TEST(test_heap_expr_position_legal);
    RUN_TEST(test_heap_inside_nullable_type_annotation_rejected);
    return UNITY_END();
}
