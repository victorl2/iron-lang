/* Phase 23 Plan 23-01 Wave 0: bounded↔strict disjoint-shape rejection.
 *
 * Tests:
 *   1. iron_type_equals returns false for [Int; <=4] vs [Int; 4]  (is_bounded differs)
 *   2. iron_analyze_buffer emits E0283 IRON_ERR_VEC_BOUNDED_TO_FIXED_FORBIDDEN
 *      when a bounded vector is assigned to a strict array binding.
 *
 * RED until Tasks 2+3 land iron_type_make_array(is_bounded) + is_bounded
 * field on Iron_Type + types_assignable disjoint-shape guard + E0283 code.
 *
 * Pattern source: tests/unit/test_readonly_body.c */

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

static int count_code(int target) {
    int n = 0;
    for (int i = 0; i < arrlen(diags.items); i++) {
        if (diags.items[i].code == target) n++;
    }
    return n;
}

static bool hint_contains(int target, const char *needle) {
    for (int i = 0; i < arrlen(diags.items); i++) {
        if (diags.items[i].code == target &&
            diags.items[i].suggestion &&
            strstr(diags.items[i].suggestion, needle))
            return true;
    }
    return false;
}

/* Case 1: [Int; <=4] and [Int; 4] must NOT be structurally equal.
 * iron_type_make_array 4th param: is_bounded. */
void test_bvec_strict_not_equal(void) {
    iron_types_init(&arena);  /* initialize primitive singletons */
    Iron_Type *int_ty  = iron_type_make_primitive(IRON_TYPE_INT); /* interned singleton */
    /* [Int; <=4] bounded=true */
    Iron_Type *bvec   = iron_type_make_array(&arena, int_ty, 4, /*is_bounded=*/true);
    /* [Int; 4] bounded=false */
    Iron_Type *strict = iron_type_make_array(&arena, int_ty, 4, /*is_bounded=*/false);
    TEST_ASSERT_FALSE(iron_type_equals(bvec, strict));
}

/* Case 2: [Int; <=3] assigned to [Int; 3] binding produces E0283.
 * §3.3: [T; <=N] and [T; N] are disjoint types. */
void test_bvec_bounded_to_fixed_emits_e0283(void) {
    analyze_src(
        "func main() -> Int {\n"
        "    val a: [Int; <=3] = [1]\n"
        "    val b: [Int; 3] = a\n"
        "    return 0\n"
        "}\n");
    TEST_ASSERT_GREATER_THAN_INT(0, count_code(IRON_ERR_VEC_BOUNDED_TO_FIXED_FORBIDDEN));
    TEST_ASSERT_TRUE(hint_contains(IRON_ERR_VEC_BOUNDED_TO_FIXED_FORBIDDEN, "§3.3:"));
    TEST_ASSERT_TRUE(hint_contains(IRON_ERR_VEC_BOUNDED_TO_FIXED_FORBIDDEN, "disjoint"));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_bvec_strict_not_equal);
    RUN_TEST(test_bvec_bounded_to_fixed_emits_e0283);
    return UNITY_END();
}
