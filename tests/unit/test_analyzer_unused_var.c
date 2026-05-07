/* Phase 17 Wave 0: TDD scaffold for VAL-05 (unused var binding) +
 * VAL-06 (unused var parameter).
 *
 * Authored RED first; flips GREEN once Plan 17-02 lands
 * IRON_WARN_UNUSED_VAR=613 + IRON_WARN_UNUSED_VAR_PARAM=614 and the new
 * src/analyzer/unused_var.c pass with dispatcher wiring in
 * src/analyzer/analyzer.c. */
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
static const char *first_msg_with_code(int target) {
    for (int i = 0; i < arrlen(diags.items); i++) {
        if (diags.items[i].code == target) return diags.items[i].message;
    }
    return NULL;
}

/* ── VAL-05 (var binding) ─────────────────────────────────────────── */

void test_val_05_unused_var_warn(void) {
    analyze_src("func main() { var x = 5; println(\"x\") }\n");
    TEST_ASSERT_GREATER_THAN_INT(0, count_with_code(IRON_WARN_UNUSED_VAR));
}
void test_val_05_warn_message(void) {
    analyze_src("func main() { var x = 5; println(\"x\") }\n");
    const char *m = first_msg_with_code(IRON_WARN_UNUSED_VAR);
    TEST_ASSERT_NOT_NULL(m);
    TEST_ASSERT_NOT_NULL(strstr(m, "never reassigned"));
}
void test_val_05_used_var_no_warn(void) {
    analyze_src("func main() { var x = 5; x = 10; println(\"{x}\") }\n");
    TEST_ASSERT_EQUAL_INT(0, count_with_code(IRON_WARN_UNUSED_VAR));
}
void test_val_05_conditional_write_no_warn(void) {
    /* Any-write-anywhere semantics per RESEARCH Pitfall 2.
     * Conditional reassignment counts as "used" — no warning. */
    analyze_src(
        "func main(cond: Bool) {\n"
        "    var x = 5\n"
        "    if cond { x = 10 }\n"
        "    println(\"{x}\")\n"
        "}\n");
    TEST_ASSERT_EQUAL_INT(0, count_with_code(IRON_WARN_UNUSED_VAR));
}
void test_val_05_compound_assign_counts(void) {
    analyze_src("func main() { var x = 5; x += 1; println(\"{x}\") }\n");
    TEST_ASSERT_EQUAL_INT(0, count_with_code(IRON_WARN_UNUSED_VAR));
}
void test_val_05_field_write_does_not_count(void) {
    /* Per CONTEXT.md: "Mutation = binding reassignment only.
     * Field writes (x.f = ...), &x address-of, pass-to-var-slot do NOT
     * count." So writing p.x = 3 must NOT mark `p` as reassigned — it
     * should still warn as an unused var binding. */
    analyze_src(
        "object Point { var x: Int; var y: Int; init() { self.x = 0; self.y = 0 } }\n"
        "func main() {\n"
        "    var p = Point()\n"
        "    p.x = 3\n"
        "    println(\"{p.x}\")\n"
        "}\n");
    TEST_ASSERT_GREATER_THAN_INT(0, count_with_code(IRON_WARN_UNUSED_VAR));
}

/* ── VAL-06 (var parameter) ───────────────────────────────────────── */

void test_val_06_unused_var_param_warn(void) {
    analyze_src("func f(var p: Int) { println(\"{p}\") }\nfunc main() { f(5) }\n");
    TEST_ASSERT_GREATER_THAN_INT(0, count_with_code(IRON_WARN_UNUSED_VAR_PARAM));
}
void test_val_06_param_message(void) {
    analyze_src("func f(var p: Int) { println(\"{p}\") }\nfunc main() { f(5) }\n");
    const char *m = first_msg_with_code(IRON_WARN_UNUSED_VAR_PARAM);
    TEST_ASSERT_NOT_NULL(m);
    TEST_ASSERT_NOT_NULL(strstr(m, "never mutated"));
}
void test_val_06_used_var_param_no_warn(void) {
    analyze_src("func f(var p: Int) { p = p + 1; println(\"{p}\") }\nfunc main() { f(5) }\n");
    TEST_ASSERT_EQUAL_INT(0, count_with_code(IRON_WARN_UNUSED_VAR_PARAM));
}
void test_val_06_param_shadowed_still_warns(void) {
    /* RESEARCH Pitfall 3: a var parameter that is only READ (never written
     * via an IDENT-LHS assign) still warns even when the body declares
     * additional bindings. Mutation = binding reassignment only — reads in
     * RHS expressions do not count. */
    analyze_src(
        "func f(var p: Int) {\n"
        "    val q = p + 1\n"
        "    println(\"{q}\")\n"
        "}\n"
        "func main() { f(5) }\n");
    TEST_ASSERT_GREATER_THAN_INT(0, count_with_code(IRON_WARN_UNUSED_VAR_PARAM));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_val_05_unused_var_warn);
    RUN_TEST(test_val_05_warn_message);
    RUN_TEST(test_val_05_used_var_no_warn);
    RUN_TEST(test_val_05_conditional_write_no_warn);
    RUN_TEST(test_val_05_compound_assign_counts);
    RUN_TEST(test_val_05_field_write_does_not_count);
    RUN_TEST(test_val_06_unused_var_param_warn);
    RUN_TEST(test_val_06_param_message);
    RUN_TEST(test_val_06_used_var_param_no_warn);
    RUN_TEST(test_val_06_param_shadowed_still_warns);
    return UNITY_END();
}
