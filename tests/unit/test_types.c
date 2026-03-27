/* test_types.c — Unity tests for Iron_Type system and Iron_Scope tree.
 *
 * Tests cover:
 *   - primitive type creation and interning
 *   - structural equality
 *   - nullable, func, array type constructors
 *   - type_to_string
 *   - scope create/define/lookup/parent-chain/shadow/duplicate-rejection
 */

#include "unity.h"
#include "analyzer/types.h"
#include "analyzer/scope.h"
#include "util/arena.h"
#include <string.h>

static Iron_Arena g_arena;

void setUp(void) {
    g_arena = iron_arena_create(1024 * 64);
    iron_types_init(&g_arena);
}

void tearDown(void) {
    iron_arena_free(&g_arena);
}

/* ── Type system tests ────────────────────────────────────────────────────── */

void test_primitive_non_null(void) {
    Iron_Type *t = iron_type_make_primitive(IRON_TYPE_INT);
    TEST_ASSERT_NOT_NULL(t);
    TEST_ASSERT_EQUAL_INT(IRON_TYPE_INT, t->kind);
}

void test_primitive_interning(void) {
    Iron_Type *a = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_Type *b = iron_type_make_primitive(IRON_TYPE_INT);
    TEST_ASSERT_EQUAL_PTR(a, b);
}

void test_primitive_interning_float(void) {
    Iron_Type *a = iron_type_make_primitive(IRON_TYPE_FLOAT);
    Iron_Type *b = iron_type_make_primitive(IRON_TYPE_FLOAT);
    TEST_ASSERT_EQUAL_PTR(a, b);
}

void test_primitive_different_kinds_not_equal_ptr(void) {
    Iron_Type *i = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_Type *f = iron_type_make_primitive(IRON_TYPE_FLOAT);
    TEST_ASSERT_NOT_EQUAL(i, f);
}

void test_type_equals_same_primitive(void) {
    Iron_Type *a = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_Type *b = iron_type_make_primitive(IRON_TYPE_INT);
    TEST_ASSERT_TRUE(iron_type_equals(a, b));
}

void test_type_equals_different_primitives(void) {
    Iron_Type *i = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_Type *f = iron_type_make_primitive(IRON_TYPE_FLOAT);
    TEST_ASSERT_FALSE(iron_type_equals(i, f));
}

void test_nullable_wraps_inner(void) {
    Iron_Type *inner = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_Type *n = iron_type_make_nullable(&g_arena, inner);
    TEST_ASSERT_NOT_NULL(n);
    TEST_ASSERT_EQUAL_INT(IRON_TYPE_NULLABLE, n->kind);
    TEST_ASSERT_EQUAL_PTR(inner, n->nullable.inner);
}

void test_nullable_equals_same(void) {
    Iron_Type *inner = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_Type *a = iron_type_make_nullable(&g_arena, inner);
    Iron_Type *b = iron_type_make_nullable(&g_arena, inner);
    TEST_ASSERT_TRUE(iron_type_equals(a, b));
}

void test_nullable_not_equals_plain(void) {
    Iron_Type *inner = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_Type *n = iron_type_make_nullable(&g_arena, inner);
    TEST_ASSERT_FALSE(iron_type_equals(n, inner));
}

void test_func_type_param_count(void) {
    Iron_Type *p0 = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_Type *p1 = iron_type_make_primitive(IRON_TYPE_BOOL);
    Iron_Type *ret = iron_type_make_primitive(IRON_TYPE_STRING);
    Iron_Type *params[2] = { p0, p1 };
    Iron_Type *fn = iron_type_make_func(&g_arena, params, 2, ret);
    TEST_ASSERT_NOT_NULL(fn);
    TEST_ASSERT_EQUAL_INT(IRON_TYPE_FUNC, fn->kind);
    TEST_ASSERT_EQUAL_INT(2, fn->func.param_count);
    TEST_ASSERT_EQUAL_PTR(ret, fn->func.return_type);
}

