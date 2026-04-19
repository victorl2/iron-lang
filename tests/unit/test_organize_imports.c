/* Phase 4 Plan 04-05 Task 01 (EDIT-09, D-08) -- organizeImports facade
 * Unity tests.
 *
 * Covers the 8 fixture cases locked in CONTEXT.md D-08:
 *   1. already-organized (idempotent)
 *   2. unsorted-within-group (alpha-sort)
 *   3. cross-group-interleaved (stdlib/deps/local order)
 *   4. duplicates (exact dedup)
 *   5. unused - bulk_analyze_done=true (removal active)
 *   6. unused - bulk_analyze_done=false (skipped + warning)
 *   7. aliased imports (both kept)
 *   8. doc-commented import + pre-import file-header (preservation)
 *
 * Strategy:
 *   - Parse Iron source in-process (parser only; no analyzer run needed
 *     because the organize_imports facade consults raw AST + diags).
 *   - Construct a minimal IronLsp_WorkspaceIndex fixture with the
 *     stdlib_cache populated for well-known stems (so classification can
 *     distinguish stdlib vs local). dep_map stays empty -- dep-path
 *     fixtures classify as local, which is fine for the sort-stability
 *     assertions (the test fixtures don't claim any import is a dep).
 *   - For Tests 5/6 we hand-build an Iron_DiagList carrying an
 *     IRON_WARN_UNUSED_IMPORT at the relevant line.
 */

#include "unity.h"

#include "lsp/facade/edit/codeaction/organize_imports.h"
#include "lsp/store/stdlib_cache.h"
#include "lsp/store/dep_map.h"
#include "lsp/store/workspace_index.h"

#include "parser/ast.h"
#include "parser/parser.h"
#include "lexer/lexer.h"
#include "diagnostics/diagnostics.h"
#include "util/arena.h"
#include "vendor/stb_ds.h"

#include <stdbool.h>
#include <string.h>

void setUp(void)    {}
void tearDown(void) {}

/* Stub: the organize_imports facade guards its lookup on wi->deps != NULL
 * and our test fixtures always leave deps == NULL, so this is never
 * actually invoked. Defined here to avoid link-pulling dep_map.c +
 * its transitive pkg/resolver/fetcher/lockfile/toml dependencies into
 * the unit-test binary. The ironls production build links the real
 * dep_map.c -- see CMakeLists.txt. */
IronLsp_DepEntry *ilsp_dep_map_lookup(IronLsp_DepMap *dm,
                                       const char *dep_name) {
    (void)dm;
    (void)dep_name;
    return NULL;
}

/* ── Parse helper ─────────────────────────────────────────────── */

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

/* ── Workspace-index fixture helpers ──────────────────────────── */

/* Build a minimal IronLsp_WorkspaceIndex whose stdlib_cache is the
 * process singleton (already seeded at first call), whose deps is NULL,
 * and whose bulk_analyze_done is set per the test's requirement. Note:
 * we do NOT call ilsp_workspace_index_create -- it would attempt to
 * walk a workspace_root, which we don't have. The facade only touches
 * wi->stdlib, wi->deps, wi->bulk_analyze_done fields on this path. */
static IronLsp_WorkspaceIndex *make_wi(bool bulk_done) {
    static IronLsp_WorkspaceIndex wi;
    memset(&wi, 0, sizeof(wi));
    wi.stdlib = ilsp_stdlib_cache_init(NULL);  /* singleton; safe repeat */
    wi.deps   = NULL;
    wi.bulk_analyze_done = bulk_done;
    return &wi;
}

/* Build a diag list containing one IRON_WARN_UNUSED_IMPORT at `line`. */
static Iron_DiagList build_unused_diaglist(Iron_Arena *arena,
                                             const char *filename,
                                             uint32_t line) {
    Iron_DiagList d = iron_diaglist_create();
    Iron_Span span = iron_span_make(filename, line, 1, line, 2);
    iron_diag_emit(&d, arena, IRON_DIAG_WARNING,
                   IRON_WARN_UNUSED_IMPORT, span,
                   "unused import", "");
    return d;
}

