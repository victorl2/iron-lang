/* Phase 20 Wave 0 (Plan 20-01): TDD scaffold for PTR-01 + PTR-12 — distinct
 * Iron_TypeKind for *T / *var T / T plus the locked covariance rule
 * (`*var T -> *T` allowed; `*T -> *var T` rejected; cross-pointee always
 * rejected).
 *
 * Authored RED first; flips GREEN once Plan 20-01 lands the IRON_TYPE_PTR
 * variant in src/analyzer/types.h plus iron_type_make_ptr in types.c plus
 * the IRON_TYPE_PTR cases in iron_type_equals + types_assignable.
 *
 * Note: types_assignable is a static helper inside src/analyzer/typecheck.c.
 * We exercise it indirectly via the full analyzer pipeline (val-decl
 * type-mismatch emission) when needed. The pure-types tests below use the
 * public iron_type_make_ptr + iron_type_equals API. */
#include "unity.h"
#include "analyzer/analyzer.h"
#include "analyzer/types.h"
#include "diagnostics/diagnostics.h"
#include "util/arena.h"
#include "stb_ds.h"

#include <string.h>

static Iron_Arena    arena;
static Iron_DiagList diags;

void setUp(void) {
    arena = iron_arena_create(131072);
    diags = iron_diaglist_create();
    iron_types_init(&arena);
}

void tearDown(void) {
    iron_diaglist_free(&diags);
    iron_arena_free(&arena);
}

static void analyze_src(const char *src) {
    Iron_AnalyzeResult r = iron_analyze_buffer(
        src, strlen(src), "test.iron",
        IRON_ANALYSIS_MODE_CLI,
        &arena, &diags, NULL,
        0);
    (void)r;
}

static int count_with_code(int target) {
    int n = 0;
    for (int i = 0; i < arrlen(diags.items); i++) {
        if (diags.items[i].code == target) n++;
    }
    return n;
}

static int total_errors(void) {
    int n = 0;
    for (int i = 0; i < arrlen(diags.items); i++) {
        if (diags.items[i].level == IRON_DIAG_ERROR) n++;
    }
    return n;
}

/* PTR-01: *T and *var T are distinct from each other (and from T). */
void test_ptr_kind_distinct(void) {
    Iron_Type *int_t = iron_type_make_primitive(IRON_TYPE_INT);
    TEST_ASSERT_NOT_NULL(int_t);
    Iron_Type *ptr_int      = iron_type_make_ptr(&arena, int_t, false);
    Iron_Type *ptr_var_int  = iron_type_make_ptr(&arena, int_t, true);
    TEST_ASSERT_NOT_NULL(ptr_int);
    TEST_ASSERT_NOT_NULL(ptr_var_int);
    TEST_ASSERT_EQUAL_INT(IRON_TYPE_PTR, ptr_int->kind);
    TEST_ASSERT_EQUAL_INT(IRON_TYPE_PTR, ptr_var_int->kind);
    /* `*Int` and `*var Int` are NOT equal */
    TEST_ASSERT_FALSE(iron_type_equals(ptr_int, ptr_var_int));
    /* `Int` and `*Int` are NOT equal */
    TEST_ASSERT_FALSE(iron_type_equals(int_t, ptr_int));
}

/* `*T` equals another `*T` with same pointee (structural equality). */
void test_ptr_kind_structural_equality(void) {
    Iron_Type *int_t = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_Type *a = iron_type_make_ptr(&arena, int_t, false);
    Iron_Type *b = iron_type_make_ptr(&arena, int_t, false);
    TEST_ASSERT_TRUE(iron_type_equals(a, b));
}

/* PTR-12 covariance: `*var T -> *T` allowed at the type-system level.
 * Note: full e2e via `&pt` codegen is deferred to Plan 20-02; here we
 * check the types_assignable rule by constructing both pointer types
 * directly via iron_type_make_ptr. The plan locks the rule:
 *   *var T -> *T  : ALLOWED  (drop mutability)
 *   *T     -> *var T : REJECTED
 * Verification path: parse a function signature taking `*Point`, parse a
 * second function with a `*var Point` parameter, and confirm assigning a
 * `*var Point` parameter to a `*Point`-typed val analyzes cleanly. */
void test_ptr_assign_var_to_nonvar_allowed(void) {
    analyze_src(
        "object Point {\n"
        "    var x: Int\n"
        "    init() { self.x = 0 }\n"
        "}\n"
        "func reads(q: *var Point) {\n"
        "    val r: *Point = q\n"
        "}\n"
        "func main() {}\n");
    /* No type-mismatch on `val r: *Point = q` -- covariance applies. */
    TEST_ASSERT_EQUAL_INT(0, count_with_code(IRON_ERR_TYPE_MISMATCH));
}

/* PTR-12 invariance: `*T -> *var T` rejected at the type-system level. */
void test_ptr_assign_nonvar_to_var_rejected(void) {
    analyze_src(
        "object Point {\n"
        "    var x: Int\n"
        "    init() { self.x = 0 }\n"
        "}\n"
        "func reads(q: *Point) {\n"
        "    val r: *var Point = q\n"
        "}\n"
        "func main() {}\n");
    /* The `val r: *var Point = q` MUST emit a type-mismatch error. */
    TEST_ASSERT_GREATER_THAN_INT(0, total_errors());
}

/* PTR-12 cross-pointee: `*Point -> *Player` rejected. */
void test_ptr_cross_pointee_rejected(void) {
    Iron_Type *int_t   = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_Type *bool_t  = iron_type_make_primitive(IRON_TYPE_BOOL);
    Iron_Type *p_int   = iron_type_make_ptr(&arena, int_t,   false);
    Iron_Type *p_bool  = iron_type_make_ptr(&arena, bool_t,  false);
    /* Different pointees: structural equality MUST be false. */
    TEST_ASSERT_FALSE(iron_type_equals(p_int, p_bool));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_ptr_kind_distinct);
    RUN_TEST(test_ptr_kind_structural_equality);
    RUN_TEST(test_ptr_assign_var_to_nonvar_allowed);
    RUN_TEST(test_ptr_assign_nonvar_to_var_rejected);
    RUN_TEST(test_ptr_cross_pointee_rejected);
    return UNITY_END();
}
