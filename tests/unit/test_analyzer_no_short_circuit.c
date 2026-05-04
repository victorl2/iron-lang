/* test_analyzer_no_short_circuit.c — HARD-03 invariant: every analyzer
 * pass runs even when prior passes have reported errors. The pre-M0 behaviour
 * was to short-circuit after resolve or typecheck errors; post-M0 every pass
 * runs to completion and accumulates diagnostics. */

#include "unity.h"
#include "analyzer/analyzer.h"
#include "util/arena.h"
#include "diagnostics/diagnostics.h"

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

/* HARD-03: a source that exercises (1) a resolve error (undefined name) AND
 * (2) a typecheck error (type mismatch on a DIFFERENT, resolvable identifier).
 *
 * Pre-M0, iron_analyze short-circuited after resolve saw the undefined-name
 * error and typecheck never ran — so the type-mismatch diagnostic never
 * appeared. Post-M0, both diagnostics appear in the diag list, proving
 * neither pass was short-circuited. */
void test_both_resolve_and_typecheck_errors_reported(void) {
    const char *src =
        "func use_undefined() -> Int {\n"
        "  return not_a_real_name\n"       /* IRON_ERR_UNDEFINED_VAR (200) */
        "}\n"
        "func type_clash() -> Int {\n"
        "  val x: Int = \"hello\"\n"       /* IRON_ERR_TYPE_MISMATCH (202) */
        "  return x\n"
        "}\n";
    Iron_AnalyzeResult r = iron_analyze_buffer(
        src, strlen(src), "both_errors.iron",
        IRON_ANALYSIS_MODE_CLI,
        &arena, &diags, NULL,
        0);
    (void)r;
    /* Both error codes expected to appear in the diag list.
     *
     * Phase 4 Plan 04-01 (EDIT-07): the literal-position RHS (String "hello"
     * bound to Int annotation) now narrows to IRON_ERR_TYPE_MISMATCH_LITERAL
     * (code 235). Accept either 202 or 235 as evidence that the typecheck
     * pass ran — HARD-03 only requires that SOME type-error diagnostic
     * be produced, not a specific code. */
    int saw_undefined = 0, saw_type_mismatch = 0;
    for (int i = 0; i < diags.count; i++) {
        if (diags.items[i].code == IRON_ERR_UNDEFINED_VAR) saw_undefined = 1;
        if (diags.items[i].code == IRON_ERR_TYPE_MISMATCH ||
            diags.items[i].code == IRON_ERR_TYPE_MISMATCH_LITERAL) {
            saw_type_mismatch = 1;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(saw_undefined,
        "resolve pass must emit undefined-var diagnostic");
    TEST_ASSERT_TRUE_MESSAGE(saw_type_mismatch,
        "typecheck pass MUST run after resolve errors (HARD-03)");
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_both_resolve_and_typecheck_errors_reported);
    return UNITY_END();
}
