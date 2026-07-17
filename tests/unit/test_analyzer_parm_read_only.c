/* Phase 18 Wave 0: TDD scaffold for PARM-01 (read-only parameter mutation
 * rejection) + PARM-02 (var-opt-in sanity).
 *
 * Authored RED first; flips GREEN once Plan 18-01 lands
 * IRON_ERR_PARM_READ_ONLY=266 in src/diagnostics/diagnostics.h and extends
 * the existing IRON_NODE_ASSIGN handler at typecheck.c with a
 * sym_kind == IRON_SYM_PARAM branch.
 *
 * Driver runs the FULL analyzer pipeline (iron_analyze_buffer) so the test
 * is implementation-site-agnostic — wherever the diagnostic surfaces
 * within the analyzer pass list, the assertions catch it.
 *
 * Pitfall 2 lock: tests assert COUNT(IRON_ERR_PARM_READ_ONLY)==1 AND
 * COUNT(IRON_ERR_VAL_REASSIGN)==0 AND COUNT(IRON_ERR_MUT_FIELD_IMMUT_RECV)==0
 * on the canonical PARM-01 cases. The branch must redirect, not double-emit. */
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

static const char *first_msg_with_code(int target) {
    for (int i = 0; i < arrlen(diags.items); i++) {
        if (diags.items[i].code == target) return diags.items[i].message;
    }
    return NULL;
}

static const char *first_sug_with_code(int target) {
    for (int i = 0; i < arrlen(diags.items); i++) {
        if (diags.items[i].code == target) return diags.items[i].suggestion;
    }
    return NULL;
}

/* PARM-01 — direct rebind of read-only parameter is rejected with
 * IRON_ERR_PARM_READ_ONLY. The Pitfall 2 lock asserts zero
 * IRON_ERR_VAL_REASSIGN double-fires. */
void test_parm_01_direct_rebind_rejected(void) {
    analyze_src(
        "func bad(p: Int) {\n"
        "    p = 99\n"
        "}\n"
        "func main() {\n"
        "    bad(5)\n"
        "}\n");
    TEST_ASSERT_GREATER_THAN_INT(0, count_with_code(IRON_ERR_PARM_READ_ONLY));
    TEST_ASSERT_EQUAL_INT(0, count_with_code(IRON_ERR_VAL_REASSIGN));
    const char *m = first_msg_with_code(IRON_ERR_PARM_READ_ONLY);
    TEST_ASSERT_NOT_NULL(m);
    TEST_ASSERT_NOT_NULL(strstr(m, "cannot mutate read-only parameter"));
    const char *s = first_sug_with_code(IRON_ERR_PARM_READ_ONLY);
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_NOT_NULL(strstr(s, "var"));
}

/* PARM-01 — compound assign on read-only parameter is rejected. */
void test_parm_01_compound_rejected(void) {
    analyze_src(
        "func bad(p: Int) {\n"
        "    p += 1\n"
        "}\n"
        "func main() {\n"
        "    bad(5)\n"
        "}\n");
    TEST_ASSERT_GREATER_THAN_INT(0, count_with_code(IRON_ERR_PARM_READ_ONLY));
    TEST_ASSERT_EQUAL_INT(0, count_with_code(IRON_ERR_VAL_REASSIGN));
}

/* PARM-01 — field-write through read-only parameter is rejected with
 * IRON_ERR_PARM_READ_ONLY. The Pitfall 2 lock asserts zero
 * IRON_ERR_MUT_FIELD_IMMUT_RECV double-fires. */
void test_parm_01_field_write_rejected(void) {
    analyze_src(
        "object Pt {\n"
        "    var x: Int\n"
        "    init(v: Int) { self.x = v }\n"
        "}\n"
        "func bad(p: Pt) {\n"
        "    p.x = 99\n"
        "}\n"
        "func main() {\n"
        "    bad(Pt(0))\n"
        "}\n");
    TEST_ASSERT_GREATER_THAN_INT(0, count_with_code(IRON_ERR_PARM_READ_ONLY));
    TEST_ASSERT_EQUAL_INT(0, count_with_code(IRON_ERR_MUT_FIELD_IMMUT_RECV));
}

/* Pitfall 2 explicit lock: exactly one E0266 emission, zero E0203 emission,
 * on the canonical direct-rebind case. */
void test_parm_01_no_double_emit_direct(void) {
    analyze_src(
        "func bad(p: Int) {\n"
        "    p = 99\n"
        "}\n"
        "func main() {\n"
        "    bad(5)\n"
        "}\n");
    TEST_ASSERT_EQUAL_INT(1, count_with_code(IRON_ERR_PARM_READ_ONLY));
    TEST_ASSERT_EQUAL_INT(0, count_with_code(IRON_ERR_VAL_REASSIGN));
}

/* PARM-02 — var-opt-in parameter accepts mutation cleanly. */
void test_parm_02_var_param_accepts_mutation(void) {
    analyze_src(
        "func ok(var p: Int) {\n"
        "    p = 99\n"
        "    p += 1\n"
        "}\n"
        "func main() {\n"
        "    ok(5)\n"
        "}\n");
    TEST_ASSERT_EQUAL_INT(0, count_with_code(IRON_ERR_PARM_READ_ONLY));
    TEST_ASSERT_EQUAL_INT(0, count_with_code(IRON_ERR_VAL_REASSIGN));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_parm_01_direct_rebind_rejected);
    RUN_TEST(test_parm_01_compound_rejected);
    RUN_TEST(test_parm_01_field_write_rejected);
    RUN_TEST(test_parm_01_no_double_emit_direct);
    RUN_TEST(test_parm_02_var_param_accepts_mutation);
    return UNITY_END();
}
