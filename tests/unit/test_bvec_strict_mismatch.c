/* Phase 23 Plan 23-01 Wave 0: VEC-04 strict array literal element-count mismatch.
 *
 * Tests:
 *   1. Analyzing `val v: [Int; 3] = [1, 2]` emits exactly one E0282
 *      IRON_ERR_VEC_STRICT_LENGTH_MISMATCH diagnostic.
 *   2. The diagnostic message contains "exactly" substring.
 *   3. The hint contains "§3.3:" spec quote.
 *
 * RED until Task 3 lands diagnostics code 282 + VEC-04 emit path in
 * VAL_DECL/VAR_DECL typecheck.c arms.
 *
 * Pattern source: tests/unit/test_readonly_body.c */

#include "unity.h"
#include "analyzer/analyzer.h"
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

static bool msg_contains(int target, const char *needle) {
    for (int i = 0; i < arrlen(diags.items); i++) {
        if (diags.items[i].code == target &&
            diags.items[i].message &&
            strstr(diags.items[i].message, needle))
            return true;
    }
    return false;
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

/* Case 1: val v: [Int; 3] = [1, 2] — 2 elements for strict [Int; 3].
 * VEC-04: fires exactly once with code 282. */
void test_strict_array_literal_count_mismatch_emits_e0282(void) {
    analyze_src(
        "func main() -> Int {\n"
        "    val v: [Int; 3] = [1, 2]\n"
        "    return 0\n"
        "}\n");
    TEST_ASSERT_EQUAL_INT(1, count_code(IRON_ERR_VEC_STRICT_LENGTH_MISMATCH));
    TEST_ASSERT_TRUE(msg_contains(IRON_ERR_VEC_STRICT_LENGTH_MISMATCH, "exactly"));
    TEST_ASSERT_TRUE(hint_contains(IRON_ERR_VEC_STRICT_LENGTH_MISMATCH, "§3.3:"));
}

/* Case 2: val v: [Int; 3] = [1, 2, 3, 4] — 4 elements for strict [Int; 3].
 * VEC-04 also fires for too-many elements. */
void test_strict_array_literal_too_many_elements_emits_e0282(void) {
    analyze_src(
        "func main() -> Int {\n"
        "    val v: [Int; 3] = [1, 2, 3, 4]\n"
        "    return 0\n"
        "}\n");
    TEST_ASSERT_EQUAL_INT(1, count_code(IRON_ERR_VEC_STRICT_LENGTH_MISMATCH));
}

/* Case 3: val v: [Int; 3] = [1, 2, 3] — correct count must NOT emit E0282. */
void test_strict_array_literal_correct_count_no_error(void) {
    analyze_src(
        "func main() -> Int {\n"
        "    val v: [Int; 3] = [1, 2, 3]\n"
        "    return 0\n"
        "}\n");
    TEST_ASSERT_EQUAL_INT(0, count_code(IRON_ERR_VEC_STRICT_LENGTH_MISMATCH));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_strict_array_literal_count_mismatch_emits_e0282);
    RUN_TEST(test_strict_array_literal_too_many_elements_emits_e0282);
    RUN_TEST(test_strict_array_literal_correct_count_no_error);
    return UNITY_END();
}
