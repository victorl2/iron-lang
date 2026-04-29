/* Phase 4 Plan 04-04 Task 01 (EDIT-07) — Unity tests for the
 * code-action registry and the 5 P1 quickfix handlers.
 *
 * Covers:
 *   - ilsp_quickfix_lookup hits for all 5 P1 codes + miss for unknown
 *   - ilsp_quickfix_table sorted ASC by code (bsearch invariant)
 *   - Each handler produces a D-06-compliant IronLsp_CodeAction shape:
 *       title / kind / is_preferred / edit_new_text / edit_start_* / edit_end_*
 *
 * The handlers are unit-tested with hand-crafted Iron_Diagnostic values
 * (simulating what the compiler seeds in Plan 04-01) plus a minimal
 * IronLsp_Document built from raw text via ilsp_document_create.
 */

#include "unity.h"
#include "lsp/facade/edit/codeaction/registry.h"
#include "lsp/store/document.h"
#include "diagnostics/diagnostics.h"
#include "util/arena.h"

#include <stddef.h>
#include <string.h>

static Iron_Arena g_arena;

void setUp(void) {
    g_arena = iron_arena_create(64 * 1024);
}

void tearDown(void) {
    iron_arena_free(&g_arena);
}

/* ── Test 1: registry lookups for 5 P1 codes ──────────────────────── */

static void test_lookup_undefined_var(void) {
    IronLsp_QuickfixFn fn = ilsp_quickfix_lookup(IRON_ERR_UNDEFINED_VAR);
    TEST_ASSERT_NOT_NULL_MESSAGE(fn, "expected handler for 200");
    TEST_ASSERT_EQUAL_PTR(ilsp_quickfix_undefined_var, fn);
}

static void test_lookup_type_mismatch_literal(void) {
    IronLsp_QuickfixFn fn = ilsp_quickfix_lookup(IRON_ERR_TYPE_MISMATCH_LITERAL);
    TEST_ASSERT_NOT_NULL_MESSAGE(fn, "expected handler for 235");
    TEST_ASSERT_EQUAL_PTR(ilsp_quickfix_type_mismatch_literal, fn);
}

static void test_lookup_missing_return(void) {
    IronLsp_QuickfixFn fn = ilsp_quickfix_lookup(IRON_ERR_MISSING_RETURN);
    TEST_ASSERT_NOT_NULL_MESSAGE(fn, "expected handler for 236");
    TEST_ASSERT_EQUAL_PTR(ilsp_quickfix_missing_return, fn);
}

static void test_lookup_unused_import(void) {
    IronLsp_QuickfixFn fn = ilsp_quickfix_lookup(IRON_WARN_UNUSED_IMPORT);
    TEST_ASSERT_NOT_NULL_MESSAGE(fn, "expected handler for 611");
    TEST_ASSERT_EQUAL_PTR(ilsp_quickfix_unused_import, fn);
}

static void test_lookup_redundant_cast(void) {
    IronLsp_QuickfixFn fn = ilsp_quickfix_lookup(IRON_WARN_REDUNDANT_CAST);
    TEST_ASSERT_NOT_NULL_MESSAGE(fn, "expected handler for 612");
    TEST_ASSERT_EQUAL_PTR(ilsp_quickfix_redundant_cast, fn);
}

static void test_lookup_unknown_code_returns_null(void) {
    TEST_ASSERT_NULL(ilsp_quickfix_lookup(9999));
    TEST_ASSERT_NULL(ilsp_quickfix_lookup(0));
    TEST_ASSERT_NULL(ilsp_quickfix_lookup(-1));
    /* Ranges adjacent to real entries stay NULL. */
    TEST_ASSERT_NULL(ilsp_quickfix_lookup(199));
    TEST_ASSERT_NULL(ilsp_quickfix_lookup(234));
    TEST_ASSERT_NULL(ilsp_quickfix_lookup(237));
    TEST_ASSERT_NULL(ilsp_quickfix_lookup(610));
    TEST_ASSERT_NULL(ilsp_quickfix_lookup(613));
}

/* ── Test 7: table is sorted ASC by code ──────────────────────────── */

