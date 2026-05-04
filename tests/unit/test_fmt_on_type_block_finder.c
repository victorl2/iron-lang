/* Phase 5 Plan 05-04 Task 2 (FMT-04, D-05) -- on-type formatting
 * block-finder + indent-edit emitter unit tests.
 *
 * Drives the facade with hand-constructed IronLsp_Document fixtures
 * on 5 boundary cases: top-level-fn body with misindented interior
 * line, already-correct fixture (minimal-edits contract), `}` typed
 * inside a string literal (RESEARCH Pitfall 5), parse-error refusal
 * (D-03 mirror), and non-brace trigger rejection (D-05 policy).
 *
 * Iron syntax used in fixtures:
 *   `func <name>() { ... }` -- top-level function decl (matches Iron
 *                              grammar; NOT `fn`).
 *   `val <name> = <expr>`   -- immutable binding (NOT `let`).
 *
 * find_enclosing_block + decl_intersects_range counterparts are
 * file-static inside on_type_format.c, so we drive behaviour through
 * the public entry ilsp_facade_format_on_type and assert on the
 * returned TextEdit count / ordering (simpler factoring per the plan's
 * read_first note). */

#include "unity.h"

#include "lsp/facade/fmt/format.h"
#include "lsp/store/document.h"
#include "lsp/facade/types.h"
#include "fmt/options.h"
#include "util/arena.h"

#include <stdatomic.h>
#include <stddef.h>
#include <string.h>

void setUp(void)    {}
void tearDown(void) {}

static IronLsp_Document *make_doc(const char *src) {
    return ilsp_document_create("file:///tmp/fmt_on_type_test.iron",
                                 src, strlen(src), /*version*/ 1);
}

/* ── Test 1: top-level fn body with wrong interior indent -> 1 edit ── */
static void test_on_type_top_level_fn_block_emits_indent_edits(void) {
    /* 3 lines, misindented body (1 space, should be 2). Typing `}` at
     * (2,0) (LSP 0-based = on the `}` line) should produce exactly one
     * TextEdit on line 1 replacing the leading " " with "  ". */
    const char *src =
        "func alpha() {\n"
        " val x = 1\n"
        "}\n";
    IronLsp_Document *d = make_doc(src);
    TEST_ASSERT_NOT_NULL(d);

    Iron_Arena arena = iron_arena_create(64 * 1024);

    IronLsp_Position pos;
    pos.line = 2; pos.character = 0;   /* on the `}` line */

    IronFmtOptions opts = iron_fmt_options_default();   /* indent_width=2 */

    IronLsp_TextEditList list = ilsp_facade_format_on_type(
        d, NULL, pos, '}', &opts, &arena, NULL);

    TEST_ASSERT_EQUAL_INT(1, (int)list.count);
    TEST_ASSERT_EQUAL_UINT32(1, list.edits[0].range.start.line);
    TEST_ASSERT_EQUAL_UINT32(0, list.edits[0].range.start.character);
    TEST_ASSERT_EQUAL_UINT32(1, list.edits[0].range.end.line);
    TEST_ASSERT_EQUAL_UINT32(1, list.edits[0].range.end.character);
    TEST_ASSERT_EQUAL_STRING("  ", list.edits[0].new_text);

    iron_arena_free(&arena);
    ilsp_document_destroy(d);
}

/* ── Test 2: already-correct indent -> 0 edits (minimal-edits rule) ── */
static void test_on_type_already_correct_indent_returns_empty(void) {
    const char *src =
        "func alpha() {\n"
        "  val x = 1\n"   /* already correct 2-space indent */
        "}\n";
    IronLsp_Document *d = make_doc(src);
    TEST_ASSERT_NOT_NULL(d);

    Iron_Arena arena = iron_arena_create(64 * 1024);

    IronLsp_Position pos;
    pos.line = 2; pos.character = 0;

    IronFmtOptions opts = iron_fmt_options_default();
    IronLsp_TextEditList list = ilsp_facade_format_on_type(
        d, NULL, pos, '}', &opts, &arena, NULL);

    /* Minimal edits contract: no edit emitted when line already matches
     * the canonical indent. */
    TEST_ASSERT_EQUAL_INT(0, (int)list.count);

    iron_arena_free(&arena);
    ilsp_document_destroy(d);
}

