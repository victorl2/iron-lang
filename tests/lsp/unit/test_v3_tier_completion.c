/* test_v3_tier_completion -- Phase 10 Plan 10-03 (TIER-03).
 *
 * Drives ilsp_complete_buckets_build directly against parsed v3_tier
 * fixtures. Asserts the detail field for each top-level FUNC_DECL
 * candidate carries the tier prefix shipped by Task 4 in
 * src/lsp/facade/edit/complete/buckets.c::emit_top_level:
 *   - readonly func    when fd->is_readonly
 *   - pure func        when fd->is_pure
 *   - func             when neither (mutual exclusion enforced by
 *                       parser at src/parser/parser.c:3162-3180)
 *
 * Per CONTEXT.md D-10, only FUNC_DECL + METHOD_DECL get tier prefixes;
 * VAL_DECL / VAR_DECL / FIELD / ENUM_VARIANT / PARAM remain untouched.
 *
 * Test harness mirrors tests/unit/test_completion_buckets.c (parse-only,
 * NULL server so buckets 4+5 short-circuit). Link-time stubs supply
 * symbols buckets.c references on the stdlib + dep paths so the unit
 * test binary does not need to drag in the full LSP store dep tree. */

#include "unity.h"

#include "lsp/facade/edit/complete/buckets.h"
#include "lsp/facade/edit/complete/context_classify.h"
#include "parser/ast.h"
#include "parser/parser.h"
#include "lexer/lexer.h"
#include "diagnostics/diagnostics.h"
#include "util/arena.h"
#include "vendor/stb_ds.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The full LSP store stack is linked via _LSP_PHASE3_NAV_FACADE_SRC
 * (stdlib_cache.c + dep_map.c are already on the link line) so no
 * stub symbols are needed here — the real implementations short-
 * circuit on NULL server, which is what we pass below. */

void setUp(void)    {}
void tearDown(void) {}

/* ── Fixture loader ──────────────────────────────────────────────────── */

static char *load_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    rewind(f);
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[n] = '\0';
    return buf;
}

static const char *fixture_path(char *buf, size_t cap, const char *name) {
    snprintf(buf, cap, "../tests/lsp/unit/v3_tier/%s", name);
    FILE *f = fopen(buf, "rb");
    if (f) { fclose(f); return buf; }
#ifdef IRON_SOURCE_TREE_ROOT
    snprintf(buf, cap, "%s/tests/lsp/unit/v3_tier/%s",
             IRON_SOURCE_TREE_ROOT, name);
    f = fopen(buf, "rb");
    if (f) { fclose(f); return buf; }
#endif
    return NULL;
}

/* Parse-only helper (mirrors test_completion_buckets.c::parse_source). */
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

/* Find a candidate by exact label match in a bucket result list. */
static const IronLsp_CompletionCandidate *
find_candidate(const IronLsp_CompletionCandidate *cands, size_t n,
                 const char *label) {
    for (size_t i = 0; i < n; i++) {
        if (cands[i].label && strcmp(cands[i].label, label) == 0) {
            return &cands[i];
        }
    }
    return NULL;
}

/* ── Test 1: readonly func candidate detail prefix ──────────────────── */

static void test_readonly_func_completion_detail_prefix(void) {
    char buf[1024];
    const char *path = fixture_path(buf, sizeof(buf), "tier_completion.iron");
    TEST_ASSERT_NOT_NULL_MESSAGE(path,
        "fixture tier_completion.iron not found");
    char *src = load_file(path);
    TEST_ASSERT_NOT_NULL_MESSAGE(src, "load_file returned NULL");

    Iron_Arena arena = iron_arena_create(64 * 1024);
    Iron_DiagList diags = iron_diaglist_create();
    Iron_Program *prog = parse_source(src, &arena, &diags);
    TEST_ASSERT_NOT_NULL_MESSAGE(prog, "parse_source returned NULL");

    IronLsp_CompletionCandidate *cands = NULL;
    size_t n = 0;
    /* STATEMENT_HEAD context drives the default 6-bucket pipeline so
     * top-level FUNC_DECL candidates land in bucket 2 with the tier-
     * prefixed detail string. NULL server so buckets 4+5 skip. */
    ilsp_complete_buckets_build(NULL, NULL, prog, 0,
                                  ILSP_CCTX_STATEMENT_HEAD, "",
                                  NULL, &arena, &cands, &n);
    TEST_ASSERT_TRUE_MESSAGE(n > 0, "no candidates emitted");

    const IronLsp_CompletionCandidate *c = find_candidate(cands, n, "length_sq");
    TEST_ASSERT_NOT_NULL_MESSAGE(c,
        "TIER-03: readonly func `length_sq` MUST appear as a candidate");
    TEST_ASSERT_NOT_NULL_MESSAGE(c->detail,
        "TIER-03: candidate detail field MUST NOT be NULL");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(c->detail, "readonly func"),
        "TIER-03: readonly func candidate detail MUST contain `readonly func`");

    iron_diaglist_free(&diags);
    iron_arena_free(&arena);
    free(src);
}

/* ── Test 2: pure func candidate detail prefix ──────────────────────── */

