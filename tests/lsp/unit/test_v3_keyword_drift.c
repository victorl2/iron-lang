/* test_v3_keyword_drift -- Phase 12 Plan 12-01 (Wave 0 stub).
 *
 * Plan 12-03 flips this to a fast in-process drift assertion that
 * the regenerated keyword_mirror.h contains all 6 v3 keywords AND
 * the regenerated grammars include them in alternation/choice.
 * Cheaper to debug than the CMake compare_files invariant test.
 * See VALIDATION.md row 12-02-KW-02 (analogous Phase 4 byte-parity).
 *
 * Wave 0 contract: binary builds, registers under
 * "unit;phase-m1-invariant;phase-12-invariant" labels, runs, and
 * exits 0 reporting the single test as IGNORED. */

#include "unity.h"

void setUp(void)    {}
void tearDown(void) {}

static void test_keyword_drift_pending(void) {
    TEST_IGNORE_MESSAGE(
        "Wave 0 stub - Plan 12-03 wires drift assertions");
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_keyword_drift_pending);
    return UNITY_END();
}
