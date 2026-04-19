/* Phase 5 Plan 05-03 Task 2 (FMT-03) -- decl-intersection unit tests
 * for ilsp_facade_format_range via the public facade entry.
 *
 * Drives the facade with hand-constructed IronLsp_Document fixtures
 * on 5 boundary cases (D-04 + RESEARCH Pitfall 4). Since
 * decl_intersects_range is file-static inside range_format.c, we
 * test it indirectly through ilsp_facade_format_range and assert on
 * the returned TextEdit count + ordering. This is the simpler
 * factoring per the plan's read_first note.
 *
 * Iron syntax used in fixtures:
 *   `func <name>() { ... }` -- top-level function decl (matches Iron
 *                              grammar; NOT `fn`).
 *   `val <name> = <expr>`   -- immutable binding (NOT `let`). */

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

/* Build a document via the production factory so all store-side
 * fields (line_idx, sha256, uri copy, etc.) are initialised the same
 * way the live server builds them. Returns NULL on OOM. */
static IronLsp_Document *make_doc(const char *src) {
    return ilsp_document_create("file:///tmp/fmt_range_test.iron",
                                 src, strlen(src), /*version*/ 1);
}

/* ── Test 1: range covers exactly one decl -> 1 TextEdit ──────────── */
static void test_range_covers_single_decl_returns_one_edit(void) {
    /* 6 lines, 2 funcs. Range covers lines 0-2 (func alpha). */
    const char *src =
        "func alpha() {\n"
        "    val x = 1\n"
        "}\n"
        "func beta() {\n"
        "    val y = 2\n"
        "}\n";
    IronLsp_Document *d = make_doc(src);
    TEST_ASSERT_NOT_NULL(d);

    Iron_Arena arena = iron_arena_create(64 * 1024);

    IronLsp_Range r;
    r.start.line = 0; r.start.character = 0;
    r.end.line   = 2; r.end.character   = 1;   /* mid-way of func alpha's '}' line */

    IronLsp_TextEditList list = ilsp_facade_format_range(
        d, NULL, r, NULL, &arena, NULL);
    TEST_ASSERT_EQUAL_INT(1, (int)list.count);

    iron_arena_free(&arena);
    ilsp_document_destroy(d);
}

/* ── Test 2: range covers both decls -> 2 TextEdits, DESC-sorted ── */
static void test_range_covers_two_decls_returns_two_edits_descending(void) {
    const char *src =
        "func alpha() {\n"
        "    val x = 1\n"
        "}\n"
        "func beta() {\n"
        "    val y = 2\n"
        "}\n";
    IronLsp_Document *d = make_doc(src);
    TEST_ASSERT_NOT_NULL(d);

    Iron_Arena arena = iron_arena_create(64 * 1024);

    IronLsp_Range r;
    r.start.line = 0; r.start.character = 0;
    r.end.line   = 5; r.end.character   = 1;   /* through end of file */

    IronLsp_TextEditList list = ilsp_facade_format_range(
        d, NULL, r, NULL, &arena, NULL);
    TEST_ASSERT_EQUAL_INT(2, (int)list.count);
    /* D-06 descending sort: edits[0] is later than edits[1]. */
    TEST_ASSERT_TRUE_MESSAGE(
        list.edits[0].range.start.line > list.edits[1].range.start.line,
        "edits must be sorted descending by start line");

    iron_arena_free(&arena);
    ilsp_document_destroy(d);
}

/* ── Test 3: range covers only blank lines -> empty list ─────────── */
static void test_range_covers_only_blank_lines_returns_empty(void) {
    const char *src =
        "func alpha() {\n"
        "    val x = 1\n"
        "}\n"
        "\n"         /* blank line 3 */
        "\n"         /* blank line 4 */
        "func beta() {\n"
        "    val y = 2\n"
        "}\n";
    IronLsp_Document *d = make_doc(src);
    TEST_ASSERT_NOT_NULL(d);

    Iron_Arena arena = iron_arena_create(64 * 1024);

    /* Request only blank lines 3-4 (0-based). */
    IronLsp_Range r;
    r.start.line = 3; r.start.character = 0;
    r.end.line   = 4; r.end.character   = 0;

    IronLsp_TextEditList list = ilsp_facade_format_range(
        d, NULL, r, NULL, &arena, NULL);
    TEST_ASSERT_EQUAL_INT(0, (int)list.count);

    iron_arena_free(&arena);
    ilsp_document_destroy(d);
}

/* ── Test 4: range starting on decl's last line includes that decl
 *
 * RESEARCH Pitfall 4: any-overlap intersection semantics. A range
 * whose start line equals decl A's end_line AND whose end line
 * falls inside decl B MUST include BOTH A and B in the output. */
static void test_range_starts_on_decl_last_line_includes_decl(void) {
    const char *src =
        "func alpha() {\n"     /* 0-based line 0 */
        "    val x = 1\n"      /* 1 */
        "}\n"                  /* 2 -- alpha ends here */
        "func beta() {\n"      /* 3 -- beta starts here */
        "    val y = 2\n"      /* 4 */
        "}\n";                 /* 5 */
    IronLsp_Document *d = make_doc(src);
    TEST_ASSERT_NOT_NULL(d);

    Iron_Arena arena = iron_arena_create(64 * 1024);

    IronLsp_Range r;
    r.start.line = 2; r.start.character = 0;    /* on alpha's '}' line */
    r.end.line   = 3; r.end.character   = 1;    /* mid-way on beta's signature line */

    IronLsp_TextEditList list = ilsp_facade_format_range(
        d, NULL, r, NULL, &arena, NULL);
    TEST_ASSERT_EQUAL_INT(2, (int)list.count);

    iron_arena_free(&arena);
    ilsp_document_destroy(d);
}

/* ── Test 5: parse error -> refuse with empty list (mirrors D-03) ── */
static void test_range_on_parse_error_returns_empty(void) {
    /* Unterminated parameter list: parser emits error node; formatter
     * refuses per D-03 (same contract as full-doc formatting). */
    const char *src = "func alpha(\n";
    IronLsp_Document *d = make_doc(src);
    TEST_ASSERT_NOT_NULL(d);

    Iron_Arena arena = iron_arena_create(64 * 1024);

    IronLsp_Range r;
    r.start.line = 0; r.start.character = 0;
    r.end.line   = 1; r.end.character   = 0;

    IronLsp_TextEditList list = ilsp_facade_format_range(
        d, NULL, r, NULL, &arena, NULL);
    TEST_ASSERT_EQUAL_INT(0, (int)list.count);

    iron_arena_free(&arena);
    ilsp_document_destroy(d);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_range_covers_single_decl_returns_one_edit);
    RUN_TEST(test_range_covers_two_decls_returns_two_edits_descending);
    RUN_TEST(test_range_covers_only_blank_lines_returns_empty);
    RUN_TEST(test_range_starts_on_decl_last_line_includes_decl);
    RUN_TEST(test_range_on_parse_error_returns_empty);
    return UNITY_END();
}
