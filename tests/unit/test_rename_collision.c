/* Phase 4 Plan 04-06 Task 01 (EDIT-12, D-10) — rename collision tests.
 *
 * Scope-chain lookup + keyword guard + same-name short-circuit. Tests
 * drive ilsp_rename_collision_check against hand-built Iron_Scope +
 * Iron_Symbol values.
 */

#include "unity.h"

#include "lsp/facade/edit/rename/collision.h"
#include "analyzer/scope.h"
#include "diagnostics/diagnostics.h"
#include "util/arena.h"

#include <stddef.h>
#include <string.h>

static Iron_Arena g_arena;

void setUp(void) {
    g_arena = iron_arena_create(32 * 1024);
}

void tearDown(void) {
    iron_arena_free(&g_arena);
}

/* Allocate an Iron_Symbol with the given name + kind. Fills defaults
 * (span 1:1..1, is_extern false, decl_node NULL). */
static Iron_Symbol *mk_sym(const char *name, Iron_SymbolKind kind) {
    Iron_Span s = { .filename = "/tmp/t.iron",
                     .line = 1, .col = 1, .end_line = 1, .end_col = 1 };
    return iron_symbol_create(&g_arena, name, kind, NULL, s);
}

/* Build a single scope with the given symbols installed. */
static Iron_Scope *mk_scope_with(Iron_Symbol **syms, size_t n_syms) {
    Iron_Scope *s = iron_scope_create(&g_arena, NULL, IRON_SCOPE_BLOCK);
    for (size_t i = 0; i < n_syms; i++) {
        iron_scope_define(s, &g_arena, syms[i]);
    }
    return s;
}

/* ── C1: new_name == old_name → SAME_NAME ─────────────────────────── */

static void test_collision_same_name(void) {
    Iron_Symbol *target = mk_sym("foo", IRON_SYM_VARIABLE);
    Iron_Scope  *scope  = mk_scope_with(&target, 1);

    IronLsp_CollisionResult r;
    memset(&r, 0, sizeof(r));
    ilsp_rename_collision_check(NULL, target, "foo", "foo",
                                   (Iron_Scope *const []){ scope }, 1,
                                   &g_arena, &r);
    TEST_ASSERT_EQUAL_INT(ILSP_COLLISION_SAME_NAME, r.kind);
}

/* ── C2: new_name shadows existing local → SCOPE_CONFLICT ────────── */

static void test_collision_shadows_existing_local(void) {
    Iron_Symbol *target = mk_sym("foo", IRON_SYM_VARIABLE);
    Iron_Symbol *other  = mk_sym("bar", IRON_SYM_VARIABLE);
    Iron_Symbol *syms[] = { target, other };
    Iron_Scope  *scope  = mk_scope_with(syms, 2);

    /* Rename foo -> bar. "bar" already exists in same scope. */
    IronLsp_CollisionResult r;
    memset(&r, 0, sizeof(r));
    ilsp_rename_collision_check(NULL, target, "foo", "bar",
                                   (Iron_Scope *const []){ scope }, 1,
                                   &g_arena, &r);
    TEST_ASSERT_EQUAL_INT(ILSP_COLLISION_SCOPE_CONFLICT, r.kind);
    TEST_ASSERT_NOT_NULL(r.conflict_name);
    TEST_ASSERT_EQUAL_STRING("bar", r.conflict_name);
    TEST_ASSERT_NOT_NULL(r.conflict_file);
}

/* ── C3: new_name is keyword "if" → KEYWORD ──────────────────────── */

static void test_collision_on_keyword_if(void) {
    Iron_Symbol *target = mk_sym("foo", IRON_SYM_VARIABLE);
    Iron_Scope  *scope  = mk_scope_with(&target, 1);

    IronLsp_CollisionResult r;
    memset(&r, 0, sizeof(r));
    ilsp_rename_collision_check(NULL, target, "foo", "if",
                                   (Iron_Scope *const []){ scope }, 1,
                                   &g_arena, &r);
    TEST_ASSERT_EQUAL_INT(ILSP_COLLISION_KEYWORD, r.kind);
}

/* ── C4: new_name is keyword "func" → KEYWORD ────────────────────── */

static void test_collision_on_keyword_func(void) {
    Iron_Symbol *target = mk_sym("foo", IRON_SYM_VARIABLE);
    Iron_Scope  *scope  = mk_scope_with(&target, 1);

    IronLsp_CollisionResult r;
    memset(&r, 0, sizeof(r));
    ilsp_rename_collision_check(NULL, target, "foo", "func",
                                   (Iron_Scope *const []){ scope }, 1,
                                   &g_arena, &r);
    TEST_ASSERT_EQUAL_INT(ILSP_COLLISION_KEYWORD, r.kind);
}

/* ── C5: new_name unconflicted → NONE ─────────────────────────────── */

static void test_collision_none_when_no_conflict(void) {
    Iron_Symbol *target = mk_sym("foo", IRON_SYM_VARIABLE);
    Iron_Scope  *scope  = mk_scope_with(&target, 1);

    IronLsp_CollisionResult r;
    memset(&r, 0, sizeof(r));
    ilsp_rename_collision_check(NULL, target, "foo", "brand_new_name",
                                   (Iron_Scope *const []){ scope }, 1,
                                   &g_arena, &r);
    TEST_ASSERT_EQUAL_INT(ILSP_COLLISION_NONE, r.kind);
}