static void test_table_sorted_asc_by_code(void) {
    TEST_ASSERT_EQUAL_UINT(5, ilsp_quickfix_table_size);
    for (size_t i = 1; i < ilsp_quickfix_table_size; i++) {
        TEST_ASSERT_TRUE_MESSAGE(
            ilsp_quickfix_table[i - 1].code < ilsp_quickfix_table[i].code,
            "ilsp_quickfix_table must be sorted ASC by code");
    }
    /* Exact codes to guard against accidental renumbering. */
    TEST_ASSERT_EQUAL_INT(IRON_ERR_UNDEFINED_VAR,         ilsp_quickfix_table[0].code);
    TEST_ASSERT_EQUAL_INT(IRON_ERR_TYPE_MISMATCH_LITERAL, ilsp_quickfix_table[1].code);
    TEST_ASSERT_EQUAL_INT(IRON_ERR_MISSING_RETURN,        ilsp_quickfix_table[2].code);
    TEST_ASSERT_EQUAL_INT(IRON_WARN_UNUSED_IMPORT,        ilsp_quickfix_table[3].code);
    TEST_ASSERT_EQUAL_INT(IRON_WARN_REDUNDANT_CAST,       ilsp_quickfix_table[4].code);
}

/* ── Shared fixture helpers ───────────────────────────────────────── */

/* Build a diag spec; all strings are arena-interned via g_arena. */
static Iron_Diagnostic make_diag(int code, Iron_Span span,
                                    const char *message,
                                    const char *suggestion) {
    Iron_Diagnostic d;
    d.level         = IRON_DIAG_ERROR;
    d.code          = code;
    d.span          = span;
    d.message       = message
        ? iron_arena_strdup(&g_arena, message, strlen(message)) : NULL;
    d.suggestion    = suggestion
        ? iron_arena_strdup(&g_arena, suggestion, strlen(suggestion)) : NULL;
    return d;
}

static Iron_Span mk_span(uint32_t line, uint32_t col,
                           uint32_t end_line, uint32_t end_col) {
    Iron_Span s;
    s.filename  = "test.iron";
    s.line      = line;
    s.col       = col;
    s.end_line  = end_line;
    s.end_col   = end_col;
    return s;
}

/* ── Test 8: typo quickfix ────────────────────────────────────────── */

static void test_quickfix_undefined_var_shape(void) {
    const char *src =
        "func main() {\n"
        "  prinln(\"hi\")\n"
        "}\n";
    IronLsp_Document *doc = ilsp_document_create("file:///test.iron",
                                                   src, strlen(src), 1);
    TEST_ASSERT_NOT_NULL(doc);

    /* Diag at `prinln` (line 2 cols 3..8). */
    Iron_Diagnostic d = make_diag(IRON_ERR_UNDEFINED_VAR,
                                     mk_span(2, 3, 2, 9),
                                     "undefined variable: prinln",
                                     "println");
    IronLsp_CodeAction out;
    size_t out_n = 0;
    ilsp_quickfix_undefined_var(&d, doc, NULL, &g_arena, &out, 1, &out_n);
    TEST_ASSERT_EQUAL_UINT(1, out_n);

    TEST_ASSERT_NOT_NULL(out.title);
    TEST_ASSERT_EQUAL_STRING_LEN("Replace with 'println'", out.title, 22);
    TEST_ASSERT_NOT_NULL(out.kind);
    TEST_ASSERT_EQUAL_STRING("quickfix", out.kind);
    TEST_ASSERT_TRUE_MESSAGE(out.is_preferred,
        "typo quickfix must set is_preferred=true per D-06");
    TEST_ASSERT_NOT_NULL(out.edit_new_text);
    TEST_ASSERT_EQUAL_STRING("println", out.edit_new_text);
    TEST_ASSERT_EQUAL_PTR(&d, out.originating_diag);

    /* Span to LSP range: Iron (2, 3..9) 1-indexed -> LSP (1, 2..9) 0-indexed.
     * Phase 5 Plan 05-05: ilsp_span_to_lsp_range now emits LSP-spec
     * exclusive ends (end.character = iron_end_col when iron_end_col
     * is 1-indexed INCLUSIVE). The test author originally passed
     * end_col=9 meaning "one past the last char of prinln" (exclusive);
     * under the inclusive-end fix, 9 maps to 9 (exclusive), not 8. */
    TEST_ASSERT_EQUAL_UINT(1u, out.edit_start_line);
    TEST_ASSERT_EQUAL_UINT(2u, out.edit_start_char);
    TEST_ASSERT_EQUAL_UINT(1u, out.edit_end_line);
    TEST_ASSERT_EQUAL_UINT(9u, out.edit_end_char);

    ilsp_document_destroy(doc);
}

