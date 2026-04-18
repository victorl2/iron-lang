/* test_lexer_doc_comment -- Phase 3 Plan 01 Task 02 (NAV-14).
 *
 * Wave 0 stub: flipped to real assertions by Plan 01 Task 02.
 * Covers:
 *   1. `///` tokenized as IRON_TOK_DOC_COMMENT with trimmed body
 *   2. Multi-line runs aggregated onto the following decl
 *   3. Blank line breaks doc-comment association
 *   4. Runs do not cross decl boundaries (Pitfall 7)
 *   5. 8 KB cap mitigation for T-03-01 (pathological input)
 *   6. All 8 decl kinds carry `doc_comment`
 */
#include "unity.h"

#include <stdbool.h>
#include <stddef.h>

void setUp(void) {}
void tearDown(void) {}

static void test_lexer_doc_comment_placeholder(void) {
    TEST_IGNORE_MESSAGE("Wave 0 stub -- implemented by Plan 01 Task 02");
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_lexer_doc_comment_placeholder);
    return UNITY_END();
}
