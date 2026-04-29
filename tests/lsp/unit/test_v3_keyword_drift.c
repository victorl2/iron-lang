/* test_v3_keyword_drift -- Phase 12 Plan 12-03 — fast in-process drift
 * assertion that the regenerated keyword_mirror.h's
 * ILSP_COMPLETION_KEYWORDS[] array contains all 6 v3 keywords (init,
 * mut, patch, pub, pure, readonly).
 *
 * Redundant with the CMake compare_files invariants (Phase 6 D-01
 * grammar drift + Phase 4 D-01 mirror drift) but cheaper to debug —
 * a missing v3 keyword fires here as a Unity assertion on the keyword
 * string instead of a configure-time compare_files diff against the
 * generated source.
 *
 * Mirrors the style of tests/unit/test_completion_keyword_mirror.c but
 * narrowed to the 6 v3 keywords from Phase 8 onward. */

#include "unity.h"

#include "keyword_mirror.h"  /* generated under ${CMAKE_BINARY_DIR}/generated; ILSP_COMPLETION_KEYWORDS[] */

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

void setUp(void)    {}
void tearDown(void) {}

/* Linear-scan the mirror array for a string match. Mirror is small (~44
 * entries today); O(n) per probe is fine. */
static bool mirror_contains(const char *kw) {
    for (size_t i = 0; i < ILSP_COMPLETION_KEYWORD_COUNT; i++) {
        if (strcmp(ILSP_COMPLETION_KEYWORDS[i], kw) == 0) return true;
    }
    return false;
}

static void test_v3_keywords_in_mirror(void) {
    /* The 6 v3 keywords introduced from Phase 79 onward. Order here is
     * the "alphabetical by first introduction phase" sort used in
     * CONTEXT.md's Keyword Roster section. */
    const char *v3[] = { "init", "mut", "patch", "pub", "pure", "readonly" };
    for (size_t i = 0; i < sizeof(v3) / sizeof(*v3); i++) {
        char msg[128];
        snprintf(msg, sizeof(msg),
            "v3 keyword '%s' missing from ILSP_COMPLETION_KEYWORDS[] "
            "— check src/lexer/lexer.c kw_table + reconfigure", v3[i]);
        TEST_ASSERT_TRUE_MESSAGE(mirror_contains(v3[i]), msg);
    }
}

/* Sanity-check the mirror is non-empty and has at least the 44 entries
 * that v3 should ship — guards against a miscompile of the generator. */
static void test_mirror_has_expected_minimum_count(void) {
    /* 38 pre-v3 keywords + 6 v3 keywords = 44. Hard-asserting >= so
     * a future Phase that adds keywords does not break this test. */
    TEST_ASSERT_TRUE_MESSAGE(ILSP_COMPLETION_KEYWORD_COUNT >= 44,
        "ILSP_COMPLETION_KEYWORDS expected to have at least 44 entries "
        "(38 pre-v3 + 6 v3); generator may have miscompiled");
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_v3_keywords_in_mirror);
    RUN_TEST(test_mirror_has_expected_minimum_count);
    return UNITY_END();
}