/* ── Tests ────────────────────────────────────────────────────── */

/* Test 1: already-organized file -> identical reformatted output
 * (idempotent). Source uses two known-stdlib names (io + math) only to
 * stay in a single group. Expected: "import io\nimport math\n". */
static void test_already_organized_single_group(void) {
    Iron_Arena     arena = iron_arena_create(64 * 1024);
    Iron_DiagList  diags = iron_diaglist_create();
    const char    *src   = "import io\nimport math\n\nfunc main() {}\n";
    Iron_Program  *prog  = parse_source(src, &arena, &diags);
    TEST_ASSERT_NOT_NULL(prog);
    TEST_ASSERT_TRUE(prog->decl_count >= 2);

    IronLsp_WorkspaceIndex *wi = make_wi(/*bulk_done=*/true);
    IronLsp_OrganizeImportsResult out = {0};
    ilsp_organize_imports(prog, NULL, &diags, wi, NULL, &arena, &out);

    TEST_ASSERT_NOT_NULL(out.new_text);
    TEST_ASSERT_EQUAL_STRING("import io\nimport math\n", out.new_text);
    TEST_ASSERT_FALSE(out.cold_workspace_warning);

    iron_diaglist_free(&diags);
    iron_arena_free(&arena);
}

/* Test 2: unsorted-within-group -> sorted within a single group.
 * apple + zoo are both local (not in stdlib cache); math is stdlib.
 * Expected: stdlib group FIRST (math), then local group (apple, zoo). */
static void test_unsorted_within_group(void) {
    Iron_Arena     arena = iron_arena_create(64 * 1024);
    Iron_DiagList  diags = iron_diaglist_create();
    const char    *src   = "import zoo\nimport apple\nimport math\n"
                            "\nfunc main() {}\n";
    Iron_Program  *prog  = parse_source(src, &arena, &diags);
    TEST_ASSERT_NOT_NULL(prog);

    IronLsp_WorkspaceIndex *wi = make_wi(/*bulk_done=*/true);
    IronLsp_OrganizeImportsResult out = {0};
    ilsp_organize_imports(prog, NULL, &diags, wi, NULL, &arena, &out);

    TEST_ASSERT_NOT_NULL(out.new_text);
    /* math is stdlib (group A), apple+zoo are local (group C).
     * Groups separated by one blank line. */
    TEST_ASSERT_EQUAL_STRING(
        "import math\n"
        "\n"
        "import apple\n"
        "import zoo\n",
        out.new_text);

    iron_diaglist_free(&diags);
    iron_arena_free(&arena);
}

/* Test 3: cross-group-interleaved -> order becomes stdlib, deps, local.
 * We don't have deps wired in the test; we test stdlib vs local ordering
 * which is sufficient to exercise the group-rank path. Expected: stdlib
 * (io + math), local (local_mod). */
static void test_cross_group_stdlib_first(void) {
    Iron_Arena     arena = iron_arena_create(64 * 1024);
    Iron_DiagList  diags = iron_diaglist_create();
    const char    *src   = "import local_mod\nimport io\nimport math\n"
                            "\nfunc main() {}\n";
    Iron_Program  *prog  = parse_source(src, &arena, &diags);
    TEST_ASSERT_NOT_NULL(prog);

    IronLsp_WorkspaceIndex *wi = make_wi(/*bulk_done=*/true);
    IronLsp_OrganizeImportsResult out = {0};
    ilsp_organize_imports(prog, NULL, &diags, wi, NULL, &arena, &out);

    TEST_ASSERT_NOT_NULL(out.new_text);
    TEST_ASSERT_EQUAL_STRING(
        "import io\n"
        "import math\n"
        "\n"
        "import local_mod\n",
        out.new_text);

    iron_diaglist_free(&diags);
    iron_arena_free(&arena);
}

/* Test 4: duplicates -> collapse to one. `import io` + `import io` ->
 * single `import io`. */
