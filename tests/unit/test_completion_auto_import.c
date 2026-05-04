/* Phase 4 Plan 04-03 Task 01 (EDIT-05, D-02) -- auto-import
 * additionalTextEdits builder tests.
 *
 * The builder walks Iron_Program->decls[] looking for the consecutive
 * top-of-file run of IRON_NODE_IMPORT_DECL. It honors existing aliases,
 * dedups on exact-bare match, and skips Iron_ErrorNode + mid-edit
 * imports (PITFALL C). For unit tests we parse a small source string,
 * invoke ilsp_auto_import_edit, and assert on the output. */

#include "unity.h"

#include "lsp/facade/edit/complete/auto_import.h"
#include "parser/ast.h"
#include "parser/parser.h"
#include "lexer/lexer.h"
#include "diagnostics/diagnostics.h"
#include "util/arena.h"
#include "vendor/stb_ds.h"

#include <string.h>

void setUp(void)    {}
void tearDown(void) {}

/* Helper: parse-only (no analyze) since auto_import only needs AST
 * structure (decl_kind + span + .path + .alias fields). */
static Iron_Program *parse_source(const char *src, Iron_Arena *arena,
                                    Iron_DiagList *diags) {
    Iron_Lexer lx = iron_lexer_create(src, "<test>", arena, diags);
    Iron_Token *toks = iron_lex_all(&lx);
    int tok_count = (int)arrlen(toks);
    Iron_Parser p = iron_parser_create(toks, tok_count, src, "<test>",
                                         arena, diags);
    Iron_Node *prog = iron_parse(&p);
    arrfree(toks);
    return (Iron_Program *)prog;
}

/* Test 1: empty file (zero top-level decls) -> insert at line 0 col 0
 * with trailing blank line. */
static void test_empty_file_inserts_at_top_with_blank_line(void) {
    Iron_Arena arena = iron_arena_create(64 * 1024);
    Iron_DiagList diags = iron_diaglist_create();
    const char *src = "";
    Iron_Program *prog = parse_source(src, &arena, &diags);

    IronLsp_AutoImportEdit edit = {0};
    const char *alias = NULL;
    ilsp_auto_import_edit(prog, NULL, "io", &arena, &edit, &alias);

    TEST_ASSERT_NOT_NULL(edit.new_text);
    TEST_ASSERT_EQUAL_UINT32(0, edit.line);
    TEST_ASSERT_EQUAL_UINT32(0, edit.character);
    TEST_ASSERT_EQUAL_STRING("import io\n\n", edit.new_text);
    TEST_ASSERT_NULL(alias);

    iron_diaglist_free(&diags);
    iron_arena_free(&arena);
}

/* Test 2: fixture with `import math` at top -> insert on line AFTER
 * the last import (0-indexed: line 1). */
static void test_insert_after_last_import(void) {
    Iron_Arena arena = iron_arena_create(64 * 1024);
    Iron_DiagList diags = iron_diaglist_create();
    const char *src = "import math\n\nfunc main() {}\n";
    Iron_Program *prog = parse_source(src, &arena, &diags);

    IronLsp_AutoImportEdit edit = {0};
    const char *alias = NULL;
    ilsp_auto_import_edit(prog, NULL, "io", &arena, &edit, &alias);

    TEST_ASSERT_NOT_NULL(edit.new_text);
    /* `import math` span is line 1 (1-indexed) -> anchor.end_line = 1;
     * insertion line (0-indexed LSP Position) = 1. */
    TEST_ASSERT_EQUAL_UINT32(1, edit.line);
    TEST_ASSERT_EQUAL_UINT32(0, edit.character);
    TEST_ASSERT_EQUAL_STRING("import io\n", edit.new_text);
    TEST_ASSERT_NULL(alias);

    iron_diaglist_free(&diags);
    iron_arena_free(&arena);
}

/* Test 3: fixture already has `import io` bare -> dedup, no edit. */
static void test_dedup_on_exact_bare_import(void) {
    Iron_Arena arena = iron_arena_create(64 * 1024);
    Iron_DiagList diags = iron_diaglist_create();
    const char *src = "import io\n\nfunc main() {}\n";
    Iron_Program *prog = parse_source(src, &arena, &diags);

    IronLsp_AutoImportEdit edit = {0};
    const char *alias = NULL;
    ilsp_auto_import_edit(prog, NULL, "io", &arena, &edit, &alias);

    TEST_ASSERT_NULL(edit.new_text);
    TEST_ASSERT_NULL(alias);

    iron_diaglist_free(&diags);
    iron_arena_free(&arena);
}

