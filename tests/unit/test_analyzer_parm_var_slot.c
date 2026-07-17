/* Phase 18 Wave 0 (Plan 02): TDD scaffold for PARM-03 (read-only argument
 * passed to a `var` parameter slot) covering BOTH IRON_NODE_CALL
 * (free-function) AND IRON_NODE_METHOD_CALL (method-call) paths.
 *
 * Authored RED first; flips GREEN once Plan 18-02 lands
 * IRON_ERR_PARM_VAR_SLOT_NEEDS_MUT=267 in src/diagnostics/diagnostics.h
 * AND extends typecheck.c IRON_NODE_CALL handler at lines 2204-2226 +
 * IRON_NODE_METHOD_CALL handler at line 2286+ with the per-arg PARM-03
 * check using the static helper arg_source_is_mutable.
 *
 * Pitfall 3 lock: a free-function-only fix is incomplete because v4
 * acceptance fixtures predominantly use methods. test_parm_03_method_call
 * _val_to_var_slot_rejected anchors method-call coverage so an arg-loop
 * extension that only patches IRON_NODE_CALL keeps THIS file RED.
 *
 * Pitfall 7 lock: hint string drops `*var` pointer mention (Phase 20
 * territory). The locked phrase is "make the argument source mutable
 * (declare as 'var')". No assertions reference `*var` here.
 *
 * Pattern source: tests/unit/test_analyzer_parm_read_only.c (Plan 18-01). */
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

/* PARM-03 — caller has val source, callee free-function has var param;
 * the call site MUST emit IRON_ERR_PARM_VAR_SLOT_NEEDS_MUT=267 with
 * the spec-locked message wording. */
void test_parm_03_val_to_var_slot_rejected(void) {
    analyze_src(
        "func mutates(var y: Int) {\n"
        "    y = y + 1\n"
        "}\n"
        "func main() {\n"
        "    val x: Int = 5\n"
        "    mutates(x)\n"
        "}\n");
    TEST_ASSERT_GREATER_THAN_INT(0,
        count_with_code(IRON_ERR_PARM_VAR_SLOT_NEEDS_MUT));
    const char *m = first_msg_with_code(IRON_ERR_PARM_VAR_SLOT_NEEDS_MUT);
    TEST_ASSERT_NOT_NULL(m);
    TEST_ASSERT_NOT_NULL(strstr(m, "cannot pass read-only argument to"));
    TEST_ASSERT_NOT_NULL(strstr(m, "'var' parameter"));
    const char *s = first_sug_with_code(IRON_ERR_PARM_VAR_SLOT_NEEDS_MUT);
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_NOT_NULL(strstr(s, "make the argument source mutable"));
}

/* PARM-03 — caller has var source, callee free-function has var param;
 * the call site analyzes cleanly (no PARM-03 diagnostic). */
void test_parm_03_var_to_var_slot_accepts(void) {
    analyze_src(
        "func mutates(var y: Int) {\n"
        "    y = y + 1\n"
        "}\n"
        "func main() {\n"
        "    var x: Int = 5\n"
        "    mutates(x)\n"
        "}\n");
    TEST_ASSERT_EQUAL_INT(0,
        count_with_code(IRON_ERR_PARM_VAR_SLOT_NEEDS_MUT));
}

/* PARM-03 — caller has val source, callee free-function has READ-ONLY
 * param (no var modifier). PARM-03 must NOT fire — readonly param is
 * compatible with val source by design. */
void test_parm_03_val_to_readonly_slot_accepts(void) {
    analyze_src(
        "func reads(y: Int) -> Int {\n"
        "    return y + 1\n"
        "}\n"
        "func main() {\n"
        "    val x: Int = 5\n"
        "    val z: Int = reads(x)\n"
        "}\n");
    TEST_ASSERT_EQUAL_INT(0,
        count_with_code(IRON_ERR_PARM_VAR_SLOT_NEEDS_MUT));
}

/* PARM-03 — Pitfall 3 method-call coverage lock. Object has a method with
 * a var parameter; caller passes a val source via obj.method(val_arg).
 * MUST emit IRON_ERR_PARM_VAR_SLOT_NEEDS_MUT. A typecheck.c arg-loop
 * extension that only patches IRON_NODE_CALL leaves THIS test RED. */
void test_parm_03_method_call_val_to_var_slot_rejected(void) {
    analyze_src(
        "object Box {\n"
        "    var v: Int\n"
        "    init(v0: Int) { self.v = v0 }\n"
        "    func bump(var amount: Int) {\n"
        "        amount = amount + 1\n"
        "        self.v = self.v + amount\n"
        "    }\n"
        "}\n"
        "func main() {\n"
        "    var b = Box(0)\n"
        "    val delta: Int = 3\n"
        "    b.bump(delta)\n"
        "}\n");
    TEST_ASSERT_GREATER_THAN_INT(0,
        count_with_code(IRON_ERR_PARM_VAR_SLOT_NEEDS_MUT));
}

/* PARM-03 — passing an rvalue (literal expression) to a var slot is
 * rejected. arg_source_is_mutable returns false for any non-IDENT,
 * non-FIELD_ACCESS expression conservatively (rvalues are not mutable
 * sources). Locks the rvalue branch of the helper. */
void test_parm_03_rvalue_to_var_slot_rejected(void) {
    analyze_src(
        "func mutates(var y: Int) {\n"
        "    y = y + 1\n"
        "}\n"
        "func main() {\n"
        "    mutates(5)\n"
        "}\n");
    TEST_ASSERT_GREATER_THAN_INT(0,
        count_with_code(IRON_ERR_PARM_VAR_SLOT_NEEDS_MUT));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_parm_03_val_to_var_slot_rejected);
    RUN_TEST(test_parm_03_var_to_var_slot_accepts);
    RUN_TEST(test_parm_03_val_to_readonly_slot_accepts);
    RUN_TEST(test_parm_03_method_call_val_to_var_slot_rejected);
    RUN_TEST(test_parm_03_rvalue_to_var_slot_rejected);
    return UNITY_END();
}
