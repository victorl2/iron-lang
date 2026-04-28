/* test_v3_tier_signature_help -- Phase 10 Plan 10-03 Wave 0 stub (TIER-04).
 *
 * Wave 0 placeholder: TEST_IGNORE_MESSAGE so the harness registers under
 * CTest with label `unit;phase-m3-invariant`. Flipped to real assertions
 * in Task 5: drives ilsp_facade_signature_help + asserts the
 * SignatureInformation.label has the `readonly` / `pure ` prefix BEFORE
 * the `func ` token for FuncDecl/MethodDecl with the corresponding tier.
 */
#include "unity.h"

void setUp(void)    {}
void tearDown(void) {}

static void test_phase_10_wave_0_stub(void) {
    TEST_IGNORE_MESSAGE("Phase 10 Wave 0 stub - flipped in Task 5 (TIER-04 signature help label prefix)");
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_phase_10_wave_0_stub);
    return UNITY_END();
}
