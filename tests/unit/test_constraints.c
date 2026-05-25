/* test_constraints.c — Phase 33 Wave 2 (OQ-01 generic constraints).
 *
 * Drives the Hashable-constraint enforcement path end-to-end via the public
 * iron_analyze_buffer facade (HARD-01 — no bypass of the analysis pipeline):
 *
 *   (a) `interface Hashable` resolves as a real IRON_SYM_INTERFACE so the
 *       `K: Hashable` bound actually bites at instantiation, and
 *   (b) the primitive-key carve-out in type_satisfies_constraint
 *       (src/analyzer/typecheck.c) treats Int / String as satisfying Hashable
 *       without an explicit `implements`, while a user object that neither
 *       declares `implements Hashable` nor structurally provides the method
 *       surface does NOT satisfy it (→ E0206 / IRON_ERR_GENERIC_CONSTRAINT).
 *
 * Each buffer inlines `interface Hashable` + a generic object bound on it and
 * constructs the type at a call site, so the existing call-site constraint
 * check (typecheck.c check_generic_constraints) fires. user_source_start_line
 * is 0 (no stdlib prepend in the buffer — Hashable is declared inline).
 *
 * E0206 == IRON_ERR_GENERIC_CONSTRAINT (semantic 200-range, see
 * src/diagnostics/diagnostics.h). We grep the rendered diagnostic codes by
 * scanning the Iron_DiagList for that code.
 */
#include "unity.h"
#include "analyzer/analyzer.h"
#include "diagnostics/diagnostics.h"
#include "util/arena.h"

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

/* Count diagnostics carrying the generic-constraint code (E0206). */
static int count_constraint_errors(void) {
    int n = 0;
    for (int i = 0; i < diags.count; i++) {
        if (diags.items[i].code == IRON_ERR_GENERIC_CONSTRAINT) n++;
    }
    return n;
}

static Iron_AnalyzeResult analyze(const char *src) {
    return iron_analyze_buffer(src, strlen(src), "constraints.iron",
                               IRON_ANALYSIS_MODE_CLI, &arena, &diags, NULL, 0);
}

/* Behavior 1: Int satisfies Hashable via the primitive carve-out → no E0206. */
static void test_int_satisfies_hashable(void) {
    const char *src =
        "interface Hashable {\n"
        "    pure func hash() -> Int\n"
        "}\n"
        "object Holder[K: Hashable] {\n"
        "    val key: K\n"
        "}\n"
        "func main() {\n"
        "    val h: Holder[Int] = Holder(key: 1)\n"
        "}\n";
    analyze(src);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, count_constraint_errors(),
        "Int must satisfy Hashable via the primitive carve-out (no E0206)");
}

/* Behavior 2: String satisfies Hashable via the carve-out → no E0206. */
static void test_string_satisfies_hashable(void) {
    const char *src =
        "interface Hashable {\n"
        "    pure func hash() -> Int\n"
        "}\n"
        "object Holder[K: Hashable] {\n"
        "    val key: K\n"
        "}\n"
        "func main() {\n"
        "    val h: Holder[String] = Holder(key: \"x\")\n"
        "}\n";
    analyze(src);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, count_constraint_errors(),
        "String must satisfy Hashable via the primitive carve-out (no E0206)");
}

/* Behavior 3: a user object with neither `implements Hashable` nor a
 * structural method match does NOT satisfy Hashable → E0206 fires. */
static void test_nonhashable_object_rejected(void) {
    const char *src =
        "interface Hashable {\n"
        "    pure func hash() -> Int\n"
        "}\n"
        "object Holder[K: Hashable] {\n"
        "    val key: K\n"
        "}\n"
        "object NotHashable {\n"
        "    val payload: Int\n"
        "}\n"
        "func main() {\n"
        "    val h: Holder[NotHashable] = Holder(key: NotHashable(payload: 1))\n"
        "}\n";
    analyze(src);
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, count_constraint_errors(),
        "a non-Hashable user object must NOT satisfy the bound (E0206 expected)");
}

/* Behavior 4: when the constraint name does NOT resolve to an interface, the
 * existing "don't false-positive" guard preserves a pass (no E0206). With the
 * real Hashable surface this guard is rarely hit in practice, but the carve-out
 * must NOT have broken it — an unknown constraint silently passes. */
static void test_unresolved_constraint_passes(void) {
    const char *src =
        "object Holder[K: NotAnInterface] {\n"
        "    val key: K\n"
        "}\n"
        "func main() {\n"
        "    val h: Holder[Int] = Holder(key: 1)\n"
        "}\n";
    analyze(src);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, count_constraint_errors(),
        "an unresolved constraint must silently pass (guard preserved)");
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_int_satisfies_hashable);
    RUN_TEST(test_string_satisfies_hashable);
    RUN_TEST(test_nonhashable_object_rejected);
    RUN_TEST(test_unresolved_constraint_passes);
    return UNITY_END();
}