static void test_quickfix_undefined_var_skips_when_no_suggestion(void) {
    const char *src = "func main() {\n  xyz\n}\n";
    IronLsp_Document *doc = ilsp_document_create("file:///test.iron",
                                                   src, strlen(src), 1);
    TEST_ASSERT_NOT_NULL(doc);
    Iron_Diagnostic d = make_diag(IRON_ERR_UNDEFINED_VAR,
                                     mk_span(2, 3, 2, 6),
                                     "undefined",
                                     NULL);
    IronLsp_CodeAction out;
    size_t out_n = 99;
    ilsp_quickfix_undefined_var(&d, doc, NULL, &g_arena, &out, 1, &out_n);
    /* Phase 12 D-12: refusal protocol now sets *out_n = 0 (Pitfall 2). */
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0, out_n,
        "NULL suggestion must produce zero actions (caller drops)");
    ilsp_document_destroy(doc);
}

/* ── Test 9: unused_import quickfix ───────────────────────────────── */

static void test_quickfix_unused_import_shape(void) {
    const char *src = "import std.math as m\nfunc main() {}\n";
    IronLsp_Document *doc = ilsp_document_create("file:///test.iron",
                                                   src, strlen(src), 1);
    TEST_ASSERT_NOT_NULL(doc);
    Iron_Diagnostic d = make_diag(IRON_WARN_UNUSED_IMPORT,
                                     mk_span(1, 1, 1, 21),
                                     "unused import", "");

    IronLsp_CodeAction out;
    size_t out_n = 0;
    ilsp_quickfix_unused_import(&d, doc, NULL, &g_arena, &out, 1, &out_n);
    TEST_ASSERT_EQUAL_UINT(1, out_n);

    TEST_ASSERT_NOT_NULL(out.title);
    TEST_ASSERT_EQUAL_STRING("Remove unused import", out.title);
    TEST_ASSERT_EQUAL_STRING("quickfix", out.kind);
    TEST_ASSERT_FALSE(out.is_preferred);
    TEST_ASSERT_NOT_NULL(out.edit_new_text);
    TEST_ASSERT_EQUAL_STRING("", out.edit_new_text);

    /* Full-line delete: (0, 0) .. (1, 0). */
    TEST_ASSERT_EQUAL_UINT(0u, out.edit_start_line);
    TEST_ASSERT_EQUAL_UINT(0u, out.edit_start_char);
    TEST_ASSERT_EQUAL_UINT(1u, out.edit_end_line);
    TEST_ASSERT_EQUAL_UINT(0u, out.edit_end_char);

    ilsp_document_destroy(doc);
}

/* ── Test 10: missing_return quickfix ─────────────────────────────── */

