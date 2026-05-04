/* Phase 4 Plan 04-02 Task 01 (EDIT-06, D-01, D-16) -- context classifier
 * drift-guard unit test.
 *
 * Exercises all 7 context values from IronLsp_CompletionContext against
 * representative cursor positions. Uses the test seam
 * ilsp_completion_context_classify_buf so the test does not need to
 * materialize an IronLsp_Document.
 */

#include "unity.h"

#include "lsp/facade/edit/complete/context_classify.h"

#include <string.h>

void setUp(void)    {}
void tearDown(void) {}

/* Helper: classify at the position of the '|' marker in the fixture
 * (convention copied from signatureHelp smoke tests). The '|' itself
 * is not part of the input buffer handed to the classifier. */
static IronLsp_CompletionContext classify_at_marker(const char *fixture) {
    const char *bar = strchr(fixture, '|');
    if (!bar) return (IronLsp_CompletionContext)-1;
    size_t cursor = (size_t)(bar - fixture);
    /* Build a buffer without the '|'. */
    size_t total = strlen(fixture) - 1;
    static char buf[512];
    memcpy(buf, fixture, cursor);
    memcpy(buf + cursor, bar + 1, strlen(bar + 1) + 1);
    return ilsp_completion_context_classify_buf(buf, total, cursor);
}

/* Test 1: `val x = |` cursor at expression head. */
static void test_expr_head(void) {
    TEST_ASSERT_EQUAL_INT(ILSP_CCTX_EXPR_HEAD,
        classify_at_marker("val x = |"));
}

/* Test 2: `receiver.|` member access. */
static void test_member_after_dot(void) {
    TEST_ASSERT_EQUAL_INT(ILSP_CCTX_MEMBER_AFTER_DOT,
        classify_at_marker("receiver.|"));
    /* Also test with an identifier in progress after the dot. */
    TEST_ASSERT_EQUAL_INT(ILSP_CCTX_MEMBER_AFTER_DOT,
        classify_at_marker("receiver.foo|"));
}

/* Test 3: `Foo::|` qualified path. */
static void test_qualified_after_colon(void) {
    TEST_ASSERT_EQUAL_INT(ILSP_CCTX_QUALIFIED_AFTER_COLON,
        classify_at_marker("Foo::|"));
}

/* Test 4: `import |` import path continuation. */
static void test_import_path(void) {
    TEST_ASSERT_EQUAL_INT(ILSP_CCTX_IMPORT_PATH,
        classify_at_marker("import |"));
    /* Also: `import foo.|` stays in IMPORT_PATH (line keyword trumps
     * member dot because import path is the dominant context). */
    TEST_ASSERT_EQUAL_INT(ILSP_CCTX_IMPORT_PATH,
        classify_at_marker("import foo|"));
}

/* Test 5: `val x: |` type position. */
static void test_type_position(void) {
    TEST_ASSERT_EQUAL_INT(ILSP_CCTX_TYPE_POSITION,
        classify_at_marker("val x: |"));
    /* Function parameter form. */
    TEST_ASSERT_EQUAL_INT(ILSP_CCTX_TYPE_POSITION,
        classify_at_marker("func foo(a: |)"));
}

/* Test 6: `match v { |` pattern position. */
static void test_pattern_position(void) {
    TEST_ASSERT_EQUAL_INT(ILSP_CCTX_PATTERN_POSITION,
        classify_at_marker("match v { |"));
    /* With a newline between and some whitespace. */
    TEST_ASSERT_EQUAL_INT(ILSP_CCTX_PATTERN_POSITION,
        classify_at_marker("match v {\n    |"));
}

/* Test 7: line start (no prior token) -> statement head. */
static void test_statement_head(void) {
    TEST_ASSERT_EQUAL_INT(ILSP_CCTX_STATEMENT_HEAD,
        classify_at_marker("|"));
    /* After a semicolon. */
    TEST_ASSERT_EQUAL_INT(ILSP_CCTX_STATEMENT_HEAD,
        classify_at_marker("val x = 1; |"));
    /* After leading indentation at a new line -> also statement head. */
    TEST_ASSERT_EQUAL_INT(ILSP_CCTX_STATEMENT_HEAD,
        classify_at_marker("val x = 1\n    |"));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_expr_head);
    RUN_TEST(test_member_after_dot);
    RUN_TEST(test_qualified_after_colon);
    RUN_TEST(test_import_path);
    RUN_TEST(test_type_position);
    RUN_TEST(test_pattern_position);
    RUN_TEST(test_statement_head);
    return UNITY_END();
}
