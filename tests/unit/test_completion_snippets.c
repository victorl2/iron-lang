/* Phase 4 Plan 04-03 Task 02 (EDIT-04, D-15) -- snippet renderer
 * golden + escape-semantics tests.
 *
 * Asserts the 5 template shapes (function / object / match / import /
 * enum-variant) produced by ilsp_snippet_render, plus the LSP Snippet
 * Syntax Appendix escape rules (`$` -> \$, `}` -> \}, `\` -> \\).
 * Includes the PITFALL D `${USER}` injection test: a parameter
 * default of `${USER}` must NOT appear un-escaped in the rendered
 * snippet body.
 */

#include "unity.h"

#include "lsp/facade/edit/complete/snippet.h"
#include "util/arena.h"

#include <string.h>

void setUp(void)    {}
void tearDown(void) {}

/* Test 1: function with two params -> template form. */
static void test_function_two_params(void) {
    Iron_Arena arena = iron_arena_create(8 * 1024);
    const char *params[] = {"a", "b"};
    IronLsp_SnippetMeta m = {
        .name = "foo",
        .param_names = params,
        .param_count = 2,
    };
    const char *out = ilsp_snippet_render(ILSP_SNIPPET_FUNCTION, &m, &arena);
    TEST_ASSERT_NOT_NULL(out);
    TEST_ASSERT_EQUAL_STRING("foo(${1:a}, ${2:b})$0", out);
    iron_arena_free(&arena);
}

/* Test 2: zero-arg function -> `name()$0`. */
static void test_function_zero_args(void) {
    Iron_Arena arena = iron_arena_create(8 * 1024);
    IronLsp_SnippetMeta m = {
        .name = "ping",
        .param_names = NULL,
        .param_count = 0,
    };
    const char *out = ilsp_snippet_render(ILSP_SNIPPET_FUNCTION, &m, &arena);
    TEST_ASSERT_NOT_NULL(out);
    TEST_ASSERT_EQUAL_STRING("ping()$0", out);
    iron_arena_free(&arena);
}

/* Test 3: object literal with 2 fields -> full form. */
static void test_object_literal_two_fields(void) {
    Iron_Arena arena = iron_arena_create(8 * 1024);
    const char *fields[] = {"x", "y"};
    IronLsp_SnippetMeta m = {
        .name = "Point",
        .field_names = fields,
        .field_count = 2,
    };
    const char *out = ilsp_snippet_render(ILSP_SNIPPET_OBJECT_LITERAL,
                                            &m, &arena);
    TEST_ASSERT_NOT_NULL(out);
    TEST_ASSERT_EQUAL_STRING(
        "Point { ${1:x}: ${2:value1}, ${3:y}: ${4:value2} }$0", out);
    iron_arena_free(&arena);
}

/* Test 3b: empty-field object -> `Name {}$0`. */
static void test_object_literal_empty(void) {
    Iron_Arena arena = iron_arena_create(8 * 1024);
    IronLsp_SnippetMeta m = {
        .name = "Empty",
        .field_names = NULL,
        .field_count = 0,
    };
    const char *out = ilsp_snippet_render(ILSP_SNIPPET_OBJECT_LITERAL,
                                            &m, &arena);
    TEST_ASSERT_NOT_NULL(out);
    TEST_ASSERT_EQUAL_STRING("Empty {}$0", out);
    iron_arena_free(&arena);
}

/* Test 4: match keyword -> literal template. */
static void test_match_template(void) {
    Iron_Arena arena = iron_arena_create(8 * 1024);
    const char *out = ilsp_snippet_render(ILSP_SNIPPET_MATCH, NULL, &arena);
    TEST_ASSERT_NOT_NULL(out);
    TEST_ASSERT_EQUAL_STRING(
        "match ${1:expr} {\n  ${2:Pattern} -> ${3:result},\n}$0", out);
    iron_arena_free(&arena);
}

/* Test 5: escape -- parameter name contains `$` -> rendered as `\$`. */
static void test_escape_dollar(void) {
    Iron_Arena arena = iron_arena_create(8 * 1024);
    const char *params[] = {"a$b"};
    IronLsp_SnippetMeta m = {
        .name = "fn",
        .param_names = params,
        .param_count = 1,
    };
    const char *out = ilsp_snippet_render(ILSP_SNIPPET_FUNCTION, &m, &arena);
    TEST_ASSERT_NOT_NULL(out);
    /* Expect `${1:a\$b}` -- the `$` inside the default is escaped. */
    TEST_ASSERT_NOT_NULL(strstr(out, "\\$"));
    /* And NOT the unescaped form (except the tab-stop markers). */
    TEST_ASSERT_EQUAL_STRING("fn(${1:a\\$b})$0", out);
    iron_arena_free(&arena);
}

