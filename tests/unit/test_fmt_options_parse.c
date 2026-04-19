/* Phase 5 Plan 05-01 (FMT-05, D-02, D-16): Unity tests for the [fmt]
 * TOML section parser.
 *
 * NOTE: scaffolded in Task 1; LIVE assertions land in Task 2 after
 * src/cli/toml.c gains the section==4 branch. Until then, this file
 * registers a single TEST_IGNORE'd test so the binary links and the
 * phase-m4-invariant label has a placeholder slot. Task 2 replaces
 * this body with the 6 real scenarios.
 *
 * Test name preserved across the Wave-0 -> LIVE flip:
 *   - test_missing_fmt_section_uses_defaults
 *   - test_all_fmt_keys_set
 *   - test_partial_fmt_keys
 *   - test_invalid_int_falls_back
 *   - test_invalid_bool_falls_back
 *   - test_unknown_key_silently_ignored
 */

#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

void test_fmt_options_parse_pending_task_2(void) {
    TEST_IGNORE_MESSAGE("Wave 0 stub -- 6 [fmt] TOML scenarios live in Task 2");
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_fmt_options_parse_pending_task_2);
    return UNITY_END();
}
