/* test_missing_return.c — Phase 4 Plan 04-01.
 *
 * Wave 0 stub. Task 02 flips each body to exercise the new
 * check_missing_return walker in src/analyzer/typecheck.c, asserting
 * IRON_ERR_MISSING_RETURN=236 with type-appropriate .suggestion
 * ("return 0;", "return 0.0;", "return false;", "return \"\";").
 */

#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static void test_non_void_return_reaches_end(void) {
    TEST_IGNORE_MESSAGE("Wave 0 stub — fills in Task 02 of 04-01");
}

static void test_void_return_skipped(void) {
    TEST_IGNORE_MESSAGE("Wave 0 stub — fills in Task 02 of 04-01");
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_non_void_return_reaches_end);
    RUN_TEST(test_void_return_skipped);
    return UNITY_END();
}
