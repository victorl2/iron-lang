/* Phase 22 Plan 02 Wave 0 RED -> GREEN: READ-06 readonly return-type whitelist.
 * Guards that IRON_ERR_READONLY_RETURN_TYPE (280) fires exactly when a readonly
 * method declares an incompatible return type, and NOT for compatible types.
 * Also exercises the Pitfall 6 optimistic-cache for self-referential structs.
 *
 * Iron syntax: readonly methods are declared inside object blocks as
 *   `readonly func f(...) -> T { ... }`.
 *
 * Pattern source: tests/unit/test_readonly_heap_escape.c */

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

static bool hint_contains(int target, const char *needle) {
    for (int i = 0; i < arrlen(diags.items); i++) {
        if (diags.items[i].code == target &&
            diags.items[i].suggestion && strstr(diags.items[i].suggestion, needle))
            return true;
    }
    return false;
}

/* READ-06 case 1: readonly method returning a primitive (Int) emits ZERO E0280.
 * Int is in the whitelist. */
void test_readonly_primitive_return_accepted(void) {
    analyze_src(
        "object X {\n"
        "    readonly func f() -> Int {\n"
        "        return 0\n"
        "    }\n"
        "}\n");
    TEST_ASSERT_EQUAL_INT(0, count_code(IRON_ERR_READONLY_RETURN_TYPE));
}

/* READ-06 case 2: readonly method returning a pointer type (*Int) emits E0280.
 * Pointer types are NOT in the whitelist. */
void test_readonly_pointer_return_rejected(void) {
    analyze_src(
        "object Y {\n"
        "    var n: Int\n"
        "}\n"
        "object X {\n"
        "    readonly func f() -> *Int {\n"
        "        return null\n"
        "    }\n"
        "}\n");
    TEST_ASSERT_EQUAL_INT(1, count_code(IRON_ERR_READONLY_RETURN_TYPE));
    TEST_ASSERT_TRUE(hint_contains(IRON_ERR_READONLY_RETURN_TYPE, "6:"));
    TEST_ASSERT_TRUE(hint_contains(IRON_ERR_READONLY_RETURN_TYPE,
                                   "primitives, fixed structs"));
}

/* READ-06 case 3: readonly method returning Bool emits ZERO E0280. */
void test_readonly_bool_return_accepted(void) {
    analyze_src(
        "object X {\n"
        "    readonly func f() -> Bool {\n"
        "        return true\n"
        "    }\n"
        "}\n");
    TEST_ASSERT_EQUAL_INT(0, count_code(IRON_ERR_READONLY_RETURN_TYPE));
}

/* READ-06 case 4: readonly method returning [Int; 4] (fixed array) emits ZERO E0280.
 * Fixed-size arrays are in the whitelist. */
void test_readonly_fixed_array_return_accepted(void) {
    analyze_src(
        "object X {\n"
        "    readonly func f() -> [Int; 4] {\n"
        "        return [1, 2, 3, 4]\n"
        "    }\n"
        "}\n");
    TEST_ASSERT_EQUAL_INT(0, count_code(IRON_ERR_READONLY_RETURN_TYPE));
}

/* READ-06 case 5: readonly method returning Int? (nullable primitive) emits ZERO E0280.
 * Nullable wrapping a compatible type is in the whitelist. */
void test_readonly_nullable_primitive_return_accepted(void) {
    analyze_src(
        "object X {\n"
        "    readonly func f() -> Int? {\n"
        "        return null\n"
        "    }\n"
        "}\n");
    TEST_ASSERT_EQUAL_INT(0, count_code(IRON_ERR_READONLY_RETURN_TYPE));
}

/* READ-06 case 6: readonly method returning a fixed struct (all-primitive fields)
 * emits ZERO E0280. Transitive struct walk passes when all fields are compatible. */
void test_readonly_fixed_struct_return_accepted(void) {
    analyze_src(
        "object Point {\n"
        "    var x: Int\n"
        "    var y: Int\n"
        "}\n"
        "object X {\n"
        "    readonly func f() -> Point {\n"
        "        return Point(0, 0)\n"
        "    }\n"
        "}\n");
    TEST_ASSERT_EQUAL_INT(0, count_code(IRON_ERR_READONLY_RETURN_TYPE));
}

/* READ-06 case 7: readonly method returning a struct with a pointer field
 * emits E0280. Transitive struct walk rejects structs with incompatible fields. */
void test_readonly_struct_with_pointer_field_rejected(void) {
    analyze_src(
        "object Node {\n"
        "    var value: Int\n"
        "    var next: *Int\n"
        "}\n"
        "object X {\n"
        "    readonly func f() -> Node {\n"
        "        return null\n"
        "    }\n"
        "}\n");
    TEST_ASSERT_EQUAL_INT(1, count_code(IRON_ERR_READONLY_RETURN_TYPE));
}

/* READ-06 case 8: readonly method returning a self-referential struct via nullable
 * emits ZERO E0280. The Pitfall 6 optimistic-cache prevents infinite recursion:
 *   Tree has field `parent: Tree?` (nullable self-reference, both fields compatible).
 * This test would stack-overflow if the optimistic-cache is missing. */
void test_readonly_self_referential_struct_accepted(void) {
    analyze_src(
        "object Tree {\n"
        "    var value: Int\n"
        "    var parent: Tree?\n"
        "}\n"
        "object X {\n"
        "    readonly func f() -> Tree {\n"
        "        return Tree(0, null)\n"
        "    }\n"
        "}\n");
    /* Pitfall 6: this test must complete without stack overflow (optimistic-cache) */
    TEST_ASSERT_EQUAL_INT(0, count_code(IRON_ERR_READONLY_RETURN_TYPE));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_readonly_primitive_return_accepted);
    RUN_TEST(test_readonly_pointer_return_rejected);
    RUN_TEST(test_readonly_bool_return_accepted);
    RUN_TEST(test_readonly_fixed_array_return_accepted);
    RUN_TEST(test_readonly_nullable_primitive_return_accepted);
    RUN_TEST(test_readonly_fixed_struct_return_accepted);
    RUN_TEST(test_readonly_struct_with_pointer_field_rejected);
    RUN_TEST(test_readonly_self_referential_struct_accepted);
    return UNITY_END();
}
