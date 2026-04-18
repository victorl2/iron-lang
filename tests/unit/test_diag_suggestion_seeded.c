/* test_diag_suggestion_seeded.c — Phase 4 Plan 04-01.
 *
 * Wave 0 stub. Task 02 flips every body to a LIVE assertion that runs
 * `iron_analyze_buffer` on a fixture that triggers one of the 5 P1 codes
 * and asserts `.suggestion != NULL` on the matching diag.
 *
 * Codes covered (Task 02 flip): IRON_ERR_UNDEFINED_VAR,
 * IRON_ERR_TYPE_MISMATCH_LITERAL, IRON_ERR_MISSING_RETURN,
 * IRON_WARN_UNUSED_IMPORT, IRON_WARN_REDUNDANT_CAST.
 */

#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static void test_undefined_var_has_suggestion(void) {
    TEST_IGNORE_MESSAGE("Wave 0 stub — fills in Task 02 of 04-01");
}

static void test_type_mismatch_literal_has_suggestion(void) {
    TEST_IGNORE_MESSAGE("Wave 0 stub — fills in Task 02 of 04-01");
}

static void test_missing_return_has_suggestion(void) {
    TEST_IGNORE_MESSAGE("Wave 0 stub — fills in Task 02 of 04-01");
}

static void test_unused_import_has_suggestion(void) {
    TEST_IGNORE_MESSAGE("Wave 0 stub — fills in Task 02 of 04-01");
}

static void test_redundant_cast_has_suggestion(void) {
    TEST_IGNORE_MESSAGE("Wave 0 stub — fills in Task 02 of 04-01");
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_undefined_var_has_suggestion);
    RUN_TEST(test_type_mismatch_literal_has_suggestion);
    RUN_TEST(test_missing_return_has_suggestion);
    RUN_TEST(test_unused_import_has_suggestion);
    RUN_TEST(test_redundant_cast_has_suggestion);
    return UNITY_END();
}