/* ── Test 3: `}` typed inside a string literal -> 0 edits ──────────
 *
 * RESEARCH Pitfall 5: `}` inside a string literal must not trigger
 * indent edits on surrounding lines. The AST-based walker finds the
 * enclosing function body block and iterates its lines; with
 * correctly-indented surrounding source, minimal-edits emits none. */
static void test_on_type_inside_string_literal_returns_empty(void) {
    const char *src =
        "func alpha() {\n"
        "  val s = \"foo}bar\"\n"
        "}\n";
    IronLsp_Document *d = make_doc(src);
    TEST_ASSERT_NOT_NULL(d);

    Iron_Arena arena = iron_arena_create(64 * 1024);

    /* Position on a byte INSIDE the string literal.
     * Line layout (0-based char index for line 1):
     *   0..1 = "  "
     *   2..4 = "val"
     *   5    = ' '
     *   6    = 's'
     *   7    = ' '
     *   8    = '='
     *   9    = ' '
     *   10   = '"'
     *   11,12,13 = 'f','o','o'
     *   14   = '}'
     *   15..17 = 'b','a','r'
     *   18   = '"'
     * Position (1, 14) sits on the `}` inside the string. */
    IronLsp_Position pos;
    pos.line = 1; pos.character = 14;

    IronFmtOptions opts = iron_fmt_options_default();
    IronLsp_TextEditList list = ilsp_facade_format_on_type(
        d, NULL, pos, '}', &opts, &arena, NULL);

    /* Surrounding source is already canonical; no edits emitted. */
    TEST_ASSERT_EQUAL_INT(0, (int)list.count);

    iron_arena_free(&arena);
    ilsp_document_destroy(d);
}

/* ── Test 4: parse error -> refuse with empty list (D-03 mirror) ── */
static void test_on_type_parse_error_returns_empty(void) {
    /* Unterminated parameter list: parser emits error node; formatter
     * refuses per D-03 (same contract as full-doc / range formatting). */
    const char *src = "func alpha(\n}";
    IronLsp_Document *d = make_doc(src);
    TEST_ASSERT_NOT_NULL(d);

    Iron_Arena arena = iron_arena_create(64 * 1024);

    IronLsp_Position pos;
    pos.line = 1; pos.character = 0;

    IronFmtOptions opts = iron_fmt_options_default();
    IronLsp_TextEditList list = ilsp_facade_format_on_type(
        d, NULL, pos, '}', &opts, &arena, NULL);

    TEST_ASSERT_EQUAL_INT(0, (int)list.count);

    iron_arena_free(&arena);
    ilsp_document_destroy(d);
}

/* ── Test 5: non-brace trigger -> empty (D-05 only `}` in v1) ─────── */
static void test_on_type_non_brace_trigger_returns_empty(void) {
    const char *src =
        "func alpha() {\n"
        " val x = 1\n"       /* deliberately misindented */
        "}\n";
    IronLsp_Document *d = make_doc(src);
    TEST_ASSERT_NOT_NULL(d);

    Iron_Arena arena = iron_arena_create(64 * 1024);

    IronLsp_Position pos;
    pos.line = 1; pos.character = 11;

    IronFmtOptions opts = iron_fmt_options_default();
    /* Trigger `;` is NOT advertised in v1 (D-05). Even though the body
     * is misindented, a non-`}` trigger must be rejected upfront with
     * an empty TextEdit[] -- defense against stale client state. */
    IronLsp_TextEditList list = ilsp_facade_format_on_type(
        d, NULL, pos, ';', &opts, &arena, NULL);

    TEST_ASSERT_EQUAL_INT(0, (int)list.count);

    iron_arena_free(&arena);
    ilsp_document_destroy(d);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_on_type_top_level_fn_block_emits_indent_edits);
    RUN_TEST(test_on_type_already_correct_indent_returns_empty);
    RUN_TEST(test_on_type_inside_string_literal_returns_empty);
    RUN_TEST(test_on_type_parse_error_returns_empty);
    RUN_TEST(test_on_type_non_brace_trigger_returns_empty);
    return UNITY_END();
}