/* ── C6: conflict in one of several ref-site scopes → SCOPE_CONFLICT ─ */

static void test_collision_in_one_of_multiple_scopes(void) {
    Iron_Symbol *target = mk_sym("foo", IRON_SYM_VARIABLE);
    Iron_Scope  *clean  = mk_scope_with(&target, 1);

    Iron_Symbol *shadow_sym = mk_sym("bar", IRON_SYM_VARIABLE);
    Iron_Symbol *syms[]    = { target, shadow_sym };
    Iron_Scope  *dirty     = mk_scope_with(syms, 2);

    Iron_Scope *const scopes[] = { clean, dirty };
    IronLsp_CollisionResult r;
    memset(&r, 0, sizeof(r));
    ilsp_rename_collision_check(NULL, target, "foo", "bar",
                                   scopes, 2, &g_arena, &r);
    TEST_ASSERT_EQUAL_INT(ILSP_COLLISION_SCOPE_CONFLICT, r.kind);
}

/* ── C7: unrelated top-level symbol in a different module is NOT a
 *         conflict (scoped-only check) → NONE ────────────────────── */

static void test_collision_ignores_out_of_scope_symbols(void) {
    Iron_Symbol *target = mk_sym("foo", IRON_SYM_VARIABLE);
    Iron_Scope  *scope  = mk_scope_with(&target, 1);
    /* "bar" exists in an unrelated scope we don't pass in. */
    Iron_Symbol *unrelated = mk_sym("bar", IRON_SYM_VARIABLE);
    Iron_Scope  *other     = mk_scope_with(&unrelated, 1);
    (void)other;  /* deliberately not passed into the checker */

    IronLsp_CollisionResult r;
    memset(&r, 0, sizeof(r));
    ilsp_rename_collision_check(NULL, target, "foo", "bar",
                                   (Iron_Scope *const []){ scope }, 1,
                                   &g_arena, &r);
    TEST_ASSERT_EQUAL_INT(ILSP_COLLISION_NONE, r.kind);
}

/* ── C8: new_name empty → EMPTY_NAME ─────────────────────────────── */

static void test_collision_empty_name(void) {
    Iron_Symbol *target = mk_sym("foo", IRON_SYM_VARIABLE);
    Iron_Scope  *scope  = mk_scope_with(&target, 1);

    IronLsp_CollisionResult r;
    memset(&r, 0, sizeof(r));
    ilsp_rename_collision_check(NULL, target, "foo", "",
                                   (Iron_Scope *const []){ scope }, 1,
                                   &g_arena, &r);
    TEST_ASSERT_EQUAL_INT(ILSP_COLLISION_EMPTY_NAME, r.kind);

    /* NULL new_name behaves as empty. */
    memset(&r, 0, sizeof(r));
    ilsp_rename_collision_check(NULL, target, "foo", NULL,
                                   (Iron_Scope *const []){ scope }, 1,
                                   &g_arena, &r);
    TEST_ASSERT_EQUAL_INT(ILSP_COLLISION_EMPTY_NAME, r.kind);
}

/* ── C9: keyword helper surface ──────────────────────────────────── */

static void test_is_iron_keyword_hits_and_misses(void) {
    TEST_ASSERT_TRUE (ilsp_rename_is_iron_keyword("if"));
    TEST_ASSERT_TRUE (ilsp_rename_is_iron_keyword("val"));
    TEST_ASSERT_TRUE (ilsp_rename_is_iron_keyword("var"));
    TEST_ASSERT_TRUE (ilsp_rename_is_iron_keyword("return"));
    TEST_ASSERT_TRUE (ilsp_rename_is_iron_keyword("func"));
    TEST_ASSERT_TRUE (ilsp_rename_is_iron_keyword("object"));
    TEST_ASSERT_TRUE (ilsp_rename_is_iron_keyword("interface"));
    TEST_ASSERT_FALSE(ilsp_rename_is_iron_keyword("printf"));
    TEST_ASSERT_FALSE(ilsp_rename_is_iron_keyword("my_var"));
    TEST_ASSERT_FALSE(ilsp_rename_is_iron_keyword(""));
    TEST_ASSERT_FALSE(ilsp_rename_is_iron_keyword(NULL));
}

/* ── C10: enum surface sanity ─────────────────────────────────────── */

static void test_collision_enum_surface(void) {
    TEST_ASSERT_EQUAL_INT(0, ILSP_COLLISION_NONE);
    TEST_ASSERT_EQUAL_INT(1, ILSP_COLLISION_SAME_NAME);
    TEST_ASSERT_EQUAL_INT(2, ILSP_COLLISION_EMPTY_NAME);
    TEST_ASSERT_EQUAL_INT(3, ILSP_COLLISION_KEYWORD);
    TEST_ASSERT_EQUAL_INT(4, ILSP_COLLISION_SCOPE_CONFLICT);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_collision_enum_surface);
    RUN_TEST(test_is_iron_keyword_hits_and_misses);
    RUN_TEST(test_collision_same_name);
    RUN_TEST(test_collision_shadows_existing_local);
    RUN_TEST(test_collision_on_keyword_if);
    RUN_TEST(test_collision_on_keyword_func);
    RUN_TEST(test_collision_none_when_no_conflict);
    RUN_TEST(test_collision_in_one_of_multiple_scopes);
    RUN_TEST(test_collision_ignores_out_of_scope_symbols);
    RUN_TEST(test_collision_empty_name);
    return UNITY_END();
}
