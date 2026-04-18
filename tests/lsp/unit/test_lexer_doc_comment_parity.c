/* test_lexer_doc_comment_parity -- Phase 3 Plan 01 Task 02 (NAV-14).
 *
 * Wave 0 stub: flipped to real assertions by Plan 01 Task 02.
 * Localized parity test -- asserts Iron_DiagList is byte-identical with
 * and without `///` prefixes on the same source. Regression localizer
 * for the full `test_parity_ironc_lsp` sweep.
 */
#include "unity.h"

#include <stdbool.h>
#include <stddef.h>

void setUp(void) {}
void tearDown(void) {}

static void test_lexer_doc_comment_parity_placeholder(void) {
    TEST_IGNORE_MESSAGE("Wave 0 stub -- implemented by Plan 01 Task 02");
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_lexer_doc_comment_parity_placeholder);
    return UNITY_END();
}
