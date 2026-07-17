/* Phase 16 Wave 0: TDD scaffold for v4 keyword binding-position rejection.
 * RED state until Plan 16-02 adds the keyword reservation + parser
 * disambiguation helper extension (iron_check_name_or_block_kw). */
#include "unity.h"
#include "lexer/lexer.h"
#include "parser/parser.h"
#include "parser/ast.h"
#include "diagnostics/diagnostics.h"
#include "util/arena.h"
#include "stb_ds.h"

#include <string.h>

static Iron_Arena    arena;
static Iron_DiagList diags;

void setUp(void) {
    arena = iron_arena_create(16 * 1024);
    diags = iron_diaglist_create();
}

void tearDown(void) {
    iron_arena_free(&arena);
    iron_diaglist_free(&diags);
}

/* Parse `src`, return diags.error_count. */
static int parse_and_count_errors(const char *src) {
    Iron_Lexer l = iron_lexer_create(src, "test.iron", &arena, &diags);
    Iron_Token *toks = iron_lex_all(&l);
    int tok_count = (int)arrlen(toks);
    Iron_Parser p = iron_parser_create(toks, tok_count, src, "test.iron",
                                       &arena, &diags);
    (void)iron_parse(&p);
    int n = diags.error_count;
    arrfree(toks);
    return n;
}

/* Return true iff at least one error in diags has code in [101, 199]. */
static bool has_parser_range_error(void) {
    for (int i = 0; i < arrlen(diags.items); i++) {
        int c = diags.items[i].code;
        if (c >= 101 && c <= 199) return true;
    }
    return false;
}

/* -- Binding-position rejection (RED until Plan 16-02 keyword reservation) -- */

void test_val_copy_rejected(void) {
    TEST_ASSERT_GREATER_THAN_INT(0, parse_and_count_errors("val copy = 1\n"));
    TEST_ASSERT_TRUE(has_parser_range_error());
}

void test_var_drop_rejected(void) {
    TEST_ASSERT_GREATER_THAN_INT(0, parse_and_count_errors("var drop = 1\n"));
    TEST_ASSERT_TRUE(has_parser_range_error());
}

void test_val_nocopy_rejected(void) {
    TEST_ASSERT_GREATER_THAN_INT(0, parse_and_count_errors("val nocopy = 1\n"));
    TEST_ASSERT_TRUE(has_parser_range_error());
}

void test_val_unchecked_rejected(void) {
    TEST_ASSERT_GREATER_THAN_INT(0, parse_and_count_errors("val unchecked = 1\n"));
    TEST_ASSERT_TRUE(has_parser_range_error());
}

void test_val_weak_rejected(void) {
    TEST_ASSERT_GREATER_THAN_INT(0, parse_and_count_errors("val weak = 1\n"));
    TEST_ASSERT_TRUE(has_parser_range_error());
}

/* -- Method-position acceptance (proves parser helper preserves stdlib idioms) -- */

void test_image_copy_call_accepted(void) {
    /* Image.copy(img) is the existing raylib.iron pattern.
     * After Plan 16-02 generalizes iron_check_name_or_init to also accept
     * IRON_TOK_COPY, this MUST still parse with zero errors. */
    TEST_ASSERT_EQUAL_INT(0, parse_and_count_errors(
        "func main() {\n"
        "    Image.copy(img)\n"
        "}\n"));
}

void test_func_image_copy_decl_accepted(void) {
    /* `copy { ... }` inside a patch object body is the current raylib.iron
     * form after Phase 98 (standalone func Type.method() is removed in v3.2).
     * After Plan 16-02 the IRON_TOK_COPY branch in the patch-body parser
     * accepts `copy { ... }` as a block construct. */
    TEST_ASSERT_EQUAL_INT(0, parse_and_count_errors(
        "object Image {}\n"
        "patch object Image {\n"
        "    copy { }\n"
        "}\n"));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_val_copy_rejected);
    RUN_TEST(test_var_drop_rejected);
    RUN_TEST(test_val_nocopy_rejected);
    RUN_TEST(test_val_unchecked_rejected);
    RUN_TEST(test_val_weak_rejected);
    RUN_TEST(test_image_copy_call_accepted);
    RUN_TEST(test_func_image_copy_decl_accepted);
    return UNITY_END();
}
