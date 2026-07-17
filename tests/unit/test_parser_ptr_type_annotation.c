/* Phase 20 Wave 0 (Plan 20-01): TDD scaffold for PTR-13/14/15 — parser
 * recognition of `*T` / `*var T` / `?*T` / `?*var T` type annotations.
 *
 * Authored RED first; flips GREEN once Plan 20-01 lands the
 * Iron_TypeAnnotation extension (is_pointer / is_var_pointer /
 * pointer_pointee fields) plus the parser leading-`?` and leading-`*`
 * handlers in iron_parse_type_annotation_impl.
 *
 * Pattern source: tests/unit/test_analyzer_parm_var_slot.c (Phase 18). */
#include "unity.h"
#include "lexer/lexer.h"
#include "parser/parser.h"
#include "parser/ast.h"
#include "diagnostics/diagnostics.h"
#include "util/arena.h"
#include "stb_ds.h"

#include <string.h>

static Iron_Arena    arena;
static Iron_DiagList diags;

void setUp(void) {
    arena = iron_arena_create(131072);
    diags = iron_diaglist_create();
}

void tearDown(void) {
    iron_diaglist_free(&diags);
    iron_arena_free(&arena);
}

/* Lex + parse helper. Returns the parsed Iron_Program (non-NULL by
 * iron_parse contract). */
static Iron_Program *parse_src(const char *src) {
    Iron_Lexer lx = iron_lexer_create(src, "test.iron", &arena, &diags);
    Iron_Token *toks = iron_lex_all(&lx);
    int n = (int)arrlen(toks);
    Iron_Parser p = iron_parser_create(toks, n, src, "test.iron",
                                       &arena, &diags);
    Iron_Node *prog = iron_parse(&p);
    return (Iron_Program *)prog;
}

/* Walk the program for the first IRON_NODE_FUNC_DECL and return its first
 * parameter's type annotation. Returns NULL if not found. */
static Iron_TypeAnnotation *first_param_type_ann(Iron_Program *prog) {
    if (!prog || !prog->decls) return NULL;
    for (int i = 0; i < prog->decl_count; i++) {
        Iron_Node *d = prog->decls[i];
        if (!d || d->kind != IRON_NODE_FUNC_DECL) continue;
        Iron_FuncDecl *fd = (Iron_FuncDecl *)d;
        if (fd->param_count <= 0 || !fd->params) continue;
        Iron_Param *pp = (Iron_Param *)fd->params[0];
        if (!pp || !pp->type_ann) continue;
        if (pp->type_ann->kind != IRON_NODE_TYPE_ANNOTATION) continue;
        return (Iron_TypeAnnotation *)pp->type_ann;
    }
    return NULL;
}

/* PTR-13: `func f(p: *Point) {}` — non-nullable ptr to Point. */
void test_ptr_simple_pointer(void) {
    Iron_Program *prog = parse_src("func f(p: *Point) {}\n");
    Iron_TypeAnnotation *ann = first_param_type_ann(prog);
    TEST_ASSERT_NOT_NULL(ann);
    TEST_ASSERT_TRUE(ann->is_pointer);
    TEST_ASSERT_FALSE(ann->is_var_pointer);
    TEST_ASSERT_FALSE(ann->is_nullable);
    TEST_ASSERT_NOT_NULL(ann->pointer_pointee);
    Iron_TypeAnnotation *inner = (Iron_TypeAnnotation *)ann->pointer_pointee;
    TEST_ASSERT_NOT_NULL(inner->name);
    TEST_ASSERT_EQUAL_STRING("Point", inner->name);
}

/* PTR-14: `func f(p: *var Point) {}` — mutable pointer. */
void test_ptr_var_pointer(void) {
    Iron_Program *prog = parse_src("func f(p: *var Point) {}\n");
    Iron_TypeAnnotation *ann = first_param_type_ann(prog);
    TEST_ASSERT_NOT_NULL(ann);
    TEST_ASSERT_TRUE(ann->is_pointer);
    TEST_ASSERT_TRUE(ann->is_var_pointer);
    TEST_ASSERT_FALSE(ann->is_nullable);
}

