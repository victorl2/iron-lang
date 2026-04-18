/* test_redundant_cast.c — Phase 4 Plan 04-01.
 *
 * Wave 0 stub. Task 02 flips each body to exercise the new redundant-cast
 * check in src/analyzer/typecheck.c, asserting IRON_WARN_REDUNDANT_CAST=612
 * with .suggestion = inner-expression source text.
 */

#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static void test_same_type_cast_fires_612(void) {
    TEST_IGNORE_MESSAGE("Wave 0 stub — fills in Task 02 of 04-01");
}

static void test_narrowing_cast_stays_quiet(void) {
    TEST_IGNORE_MESSAGE("Wave 0 stub — fills in Task 02 of 04-01");
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_same_type_cast_fires_612);
    RUN_TEST(test_narrowing_cast_stays_quiet);
    return UNITY_END();
}
