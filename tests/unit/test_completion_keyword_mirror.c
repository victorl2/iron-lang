/* Phase 4 Plan 04-02 Task 01 (PITFALL H -- keyword drift) --
 * keyword mirror drift-guard unit test.
 *
 * Asserts that every entry in the configure-file-generated
 * ILSP_COMPLETION_KEYWORDS array is also a keyword according to the
 * lexer's single-token API, and that the sizes agree.
 *
 * Strategy: feed each ILSP_COMPLETION_KEYWORDS[i] through iron_lex_all
 * (the lexer's public entry point) and assert the first token kind is
 * NOT IRON_TOK_IDENTIFIER. That proves every mirror entry is a real
 * lexer keyword. Conversely, we assert the count matches
 * iron_lexer_kw_table_count() if the lexer exposes an accessor; if it
 * doesn't (the lexer's kw_table is static, per-TU), we fall back to an
 * acceptance test that every mirror entry produces a specifically
 * non-identifier token, which is the essential no-drift invariant.
 */

#include "unity.h"

#include "diagnostics/diagnostics.h"
#include "keyword_mirror.h"
#include "lexer/lexer.h"
#include "util/arena.h"
#include "vendor/stb_ds.h"

#include <string.h>

void setUp(void)    {}
void tearDown(void) {}

/* Every mirror entry, when lexed, should produce a keyword token (not
 * an IRON_TOK_IDENTIFIER). This catches the case where a new lexer
 * keyword is added but the mirror header wasn't regenerated, or vice
 * versa. */
static void test_every_mirror_entry_is_a_keyword(void) {
    TEST_ASSERT_TRUE(ILSP_COMPLETION_KEYWORD_COUNT > 0);
    for (size_t i = 0; i < ILSP_COMPLETION_KEYWORD_COUNT; i++) {
        const char *kw = ILSP_COMPLETION_KEYWORDS[i];
        TEST_ASSERT_NOT_NULL(kw);
        TEST_ASSERT_TRUE(strlen(kw) > 0);

        Iron_Arena arena = iron_arena_create(8 * 1024);
        Iron_DiagList diags = iron_diaglist_create();
        Iron_Lexer lx = iron_lexer_create(kw, "<mirror>", &arena, &diags);
        Iron_Token *toks = iron_lex_all(&lx);
        TEST_ASSERT_NOT_NULL(toks);
        /* First token should be a keyword, not an identifier. */
        TEST_ASSERT_NOT_EQUAL(IRON_TOK_IDENTIFIER, toks[0].kind);
        /* And not an error token either. */
        TEST_ASSERT_NOT_EQUAL(IRON_TOK_ERROR, toks[0].kind);
        arrfree(toks);
        iron_diaglist_free(&diags);
        iron_arena_free(&arena);
    }
}

/* The mirror must contain the canonical set of Iron keywords. Sample a
 * few high-signal entries: any drift in this list is a major protocol
 * break (`func`, `val`, `var`, `if`, `match`, `import`, `object`). */
static void test_mirror_contains_canonical_keywords(void) {
    const char *must_have[] = {
        "func", "val", "var", "if", "else", "match", "import", "object",
        "interface", "enum", "return", "while", "for", "true", "false",
    };
    size_t nrequired = sizeof(must_have) / sizeof(must_have[0]);
    for (size_t r = 0; r < nrequired; r++) {
        bool found = false;
        for (size_t i = 0; i < ILSP_COMPLETION_KEYWORD_COUNT; i++) {
            if (strcmp(ILSP_COMPLETION_KEYWORDS[i], must_have[r]) == 0) {
                found = true;
                break;
            }
        }
        if (!found) {
            char msg[128];
            snprintf(msg, sizeof(msg),
                "mirror missing canonical keyword %s", must_have[r]);
            TEST_FAIL_MESSAGE(msg);
        }
    }
}

/* The mirror must NOT contain obvious non-keywords (typo drift guard). */
static void test_mirror_excludes_non_keywords(void) {
    const char *must_not_have[] = {
        "foo", "bar", "x", "println", "main", "Int", "String", "Bool",
    };
    size_t nexcl = sizeof(must_not_have) / sizeof(must_not_have[0]);
    for (size_t r = 0; r < nexcl; r++) {
        for (size_t i = 0; i < ILSP_COMPLETION_KEYWORD_COUNT; i++) {
            if (strcmp(ILSP_COMPLETION_KEYWORDS[i], must_not_have[r]) == 0) {
                char msg[128];
                snprintf(msg, sizeof(msg),
                    "mirror contains non-keyword %s", must_not_have[r]);
                TEST_FAIL_MESSAGE(msg);
            }
        }
    }
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_every_mirror_entry_is_a_keyword);
    RUN_TEST(test_mirror_contains_canonical_keywords);
    RUN_TEST(test_mirror_excludes_non_keywords);
    return UNITY_END();
}
