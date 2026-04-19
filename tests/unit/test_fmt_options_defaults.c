/* Phase 5 Plan 05-01 (FMT-05, D-02): asserts iron_fmt_options_default()
 * returns the v1 locked defaults. CRITICAL: indent_width = 2 (NOT 4)
 * per RESEARCH A1 override -- preserves all integration goldens. */

#include "unity.h"
#include "fmt/options.h"

void setUp(void) {}
void tearDown(void) {}

void test_defaults_match_v1_locks(void) {
    IronFmtOptions o = iron_fmt_options_default();
    TEST_ASSERT_EQUAL_INT(100,  o.line_width);
    TEST_ASSERT_EQUAL_INT(2,    o.indent_width);  /* NOT 4 -- RESEARCH A1 */
    TEST_ASSERT_FALSE(o.use_tabs);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_defaults_match_v1_locks);
    return UNITY_END();
}
