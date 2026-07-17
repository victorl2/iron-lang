/* Phase 21 Wave 0 (Plan 02): TDD scaffold for POL-05 — Iron_Symbol.is_leaked
 * flag set by resolve.c IRON_NODE_LEAK arm.
 *
 * 4 cases verify the is_leaked flag:
 *   Case 1: `leak p` after `val p = heap Point(...)` — sym(p).is_leaked == true
 *   Case 2: `val p = heap Point(...)` with no leak — sym(p).is_leaked == false
 *   Case 3: two allocs, leak only one — sym(a).is_leaked true, sym(b).is_leaked false
 *   Case 4: double-leak same binding — idempotent; sym(p).is_leaked true, no errors
 *
 * Authored RED first; `is_leaked` field does NOT exist yet on Iron_Symbol
 * (scope.h). Build fails with "no member named 'is_leaked'" until Task 2
 * adds the field. Assertions flip GREEN after Task 2 lands.
 *
 * Symbol access strategy: use a reference ident `val _ref = p` at the end of
 * each function body so `((Iron_Ident *)ref_decl->init)->resolved_sym` gives
 * us direct access to p's Iron_Symbol post-resolve. This avoids the need to
 * walk the scope-tree downward (Iron_Scope has no children pointer).
 *
 * Pattern source: tests/unit/test_resolver.c (resolved_sym via ident reference). */

#include "unity.h"
#include "analyzer/analyzer.h"
#include "analyzer/scope.h"
#include "diagnostics/diagnostics.h"
#include "parser/ast.h"
#include "util/arena.h"
#include "stb_ds.h"

#include <string.h>

/* ── Module-level fixtures ────────────────────────────────────────────────── */

static Iron_Arena    g_arena;
static Iron_DiagList g_diags;

void setUp(void) {
    g_arena = iron_arena_create(131072);
    g_diags  = iron_diaglist_create();
}

void tearDown(void) {
    iron_diaglist_free(&g_diags);
    iron_arena_free(&g_arena);
}

/* ── Pipeline helper ──────────────────────────────────────────────────────── */

static Iron_AnalyzeResult analyze_src(const char *src) {
    return iron_analyze_buffer(
        src, strlen(src), "test.iron",
        IRON_ANALYSIS_MODE_CLI,
        &g_arena, &g_diags, NULL, 0);
}

static int count_errors(void) {
    int n = 0;
    for (int i = 0; i < arrlen(g_diags.items); i++) {
        if (g_diags.items[i].level == IRON_DIAG_ERROR) n++;
    }
    return n;
}

/* Walk the first function body and find the N-th val-decl whose init is an
 * IRON_NODE_IDENT (a simple reference to another binding). Returns the
 * resolved_sym of that ident, or NULL if not found.
 * ref_index is 0-based among val decls whose init kind == IRON_NODE_IDENT. */
static Iron_Symbol *find_ref_sym(Iron_Program *prog, int ref_index) {
    if (!prog || prog->decl_count < 1) return NULL;

    for (int di = 0; di < prog->decl_count; di++) {
        Iron_Node *decl = prog->decls[di];
        if (!decl || decl->kind != IRON_NODE_FUNC_DECL) continue;

        Iron_FuncDecl *fd = (Iron_FuncDecl *)decl;
        if (!fd->body || fd->body->kind != IRON_NODE_BLOCK) continue;

        Iron_Block *body = (Iron_Block *)fd->body;
        int hit = 0;
        for (int si = 0; si < body->stmt_count; si++) {
            Iron_Node *stmt = body->stmts[si];
            if (!stmt || stmt->kind != IRON_NODE_VAL_DECL) continue;
            Iron_ValDecl *vd = (Iron_ValDecl *)stmt;
            if (!vd->init || vd->init->kind != IRON_NODE_IDENT) continue;
            if (hit == ref_index) {
                Iron_Ident *id = (Iron_Ident *)vd->init;
                return id->resolved_sym;
            }
            hit++;
        }
    }
    return NULL;
}

/* ── Case 1: `leak p` sets sym(p).is_leaked = true ────────────────────────── */

