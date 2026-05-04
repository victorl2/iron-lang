/* test_semver.c — Unity tests for the iron-compiler semver constraint parser
 * (Phase 95 PIN-01).
 *
 * Locks Plan 95-01 Task 2 acceptance criteria:
 *   - Every operator (>=, >, <=, <, =, no-op = exact, ^, ~) parses and
 *     compares correctly.
 *   - Comma-separated AND ranges parse as multiple clauses; all must hold.
 *   - Whitespace tolerance around operators and commas.
 *   - iron_semver_suggest_version returns the lower bound for >= / = /
 *     ^ / ~ / no-op forms; NULL for pure </<= constraints.
 *   - Malformed inputs ("", ">= ", ">= 3.x.0", ">= 3.2", "*", ">= 3.2.0,",
 *     ">>= 3.2.0") all return NULL.
 *   - Pre-release / build-metadata suffixes on the version-being-checked
 *     are stripped before comparison (v3.2 simplification).
 *   - iron_semver_free is NULL-safe and leak-clean on valid constraints.
 */

#include "unity.h"
#include "cli/semver.h"

#include <string.h>

void setUp(void)    {}
void tearDown(void) {}

/* ── Operators: >= ──────────────────────────────────────────────────────── */
void test_v95_parse_ge_satisfies_lower_equal_upper(void) {
    IronSemverConstraint *c = iron_semver_parse(">= 1.2.3");
    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_TRUE(iron_semver_satisfies(c, "1.2.3"));
    TEST_ASSERT_TRUE(iron_semver_satisfies(c, "1.2.4"));
    TEST_ASSERT_FALSE(iron_semver_satisfies(c, "1.2.2"));
    iron_semver_free(c);
}

/* ── Operators: > ───────────────────────────────────────────────────────── */
void test_v95_parse_gt_strict(void) {
    IronSemverConstraint *c = iron_semver_parse("> 1.2.3");
    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_FALSE(iron_semver_satisfies(c, "1.2.3"));
    TEST_ASSERT_TRUE(iron_semver_satisfies(c, "1.2.4"));
    iron_semver_free(c);
}

/* ── Operators: <= ──────────────────────────────────────────────────────── */
void test_v95_parse_le_satisfies_lower_equal_upper(void) {
    IronSemverConstraint *c = iron_semver_parse("<= 1.2.3");
    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_TRUE(iron_semver_satisfies(c, "1.2.3"));
    TEST_ASSERT_FALSE(iron_semver_satisfies(c, "1.2.4"));
    iron_semver_free(c);
}

/* ── Operators: < ───────────────────────────────────────────────────────── */
void test_v95_parse_lt_strict(void) {
    IronSemverConstraint *c = iron_semver_parse("< 1.2.3");
    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_FALSE(iron_semver_satisfies(c, "1.2.3"));
    TEST_ASSERT_TRUE(iron_semver_satisfies(c, "1.2.2"));
    iron_semver_free(c);
}

/* ── Operators: = ───────────────────────────────────────────────────────── */
void test_v95_parse_eq_exact(void) {
    IronSemverConstraint *c = iron_semver_parse("= 1.2.3");
    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_TRUE(iron_semver_satisfies(c, "1.2.3"));
    TEST_ASSERT_FALSE(iron_semver_satisfies(c, "1.2.4"));
    iron_semver_free(c);
}

/* ── Operators: no operator (default exact) ─────────────────────────────── */
void test_v95_parse_no_operator_is_exact(void) {
    IronSemverConstraint *c = iron_semver_parse("1.2.3");
    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_TRUE(iron_semver_satisfies(c, "1.2.3"));
    TEST_ASSERT_FALSE(iron_semver_satisfies(c, "1.2.4"));
    iron_semver_free(c);
}

/* ── Operators: ^ (compatible-with same major) ──────────────────────────── */
void test_v95_parse_caret_same_major(void) {
    IronSemverConstraint *c = iron_semver_parse("^1.2.3");
    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_TRUE(iron_semver_satisfies(c, "1.2.3"));
    TEST_ASSERT_TRUE(iron_semver_satisfies(c, "1.9.0"));
    TEST_ASSERT_FALSE(iron_semver_satisfies(c, "2.0.0"));
    /* below clause: should reject */
    TEST_ASSERT_FALSE(iron_semver_satisfies(c, "1.2.2"));
    iron_semver_free(c);
}

/* ── Operators: ^0.X.Y (Cargo's pre-1.0 special case) ───────────────────── */
void test_v95_parse_caret_zero_major_is_minor_locked(void) {
    IronSemverConstraint *c = iron_semver_parse("^0.1.2");
    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_TRUE(iron_semver_satisfies(c, "0.1.2"));
    TEST_ASSERT_TRUE(iron_semver_satisfies(c, "0.1.9"));
    TEST_ASSERT_FALSE(iron_semver_satisfies(c, "0.2.0"));
    iron_semver_free(c);
}

/* ── Operators: ~ (compatible-with same minor) ──────────────────────────── */
void test_v95_parse_tilde_same_minor(void) {
    IronSemverConstraint *c = iron_semver_parse("~1.2.3");
    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_TRUE(iron_semver_satisfies(c, "1.2.3"));
    TEST_ASSERT_TRUE(iron_semver_satisfies(c, "1.2.9"));
    TEST_ASSERT_FALSE(iron_semver_satisfies(c, "1.3.0"));
    iron_semver_free(c);
}

