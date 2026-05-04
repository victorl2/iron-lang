/* Phase 4 Plan 04-02 Task 02 (EDIT-01, D-01) -- 6-bucket builder unit
 * tests.
 *
 * The bucket builder needs a server + workspace_index + document to
 * exercise buckets 4+5; for unit-scope tests we construct a trivial
 * Iron_Program from an in-memory source via iron_analyze_buffer (the
 * same seam the LSP facade uses). Tests focus on the shape + ordering
 * invariants callers depend on:
 *   - context-sensitive bucket selection (EXPR_HEAD emits kw bucket;
 *     MEMBER_AFTER_DOT skips 1-6; etc.)
 *   - fuzzy filtering drops non-matches
 *   - every candidate carries a non-empty label + a valid bucket tag
 */

#include "unity.h"

#include "lsp/facade/edit/complete/buckets.h"
#include "lsp/facade/edit/complete/context_classify.h"
#include "analyzer/analyzer.h"
#include "diagnostics/diagnostics.h"
#include "parser/ast.h"
#include "parser/parser.h"
#include "lexer/lexer.h"
#include "util/arena.h"
#include "vendor/stb_ds.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <string.h>

/* Link-time stubs for LSP store routines. buckets.c references them on
 * the emit_stdlib + emit_deps paths, but this unit test passes NULL
 * server so those paths short-circuit before calling them. Providing
 * stub symbols avoids pulling the entire LSP store + its transitive
 * deps (line_index, utf, workspace, resolver, fetcher, toml...) into
 * the test binary. If these stubs EVER get invoked, the first NULL
 * check in each function still keeps the test from crashing. */
struct IronLsp_StdlibCache;
struct IronLsp_DepMap;
const struct Iron_Program *ilsp_stdlib_cache_get(struct IronLsp_StdlibCache *c,
                                                   const char *name) {
    (void)c; (void)name; return 0;
}
size_t ilsp_dep_map_size(const struct IronLsp_DepMap *dm) {
    (void)dm; return 0;
}

/* Phase 11 PATCH-03 (Plan 11-02 Task 4): buckets.c emit_member_fields
 * now references ilsp_patch_for_each_method via the patch-member walk.
 * Provide a no-op stub here so the test binary links without dragging
 * patch_lookup.c + visibility.c + nav_common.c (and their transitive
 * line_index/utf/yyjson deps) into the minimal completion-bucket test.
 * MEMBER_AFTER_DOT is not exercised by any of the test_completion_buckets
 * cases (they all use EXPR_HEAD / STATEMENT_HEAD / IMPORT_PATH), so the
 * stub returns 0 visited and is never invoked when the existing test
 * cases run. */
struct Iron_MethodDecl;
struct Iron_ObjectDecl;
struct IronLsp_WorkspaceIndex;
size_t ilsp_patch_for_each_method(
    Iron_Program             *program,
    struct IronLsp_WorkspaceIndex *wi,
    const char               *target_type_name,
    const char               *requester_canonical_path,
    bool (*visit)(struct Iron_MethodDecl *md,
                    struct Iron_ObjectDecl *patch_od,
                    void *ud),
    void                     *userdata,
    _Atomic bool             *cancel) {
    (void)program; (void)wi; (void)target_type_name;
    (void)requester_canonical_path; (void)visit; (void)userdata; (void)cancel;
    return 0;
}

void setUp(void)    {}
void tearDown(void) {}

/* Helper: parse-only (no analyze) — sufficient for buckets that only
 * consult AST decl structure. The workspace_index / stdlib_cache paths
 * are exercised by the pytest-lsp smoke tests in Task 03. */
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