static void test_dedup_exact_duplicates(void) {
    Iron_Arena     arena = iron_arena_create(64 * 1024);
    Iron_DiagList  diags = iron_diaglist_create();
    const char    *src   = "import io\nimport io\n\nfunc main() {}\n";
    Iron_Program  *prog  = parse_source(src, &arena, &diags);
    TEST_ASSERT_NOT_NULL(prog);

    IronLsp_WorkspaceIndex *wi = make_wi(/*bulk_done=*/true);
    IronLsp_OrganizeImportsResult out = {0};
    ilsp_organize_imports(prog, NULL, &diags, wi, NULL, &arena, &out);

    TEST_ASSERT_NOT_NULL(out.new_text);
    TEST_ASSERT_EQUAL_STRING("import io\n", out.new_text);

    iron_diaglist_free(&diags);
    iron_arena_free(&arena);
}

/* Test 5: unused detection when bulk_analyze_done=true.
 * Fixture: `import math as m` (aliased so analyzer emits warning when
 * unused) + `import io`. We hand-construct the diag list to flag the
 * `math` import as unused. Expected: `math` dropped; only `import io`. */
static void test_unused_removed_when_bulk_done(void) {
    Iron_Arena     arena = iron_arena_create(64 * 1024);
    Iron_DiagList  diags = iron_diaglist_create();
    const char    *src   = "import math as m\nimport io\n\nfunc main() {}\n";
    Iron_Program  *prog  = parse_source(src, &arena, &diags);
    TEST_ASSERT_NOT_NULL(prog);

    /* Hand-build diag list with unused-import flag on line 1 (math). */
    Iron_DiagList flag_diags =
        build_unused_diaglist(&arena, "<test>", /*line=*/1);

    IronLsp_WorkspaceIndex *wi = make_wi(/*bulk_done=*/true);
    IronLsp_OrganizeImportsResult out = {0};
    ilsp_organize_imports(prog, NULL, &flag_diags, wi, NULL, &arena, &out);

    TEST_ASSERT_NOT_NULL(out.new_text);
    TEST_ASSERT_EQUAL_STRING("import io\n", out.new_text);
    TEST_ASSERT_FALSE(out.cold_workspace_warning);

    iron_diaglist_free(&flag_diags);
    iron_diaglist_free(&diags);
    iron_arena_free(&arena);
}

/* Test 6: unused detection SKIPPED when bulk_analyze_done=false.
 * Same fixture as Test 5 but the workspace index is cold. Expected:
 * sort+dedup applied, BUT both imports remain, AND
 * cold_workspace_warning == true. */
static void test_unused_skipped_when_cold(void) {
    Iron_Arena     arena = iron_arena_create(64 * 1024);
    Iron_DiagList  diags = iron_diaglist_create();
    const char    *src   = "import math as m\nimport io\n\nfunc main() {}\n";
    Iron_Program  *prog  = parse_source(src, &arena, &diags);
    TEST_ASSERT_NOT_NULL(prog);

    Iron_DiagList flag_diags =
        build_unused_diaglist(&arena, "<test>", /*line=*/1);

    IronLsp_WorkspaceIndex *wi = make_wi(/*bulk_done=*/false);
    IronLsp_OrganizeImportsResult out = {0};
    ilsp_organize_imports(prog, NULL, &flag_diags, wi, NULL, &arena, &out);

    TEST_ASSERT_NOT_NULL(out.new_text);
    /* Both stdlib imports kept, sorted alpha; math gets its alias back. */
    TEST_ASSERT_EQUAL_STRING(
        "import io\n"
        "import math as m\n",
        out.new_text);
    TEST_ASSERT_TRUE_MESSAGE(out.cold_workspace_warning,
        "cold workspace must set cold_workspace_warning signal");

    iron_diaglist_free(&flag_diags);
    iron_diaglist_free(&diags);
    iron_arena_free(&arena);
}

/* Test 7: aliased imports stay both. `import io as i` + `import io` ->
 * both kept; non-aliased first. */
