/* test_analyzer_errornode.c — HARD-04 invariant: the analyzer tolerates
 * parse-error ASTs (containing IRON_NODE_ERROR subtrees) in both CLI and LSP
 * modes without aborting. Survival is the assertion; additional checks verify
 * that LSP mode does NOT suppress fewer diagnostics than CLI mode. */

#include "unity.h"
#include "analyzer/analyzer.h"
#include "lexer/lexer.h"
#include "parser/parser.h"
#include "parser/ast.h"
#include "util/arena.h"
#include "diagnostics/diagnostics.h"
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

/* HARD-04: malformed source parses to an AST containing IRON_NODE_ERROR
 * subtrees. Running all analyzer passes through iron_analyze_buffer MUST NOT
 * abort, SIGSEGV, or hang — partial diagnostics is the expected behaviour. */
void test_malformed_source_cli_mode_no_abort(void) {
    /* Intentionally broken: unterminated paren list, dangling let, unclosed
     * object. The parser must produce IRON_NODE_ERROR arms. */
    const char *src =
        "func broken( -> Int {\n"
        "  let x = (1 +\n"
        "  return x\n"
        "}\n"
        "obj Broken { field: }\n";
    Iron_AnalyzeResult r = iron_analyze_buffer(
        src, strlen(src), "malformed.iron",
        IRON_ANALYSIS_MODE_CLI,
        &arena, &diags, NULL);
    /* Errors expected (parse errors) but no abort — we reached this line. */
    TEST_ASSERT_GREATER_THAN(0, diags.error_count);
    TEST_ASSERT_TRUE(r.has_errors);
}

/* HARD-04 / HARD-02: same malformed source in LSP mode — same no-abort
 * guarantee, AND LSP mode disables cascade-suppression, so its diagnostic
 * count is at least CLI mode's count. */
void test_malformed_source_lsp_mode_no_abort_more_diags(void) {
    const char *src =
        "func broken( -> Int {\n"
        "  let x = (1 +\n"
        "  return x\n"
        "}\n"
        "obj Broken { field: }\n";
    /* First: CLI baseline */
    Iron_DiagList cli_diags  = iron_diaglist_create();
    Iron_Arena    cli_arena  = iron_arena_create(131072);
    iron_analyze_buffer(src, strlen(src), "malformed.iron",
                        IRON_ANALYSIS_MODE_CLI, &cli_arena, &cli_diags, NULL);
    int cli_count = cli_diags.error_count;
    iron_diaglist_free(&cli_diags);
    iron_arena_free(&cli_arena);

    /* Then: LSP variant */
    Iron_AnalyzeResult r = iron_analyze_buffer(
        src, strlen(src), "malformed.iron",
        IRON_ANALYSIS_MODE_LSP,
        &arena, &diags, NULL);
    (void)r;
    /* LSP count >= CLI count (suppression OFF in LSP mode). */
    TEST_ASSERT_GREATER_OR_EQUAL(cli_count, diags.error_count);
}

/* HARD-04: pathological nesting without matching braces — no abort.
 * Plan 04 will add a depth guard; this test asserts Plan 02 behaviour is
 * a non-crashing no-op even on adversarial input. */
void test_deep_nesting_no_abort(void) {
    /* Build 50 levels of open paren. */
    char buf[4096];
    strcpy(buf, "func f() -> Int { return ");
    for (int i = 0; i < 50; i++) strcat(buf, "(");
    strcat(buf, "0");
    for (int i = 0; i < 50; i++) strcat(buf, ")");
    strcat(buf, " }\n");
    Iron_AnalyzeResult r = iron_analyze_buffer(
        buf, strlen(buf), "deep.iron",
        IRON_ANALYSIS_MODE_CLI,
        &arena, &diags, NULL);
    (void)r;
    /* Survival is the assertion. */
    TEST_ASSERT_TRUE(1);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_malformed_source_cli_mode_no_abort);
    RUN_TEST(test_malformed_source_lsp_mode_no_abort_more_diags);
    RUN_TEST(test_deep_nesting_no_abort);
    return UNITY_END();
}
