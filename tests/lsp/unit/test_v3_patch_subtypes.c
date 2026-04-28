/* Phase 11 Plan 11-02 Wave 0 stub (PATCH-02).
 *
 * Asserts the typeHierarchy/subtypes resolver surfaces patched methods
 * as virtual TypeHierarchyItem entries with kind=SymbolKind.Method (6)
 * and detail="[patch from <relpath>]".
 *
 * Wave 0 stub — flipped to real assertions in Task 5.
 */

#include "unity.h"

void setUp(void)    {}
void tearDown(void) {}

static void test_phase_11_wave_0_subtypes_stub(void) {
    TEST_IGNORE_MESSAGE(
        "Phase 11 Wave 0 stub - flipped in Task 5 (PATCH-02 typeHierarchy/subtypes)");
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_phase_11_wave_0_subtypes_stub);
    return UNITY_END();
}
