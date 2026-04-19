/* Phase 5 Plan 05-01 (FMT-05): asserts iron_print_ast honors opts->use_tabs
 * and opts->indent_width end-to-end through the printer. Drives the printer
 * directly via a parsed AST and inspects the indentation in the output. */

#include "unity.h"
#include "fmt/format.h"
#include "fmt/options.h"
#include "parser/printer.h"
#include "lexer/lexer.h"
#include "parser/parser.h"
#include "parser/ast.h"
#include "util/arena.h"
#include "diagnostics/diagnostics.h"
#include "stb_ds.h"

#include <string.h>
#include <stdlib.h>

static Iron_Arena    arena;
static Iron_DiagList diags;

void setUp(void) {
    arena = iron_arena_create(64 * 1024);
    diags = iron_diaglist_create();
}

void tearDown(void) {
    iron_arena_free(&arena);
    iron_diaglist_free(&diags);
}

static Iron_Node *parse_src(const char *src) {
    Iron_Lexer  lexer  = iron_lexer_create(src, "test.iron", &arena, &diags);
    Iron_Token *tokens = iron_lex_all(&lexer);
    int         count  = (int)arrlen(tokens);
    Iron_Parser parser = iron_parser_create(tokens, count, src, "test.iron",
                                            &arena, &diags);
    Iron_Node  *ast    = iron_parse(&parser);
    arrfree(tokens);
    return ast;
}

/* Locate first "let" or first "val" token-substring on its own line and
 * count leading-whitespace bytes between the preceding '\n' and the first
 * non-whitespace char on that line. */
static size_t leading_ws_before(const char *out, const char *needle) {
    const char *hit = strstr(out, needle);
    if (!hit) return (size_t)-1;
    /* walk backwards to the start of the line */
    const char *p = hit;
    while (p > out && p[-1] != '\n') p--;
    return (size_t)(hit - p);
}

static int contains_tab_indent_before(const char *out, const char *needle) {
    const char *hit = strstr(out, needle);
    if (!hit) return 0;
    const char *p = hit;
    while (p > out && p[-1] != '\n') p--;
    /* Any TAB char in the leading-whitespace region? */
    while (p < hit) {
        if (*p == '\t') return 1;
        p++;
    }
    return 0;
}

void test_indent_width_4_yields_4_space_indent(void) {
    /* Body statements inside a func block get 1 indent level by the
     * existing printer; with indent_width=4 that's 4 leading spaces. */
    const char *src = "func f() { val x = 1 }";
    Iron_Node  *ast = parse_src(src);
    TEST_ASSERT_EQUAL_INT(0, diags.error_count);

    IronFmtOptions opts = iron_fmt_options_default();
    opts.indent_width = 4;
    opts.use_tabs     = false;

    char *out = iron_print_ast(ast, &opts, &arena);
    TEST_ASSERT_NOT_NULL(out);
    /* Some token should appear on an indented line; "val" works for the
     * body of the function. */
    size_t ws = leading_ws_before(out, "val");
    TEST_ASSERT_NOT_EQUAL((size_t)-1, ws);
    TEST_ASSERT_EQUAL_size_t(4, ws);
}

void test_use_tabs_yields_tab_indent(void) {
    const char *src = "func f() { val x = 1 }";
    Iron_Node  *ast = parse_src(src);
    TEST_ASSERT_EQUAL_INT(0, diags.error_count);

    IronFmtOptions opts = iron_fmt_options_default();
    opts.indent_width = 2;     /* irrelevant when use_tabs is true */
    opts.use_tabs     = true;

    char *out = iron_print_ast(ast, &opts, &arena);
    TEST_ASSERT_NOT_NULL(out);
    TEST_ASSERT_TRUE(contains_tab_indent_before(out, "val"));
}

void test_null_opts_yields_2_space_indent(void) {
    /* Pre-Phase-5 equivalence: NULL opts -> 2 spaces per level. */
    const char *src = "func f() { val x = 1 }";
    Iron_Node  *ast = parse_src(src);
    TEST_ASSERT_EQUAL_INT(0, diags.error_count);

    char *out = iron_print_ast(ast, NULL, &arena);
    TEST_ASSERT_NOT_NULL(out);
    size_t ws = leading_ws_before(out, "val");
    TEST_ASSERT_NOT_EQUAL((size_t)-1, ws);
    TEST_ASSERT_EQUAL_size_t(2, ws);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_indent_width_4_yields_4_space_indent);
    RUN_TEST(test_use_tabs_yields_tab_indent);
    RUN_TEST(test_null_opts_yields_2_space_indent);
    return UNITY_END();
}
