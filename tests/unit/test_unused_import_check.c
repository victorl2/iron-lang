/* test_unused_import_check.c — Phase 4 Plan 04-01.
 *
 * Wave 0 stub. Task 02 flips each body to exercise the new
 * emit_unused_imports post-pass walker in src/analyzer/resolve.c,
 * asserting IRON_WARN_UNUSED_IMPORT=611 with non-NULL .suggestion.
 */

#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static void test_used_import_stays_quiet(void) {
    TEST_IGNORE_MESSAGE("Wave 0 stub — fills in Task 02 of 04-01");
}

static void test_unused_import_fires_611(void) {
    TEST_IGNORE_MESSAGE("Wave 0 stub — fills in Task 02 of 04-01");
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_used_import_stays_quiet);
    RUN_TEST(test_unused_import_fires_611);
    return UNITY_END();
}