/* ── Comma-AND ranges ───────────────────────────────────────────────────── */
void test_v95_parse_comma_range_both_clauses_required(void) {
    IronSemverConstraint *c = iron_semver_parse(">= 3.0.0, < 4.0.0");
    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_TRUE(iron_semver_satisfies(c, "3.5.0"));
    TEST_ASSERT_FALSE(iron_semver_satisfies(c, "2.9.0"));
    TEST_ASSERT_FALSE(iron_semver_satisfies(c, "4.0.0"));
    iron_semver_free(c);
}

/* ── Whitespace tolerance ───────────────────────────────────────────────── */
void test_v95_parse_whitespace_tolerant(void) {
    IronSemverConstraint *c = iron_semver_parse(">=3.0.0,<4.0.0");
    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_TRUE(iron_semver_satisfies(c, "3.5.0"));
    TEST_ASSERT_FALSE(iron_semver_satisfies(c, "2.9.0"));
    TEST_ASSERT_FALSE(iron_semver_satisfies(c, "4.0.0"));
    iron_semver_free(c);
}

/* ── Suggest: positive cases for every form that has a lower bound ──────── */
void test_v95_suggest_version_for_each_op_form(void) {
    IronSemverConstraint *c;

    c = iron_semver_parse(">= 3.2.0");
    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_EQUAL_STRING("3.2.0", iron_semver_suggest_version(c));
    iron_semver_free(c);

    c = iron_semver_parse(">= 3.0.0, < 4.0.0");
    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_EQUAL_STRING("3.0.0", iron_semver_suggest_version(c));
    iron_semver_free(c);

    c = iron_semver_parse("^3.2.0");
    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_EQUAL_STRING("3.2.0", iron_semver_suggest_version(c));
    iron_semver_free(c);

    c = iron_semver_parse("~3.2.0");
    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_EQUAL_STRING("3.2.0", iron_semver_suggest_version(c));
    iron_semver_free(c);

    c = iron_semver_parse("= 3.2.0");
    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_EQUAL_STRING("3.2.0", iron_semver_suggest_version(c));
    iron_semver_free(c);

    c = iron_semver_parse("3.2.0");
    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_EQUAL_STRING("3.2.0", iron_semver_suggest_version(c));
    iron_semver_free(c);
}

/* ── Suggest: NULL for pure upper-bound constraints ─────────────────────── */
void test_v95_suggest_version_null_for_pure_upper_bound(void) {
    IronSemverConstraint *c = iron_semver_parse("< 4.0.0");
    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_NULL(iron_semver_suggest_version(c));
    iron_semver_free(c);

    c = iron_semver_parse("<= 4.0.0");
    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_NULL(iron_semver_suggest_version(c));
    iron_semver_free(c);
}

/* ── Malformed input rejection (parse returns NULL for every case) ──────── */
void test_v95_parse_returns_null_on_malformed(void) {
    TEST_ASSERT_NULL(iron_semver_parse(""));
    TEST_ASSERT_NULL(iron_semver_parse(">= "));
    TEST_ASSERT_NULL(iron_semver_parse(">= 3.x.0"));
    TEST_ASSERT_NULL(iron_semver_parse(">= 3.2"));
    TEST_ASSERT_NULL(iron_semver_parse("*"));
    TEST_ASSERT_NULL(iron_semver_parse(">= 3.2.0,"));
    TEST_ASSERT_NULL(iron_semver_parse(">>= 3.2.0"));
}

/* ── Pre-release suffix tolerance (suffix stripped before comparison) ───── */
void test_v95_satisfies_strips_pre_release_suffix(void) {
    IronSemverConstraint *c = iron_semver_parse(">= 3.2.0");
    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_TRUE(iron_semver_satisfies(c, "3.2.0-alpha"));
    TEST_ASSERT_TRUE(iron_semver_satisfies(c, "3.2.0-beta+build.123"));
    iron_semver_free(c);

    c = iron_semver_parse("= 3.2.0");
    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_TRUE(iron_semver_satisfies(c, "3.2.0-rc1"));
    iron_semver_free(c);
}

/* ── free is NULL-safe ──────────────────────────────────────────────────── */
void test_v95_free_is_null_safe(void) {
    iron_semver_free(NULL); /* must not crash */
    /* And free of a valid constraint completes cleanly. */
    IronSemverConstraint *c = iron_semver_parse(">= 1.0.0, < 2.0.0");
    TEST_ASSERT_NOT_NULL(c);
    iron_semver_free(c);
    TEST_ASSERT_TRUE(true);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_v95_parse_ge_satisfies_lower_equal_upper);
    RUN_TEST(test_v95_parse_gt_strict);
    RUN_TEST(test_v95_parse_le_satisfies_lower_equal_upper);
    RUN_TEST(test_v95_parse_lt_strict);
    RUN_TEST(test_v95_parse_eq_exact);
    RUN_TEST(test_v95_parse_no_operator_is_exact);
    RUN_TEST(test_v95_parse_caret_same_major);
    RUN_TEST(test_v95_parse_caret_zero_major_is_minor_locked);
    RUN_TEST(test_v95_parse_tilde_same_minor);
    RUN_TEST(test_v95_parse_comma_range_both_clauses_required);
    RUN_TEST(test_v95_parse_whitespace_tolerant);
    RUN_TEST(test_v95_suggest_version_for_each_op_form);
    RUN_TEST(test_v95_suggest_version_null_for_pure_upper_bound);
    RUN_TEST(test_v95_parse_returns_null_on_malformed);
    RUN_TEST(test_v95_satisfies_strips_pre_release_suffix);
    RUN_TEST(test_v95_free_is_null_safe);
    return UNITY_END();
}
