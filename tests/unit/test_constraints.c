/* test_constraints.c — Phase 33 Wave 0 RED anchor (OQ-01 generic constraints).
 *
 * Asserts the Hashable-constraint resolution path: `type_satisfies_constraint`
 * must (a) resolve `Hashable` as a real IRON_SYM_INTERFACE so the bound bites,
 * and (b) carve out known-hashable primitives (Int / String) as satisfying it.
 *
 * This is intentionally RED until Wave 2 lands:
 *   - `interface Hashable { ... }` as a prepended stdlib surface, and
 *   - the primitive-key carve-out inside type_satisfies_constraint
 *     (Pitfall 2: a constraint that can't resolve silently passes).
 *
 * The harness compiles + links today (so ironc + the test build clean under
 * -Werror); the body fails until the carve-out + Hashable surface exist. Wave 2
 * replaces the TEST_FAIL bodies with real assertions driving
 * iron_analyze_buffer over a `Map[String,Int]` (GREEN) and a
 * `Map[NonHashable,Int]` (E0206) buffer.
 */
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

/* RED: Hashable must resolve as IRON_SYM_INTERFACE before Map/Set check, else
 * the constraint silently passes (typecheck.c type_satisfies_constraint). */
static void test_hashable_resolves_as_interface(void) {
    TEST_FAIL_MESSAGE(
        "RED: Wave 2 must prepend `interface Hashable` so it resolves as "
        "IRON_SYM_INTERFACE and the Map[K: Hashable, V] bound bites.");
}

/* RED: Int / String are primitives; type_satisfies_constraint rejects
 * primitives today. Wave 2 adds the known-hashable-primitive carve-out. */
static void test_primitive_key_carveout(void) {
    TEST_FAIL_MESSAGE(
        "RED: Wave 2 must carve out Int/String as satisfying Hashable so "
        "Map[String,Int] / Set[Int] compile without E0206.");
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_hashable_resolves_as_interface);
    RUN_TEST(test_primitive_key_carveout);
    return UNITY_END();
}