static void test_aliased_both_kept(void) {
    Iron_Arena     arena = iron_arena_create(64 * 1024);
    Iron_DiagList  diags = iron_diaglist_create();
    const char    *src   = "import io as i\nimport io\n\nfunc main() {}\n";
    Iron_Program  *prog  = parse_source(src, &arena, &diags);
    TEST_ASSERT_NOT_NULL(prog);

    IronLsp_WorkspaceIndex *wi = make_wi(/*bulk_done=*/true);
    IronLsp_OrganizeImportsResult out = {0};
    ilsp_organize_imports(prog, NULL, &diags, wi, NULL, &arena, &out);

    TEST_ASSERT_NOT_NULL(out.new_text);
    TEST_ASSERT_EQUAL_STRING(
        "import io\n"
        "import io as i\n",
        out.new_text);

    iron_diaglist_free(&diags);
    iron_arena_free(&arena);
}

/* Test 8: doc-commented import + file-header preservation.
 * File-header blank lines before the first import are NOT in the
 * replaced range (the facade replaces starting at the first import's
 * line, not at byte 0). Doc-comment ON the import is preserved and
 * moves with it. Expected new_text includes `/// doc for math\nimport math\n`. */
static void test_doc_comment_preserved(void) {
    Iron_Arena     arena = iron_arena_create(64 * 1024);
    Iron_DiagList  diags = iron_diaglist_create();
    /* Line 1: blank. Line 2: doc. Line 3: import math. Line 4: import io. */
    const char    *src   = "\n"
                            "/// doc for math\n"
                            "import math\n"
                            "import io\n"
                            "\n"
                            "func main() {}\n";
    Iron_Program  *prog  = parse_source(src, &arena, &diags);
    TEST_ASSERT_NOT_NULL(prog);

    IronLsp_WorkspaceIndex *wi = make_wi(/*bulk_done=*/true);
    IronLsp_OrganizeImportsResult out = {0};
    ilsp_organize_imports(prog, NULL, &diags, wi, NULL, &arena, &out);

    TEST_ASSERT_NOT_NULL(out.new_text);
    /* Both io + math are stdlib; alpha-sort => io first. math keeps its
     * doc_comment. */
    TEST_ASSERT_EQUAL_STRING(
        "import io\n"
        "/// doc for math\n"
        "import math\n",
        out.new_text);

    iron_diaglist_free(&diags);
    iron_arena_free(&arena);
}

/* ── Refusal / edge cases ─────────────────────────────────────── */

/* NULL program -> no edit emitted. */
static void test_null_program_no_edit(void) {
    Iron_Arena arena = iron_arena_create(4 * 1024);
    IronLsp_OrganizeImportsResult out = {0};
    ilsp_organize_imports(NULL, NULL, NULL, NULL, NULL, &arena, &out);
    TEST_ASSERT_NULL(out.new_text);
    iron_arena_free(&arena);
}

/* File with zero imports -> no edit. */
static void test_no_imports_no_edit(void) {
    Iron_Arena    arena = iron_arena_create(64 * 1024);
    Iron_DiagList diags = iron_diaglist_create();
    const char   *src   = "func main() {}\n";
    Iron_Program *prog  = parse_source(src, &arena, &diags);

    IronLsp_WorkspaceIndex *wi = make_wi(/*bulk_done=*/true);
    IronLsp_OrganizeImportsResult out = {0};
    ilsp_organize_imports(prog, NULL, &diags, wi, NULL, &arena, &out);
    TEST_ASSERT_NULL(out.new_text);

    iron_diaglist_free(&diags);
    iron_arena_free(&arena);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_already_organized_single_group);
    RUN_TEST(test_unsorted_within_group);
    RUN_TEST(test_cross_group_stdlib_first);
    RUN_TEST(test_dedup_exact_duplicates);
    RUN_TEST(test_unused_removed_when_bulk_done);
    RUN_TEST(test_unused_skipped_when_cold);
    RUN_TEST(test_aliased_both_kept);
    RUN_TEST(test_doc_comment_preserved);
    RUN_TEST(test_null_program_no_edit);
    RUN_TEST(test_no_imports_no_edit);
    return UNITY_END();
}
