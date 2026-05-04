/* test_v3_patch_completion -- Phase 11 Plan 11-02 (PATCH-03).
 *
 * Drives ilsp_complete_buckets_build directly against parsed v3_patch
 * fixtures with ILSP_CCTX_MEMBER_AFTER_DOT context. Asserts patch methods
 * appear alongside native methods at <expr>. completion sites:
 *   1. User-object receiver (`val f: Foo; f.|`) → both native_method
 *      AND patched_method appear in the candidate list.
 *   2. TIER-03 prefix flows through patches: detail field on the patch
 *      candidate contains "func" (Phase 10 D-10 idiom routing through
 *      maybe_push).
 *   3. Empty-program / no-patch sanity: a fixture without any patch
 *      decl emits no spurious patch candidates (false-positive guard).
 *
 * Test harness mirrors test_v3_tier_completion.c: parse-only via
 * iron_parse, NULL server (so workspace_index walks short-circuit;
 * same-program patches still flow through patch_lookup helpers'
 * direct-decl walk per Plan 11-01 deviation #2). IronLsp_Document
 * stand-in is required since emit_member_fields walks doc->text to
 * find the receiver token before `.`.
 */

#include "unity.h"

#include "lsp/facade/edit/complete/buckets.h"
#include "lsp/facade/edit/complete/context_classify.h"
#include "lsp/store/document.h"
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

void setUp(void)    {}
void tearDown(void) {}

/* ── Fixture loader ──────────────────────────────────────────────── */

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
#ifdef IRON_SOURCE_TREE_ROOT
    snprintf(buf, cap, "%s/tests/lsp/unit/v3_patch/%s",
             IRON_SOURCE_TREE_ROOT, name);
    FILE *f = fopen(buf, "rb");
    if (f) { fclose(f); return buf; }
#endif
    snprintf(buf, cap, "../tests/lsp/unit/v3_patch/%s", name);
    FILE *f2 = fopen(buf, "rb");
    if (f2) { fclose(f2); return buf; }
    return NULL;
}

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

/* Find a candidate by exact label match. */
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

/* Find the cursor byte offset immediately after the LAST `<recv>.<name>(`
 * call expression in `src`. We position the cursor right after the `.`
 * of the receiver expression so emit_member_fields walks back to find
 * the receiver ident, looks up its declared type via val/var, and emits
 * the type's fields + methods (including patch methods). */
static size_t cursor_after_last_dot(const char *src, const char *recv_token) {
    /* Find the LAST `<recv>.` occurrence (so the val/var binding above
     * has been parsed by the time the cursor is positioned). */
    size_t recv_len = strlen(recv_token);
    const char *best = NULL;
    const char *p = src;
    while (*p) {
        if (strncmp(p, recv_token, recv_len) == 0 && p[recv_len] == '.') {
            /* Boundary: the char before recv_token must NOT be ident-
             * continuing (so we don't pick up substring matches like
             * "self" inside a name). */
            if (p == src ||
                !(((unsigned char)p[-1] >= 'A' && (unsigned char)p[-1] <= 'Z') ||
                  ((unsigned char)p[-1] >= 'a' && (unsigned char)p[-1] <= 'z') ||
                  ((unsigned char)p[-1] >= '0' && (unsigned char)p[-1] <= '9') ||
                  p[-1] == '_')) {
                best = p + recv_len + 1;  /* cursor right after `.` */
            }
        }
        p++;
    }
    if (!best) return 0;
    return (size_t)(best - src);
}

/* ── Test 1: user-object receiver — both native + patch surface ─── */

static void test_user_object_completion_includes_patch_method(void) {
    char buf[1024];
    const char *path = fixture_path(buf, sizeof(buf),
                                     "patch_completion_user_object.iron");
    TEST_ASSERT_NOT_NULL_MESSAGE(path,
        "fixture patch_completion_user_object.iron not found");
    char *src = load_file(path);
    TEST_ASSERT_NOT_NULL(src);

    Iron_Arena arena = iron_arena_create(64 * 1024);
    Iron_DiagList diags = iron_diaglist_create();
    Iron_Program *prog = parse_source(src, &arena, &diags);
    TEST_ASSERT_NOT_NULL_MESSAGE(prog, "parse failed");

    /* Build an IronLsp_Document so emit_member_fields can walk doc->text
     * back over the receiver ident. */
    IronLsp_Document *doc = ilsp_document_create(
        "/tmp/test_v3_patch_completion_user_object.iron",
        src, strlen(src), 1);
    TEST_ASSERT_NOT_NULL(doc);

    /* Cursor placed right after the `.` in `f.native_method()`. */
    size_t cursor = cursor_after_last_dot(src, "f");
    TEST_ASSERT_GREATER_THAN_size_t(0, cursor);

    IronLsp_CompletionCandidate *cands = NULL;
    size_t n = 0;
    /* NULL server — workspace_index walks skipped; same-program patches
     * still flow via the patch_lookup helper's program-decls walk
     * (Plan 11-01 deviation #2). */
    ilsp_complete_buckets_build(NULL, doc, prog, cursor,
                                  ILSP_CCTX_MEMBER_AFTER_DOT, "",
                                  NULL, &arena, &cands, &n);
    TEST_ASSERT_TRUE_MESSAGE(n > 0,
        "PATCH-03: no candidates emitted at member-after-dot");

    /* Native method must surface (regression guard). */
    const IronLsp_CompletionCandidate *native = find_candidate(
        cands, n, "native_method");
    TEST_ASSERT_NOT_NULL_MESSAGE(native,
        "PATCH-03: native method `native_method` MUST be a candidate");

    /* Patch method must surface (the actual PATCH-03 acceptance). */
    const IronLsp_CompletionCandidate *patched = find_candidate(
        cands, n, "patched_method");
    TEST_ASSERT_NOT_NULL_MESSAGE(patched,
        "PATCH-03: patch method `patched_method` MUST be a candidate "
        "alongside native methods");

    /* Both share kind=METHOD. */
    TEST_ASSERT_EQUAL_INT_MESSAGE(2 /* LSP_CK_METHOD */, patched->kind,
        "PATCH-03: patch candidate kind MUST equal LSP_CK_METHOD (2)");

    iron_diaglist_free(&diags);
    iron_arena_free(&arena);
    ilsp_document_destroy(doc);
    free(src);
}

