/* test_type_mismatch_literal.c — Phase 4 Plan 04-01.
 *
 * Wave 0 stub. Task 02 flips each body to exercise the new literal-position
 * narrowing of IRON_ERR_TYPE_MISMATCH in src/analyzer/typecheck.c:
 *   - literal RHS of wrong type emits code 235 (not 202) with a retyped-
 *     literal .suggestion (e.g. "42.0" for Int->Float)
 *   - non-literal RHS continues to emit code 202 with .suggestion==NULL
 */

#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static void test_int_to_float_literal_emits_235(void) {
    TEST_IGNORE_MESSAGE("Wave 0 stub — fills in Task 02 of 04-01");
}

static void test_non_literal_mismatch_stays_202(void) {
    TEST_IGNORE_MESSAGE("Wave 0 stub — fills in Task 02 of 04-01");
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_int_to_float_literal_emits_235);
    RUN_TEST(test_non_literal_mismatch_stays_202);
    return UNITY_END();
}