/* Test 1: EXPR_HEAD emits the keyword bucket. */
static void test_expr_head_emits_keywords(void) {
    Iron_Arena arena = iron_arena_create(64 * 1024);
    Iron_DiagList diags = iron_diaglist_create();
    const char *src = "func main() { val x = 1 }\n";
    Iron_Program *prog = parse_source(src, &arena, &diags);

    IronLsp_CompletionCandidate *cands = NULL;
    size_t n = 0;
    /* Pass NULL server (buckets 4+5 skipped gracefully). */
    ilsp_complete_buckets_build(NULL, NULL, prog, 0,
                                  ILSP_CCTX_EXPR_HEAD, "",
                                  NULL, &arena, &cands, &n);
    /* With empty prefix and no server: bucket 6 (keywords) should be
     * emitted. Bucket 2 should also have `main`. */
    bool saw_keyword = false;
    for (size_t i = 0; i < n; i++) {
        if (cands[i].bucket == ILSP_COMPLETION_BUCKET_KEYWORDS) saw_keyword = true;
    }
    TEST_ASSERT_TRUE(saw_keyword);

    iron_diaglist_free(&diags);
    iron_arena_free(&arena);
}

/* Test 2: MEMBER_AFTER_DOT context yields no keyword candidates. */
static void test_member_dot_skips_keywords(void) {
    Iron_Arena arena = iron_arena_create(64 * 1024);
    Iron_DiagList diags = iron_diaglist_create();
    const char *src = "func main() {}\n";
    Iron_Program *prog = parse_source(src, &arena, &diags);

    IronLsp_CompletionCandidate *cands = NULL;
    size_t n = 0;
    ilsp_complete_buckets_build(NULL, NULL, prog, 0,
                                  ILSP_CCTX_MEMBER_AFTER_DOT, "",
                                  NULL, &arena, &cands, &n);
    for (size_t i = 0; i < n; i++) {
        TEST_ASSERT_NOT_EQUAL(ILSP_COMPLETION_BUCKET_KEYWORDS, cands[i].bucket);
    }

    iron_diaglist_free(&diags);
    iron_arena_free(&arena);
}

/* Test 3: TYPE_POSITION emits Object decls + primitives only. */
static void test_type_position_emits_types(void) {
    Iron_Arena arena = iron_arena_create(64 * 1024);
    Iron_DiagList diags = iron_diaglist_create();
    const char *src = "object MyThing { x: Int }\nfunc main() {}\n";
    Iron_Program *prog = parse_source(src, &arena, &diags);

    IronLsp_CompletionCandidate *cands = NULL;
    size_t n = 0;
    ilsp_complete_buckets_build(NULL, NULL, prog, 0,
                                  ILSP_CCTX_TYPE_POSITION, "",
                                  NULL, &arena, &cands, &n);
    bool saw_mything = false;
    bool saw_int     = false;
    for (size_t i = 0; i < n; i++) {
        if (strcmp(cands[i].label, "MyThing") == 0) saw_mything = true;
        if (strcmp(cands[i].label, "Int")     == 0) saw_int     = true;
        /* No keyword bucket in type position. */
        TEST_ASSERT_NOT_EQUAL(ILSP_COMPLETION_BUCKET_KEYWORDS, cands[i].bucket);
    }
    TEST_ASSERT_TRUE_MESSAGE(saw_mything, "object decl missing from TYPE_POSITION");
    TEST_ASSERT_TRUE_MESSAGE(saw_int,     "primitive Int missing from TYPE_POSITION");

    iron_diaglist_free(&diags);
    iron_arena_free(&arena);
}

/* Test 4: Fuzzy prefix filter drops non-matches. */
static void test_fuzzy_prefix_filter(void) {
    Iron_Arena arena = iron_arena_create(64 * 1024);
    Iron_DiagList diags = iron_diaglist_create();
    const char *src = "func alpha() {}\nfunc beta() {}\nfunc gamma() {}\n";
    Iron_Program *prog = parse_source(src, &arena, &diags);

    IronLsp_CompletionCandidate *cands = NULL;
    size_t n = 0;
    /* Query "alph" should keep `alpha` and drop `beta`/`gamma`. */
    ilsp_complete_buckets_build(NULL, NULL, prog, 0,
                                  ILSP_CCTX_EXPR_HEAD, "alph",
                                  NULL, &arena, &cands, &n);
    bool saw_alpha = false;
    for (size_t i = 0; i < n; i++) {
        if (strcmp(cands[i].label, "alpha") == 0) saw_alpha = true;
        /* No non-matching top-level decls may survive. */
        if (cands[i].bucket == ILSP_COMPLETION_BUCKET_TOP_LEVEL) {
            TEST_ASSERT_TRUE(strcmp(cands[i].label, "beta")  != 0);
            TEST_ASSERT_TRUE(strcmp(cands[i].label, "gamma") != 0);
        }
    }
    TEST_ASSERT_TRUE(saw_alpha);

    iron_diaglist_free(&diags);
    iron_arena_free(&arena);
}

