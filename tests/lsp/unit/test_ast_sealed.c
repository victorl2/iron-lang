/* test_ast_sealed -- Phase 3 Plan 01 Task 03 (NAV-15).
 *
 * Wave 0 stub: flipped to real assertions by Plan 01 Task 03.
 * Covers:
 *   1. analyzer sets program->sealed = true on success
 *   2. IRON_AST_ASSERT_UNSEALED is a no-op in release (NDEBUG)
 *   3. IRON_AST_ASSERT_UNSEALED fires iron_ice in debug
 */
#include "unity.h"

#include <stdbool.h>
#include <stddef.h>

void setUp(void) {}
void tearDown(void) {}

static void test_ast_sealed_placeholder(void) {
    TEST_IGNORE_MESSAGE("Wave 0 stub -- implemented by Plan 01 Task 03");
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_ast_sealed_placeholder);
    return UNITY_END();
}