void test_leak_sets_is_leaked_true(void) {
    /* Use `val _ref = p` at end so we can access p's resolved_sym via the ident. */
    static const char *src =
        "object Point {\n"
        "    val x: Int\n"
        "    val y: Int\n"
        "}\n"
        "func main() {\n"
        "    val p = heap Point(1, 2)\n"
        "    leak p\n"
        "    val _ref = p\n"
        "}\n";

    Iron_AnalyzeResult res = analyze_src(src);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, count_errors(),
        "Unexpected errors in test_leak_sets_is_leaked_true");

    /* Get p's symbol via the _ref ident (first ident-init val decl) */
    Iron_Symbol *sym = find_ref_sym(res.program, 0);
    TEST_ASSERT_NOT_NULL_MESSAGE(sym, "Symbol 'p' not found via _ref ident");

    /* POL-05: is_leaked must be true after `leak p` */
    TEST_ASSERT_TRUE_MESSAGE(sym->is_leaked,
        "Expected sym->is_leaked == true after 'leak p'");
}

/* ── Case 2: no leak stmt — sym(p).is_leaked = false ──────────────────────── */

void test_no_leak_keeps_is_leaked_false(void) {
    static const char *src =
        "object Point {\n"
        "    val x: Int\n"
        "    val y: Int\n"
        "}\n"
        "func main() {\n"
        "    val p = heap Point(1, 2)\n"
        "    val _ref = p\n"
        "}\n";

    Iron_AnalyzeResult res = analyze_src(src);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, count_errors(),
        "Unexpected errors in test_no_leak_keeps_is_leaked_false");

    /* Get p's symbol via the _ref ident */
    Iron_Symbol *sym = find_ref_sym(res.program, 0);
    TEST_ASSERT_NOT_NULL_MESSAGE(sym, "Symbol 'p' not found via _ref ident");

    /* POL-05: is_leaked must be false — no leak statement present */
    TEST_ASSERT_FALSE_MESSAGE(sym->is_leaked,
        "Expected sym->is_leaked == false (no leak statement present)");
}

/* ── Case 3: two allocs, leak only one ─────────────────────────────────────── */

void test_leak_selective_sets_only_target(void) {
    static const char *src =
        "object Point {\n"
        "    val x: Int\n"
        "    val y: Int\n"
        "}\n"
        "func main() {\n"
        "    val a = heap Point(1, 2)\n"
        "    val b = heap Point(3, 4)\n"
        "    leak a\n"
        "    val _refa = a\n"
        "    val _refb = b\n"
        "}\n";

    Iron_AnalyzeResult res = analyze_src(src);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, count_errors(),
        "Unexpected errors in test_leak_selective_sets_only_target");

    /* First ident-init val decl = _refa -> resolves to a */
    Iron_Symbol *sym_a = find_ref_sym(res.program, 0);
    /* Second ident-init val decl = _refb -> resolves to b */
    Iron_Symbol *sym_b = find_ref_sym(res.program, 1);
    TEST_ASSERT_NOT_NULL_MESSAGE(sym_a, "Symbol 'a' not found");
    TEST_ASSERT_NOT_NULL_MESSAGE(sym_b, "Symbol 'b' not found");

    /* POL-05: only 'a' was leaked */
    TEST_ASSERT_TRUE_MESSAGE(sym_a->is_leaked,
        "Expected sym(a)->is_leaked == true");
    TEST_ASSERT_FALSE_MESSAGE(sym_b->is_leaked,
        "Expected sym(b)->is_leaked == false (not leaked)");
}

/* ── Case 4: double-leak same binding — idempotent ─────────────────────────── */

void test_double_leak_is_idempotent(void) {
    static const char *src =
        "object Point {\n"
        "    val x: Int\n"
        "    val y: Int\n"
        "}\n"
        "func main() {\n"
        "    val p = heap Point(1, 2)\n"
        "    leak p\n"
        "    leak p\n"
        "    val _ref = p\n"
        "}\n";

    Iron_AnalyzeResult res = analyze_src(src);

    /* Double-leak must not produce resolve errors — is_leaked is idempotent */
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, count_errors(),
        "Expected no errors from double-leak (idempotent flag set)");

    /* Get p's symbol via the _ref ident */
    Iron_Symbol *sym = find_ref_sym(res.program, 0);
    TEST_ASSERT_NOT_NULL_MESSAGE(sym, "Symbol 'p' not found via _ref ident");

    /* POL-05: flag remains true after second leak */
    TEST_ASSERT_TRUE_MESSAGE(sym->is_leaked,
        "Expected sym->is_leaked == true after double-leak");
}

/* ── main ─────────────────────────────────────────────────────────────────── */

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_leak_sets_is_leaked_true);
    RUN_TEST(test_no_leak_keeps_is_leaked_false);
    RUN_TEST(test_leak_selective_sets_only_target);
    RUN_TEST(test_double_leak_is_idempotent);
    return UNITY_END();
}
