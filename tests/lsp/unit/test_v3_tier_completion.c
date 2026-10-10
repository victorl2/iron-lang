/* test_v3_tier_completion -- Phase 10 Plan 10-03 (TIER-03).
 *
 * Drives ilsp_complete_buckets_build directly against parsed v3_tier
 * fixtures. Object-body methods (readonly, pure and plain) are hoisted
 * to the program's top-level decls by the parser, but a method is only
 * reachable through its receiver, so the top-level bucket must not
 * offer them by their bare name. VAL_DECL / VAR_DECL candidates carry
 * no tier prefix (CONTEXT.md D-10).
 *
 * Test harness mirrors tests/unit/test_completion_buckets.c (parse-only,
 * NULL server so buckets 4+5 short-circuit). */

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

/* ── Tests 1-3: methods are not offered by their bare name ─────────────
 *
 * A method is called through its receiver (`v.length_sq()`); a bare
 * `length_sq()` is an undefined identifier, even inside another method
 * of the same object. The parser hoists object-body methods to the
 * program's top-level decls, so the top-level bucket must skip them.
 * Their tier-prefixed details (`readonly func length_sq() -> Int`) are
 * offered by member completion after `v.`, covered by
 * tests/lsp/smoke/edit/test_body_completion_smoke.py. */

static void assert_method_not_bare(const char *name) {
    char buf[1024];
    const char *path = fixture_path(buf, sizeof(buf), "tier_completion.iron");
    TEST_ASSERT_NOT_NULL_MESSAGE(path, "fixture tier_completion.iron not found");
    char *src = load_file(path);
    TEST_ASSERT_NOT_NULL_MESSAGE(src, "load_file returned NULL");

    Iron_Arena arena = iron_arena_create(64 * 1024);
    Iron_DiagList diags = iron_diaglist_create();
    Iron_Program *prog = parse_source(src, &arena, &diags);
    TEST_ASSERT_NOT_NULL_MESSAGE(prog, "parse_source returned NULL");

    IronLsp_CompletionCandidate *cands = NULL;
    size_t n = 0;
    ilsp_complete_buckets_build(NULL, NULL, prog, 0,
                                  ILSP_CCTX_STATEMENT_HEAD, "",
                                  NULL, &arena, &cands, &n);
    TEST_ASSERT_TRUE_MESSAGE(n > 0, "no candidates emitted");
    TEST_ASSERT_NOT_NULL_MESSAGE(find_candidate(cands, n, "Vec"),
        "the object `Vec` MUST appear as a candidate");
    TEST_ASSERT_NULL_MESSAGE(find_candidate(cands, n, name),
        "a method MUST NOT appear as a bare top-level candidate");

    iron_diaglist_free(&diags);
    iron_arena_free(&arena);
    free(src);
}

static void test_readonly_method_not_bare(void) { assert_method_not_bare("length_sq"); }
static void test_pure_method_not_bare(void)     { assert_method_not_bare("add"); }
static void test_plain_method_not_bare(void)    { assert_method_not_bare("mutate"); }

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
    RUN_TEST(test_readonly_method_not_bare);
    RUN_TEST(test_pure_method_not_bare);
    RUN_TEST(test_plain_method_not_bare);
    RUN_TEST(test_non_func_decls_have_no_tier_prefix);
    return UNITY_END();
}
