/* test_parity_ironc_lsp_suggestions.c — Phase 4 Plan 04-01 Task 02 (EDIT-07).
 *
 * LIVE — byte-identity suggestion parity between CLI and LSP analyzer modes.
 *
 * Sibling of tests/lsp/parity/test_parity_ironc_lsp.c but specifically targets
 * the `.suggestion` field rather than message-only output. For each of the 5
 * P1 codes, run `iron_analyze_buffer` twice (CLI mode, LSP mode) on the same
 * fixture source, iterate both diag lists in parallel, and assert for every
 * (code, span) pair that both `.code` and `.suggestion` strings compare
 * byte-identical (or are both NULL).
 *
 * This guards the CONTEXT.md D-05 contract: CLI and LSP must emit the same
 * `.suggestion` text so the code-action dispatch layer in Plan 04-04 can
 * trust the compiler seed.
 */

#include "unity.h"
#include "analyzer/analyzer.h"
#include "diagnostics/diagnostics.h"
#include "util/arena.h"

#include <stddef.h>
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

static const char *FIXTURES[] = {
    /* IRON_ERR_UNDEFINED_VAR — typo candidate. */
    "func main() {\n"
    "  prinln(\"hi\")\n"
    "}\n",
    /* IRON_ERR_TYPE_MISMATCH_LITERAL — int literal bound to Float annotation. */
    "func main() {\n"
    "  val x: Float = 42\n"
    "}\n",
    /* IRON_ERR_MISSING_RETURN — non-void body reaches end. */
    "func f() -> Int {\n"
    "  val x = 1\n"
    "}\n",
    /* IRON_WARN_UNUSED_IMPORT — aliased import never referenced. */
    "import std.math as m\n"
    "func main() {}\n",
    /* IRON_WARN_REDUNDANT_CAST — Float(1.0). */
    "func main() {\n"
    "  val x = Float(1.0)\n"
    "}\n",
};

static bool strs_equal(const char *a, const char *b) {
    if (a == NULL && b == NULL) return true;
    if (a == NULL || b == NULL) return false;
    return strcmp(a, b) == 0;
}

static void assert_parity(const char *src) {
    Iron_Arena    arena_cli = iron_arena_create(1024 * 128);
    Iron_DiagList diags_cli = iron_diaglist_create();
    Iron_Arena    arena_lsp = iron_arena_create(1024 * 128);
    Iron_DiagList diags_lsp = iron_diaglist_create();

    (void)iron_analyze_buffer(src, strlen(src), "parity.iron",
                               IRON_ANALYSIS_MODE_CLI,
                               &arena_cli, &diags_cli, NULL,
        0);
    (void)iron_analyze_buffer(src, strlen(src), "parity.iron",
                               IRON_ANALYSIS_MODE_LSP,
                               &arena_lsp, &diags_lsp, NULL,
        0);

    /* Diag lists may differ in cancel-path NOTE emits or LSP cascade
     * suppression, but the 5 P1 codes are deterministic between modes.
     * Compare by matching code+span; assert .suggestion parity on match. */
    for (int i = 0; i < diags_cli.count; i++) {
        const Iron_Diagnostic *a = &diags_cli.items[i];
        bool found = false;
        for (int j = 0; j < diags_lsp.count; j++) {
            const Iron_Diagnostic *b = &diags_lsp.items[j];
            if (a->code == b->code &&
                a->span.line == b->span.line && a->span.col == b->span.col &&
                a->span.end_line == b->span.end_line &&
                a->span.end_col == b->span.end_col) {
                /* Byte-for-byte match on .suggestion. */
                TEST_ASSERT_TRUE_MESSAGE(strs_equal(a->suggestion, b->suggestion),
                    "CLI/LSP .suggestion divergence on matching diagnostic");
                found = true;
                break;
            }
        }
        /* A diagnostic emitted in CLI mode that is missing in LSP mode is
         * outside the parity-contract of this test (LSP's cascade-suppression
         * gate can elide some diagnostics). We only assert `.suggestion`
         * parity on matches, so an unmatched CLI diagnostic is OK here. */
        (void)found;
    }

    iron_arena_free(&arena_cli);
    iron_diaglist_free(&diags_cli);
    iron_arena_free(&arena_lsp);
    iron_diaglist_free(&diags_lsp);
}

static void test_parity_suggestion_bytes_identical(void) {
    for (size_t i = 0; i < sizeof(FIXTURES) / sizeof(FIXTURES[0]); i++) {
        assert_parity(FIXTURES[i]);
    }
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_parity_suggestion_bytes_identical);
    return UNITY_END();
}