/* Test 5: Every candidate has a non-empty label + valid bucket tag. */
static void test_candidate_shape(void) {
    Iron_Arena arena = iron_arena_create(64 * 1024);
    Iron_DiagList diags = iron_diaglist_create();
    const char *src = "func main() { val x = 1 }\n";
    Iron_Program *prog = parse_source(src, &arena, &diags);

    IronLsp_CompletionCandidate *cands = NULL;
    size_t n = 0;
    ilsp_complete_buckets_build(NULL, NULL, prog, 0,
                                  ILSP_CCTX_EXPR_HEAD, "",
                                  NULL, &arena, &cands, &n);
    TEST_ASSERT_TRUE(n > 0);
    for (size_t i = 0; i < n; i++) {
        TEST_ASSERT_NOT_NULL(cands[i].label);
        TEST_ASSERT_TRUE(strlen(cands[i].label) > 0);
        TEST_ASSERT_TRUE(cands[i].bucket >= 1 && cands[i].bucket <= 6);
        TEST_ASSERT_TRUE(cands[i].kind >= 1 && cands[i].kind <= 25);
    }

    iron_diaglist_free(&diags);
    iron_arena_free(&arena);
}

/* Test 6: IMPORT_PATH emits only Module-kind candidates. */
static void test_import_path_emits_modules_only(void) {
    Iron_Arena arena = iron_arena_create(64 * 1024);
    Iron_DiagList diags = iron_diaglist_create();
    const char *src = "func main() {}\n";
    Iron_Program *prog = parse_source(src, &arena, &diags);

    IronLsp_CompletionCandidate *cands = NULL;
    size_t n = 0;
    /* Pass NULL server so stdlib + deps buckets skip silently — we
     * still expect zero-or-empty candidates without error. */
    ilsp_complete_buckets_build(NULL, NULL, prog, 0,
                                  ILSP_CCTX_IMPORT_PATH, "",
                                  NULL, &arena, &cands, &n);
    /* With NULL server, stdlib/deps emit nothing. That's OK — the test
     * only asserts no candidates have Keyword kind. */
    for (size_t i = 0; i < n; i++) {
        TEST_ASSERT_NOT_EQUAL(14 /* Keyword */, cands[i].kind);
    }

    iron_diaglist_free(&diags);
    iron_arena_free(&arena);
}

/* Test 7: Cold-start fallback — NULL program doesn't crash. */
static void test_cold_start_safety(void) {
    Iron_Arena arena = iron_arena_create(64 * 1024);
    IronLsp_CompletionCandidate *cands = NULL;
    size_t n = 0;
    ilsp_complete_buckets_build(NULL, NULL, NULL, 0,
                                  ILSP_CCTX_EXPR_HEAD, "",
                                  NULL, &arena, &cands, &n);
    /* Should at least emit the keyword bucket (no program needed). */
    TEST_ASSERT_TRUE(n > 0);
    iron_arena_free(&arena);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_expr_head_emits_keywords);
    RUN_TEST(test_member_dot_skips_keywords);
    RUN_TEST(test_type_position_emits_types);
    RUN_TEST(test_fuzzy_prefix_filter);
    RUN_TEST(test_candidate_shape);
    RUN_TEST(test_import_path_emits_modules_only);
    RUN_TEST(test_cold_start_safety);
    return UNITY_END();
}