/* Test 6: escape -- default contains literal `}` -> rendered as `\}`. */
static void test_escape_brace(void) {
    Iron_Arena arena = iron_arena_create(8 * 1024);
    const char *params[] = {"a}b"};
    IronLsp_SnippetMeta m = {
        .name = "fn",
        .param_names = params,
        .param_count = 1,
    };
    const char *out = ilsp_snippet_render(ILSP_SNIPPET_FUNCTION, &m, &arena);
    TEST_ASSERT_NOT_NULL(out);
    TEST_ASSERT_EQUAL_STRING("fn(${1:a\\}b})$0", out);
    iron_arena_free(&arena);
}

/* Test 7: escape -- default contains `\` -> rendered as `\\`. */
static void test_escape_backslash(void) {
    Iron_Arena arena = iron_arena_create(8 * 1024);
    const char *params[] = {"a\\b"};
    IronLsp_SnippetMeta m = {
        .name = "fn",
        .param_names = params,
        .param_count = 1,
    };
    const char *out = ilsp_snippet_render(ILSP_SNIPPET_FUNCTION, &m, &arena);
    TEST_ASSERT_NOT_NULL(out);
    TEST_ASSERT_EQUAL_STRING("fn(${1:a\\\\b})$0", out);
    iron_arena_free(&arena);
}

/* Test 8: enum variant, payload-less -> `EnumName.Variant$0`. */
static void test_enum_variant_payload_less(void) {
    Iron_Arena arena = iron_arena_create(8 * 1024);
    IronLsp_SnippetMeta m = {
        .name = "Color",
        .variant_name = "Red",
        .payload_count = 0,
    };
    const char *out = ilsp_snippet_render(ILSP_SNIPPET_ENUM_VARIANT,
                                            &m, &arena);
    TEST_ASSERT_NOT_NULL(out);
    TEST_ASSERT_EQUAL_STRING("Color.Red$0", out);
    iron_arena_free(&arena);
}

/* Test 8b: enum variant, single payload. */
static void test_enum_variant_single_payload(void) {
    Iron_Arena arena = iron_arena_create(8 * 1024);
    IronLsp_SnippetMeta m = {
        .name = "Option",
        .variant_name = "Some",
        .payload_count = 1,
    };
    const char *out = ilsp_snippet_render(ILSP_SNIPPET_ENUM_VARIANT,
                                            &m, &arena);
    TEST_ASSERT_NOT_NULL(out);
    TEST_ASSERT_EQUAL_STRING("Option.Some(${1:payload})$0", out);
    iron_arena_free(&arena);
}

/* Test 9 (PITFALL D): a parameter name of `${USER}` MUST NOT be
 * emitted verbatim -- the renderer must escape `$` and `}` so no
 * client-side variable substitution occurs. */
static void test_pitfall_d_user_injection(void) {
    Iron_Arena arena = iron_arena_create(8 * 1024);
    const char *params[] = {"${USER}"};
    IronLsp_SnippetMeta m = {
        .name = "leaky",
        .param_names = params,
        .param_count = 1,
    };
    const char *out = ilsp_snippet_render(ILSP_SNIPPET_FUNCTION, &m, &arena);
    TEST_ASSERT_NOT_NULL(out);
    /* The raw `${USER}` should NOT appear as a substring -- every `$`
     * and `}` inside the default must be backslash-escaped so the
     * snippet engine sees a literal string, not a variable token. */
    TEST_ASSERT_NULL(strstr(out, "${USER}"));
    TEST_ASSERT_EQUAL_STRING("leaky(${1:\\${USER\\}})$0", out);
    iron_arena_free(&arena);
}

/* Test 10: import template. */
static void test_import_template(void) {
    Iron_Arena arena = iron_arena_create(8 * 1024);
    const char *out = ilsp_snippet_render(ILSP_SNIPPET_IMPORT, NULL, &arena);
    TEST_ASSERT_NOT_NULL(out);
    TEST_ASSERT_EQUAL_STRING("import ${1:module}$0", out);
    iron_arena_free(&arena);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_function_two_params);
    RUN_TEST(test_function_zero_args);
    RUN_TEST(test_object_literal_two_fields);
    RUN_TEST(test_object_literal_empty);
    RUN_TEST(test_match_template);
    RUN_TEST(test_escape_dollar);
    RUN_TEST(test_escape_brace);
    RUN_TEST(test_escape_backslash);
    RUN_TEST(test_enum_variant_payload_less);
    RUN_TEST(test_enum_variant_single_payload);
    RUN_TEST(test_pitfall_d_user_injection);
    RUN_TEST(test_import_template);
    return UNITY_END();
}