void test_func_type_equals(void) {
    Iron_Type *p = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_Type *r = iron_type_make_primitive(IRON_TYPE_BOOL);
    Iron_Type *params[1] = { p };
    Iron_Type *fn1 = iron_type_make_func(&g_arena, params, 1, r);
    Iron_Type *fn2 = iron_type_make_func(&g_arena, params, 1, r);
    TEST_ASSERT_TRUE(iron_type_equals(fn1, fn2));
}

void test_type_to_string_int(void) {
    Iron_Type *t = iron_type_make_primitive(IRON_TYPE_INT);
    const char *s = iron_type_to_string(t, &g_arena);
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_EQUAL_STRING("Int", s);
}

void test_type_to_string_float(void) {
    Iron_Type *t = iron_type_make_primitive(IRON_TYPE_FLOAT);
    const char *s = iron_type_to_string(t, &g_arena);
    TEST_ASSERT_EQUAL_STRING("Float", s);
}

void test_type_to_string_bool(void) {
    Iron_Type *t = iron_type_make_primitive(IRON_TYPE_BOOL);
    const char *s = iron_type_to_string(t, &g_arena);
    TEST_ASSERT_EQUAL_STRING("Bool", s);
}

void test_type_to_string_nullable(void) {
    Iron_Type *inner = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_Type *n = iron_type_make_nullable(&g_arena, inner);
    const char *s = iron_type_to_string(n, &g_arena);
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_EQUAL_STRING("Int?", s);
}

/* ── Scope tests ──────────────────────────────────────────────────────────── */

void test_scope_create(void) {
    Iron_Scope *s = iron_scope_create(&g_arena, NULL, IRON_SCOPE_GLOBAL);
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_NULL(s->parent);
    TEST_ASSERT_EQUAL_INT(IRON_SCOPE_GLOBAL, s->kind);
}

void test_scope_create_with_parent(void) {
    Iron_Scope *parent = iron_scope_create(&g_arena, NULL, IRON_SCOPE_GLOBAL);
    Iron_Scope *child = iron_scope_create(&g_arena, parent, IRON_SCOPE_BLOCK);
    TEST_ASSERT_EQUAL_PTR(parent, child->parent);
    TEST_ASSERT_EQUAL_INT(IRON_SCOPE_BLOCK, child->kind);
}

void test_scope_define_and_lookup(void) {
    Iron_Scope *s = iron_scope_create(&g_arena, NULL, IRON_SCOPE_GLOBAL);
    Iron_Span span = {0};
    Iron_Symbol *sym = iron_symbol_create(&g_arena, "x", IRON_SYM_VARIABLE, NULL, span);
    bool ok = iron_scope_define(s, &g_arena, sym);
    TEST_ASSERT_TRUE(ok);
    Iron_Symbol *found = iron_scope_lookup(s, "x");
    TEST_ASSERT_NOT_NULL(found);
    TEST_ASSERT_EQUAL_STRING("x", found->name);
}

void test_scope_lookup_undefined_returns_null(void) {
    Iron_Scope *s = iron_scope_create(&g_arena, NULL, IRON_SCOPE_GLOBAL);
    Iron_Symbol *found = iron_scope_lookup(s, "undefined_symbol");
    TEST_ASSERT_NULL(found);
}

void test_scope_lookup_walks_parent_chain(void) {
    Iron_Scope *parent = iron_scope_create(&g_arena, NULL, IRON_SCOPE_GLOBAL);
    Iron_Scope *child = iron_scope_create(&g_arena, parent, IRON_SCOPE_BLOCK);
    Iron_Span span = {0};
    Iron_Symbol *sym = iron_symbol_create(&g_arena, "y", IRON_SYM_VARIABLE, NULL, span);
    iron_scope_define(parent, &g_arena, sym);
    /* lookup from child should find symbol defined in parent */
    Iron_Symbol *found = iron_scope_lookup(child, "y");
    TEST_ASSERT_NOT_NULL(found);
    TEST_ASSERT_EQUAL_STRING("y", found->name);
}