/* ── Test 2: TIER-03 prefix flows through patches ────────────────── */

static void test_completion_tier_prefix_flows_through_patches(void) {
    char buf[1024];
    const char *path = fixture_path(buf, sizeof(buf),
                                     "patch_completion_user_object.iron");
    TEST_ASSERT_NOT_NULL(path);
    char *src = load_file(path);
    TEST_ASSERT_NOT_NULL(src);

    Iron_Arena arena = iron_arena_create(64 * 1024);
    Iron_DiagList diags = iron_diaglist_create();
    Iron_Program *prog = parse_source(src, &arena, &diags);
    TEST_ASSERT_NOT_NULL(prog);

    IronLsp_Document *doc = ilsp_document_create(
        "/tmp/test_v3_patch_completion_tier.iron",
        src, strlen(src), 1);
    TEST_ASSERT_NOT_NULL(doc);

    size_t cursor = cursor_after_last_dot(src, "f");
    TEST_ASSERT_GREATER_THAN_size_t(0, cursor);

    IronLsp_CompletionCandidate *cands = NULL;
    size_t n = 0;
    ilsp_complete_buckets_build(NULL, doc, prog, cursor,
                                  ILSP_CCTX_MEMBER_AFTER_DOT, "",
                                  NULL, &arena, &cands, &n);

    const IronLsp_CompletionCandidate *patched = find_candidate(
        cands, n, "patched_method");
    TEST_ASSERT_NOT_NULL_MESSAGE(patched,
        "TIER-03 flow: `patched_method` candidate required for tier check");
    TEST_ASSERT_NOT_NULL_MESSAGE(patched->detail,
        "TIER-03 flow: patched candidate detail field MUST NOT be NULL");
    /* Fixture declares `readonly func patched_method`. The TIER-03 idiom
     * (Phase 10 D-10) writes the modifier-aware `readonly func` prefix
     * into the detail string via maybe_push. */
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(patched->detail, "func"),
        "TIER-03 flow: patched candidate detail MUST contain `func` "
        "(tier prefix machinery flowing through emit_patch_member_field)");

    iron_diaglist_free(&diags);
    iron_arena_free(&arena);
    ilsp_document_destroy(doc);
    free(src);
}

/* ── Test 3: false-positive guard — no patches → no patch candidates ─ */

static void test_no_patch_decls_emits_no_patch_candidates(void) {
    /* Inline source: object Bar with one method, NO patches. Member-
     * completion on `b.` must surface only the native method (no patch
     * candidates leak in). Top-level `val b: Bar` so the receiver-
     * resolver in emit_member_fields finds the binding (it walks
     * program->decls only, not function bodies). */
    const char *src =
        "object Bar {\n"
        "    val v: Int\n"
        "    readonly func only_native() -> Int { return self.v }\n"
        "}\n"
        "val b: Bar = Bar(7)\n"
        "func main() -> Int {\n"
        "    return b.only_native()\n"
        "}\n";

    Iron_Arena arena = iron_arena_create(64 * 1024);
    Iron_DiagList diags = iron_diaglist_create();
    Iron_Program *prog = parse_source(src, &arena, &diags);
    TEST_ASSERT_NOT_NULL(prog);

    IronLsp_Document *doc = ilsp_document_create(
        "/tmp/test_v3_patch_completion_neg.iron",
        src, strlen(src), 1);
    TEST_ASSERT_NOT_NULL(doc);

    size_t cursor = cursor_after_last_dot(src, "b");
    TEST_ASSERT_GREATER_THAN_size_t(0, cursor);

    IronLsp_CompletionCandidate *cands = NULL;
    size_t n = 0;
    ilsp_complete_buckets_build(NULL, doc, prog, cursor,
                                  ILSP_CCTX_MEMBER_AFTER_DOT, "",
                                  NULL, &arena, &cands, &n);

    /* The native method must be there. */
    TEST_ASSERT_NOT_NULL_MESSAGE(find_candidate(cands, n, "only_native"),
        "false-positive guard: native method must still appear");

    /* No patch-only labels should leak in. The fixture has no patch
     * decls so `patched_method` (used by Test 1) MUST NOT appear. */
    TEST_ASSERT_NULL_MESSAGE(find_candidate(cands, n, "patched_method"),
        "PATCH-03 false-positive: a fixture without patch decls MUST NOT "
        "emit `patched_method` candidates");

    iron_diaglist_free(&diags);
    iron_arena_free(&arena);
    ilsp_document_destroy(doc);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_user_object_completion_includes_patch_method);
    RUN_TEST(test_completion_tier_prefix_flows_through_patches);
    RUN_TEST(test_no_patch_decls_emits_no_patch_candidates);
    return UNITY_END();
}
