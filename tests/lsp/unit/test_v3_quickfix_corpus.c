/* test_v3_quickfix_corpus -- Phase 12 Plan 12-01 (Wave 0 stub).
 *
 * Plan 12-03 flips this to real assertions across the v3_quickfix
 * fixture corpus. See VALIDATION.md "Per-Task Verification Map" rows
 * 12-02-QF-01..02 and 12-03-QF-03..05.
 *
 * Wave 0 contract: binary builds, registers under
 * "unit;phase-m1-invariant;phase-12-invariant" labels, runs, and
 * exits 0 reporting the single test as IGNORED. */

#include "unity.h"

void setUp(void)    {}
void tearDown(void) {}

static void test_corpus_pending(void) {
    TEST_IGNORE_MESSAGE(
        "Wave 0 stub - Plan 12-03 wires fixtures + assertions");
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_corpus_pending);
    return UNITY_END();
}