static void test_pure_func_completion_detail_prefix(void) {
    char buf[1024];
    const char *path = fixture_path(buf, sizeof(buf), "tier_completion.iron");
    TEST_ASSERT_NOT_NULL_MESSAGE(path, "fixture not found");
    char *src = load_file(path);
    TEST_ASSERT_NOT_NULL_MESSAGE(src, "load_file returned NULL");

    Iron_Arena arena = iron_arena_create(64 * 1024);
    Iron_DiagList diags = iron_diaglist_create();
    Iron_Program *prog = parse_source(src, &arena, &diags);
    TEST_ASSERT_NOT_NULL_MESSAGE(prog, "parse failed");

    IronLsp_CompletionCandidate *cands = NULL;
    size_t n = 0;
    ilsp_complete_buckets_build(NULL, NULL, prog, 0,
                                  ILSP_CCTX_STATEMENT_HEAD, "",
                                  NULL, &arena, &cands, &n);

    const IronLsp_CompletionCandidate *c = find_candidate(cands, n, "add");
    TEST_ASSERT_NOT_NULL_MESSAGE(c,
        "TIER-03: pure func `add` MUST appear as a candidate");
    TEST_ASSERT_NOT_NULL_MESSAGE(c->detail,
        "TIER-03: candidate detail field MUST NOT be NULL");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(c->detail, "pure func"),
        "TIER-03: pure func candidate detail MUST contain `pure func`");

    iron_diaglist_free(&diags);
    iron_arena_free(&arena);
    free(src);
}

/* ── Test 3: plain func candidate has `func` detail (no tier prefix) ── */

static void test_plain_func_completion_detail_no_tier_prefix(void) {
    char buf[1024];
    const char *path = fixture_path(buf, sizeof(buf), "tier_completion.iron");
    TEST_ASSERT_NOT_NULL_MESSAGE(path, "fixture not found");
    char *src = load_file(path);
    TEST_ASSERT_NOT_NULL_MESSAGE(src, "load_file returned NULL");

    Iron_Arena arena = iron_arena_create(64 * 1024);
    Iron_DiagList diags = iron_diaglist_create();
    Iron_Program *prog = parse_source(src, &arena, &diags);
    TEST_ASSERT_NOT_NULL_MESSAGE(prog, "parse failed");

    IronLsp_CompletionCandidate *cands = NULL;
    size_t n = 0;
    ilsp_complete_buckets_build(NULL, NULL, prog, 0,
                                  ILSP_CCTX_STATEMENT_HEAD, "",
                                  NULL, &arena, &cands, &n);

    const IronLsp_CompletionCandidate *c = find_candidate(cands, n, "mutate");
    TEST_ASSERT_NOT_NULL_MESSAGE(c,
        "TIER-03: plain func `mutate` MUST appear as a candidate");
    TEST_ASSERT_NOT_NULL_MESSAGE(c->detail,
        "TIER-03: candidate detail field MUST NOT be NULL");
    /* No `readonly` and no `pure` prefix word — the detail begins with
     * the bare `func` token. Mutual exclusion is parser-enforced. */
    TEST_ASSERT_NULL_MESSAGE(strstr(c->detail, "readonly"),
        "TIER-03: plain func detail MUST NOT contain `readonly`");
    TEST_ASSERT_NULL_MESSAGE(strstr(c->detail, "pure"),
        "TIER-03: plain func detail MUST NOT contain `pure`");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(c->detail, "func"),
        "TIER-03: plain func detail MUST contain bare `func`");

    iron_diaglist_free(&diags);
    iron_arena_free(&arena);
    free(src);
}

/* ── Test 4: VAL/VAR/FIELD candidates remain untouched (D-10) ───────── */

static void test_non_func_decls_have_no_tier_prefix(void) {
    /* Top-level val + var declarations should not pick up `readonly` or
     * `pure` prefixes from the TIER-03 path. */
    const char *src =
        "val pi: Int = 314\n"
        "var counter: Int = 0\n"
        "func touch() {}\n";
    Iron_Arena arena = iron_arena_create(32 * 1024);
    Iron_DiagList diags = iron_diaglist_create();
    Iron_Program *prog = parse_source(src, &arena, &diags);
    TEST_ASSERT_NOT_NULL(prog);

    IronLsp_CompletionCandidate *cands = NULL;
    size_t n = 0;
    ilsp_complete_buckets_build(NULL, NULL, prog, 0,
                                  ILSP_CCTX_STATEMENT_HEAD, "",
                                  NULL, &arena, &cands, &n);

    const IronLsp_CompletionCandidate *pi = find_candidate(cands, n, "pi");
    if (pi && pi->detail) {
        TEST_ASSERT_NULL_MESSAGE(strstr(pi->detail, "readonly"),
            "D-10: val candidate MUST NOT carry `readonly` prefix");
        TEST_ASSERT_NULL_MESSAGE(strstr(pi->detail, "pure"),
            "D-10: val candidate MUST NOT carry `pure` prefix");
    }
    const IronLsp_CompletionCandidate *cnt = find_candidate(cands, n, "counter");
    if (cnt && cnt->detail) {
        TEST_ASSERT_NULL_MESSAGE(strstr(cnt->detail, "readonly"),
            "D-10: var candidate MUST NOT carry `readonly` prefix");
        TEST_ASSERT_NULL_MESSAGE(strstr(cnt->detail, "pure"),
            "D-10: var candidate MUST NOT carry `pure` prefix");
    }
    iron_diaglist_free(&diags);
    iron_arena_free(&arena);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_readonly_func_completion_detail_prefix);
    RUN_TEST(test_pure_func_completion_detail_prefix);
    RUN_TEST(test_plain_func_completion_detail_no_tier_prefix);
    RUN_TEST(test_non_func_decls_have_no_tier_prefix);
    return UNITY_END();
}
