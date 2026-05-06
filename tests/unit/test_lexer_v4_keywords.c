/* Phase 16 Wave 0: TDD scaffold for v4 keyword tokenization.
 * RED state until Plan 16-02 adds IRON_TOK_COPY/DROP/NOCOPY/UNCHECKED/WEAK
 * to src/lexer/lexer.h and the kw_table entries to src/lexer/lexer.c. */
#include "unity.h"
#include "lexer/lexer.h"
#include "diagnostics/diagnostics.h"
#include "util/arena.h"
#include "stb_ds.h"

#include <string.h>

static Iron_Arena    arena;
static Iron_DiagList diags;

void setUp(void) {
    arena = iron_arena_create(8192);
    diags = iron_diaglist_create();
}

void tearDown(void) {
    iron_arena_free(&arena);
    iron_diaglist_free(&diags);
}

static Iron_Token *lex(const char *src) {
    Iron_Lexer l = iron_lexer_create(src, "test.iron", &arena, &diags);
    return iron_lex_all(&l);
}

void test_keyword_copy_tokenizes_as_copy(void) {
    Iron_Token *toks = lex("copy");
    TEST_ASSERT_EQUAL(IRON_TOK_COPY, toks[0].kind);
    arrfree(toks);
}

void test_keyword_drop_tokenizes_as_drop(void) {
    Iron_Token *toks = lex("drop");
    TEST_ASSERT_EQUAL(IRON_TOK_DROP, toks[0].kind);
    arrfree(toks);
}

void test_keyword_nocopy_tokenizes_as_nocopy(void) {
    Iron_Token *toks = lex("nocopy");
    TEST_ASSERT_EQUAL(IRON_TOK_NOCOPY, toks[0].kind);
    arrfree(toks);
}

void test_keyword_unchecked_tokenizes_as_unchecked(void) {
    Iron_Token *toks = lex("unchecked");
    TEST_ASSERT_EQUAL(IRON_TOK_UNCHECKED, toks[0].kind);
    arrfree(toks);
}

void test_keyword_weak_tokenizes_as_weak(void) {
    Iron_Token *toks = lex("weak");
    TEST_ASSERT_EQUAL(IRON_TOK_WEAK, toks[0].kind);
    arrfree(toks);
}

void test_v4_keywords_not_lexed_as_identifier(void) {
    /* Catches Pitfall 1: out-of-order kw_table entry where bsearch
     * misses the keyword and silently produces IRON_TOK_IDENTIFIER. */
    const char *cases[] = { "copy", "drop", "nocopy", "unchecked", "weak" };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        Iron_Token *toks = lex(cases[i]);
        TEST_ASSERT_NOT_EQUAL_MESSAGE(IRON_TOK_IDENTIFIER, toks[0].kind, cases[i]);
        arrfree(toks);
    }
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_keyword_copy_tokenizes_as_copy);
    RUN_TEST(test_keyword_drop_tokenizes_as_drop);
    RUN_TEST(test_keyword_nocopy_tokenizes_as_nocopy);
    RUN_TEST(test_keyword_unchecked_tokenizes_as_unchecked);
    RUN_TEST(test_keyword_weak_tokenizes_as_weak);
    RUN_TEST(test_v4_keywords_not_lexed_as_identifier);
    return UNITY_END();
}