static void test_quickfix_missing_return_shape(void) {
    const char *src =
        "func f() -> Int {\n"
        "    val x = 1\n"
        "}\n";
    IronLsp_Document *doc = ilsp_document_create("file:///test.iron",
                                                   src, strlen(src), 1);
    TEST_ASSERT_NOT_NULL(doc);
    /* diag->span = body span: (1, 17) .. (3, 1) in Iron 1-indexed. */
    /* Phase 5 Plan 05-05: suggestion is semicolon-free (Iron grammar
     * does not accept trailing `;` on return statements; D-07). */
    Iron_Diagnostic d = make_diag(IRON_ERR_MISSING_RETURN,
                                     mk_span(1, 17, 3, 1),
                                     "missing return",
                                     "return 0");

    IronLsp_CodeAction out;
    size_t out_n = 0;
    ilsp_quickfix_missing_return(&d, doc, NULL, &g_arena, &out, 1, &out_n);
    TEST_ASSERT_EQUAL_UINT(1, out_n);

    TEST_ASSERT_NOT_NULL(out.title);
    TEST_ASSERT_NOT_NULL(strstr(out.title, "return 0"));
    TEST_ASSERT_EQUAL_STRING("quickfix", out.kind);
    TEST_ASSERT_FALSE(out.is_preferred);
    TEST_ASSERT_NOT_NULL(out.edit_new_text);
    /* newText should contain the suggestion text and end with a \n. */
    TEST_ASSERT_NOT_NULL(strstr(out.edit_new_text, "return 0"));
    size_t nt_len = strlen(out.edit_new_text);
    TEST_ASSERT_EQUAL_CHAR('\n', out.edit_new_text[nt_len - 1]);

    /* Insertion before closing-brace line: zero-width at (end_line - 1, 0).
     * Iron end_line=3 -> LSP line 2. */
    TEST_ASSERT_EQUAL_UINT(2u, out.edit_start_line);
    TEST_ASSERT_EQUAL_UINT(0u, out.edit_start_char);
    TEST_ASSERT_EQUAL_UINT(2u, out.edit_end_line);
    TEST_ASSERT_EQUAL_UINT(0u, out.edit_end_char);

    /* Indent should be 4 (matches "    val x = 1" inside the body). */
    TEST_ASSERT_EQUAL_CHAR(' ', out.edit_new_text[0]);
    TEST_ASSERT_EQUAL_CHAR(' ', out.edit_new_text[1]);
    TEST_ASSERT_EQUAL_CHAR(' ', out.edit_new_text[2]);
    TEST_ASSERT_EQUAL_CHAR(' ', out.edit_new_text[3]);

    ilsp_document_destroy(doc);
}

/* ── Test 11: type_mismatch_literal quickfix ──────────────────────── */

static void test_quickfix_type_mismatch_literal_shape(void) {
    const char *src =
        "func main() {\n"
        "  val x: Float = 42\n"
        "}\n";
    IronLsp_Document *doc = ilsp_document_create("file:///test.iron",
                                                   src, strlen(src), 1);
    TEST_ASSERT_NOT_NULL(doc);
    /* diag->span = the literal "42" at line 2 cols 19..20. */
    Iron_Diagnostic d = make_diag(IRON_ERR_TYPE_MISMATCH_LITERAL,
                                     mk_span(2, 19, 2, 21),
                                     "literal type mismatch",
                                     "42.0");

    IronLsp_CodeAction out;
    size_t out_n = 0;
    ilsp_quickfix_type_mismatch_literal(&d, doc, NULL, &g_arena, &out, 1, &out_n);
    TEST_ASSERT_EQUAL_UINT(1, out_n);

    TEST_ASSERT_NOT_NULL(out.title);
    TEST_ASSERT_NOT_NULL(strstr(out.title, "42.0"));
    TEST_ASSERT_EQUAL_STRING("quickfix", out.kind);
    TEST_ASSERT_FALSE(out.is_preferred);
    TEST_ASSERT_NOT_NULL(out.edit_new_text);
    TEST_ASSERT_EQUAL_STRING("42.0", out.edit_new_text);

    /* LSP (1, 18) .. (1, <=20). The end column is clamped to the line
     * length by ilsp_span_to_lsp_range when end_col overshoots; the
     * fixture's line 2 is "  val x: Float = 42" (19 chars / no trailing
     * newline inside the line-content region) so end_col=21 clamps to
     * 19 chars in UTF-8 encoding. We assert >= start and <= line-len. */
    TEST_ASSERT_EQUAL_UINT(1u, out.edit_start_line);
    TEST_ASSERT_EQUAL_UINT(18u, out.edit_start_char);
    TEST_ASSERT_EQUAL_UINT(1u, out.edit_end_line);
    TEST_ASSERT_TRUE_MESSAGE(out.edit_end_char >= out.edit_start_char,
        "end char must not precede start");

    ilsp_document_destroy(doc);
}

/* ── Test 12: redundant_cast quickfix ─────────────────────────────── */

