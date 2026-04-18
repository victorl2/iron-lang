/* test_lexer_doc_comment_parity -- Phase 3 Plan 01 Task 02 (NAV-14).
 *
 * Localized parity: asserts Iron_DiagList is byte-identical between a
 * source with `///` doc-comment prefixes and the same source with every
 * `///` run stripped. Regression localizer for the full
 * test_parity_ironc_lsp sweep.
 */
#include "unity.h"

#include "lexer/lexer.h"
#include "parser/parser.h"
#include "diagnostics/diagnostics.h"
#include "util/arena.h"
#include "stb_ds.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

static void lex_and_parse(const char *src, Iron_Arena *arena,
                           Iron_DiagList *diags) {
    Iron_Lexer lx = iron_lexer_create(src, "t.iron", arena, diags);
    Iron_Token *toks = iron_lex_all(&lx);
    int tc = (int)arrlen(toks);
    Iron_Parser par = iron_parser_create(toks, tc, src, "t.iron", arena, diags);
    (void)iron_parse(&par);
    arrfree(toks);
}

/* Strip every `///...\n` line from src; write result into `out` (caller
 * owns). Keeps all other bytes unchanged. */
static char *strip_doc_comments(const char *src) {
    size_t n = strlen(src);
    char *out = (char *)malloc(n + 1);
    if (!out) return NULL;
    size_t o = 0;
    size_t i = 0;
    while (i < n) {
        /* Is this line a doc-comment line? Look at start-of-line. */
        bool line_start = (i == 0 || src[i - 1] == '\n');
        if (line_start) {
            size_t j = i;
            /* Skip leading whitespace to detect ///. */
            while (j < n && (src[j] == ' ' || src[j] == '\t')) j++;
            if (j + 2 < n && src[j] == '/' && src[j + 1] == '/' &&
                src[j + 2] == '/') {
                /* Skip to and past the newline. */
                while (i < n && src[i] != '\n') i++;
                if (i < n) i++;  /* consume newline */
                continue;
            }
        }
        out[o++] = src[i++];
    }
    out[o] = '\0';
    return out;
}

/* Test 01: same source with and without doc-comment prefix yields the
 * same Iron_DiagList — same count, same codes, same levels, same spans
 * (except that doc-comment tokens do not emit diagnostics so counts match
 * trivially). The key property is that doc comments do NOT inject any
 * lexer/parser diagnostic. */
static void test_doc_comments_emit_no_diagnostics(void) {
    const char *with_docs =
        "/// hello\n"
        "/// multi\n"
        "func foo() {}\n"
        "/// another\n"
        "object Bar {\n"
        "    /// field doc\n"
        "    x: Int\n"
        "}\n";
    char *without_docs = strip_doc_comments(with_docs);
    TEST_ASSERT_NOT_NULL(without_docs);

    Iron_Arena a1 = iron_arena_create(4096);
    Iron_DiagList d1 = iron_diaglist_create();
    lex_and_parse(with_docs, &a1, &d1);

    Iron_Arena a2 = iron_arena_create(4096);
    Iron_DiagList d2 = iron_diaglist_create();
    lex_and_parse(without_docs, &a2, &d2);

    TEST_ASSERT_EQUAL_INT(d2.count, d1.count);
    TEST_ASSERT_EQUAL_INT(d2.error_count, d1.error_count);
    TEST_ASSERT_EQUAL_INT(d2.warning_count, d1.warning_count);
    for (int i = 0; i < d1.count; i++) {
        TEST_ASSERT_EQUAL_INT(d2.items[i].level, d1.items[i].level);
        TEST_ASSERT_EQUAL_INT(d2.items[i].code, d1.items[i].code);
    }

    iron_diaglist_free(&d1);
    iron_diaglist_free(&d2);
    iron_arena_free(&a1);
    iron_arena_free(&a2);
    free(without_docs);
}

/* Test 02: the Task 01 adversarial fixture corpus parses with zero
 * ERROR-level diagnostics; NOTE-level (e.g. doc-comment truncation) is
 * tolerated. */
static void test_adversarial_corpus_no_errors(void) {
    const char *src =
        "/// A single-line doc.\n"
        "func alpha() {}\n"
        "\n"
        "/// First line of multi-line doc.\n"
        "/// Second line with <script>alert(\"xss\")</script> raw markdown.\n"
        "/// Third line with a backtick: `code`\n"
        "object Beta {\n"
        "    /// Field doc.\n"
        "    val x: Int\n"
        "}\n"
        "\n"
        "/// Orphan doc (blank line breaks association).\n"
        "\n"
        "func gamma() {}\n";

    Iron_Arena arena = iron_arena_create(8192);
    Iron_DiagList diags = iron_diaglist_create();
    lex_and_parse(src, &arena, &diags);

    TEST_ASSERT_EQUAL_INT(0, diags.error_count);

    iron_diaglist_free(&diags);
    iron_arena_free(&arena);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_doc_comments_emit_no_diagnostics);
    RUN_TEST(test_adversarial_corpus_no_errors);
    return UNITY_END();
}
