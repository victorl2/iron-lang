/* test_v3_keyword_filter -- Phase 12 Plan 12-01 (Wave 0 stub).
 *
 * Plan 12-02 flips this to real predicate-matrix assertions over
 * ilsp_keyword_visible_at across the 6 v3 keywords plus 4 baseline
 * existing keywords. See VALIDATION.md row 12-02-KW-03 + 12-02-KW-03b.
 *
 * Wave 0 contract: binary builds, registers under
 * "unit;phase-m1-invariant;phase-12-invariant" labels, runs, and
 * exits 0 reporting the single test as IGNORED. */

#include "unity.h"

void setUp(void)    {}
void tearDown(void) {}

static void test_keyword_filter_pending(void) {
    TEST_IGNORE_MESSAGE(
        "Wave 0 stub - Plan 12-02 implements keyword_filter + flips assertions");
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_keyword_filter_pending);
    return UNITY_END();
}