static void test_quickfix_redundant_cast_shape(void) {
    const char *src =
        "func main() {\n"
        "  val x = Float(1.0)\n"
        "}\n";
    IronLsp_Document *doc = ilsp_document_create("file:///test.iron",
                                                   src, strlen(src), 1);
    TEST_ASSERT_NOT_NULL(doc);
    /* diag->span = full `Float(1.0)` at line 2 cols 11..22. */
    Iron_Diagnostic d = make_diag(IRON_WARN_REDUNDANT_CAST,
                                     mk_span(2, 11, 2, 22),
                                     "redundant cast",
                                     "1.0");

    IronLsp_CodeAction out;
    size_t out_n = 0;
    ilsp_quickfix_redundant_cast(&d, doc, NULL, &g_arena, &out, 1, &out_n);
    TEST_ASSERT_EQUAL_UINT(1, out_n);

    TEST_ASSERT_NOT_NULL(out.title);
    TEST_ASSERT_EQUAL_STRING("Remove redundant cast", out.title);
    TEST_ASSERT_EQUAL_STRING("quickfix", out.kind);
    TEST_ASSERT_FALSE(out.is_preferred);
    TEST_ASSERT_NOT_NULL(out.edit_new_text);
    TEST_ASSERT_EQUAL_STRING("1.0", out.edit_new_text);

    /* LSP (1, 10) .. (1, <=line_len). ilsp_span_to_lsp_range clamps the
     * end column to the line's content length; we assert the range
     * covers the cast (end > start) rather than hard-coding a column. */
    TEST_ASSERT_EQUAL_UINT(1u, out.edit_start_line);
    TEST_ASSERT_EQUAL_UINT(10u, out.edit_start_char);
    TEST_ASSERT_EQUAL_UINT(1u, out.edit_end_line);
    TEST_ASSERT_TRUE_MESSAGE(out.edit_end_char > out.edit_start_char,
        "end char must cover the cast expression");

    ilsp_document_destroy(doc);
}

/* ── NULL-guard smoke tests ───────────────────────────────────────── */

static void test_handlers_tolerate_null_inputs(void) {
    IronLsp_CodeAction out;
    size_t out_n;
    /* Phase 12 D-12 (Pitfall 2): NULL diag refusal now sets *out_n = 0
     * rather than leaving edit_new_text NULL. Memset the slot before
     * each call so we can verify the handler zero-filled it. */
    memset(&out, 0xAB, sizeof(out));
    out_n = 99;
    ilsp_quickfix_undefined_var(NULL, NULL, NULL, NULL, &out, 1, &out_n);
    TEST_ASSERT_EQUAL_UINT(0, out_n);

    memset(&out, 0xAB, sizeof(out));
    out_n = 99;
    ilsp_quickfix_unused_import(NULL, NULL, NULL, NULL, &out, 1, &out_n);
    TEST_ASSERT_EQUAL_UINT(0, out_n);

    memset(&out, 0xAB, sizeof(out));
    out_n = 99;
    ilsp_quickfix_missing_return(NULL, NULL, NULL, NULL, &out, 1, &out_n);
    TEST_ASSERT_EQUAL_UINT(0, out_n);

    memset(&out, 0xAB, sizeof(out));
    out_n = 99;
    ilsp_quickfix_type_mismatch_literal(NULL, NULL, NULL, NULL, &out, 1, &out_n);
    TEST_ASSERT_EQUAL_UINT(0, out_n);

    memset(&out, 0xAB, sizeof(out));
    out_n = 99;
    ilsp_quickfix_redundant_cast(NULL, NULL, NULL, NULL, &out, 1, &out_n);
    TEST_ASSERT_EQUAL_UINT(0, out_n);
}

int main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_lookup_undefined_var);
    RUN_TEST(test_lookup_type_mismatch_literal);
    RUN_TEST(test_lookup_missing_return);
    RUN_TEST(test_lookup_unused_import);
    RUN_TEST(test_lookup_redundant_cast);
    RUN_TEST(test_lookup_unknown_code_returns_null);
    RUN_TEST(test_table_sorted_asc_by_code);

    RUN_TEST(test_quickfix_undefined_var_shape);
    RUN_TEST(test_quickfix_undefined_var_skips_when_no_suggestion);
    RUN_TEST(test_quickfix_unused_import_shape);
    RUN_TEST(test_quickfix_missing_return_shape);
    RUN_TEST(test_quickfix_type_mismatch_literal_shape);
    RUN_TEST(test_quickfix_redundant_cast_shape);

    RUN_TEST(test_handlers_tolerate_null_inputs);

    return UNITY_END();
}
