/* test_parity_ironc_lsp_suggestions.c — Phase 4 Plan 04-01.
 *
 * Wave 0 stub. Task 02 fills in a byte-identity suggestion parity test
 * between CLI and LSP `iron_analyze_buffer` emit over 5 fixture buffers
 * (one per P1 code). Sibling of tests/lsp/parity/test_parity_ironc_lsp.c
 * but specifically targets `.suggestion` rather than message-only parity.
 */

#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static void test_parity_suggestion_bytes_identical(void) {
    TEST_IGNORE_MESSAGE("Wave 0 stub — fills in Task 02 of 04-01");
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_parity_suggestion_bytes_identical);
    return UNITY_END();
}