void test_scope_define_duplicate_rejected(void) {
    Iron_Scope *s = iron_scope_create(&g_arena, NULL, IRON_SCOPE_GLOBAL);
    Iron_Span span = {0};
    Iron_Symbol *sym1 = iron_symbol_create(&g_arena, "z", IRON_SYM_VARIABLE, NULL, span);
    Iron_Symbol *sym2 = iron_symbol_create(&g_arena, "z", IRON_SYM_VARIABLE, NULL, span);
    bool first  = iron_scope_define(s, &g_arena, sym1);
    bool second = iron_scope_define(s, &g_arena, sym2);
    TEST_ASSERT_TRUE(first);
    TEST_ASSERT_FALSE(second);
}

void test_scope_shadow_parent(void) {
    Iron_Scope *parent = iron_scope_create(&g_arena, NULL, IRON_SCOPE_GLOBAL);
    Iron_Scope *child  = iron_scope_create(&g_arena, parent, IRON_SCOPE_BLOCK);
    Iron_Span span = {0};
    Iron_Symbol *sym_parent = iron_symbol_create(&g_arena, "w", IRON_SYM_VARIABLE, NULL, span);
    Iron_Symbol *sym_child  = iron_symbol_create(&g_arena, "w", IRON_SYM_VARIABLE, NULL, span);
    iron_scope_define(parent, &g_arena, sym_parent);
    iron_scope_define(child,  &g_arena, sym_child);
    /* child lookup should return the child-scope symbol (shadow) */
    Iron_Symbol *found = iron_scope_lookup(child, "w");
    TEST_ASSERT_EQUAL_PTR(sym_child, found);
    /* parent scope should still have its own symbol */
    Iron_Symbol *found_parent = iron_scope_lookup_local(parent, "w");
    TEST_ASSERT_EQUAL_PTR(sym_parent, found_parent);
}

void test_scope_lookup_local_does_not_walk_parent(void) {
    Iron_Scope *parent = iron_scope_create(&g_arena, NULL, IRON_SCOPE_GLOBAL);
    Iron_Scope *child  = iron_scope_create(&g_arena, parent, IRON_SCOPE_BLOCK);
    Iron_Span span = {0};
    Iron_Symbol *sym = iron_symbol_create(&g_arena, "v", IRON_SYM_VARIABLE, NULL, span);
    iron_scope_define(parent, &g_arena, sym);
    Iron_Symbol *found = iron_scope_lookup_local(child, "v");
    TEST_ASSERT_NULL(found);
}

/* ── main ─────────────────────────────────────────────────────────────────── */

int main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_primitive_non_null);
    RUN_TEST(test_primitive_interning);
    RUN_TEST(test_primitive_interning_float);
    RUN_TEST(test_primitive_different_kinds_not_equal_ptr);
    RUN_TEST(test_type_equals_same_primitive);
    RUN_TEST(test_type_equals_different_primitives);
    RUN_TEST(test_nullable_wraps_inner);
    RUN_TEST(test_nullable_equals_same);
    RUN_TEST(test_nullable_not_equals_plain);
    RUN_TEST(test_func_type_param_count);
    RUN_TEST(test_func_type_equals);
    RUN_TEST(test_type_to_string_int);
    RUN_TEST(test_type_to_string_float);
    RUN_TEST(test_type_to_string_bool);
    RUN_TEST(test_type_to_string_nullable);
    RUN_TEST(test_scope_create);
    RUN_TEST(test_scope_create_with_parent);
    RUN_TEST(test_scope_define_and_lookup);
    RUN_TEST(test_scope_lookup_undefined_returns_null);
    RUN_TEST(test_scope_lookup_walks_parent_chain);
    RUN_TEST(test_scope_define_duplicate_rejected);
    RUN_TEST(test_scope_shadow_parent);
    RUN_TEST(test_scope_lookup_local_does_not_walk_parent);

    return UNITY_END();
}
