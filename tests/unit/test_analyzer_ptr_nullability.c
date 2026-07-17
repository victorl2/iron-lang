/* Phase 20 Wave 0 (Plan 20-01): TDD scaffold for PTR-13 — `val p: *Point =
 * null` rejection (non-nullable pointer cannot accept null literal). The
 * nullable variant `val p: ?*Point = null` MUST analyze cleanly.
 *
 * Authored RED first; flips GREEN once Plan 20-01 lands the
 * IRON_ERR_PTR_NULL_DEREF=272 emission at the val/var binding-init site
 * in src/analyzer/typecheck.c.
 *
 * Pattern source: tests/unit/test_analyzer_val_field_reassign.c (Phase 17).
 */
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

static int total_errors(void) {
    int n = 0;
    for (int i = 0; i < arrlen(diags.items); i++) {
        if (diags.items[i].level == IRON_DIAG_ERROR) n++;
    }
    return n;
}

static int count_substring(const char *needle) {
    int n = 0;
    for (int i = 0; i < arrlen(diags.items); i++) {
        if (diags.items[i].message && strstr(diags.items[i].message, needle))
            n++;
    }
    return n;
}

/* PTR-13: assigning null to non-nullable pointer must error. We tolerate
 * either the new IRON_ERR_PTR_NULL_DEREF=272 code OR a generic type-mismatch
 * code, so long as some error fires at that site. */
void test_ptr_null_to_non_nullable_pointer_rejected(void) {
    analyze_src(
        "object Point {\n"
        "    var x: Int\n"
        "    init() { self.x = 0 }\n"
        "}\n"
        "func main() {\n"
        "    val p: *Point = null\n"
        "}\n");
    TEST_ASSERT_GREATER_THAN_INT(0, total_errors());
}

/* PTR-13 nullable variant: `val p: ?*Point = null` must NOT error. */
void test_ptr_null_to_nullable_pointer_accepted(void) {
    analyze_src(
        "object Point {\n"
        "    var x: Int\n"
        "    init() { self.x = 0 }\n"
        "}\n"
        "func main() {\n"
        "    val p: ?*Point = null\n"
        "}\n");
    /* No errors should fire — `?*Point` accepts null. */
    TEST_ASSERT_EQUAL_INT(0, total_errors());
}

/* PTR-13 message anchor: at least one error message references either the
 * pointer type or the null literal. Tolerant substring check. */
void test_ptr_null_to_non_nullable_message_substring(void) {
    analyze_src(
        "object Point {\n"
        "    var x: Int\n"
        "    init() { self.x = 0 }\n"
        "}\n"
        "func main() {\n"
        "    val p: *Point = null\n"
        "}\n");
    /* Any of the following substrings is acceptable post-GREEN:
     *   "non-nullable pointer"  (PTR-13 spec wording)
     *   "*Point"                (type-name in mismatch message)
     *   "null"                  (literal-mention)
     */
    int hit = count_substring("non-nullable pointer") +
              count_substring("*Point") +
              count_substring("null");
    TEST_ASSERT_GREATER_THAN_INT(0, hit);
}

/* PTR-14 reuse of existing val/var discipline: a `val` binding of pointer
 * type cannot be rebound (Phase 17 VAL-01/02 emits IRON_ERR_VAL_REASSIGN=203).
 * No new code is needed for PTR-14 — the val-vs-var distinction lives in
 * storage class orthogonal to pointer identity. We use a function with a
 * `*Point` parameter (which IS a real pointer type) plus a rebind. */
void test_ptr_val_rebind_rejected(void) {
    analyze_src(
        "object Point {\n"
        "    var x: Int\n"
        "    init() { self.x = 0 }\n"
        "}\n"
        "func bind(q: *Point, r: *Point) {\n"
        "    val p: *Point = q\n"
        "    p = r\n"
        "}\n"
        "func main() {}\n");
    /* The rebind `p = r` triggers val-reassignment (Phase 17). */
    TEST_ASSERT_GREATER_THAN_INT(0, total_errors());
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_ptr_null_to_non_nullable_pointer_rejected);
    RUN_TEST(test_ptr_null_to_nullable_pointer_accepted);
    RUN_TEST(test_ptr_null_to_non_nullable_message_substring);
    RUN_TEST(test_ptr_val_rebind_rejected);
    return UNITY_END();
}