/* PTR-13: `func f(p: ?*Point) {}` — leading `?` flags pointer as nullable. */
void test_ptr_nullable_pointer(void) {
    Iron_Program *prog = parse_src("func f(p: ?*Point) {}\n");
    Iron_TypeAnnotation *ann = first_param_type_ann(prog);
    TEST_ASSERT_NOT_NULL(ann);
    TEST_ASSERT_TRUE(ann->is_pointer);
    TEST_ASSERT_FALSE(ann->is_var_pointer);
    TEST_ASSERT_TRUE(ann->is_nullable);
}

/* PTR-13/14 combined: `func f(p: ?*var Point) {}` — nullable mutable ptr. */
void test_ptr_nullable_var_pointer(void) {
    Iron_Program *prog = parse_src("func f(p: ?*var Point) {}\n");
    Iron_TypeAnnotation *ann = first_param_type_ann(prog);
    TEST_ASSERT_NOT_NULL(ann);
    TEST_ASSERT_TRUE(ann->is_pointer);
    TEST_ASSERT_TRUE(ann->is_var_pointer);
    TEST_ASSERT_TRUE(ann->is_nullable);
}

/* `func f(p: **Point) {}` — multi-level pointer SYNTAX is legal at parse
 * time; multi-level auto-deref is rejected later (Plan 20-02). */
void test_ptr_double_pointer(void) {
    Iron_Program *prog = parse_src("func f(p: **Point) {}\n");
    Iron_TypeAnnotation *ann = first_param_type_ann(prog);
    TEST_ASSERT_NOT_NULL(ann);
    TEST_ASSERT_TRUE(ann->is_pointer);
    TEST_ASSERT_NOT_NULL(ann->pointer_pointee);
    Iron_TypeAnnotation *inner = (Iron_TypeAnnotation *)ann->pointer_pointee;
    TEST_ASSERT_TRUE(inner->is_pointer);
}

/* Regression: existing trailing-`?` non-pointer path still works.
 * `func f(p: Int?) {}` — is_nullable=true AND is_pointer=false. */
void test_ptr_trailing_nullable_non_pointer(void) {
    Iron_Program *prog = parse_src("func f(p: Int?) {}\n");
    Iron_TypeAnnotation *ann = first_param_type_ann(prog);
    TEST_ASSERT_NOT_NULL(ann);
    TEST_ASSERT_FALSE(ann->is_pointer);
    TEST_ASSERT_TRUE(ann->is_nullable);
    TEST_ASSERT_NOT_NULL(ann->name);
    TEST_ASSERT_EQUAL_STRING("Int", ann->name);
}

/* val p: Int? regression — trailing-`?` path on a top-level binding. */
void test_ptr_trailing_nullable_local(void) {
    /* This is purely a parser regression check; we just want the binding to
     * parse without IRON_ERR_UNEXPECTED_TOKEN. */
    (void)parse_src("func g() { val p: Int? = null }\n");
    int err_count = 0;
    for (int i = 0; i < arrlen(diags.items); i++) {
        if (diags.items[i].level == IRON_DIAG_ERROR &&
            diags.items[i].code == IRON_ERR_UNEXPECTED_TOKEN) err_count++;
    }
    TEST_ASSERT_EQUAL_INT(0, err_count);
}

/* Negative parser-level: bare `*T1` at expression position is malformed.
 * Just verify the parser surfaces SOME diagnostic (substring tolerance). */
void test_ptr_star_at_expr_position_is_error(void) {
    (void)parse_src("func g() { val x = *T1 }\n");
    /* Either parser or analyzer should reject — we're tolerant about which. */
    int err = 0;
    for (int i = 0; i < arrlen(diags.items); i++) {
        if (diags.items[i].level == IRON_DIAG_ERROR) err++;
    }
    TEST_ASSERT_GREATER_THAN_INT(0, err);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_ptr_simple_pointer);
    RUN_TEST(test_ptr_var_pointer);
    RUN_TEST(test_ptr_nullable_pointer);
    RUN_TEST(test_ptr_nullable_var_pointer);
    RUN_TEST(test_ptr_double_pointer);
    RUN_TEST(test_ptr_trailing_nullable_non_pointer);
    RUN_TEST(test_ptr_trailing_nullable_local);
    RUN_TEST(test_ptr_star_at_expr_position_is_error);
    return UNITY_END();
}