/* Test 4: fixture has `import io as o` -> no edit AND alias returned. */
static void test_alias_honor(void) {
    Iron_Arena arena = iron_arena_create(64 * 1024);
    Iron_DiagList diags = iron_diaglist_create();
    const char *src = "import io as o\n\nfunc main() {}\n";
    Iron_Program *prog = parse_source(src, &arena, &diags);

    IronLsp_AutoImportEdit edit = {0};
    const char *alias = NULL;
    ilsp_auto_import_edit(prog, NULL, "io", &arena, &edit, &alias);

    TEST_ASSERT_NULL(edit.new_text);
    TEST_ASSERT_NOT_NULL(alias);
    TEST_ASSERT_EQUAL_STRING("o", alias);

    iron_diaglist_free(&diags);
    iron_arena_free(&arena);
}

/* Test 5: nested module path (contains '.') -> no edit, no alias
 * (deferred per CONTEXT.md D-02 nested-path carve-out). */
static void test_nested_module_path_no_edit(void) {
    Iron_Arena arena = iron_arena_create(64 * 1024);
    Iron_DiagList diags = iron_diaglist_create();
    const char *src = "func main() {}\n";
    Iron_Program *prog = parse_source(src, &arena, &diags);

    IronLsp_AutoImportEdit edit = {0};
    const char *alias = NULL;
    ilsp_auto_import_edit(prog, NULL, "foo.bar.baz", &arena, &edit, &alias);

    TEST_ASSERT_NULL(edit.new_text);
    TEST_ASSERT_NULL(alias);

    iron_diaglist_free(&diags);
    iron_arena_free(&arena);
}

/* Test 6: file with NO imports but one top-level func -> insert at
 * line 0 col 0 with trailing blank line. */
static void test_no_imports_single_func_inserts_at_top(void) {
    Iron_Arena arena = iron_arena_create(64 * 1024);
    Iron_DiagList diags = iron_diaglist_create();
    const char *src = "func main() { val x = 1 }\n";
    Iron_Program *prog = parse_source(src, &arena, &diags);

    IronLsp_AutoImportEdit edit = {0};
    const char *alias = NULL;
    ilsp_auto_import_edit(prog, NULL, "io", &arena, &edit, &alias);

    TEST_ASSERT_NOT_NULL(edit.new_text);
    TEST_ASSERT_EQUAL_UINT32(0, edit.line);
    TEST_ASSERT_EQUAL_UINT32(0, edit.character);
    TEST_ASSERT_EQUAL_STRING("import io\n\n", edit.new_text);
    TEST_ASSERT_NULL(alias);

    iron_diaglist_free(&diags);
    iron_arena_free(&arena);
}

/* Test 7: multiple imports -> anchor is the LAST import. */
static void test_multiple_imports_anchor_last(void) {
    Iron_Arena arena = iron_arena_create(64 * 1024);
    Iron_DiagList diags = iron_diaglist_create();
    const char *src = "import math\nimport string\n\nfunc main() {}\n";
    Iron_Program *prog = parse_source(src, &arena, &diags);

    IronLsp_AutoImportEdit edit = {0};
    const char *alias = NULL;
    ilsp_auto_import_edit(prog, NULL, "io", &arena, &edit, &alias);

    TEST_ASSERT_NOT_NULL(edit.new_text);
    /* `import string` is on line 2 (1-indexed) -> end_line = 2;
     * insertion (0-indexed LSP) = 2. */
    TEST_ASSERT_EQUAL_UINT32(2, edit.line);
    TEST_ASSERT_EQUAL_UINT32(0, edit.character);
    TEST_ASSERT_EQUAL_STRING("import io\n", edit.new_text);
    TEST_ASSERT_NULL(alias);

    iron_diaglist_free(&diags);
    iron_arena_free(&arena);
}

/* Test 8: NULL program -> treats as empty file (same as empty-source
 * case: insert at line 0 col 0 with trailing blank line). This is the
 * safest fallback during cold-start when no parsed program exists yet. */
static void test_null_program_treated_as_empty(void) {
    Iron_Arena arena = iron_arena_create(64 * 1024);

    IronLsp_AutoImportEdit edit = {0};
    const char *alias = NULL;
    ilsp_auto_import_edit(NULL, NULL, "io", &arena, &edit, &alias);

    TEST_ASSERT_NOT_NULL(edit.new_text);
    TEST_ASSERT_EQUAL_UINT32(0, edit.line);
    TEST_ASSERT_EQUAL_UINT32(0, edit.character);
    TEST_ASSERT_EQUAL_STRING("import io\n\n", edit.new_text);
    TEST_ASSERT_NULL(alias);

    iron_arena_free(&arena);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_empty_file_inserts_at_top_with_blank_line);
    RUN_TEST(test_insert_after_last_import);
    RUN_TEST(test_dedup_on_exact_bare_import);
    RUN_TEST(test_alias_honor);
    RUN_TEST(test_nested_module_path_no_edit);
    RUN_TEST(test_no_imports_single_func_inserts_at_top);
    RUN_TEST(test_multiple_imports_anchor_last);
    RUN_TEST(test_null_program_treated_as_empty);
    return UNITY_END();
}
