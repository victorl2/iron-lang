#include "unity.h"
#include "lexer/lexer.h"
#include "diagnostics/diagnostics.h"
#include "util/arena.h"
#include "stb_ds.h"

#include <string.h>

/* ── Module-level fixtures ───────────────────────────────────────────────── */

static Iron_Arena   arena;
static Iron_DiagList diags;

void setUp(void) {
    arena = iron_arena_create(8192);
    diags = iron_diaglist_create();
}

void tearDown(void) {
    iron_arena_free(&arena);
    iron_diaglist_free(&diags);
}

/* ── Helper ──────────────────────────────────────────────────────────────── */

static Iron_Token *lex(const char *src) {
    Iron_Lexer l = iron_lexer_create(src, "test.iron", &arena, &diags);
    return iron_lex_all(&l);
}

/* ── Keyword tests ───────────────────────────────────────────────────────── */

void test_keyword_val(void) {
    Iron_Token *toks = lex("val");
    TEST_ASSERT_EQUAL(IRON_TOK_VAL, toks[0].kind);
    arrfree(toks);
}

void test_keyword_func(void) {
    Iron_Token *toks = lex("func");
    TEST_ASSERT_EQUAL(IRON_TOK_FUNC, toks[0].kind);
    arrfree(toks);
}

void test_all_37_keywords(void) {
    typedef struct { const char *word; Iron_TokenKind kind; } KW;
    KW cases[] = {
        { "and",        IRON_TOK_AND        },
        { "await",      IRON_TOK_AWAIT      },
        { "comptime",   IRON_TOK_COMPTIME   },
        { "defer",      IRON_TOK_DEFER      },
        { "elif",       IRON_TOK_ELIF       },
        { "else",       IRON_TOK_ELSE       },
        { "enum",       IRON_TOK_ENUM       },
        { "extends",    IRON_TOK_EXTENDS    },
        { "false",      IRON_TOK_FALSE      },
        { "for",        IRON_TOK_FOR        },
        { "free",       IRON_TOK_FREE       },
        { "func",       IRON_TOK_FUNC       },
        { "heap",       IRON_TOK_HEAP       },
        { "if",         IRON_TOK_IF         },
        { "implements", IRON_TOK_IMPLEMENTS },
        { "import",     IRON_TOK_IMPORT     },
        { "in",         IRON_TOK_IN         },
        { "interface",  IRON_TOK_INTERFACE  },
        { "is",         IRON_TOK_IS         },
        { "leak",       IRON_TOK_LEAK       },
        { "match",      IRON_TOK_MATCH      },
        { "not",        IRON_TOK_NOT        },
        { "null",       IRON_TOK_NULL_KW    },
        { "object",     IRON_TOK_OBJECT     },
        { "or",         IRON_TOK_OR         },
        { "parallel",   IRON_TOK_PARALLEL   },
        { "pool",       IRON_TOK_POOL       },
        { "private",    IRON_TOK_PRIVATE    },
        { "rc",         IRON_TOK_RC         },
        { "return",     IRON_TOK_RETURN     },
        { "self",       IRON_TOK_SELF       },
        { "spawn",      IRON_TOK_SPAWN      },
        { "super",      IRON_TOK_SUPER      },
        { "true",       IRON_TOK_TRUE       },
        { "val",        IRON_TOK_VAL        },
        { "var",        IRON_TOK_VAR        },
        { "while",      IRON_TOK_WHILE      },
    };
    int count = (int)(sizeof(cases) / sizeof(cases[0]));
    TEST_ASSERT_EQUAL_INT(37, count);

    for (int i = 0; i < count; i++) {
        Iron_Arena   a = iron_arena_create(4096);
        Iron_DiagList d = iron_diaglist_create();
        Iron_Lexer l = iron_lexer_create(cases[i].word, "test.iron", &a, &d);
        Iron_Token *toks = iron_lex_all(&l);
        TEST_ASSERT_EQUAL_MESSAGE(cases[i].kind, toks[0].kind, cases[i].word);
        arrfree(toks);
        iron_arena_free(&a);
        iron_diaglist_free(&d);
    }
}

void test_identifier_not_keyword(void) {
    Iron_Token *toks = lex("value");
    TEST_ASSERT_EQUAL(IRON_TOK_IDENTIFIER, toks[0].kind);
    TEST_ASSERT_EQUAL_STRING("value", toks[0].value);
    arrfree(toks);
}

/* ── Literal tests ───────────────────────────────────────────────────────── */

void test_integer_literal(void) {
    Iron_Token *toks = lex("123");
    TEST_ASSERT_EQUAL(IRON_TOK_INTEGER, toks[0].kind);
    TEST_ASSERT_EQUAL_STRING("123", toks[0].value);
    arrfree(toks);
}

void test_float_literal(void) {
    Iron_Token *toks = lex("3.14");
    TEST_ASSERT_EQUAL(IRON_TOK_FLOAT, toks[0].kind);
    TEST_ASSERT_EQUAL_STRING("3.14", toks[0].value);
    arrfree(toks);
}

void test_string_literal(void) {
    Iron_Token *toks = lex("\"hello\"");
    TEST_ASSERT_EQUAL(IRON_TOK_STRING, toks[0].kind);
    TEST_ASSERT_EQUAL_STRING("hello", toks[0].value);
    arrfree(toks);
}

void test_interpolated_string(void) {
    Iron_Token *toks = lex("\"Hello {name}\"");
    TEST_ASSERT_EQUAL(IRON_TOK_INTERP_STRING, toks[0].kind);
    arrfree(toks);
}

void test_bool_true(void) {
    Iron_Token *toks = lex("true");
    TEST_ASSERT_EQUAL(IRON_TOK_TRUE, toks[0].kind);
    arrfree(toks);
}

void test_bool_false(void) {
    Iron_Token *toks = lex("false");
    TEST_ASSERT_EQUAL(IRON_TOK_FALSE, toks[0].kind);
    arrfree(toks);
}

void test_null_literal(void) {
    Iron_Token *toks = lex("null");
    TEST_ASSERT_EQUAL(IRON_TOK_NULL_KW, toks[0].kind);
    arrfree(toks);
}

/* ── Operator tests ──────────────────────────────────────────────────────── */

void test_all_operators(void) {
    typedef struct { const char *op; Iron_TokenKind kind; } OP;
    OP cases[] = {
        { "+",  IRON_TOK_PLUS        },
        { "-",  IRON_TOK_MINUS       },
        { "*",  IRON_TOK_STAR        },
        { "/",  IRON_TOK_SLASH       },
        { "%",  IRON_TOK_PERCENT     },
        { "=",  IRON_TOK_ASSIGN      },
        { "==", IRON_TOK_EQUALS      },
        { "!=", IRON_TOK_NOT_EQUALS  },
        { "<",  IRON_TOK_LESS        },
        { ">",  IRON_TOK_GREATER     },
        { "<=", IRON_TOK_LESS_EQ     },
        { ">=", IRON_TOK_GREATER_EQ  },
        { ".",  IRON_TOK_DOT         },
        { "..", IRON_TOK_DOTDOT      },
        { ",",  IRON_TOK_COMMA       },
        { ":",  IRON_TOK_COLON       },
        { "->", IRON_TOK_ARROW       },
        { "?",  IRON_TOK_QUESTION    },
        { "+=", IRON_TOK_PLUS_ASSIGN  },
        { "-=", IRON_TOK_MINUS_ASSIGN },
        { "*=", IRON_TOK_STAR_ASSIGN  },
        { "/=", IRON_TOK_SLASH_ASSIGN },
    };
    int count = (int)(sizeof(cases) / sizeof(cases[0]));

    for (int i = 0; i < count; i++) {
        Iron_Arena   a = iron_arena_create(4096);
        Iron_DiagList d = iron_diaglist_create();
        Iron_Lexer l = iron_lexer_create(cases[i].op, "test.iron", &a, &d);
        Iron_Token *toks = iron_lex_all(&l);
        TEST_ASSERT_EQUAL_MESSAGE(cases[i].kind, toks[0].kind, cases[i].op);
        arrfree(toks);
        iron_arena_free(&a);
        iron_diaglist_free(&d);
    }
}

void test_arrow_operator(void) {
    Iron_Token *toks = lex("->");
    TEST_ASSERT_EQUAL(IRON_TOK_ARROW, toks[0].kind);
    arrfree(toks);
}

void test_dotdot_operator(void) {
    Iron_Token *toks = lex("..");
    TEST_ASSERT_EQUAL(IRON_TOK_DOTDOT, toks[0].kind);
    arrfree(toks);
}

void test_compound_assign(void) {
    Iron_Token *toks = lex("+=");
    TEST_ASSERT_EQUAL(IRON_TOK_PLUS_ASSIGN, toks[0].kind);
    arrfree(toks);

    toks = lex("-=");
    TEST_ASSERT_EQUAL(IRON_TOK_MINUS_ASSIGN, toks[0].kind);
    arrfree(toks);

    toks = lex("*=");
    TEST_ASSERT_EQUAL(IRON_TOK_STAR_ASSIGN, toks[0].kind);
    arrfree(toks);

    toks = lex("/=");
    TEST_ASSERT_EQUAL(IRON_TOK_SLASH_ASSIGN, toks[0].kind);
    arrfree(toks);
}

/* ── Delimiter tests ─────────────────────────────────────────────────────── */

void test_all_delimiters(void) {
    typedef struct { const char *ch; Iron_TokenKind kind; } DL;
    DL cases[] = {
        { "(",  IRON_TOK_LPAREN    },
        { ")",  IRON_TOK_RPAREN    },
        { "[",  IRON_TOK_LBRACKET  },
        { "]",  IRON_TOK_RBRACKET  },
        { "{",  IRON_TOK_LBRACE    },
        { "}",  IRON_TOK_RBRACE    },
        { ";",  IRON_TOK_SEMICOLON },
    };
    int count = (int)(sizeof(cases) / sizeof(cases[0]));

    for (int i = 0; i < count; i++) {
        Iron_Arena   a = iron_arena_create(4096);
        Iron_DiagList d = iron_diaglist_create();
        Iron_Lexer l = iron_lexer_create(cases[i].ch, "test.iron", &a, &d);
        Iron_Token *toks = iron_lex_all(&l);
        TEST_ASSERT_EQUAL_MESSAGE(cases[i].kind, toks[0].kind, cases[i].ch);
        arrfree(toks);
        iron_arena_free(&a);
        iron_diaglist_free(&d);
    }
}

/* ── Comment tests ───────────────────────────────────────────────────────── */

void test_comment_skipped(void) {
    /* "val x = 10 -- comment" -> val, x, =, 10, then newline (from comment), then EOF */
    Iron_Token *toks = lex("val x = 10 -- comment");
    TEST_ASSERT_EQUAL(IRON_TOK_VAL,        toks[0].kind);
    TEST_ASSERT_EQUAL(IRON_TOK_IDENTIFIER, toks[1].kind);
    TEST_ASSERT_EQUAL(IRON_TOK_ASSIGN,     toks[2].kind);
    TEST_ASSERT_EQUAL(IRON_TOK_INTEGER,    toks[3].kind);
    /* Comment + its trailing newline become NEWLINE token(s).
     * No comment text in token stream. Last non-EOF token before EOF is NEWLINE. */
    /* Last token must be EOF. */
    int n = (int)arrlen(toks);
    TEST_ASSERT_EQUAL(IRON_TOK_EOF, toks[n - 1].kind);
    /* Ensure no token has a string value containing "comment". */
    for (int i = 0; i < n; i++) {
        if (toks[i].value != NULL) {
            TEST_ASSERT_NOT_EQUAL(0, strcmp(toks[i].value, "comment"));
        }
    }
    arrfree(toks);
}

void test_comment_only_line(void) {
    /* "-- this is a comment" on its own line: no identifier tokens. */
    Iron_Token *toks = lex("-- this is a comment");
    int n = (int)arrlen(toks);
    /* Should produce at most a NEWLINE then EOF (no identifier tokens). */
    TEST_ASSERT_EQUAL(IRON_TOK_EOF, toks[n - 1].kind);
    for (int i = 0; i < n - 1; i++) {
        TEST_ASSERT_EQUAL(IRON_TOK_NEWLINE, toks[i].kind);
    }
    arrfree(toks);
}

/* ── Span tests ──────────────────────────────────────────────────────────── */

void test_span_line_col(void) {
    /* "val x = 10" -> val@1:1, x@1:5, =@1:7, 10@1:9 */
    Iron_Token *toks = lex("val x = 10");
    /* val */
    TEST_ASSERT_EQUAL_UINT32(1, toks[0].line);
    TEST_ASSERT_EQUAL_UINT32(1, toks[0].col);
    /* x */
    TEST_ASSERT_EQUAL_UINT32(1, toks[1].line);
    TEST_ASSERT_EQUAL_UINT32(5, toks[1].col);
    /* = */
    TEST_ASSERT_EQUAL_UINT32(1, toks[2].line);
    TEST_ASSERT_EQUAL_UINT32(7, toks[2].col);
    /* 10 */
    TEST_ASSERT_EQUAL_UINT32(1, toks[3].line);
    TEST_ASSERT_EQUAL_UINT32(9, toks[3].col);
    arrfree(toks);
}

void test_span_multiline(void) {
    /* Line 1: "val x = 1" -> line 1 tokens
     * Line 2: "var y = 2" -> line 2 tokens */
    Iron_Token *toks = lex("val x = 1\nvar y = 2");
    /* val on line 1 */
    TEST_ASSERT_EQUAL_UINT32(1, toks[0].line);
    /* After newline, var on line 2 */
    /* Find the VAR token. */
    int found = 0;
    int n = (int)arrlen(toks);
    for (int i = 0; i < n; i++) {
        if (toks[i].kind == IRON_TOK_VAR) {
            TEST_ASSERT_EQUAL_UINT32(2, toks[i].line);
            TEST_ASSERT_EQUAL_UINT32(1, toks[i].col);
            found = 1;
            break;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(found, "VAR token not found on line 2");
    arrfree(toks);
}

/* ── Error tests ─────────────────────────────────────────────────────────── */

void test_unterminated_string_error(void) {
    Iron_Token *toks = lex("\"hello");
    TEST_ASSERT_EQUAL(1, diags.error_count);
    TEST_ASSERT_EQUAL(IRON_ERR_UNTERMINATED_STRING, diags.items[0].code);
    /* Span should point to start of string: line 1, col 1. */
    TEST_ASSERT_EQUAL_UINT32(1, diags.items[0].span.line);
    TEST_ASSERT_EQUAL_UINT32(1, diags.items[0].span.col);
    arrfree(toks);
}

void test_invalid_char_error(void) {
    Iron_Token *toks = lex("@");
    TEST_ASSERT_EQUAL(1, diags.error_count);
    TEST_ASSERT_EQUAL(IRON_ERR_INVALID_CHAR, diags.items[0].code);
    arrfree(toks);
}

void test_three_independent_errors(void) {
    /* Three independent invalid characters, each on its own — lexer must continue
     * and produce exactly 3 diagnostics. */
    Iron_Token *toks = lex("@ # $");
    TEST_ASSERT_EQUAL_INT(3, diags.error_count);
    arrfree(toks);
}

/* ── Sequence tests ──────────────────────────────────────────────────────── */

void test_val_x_assign_10(void) {
    /* "val x = 10" -> [VAL, IDENTIFIER("x"), ASSIGN, INTEGER("10"), EOF] */
    Iron_Token *toks = lex("val x = 10");
    TEST_ASSERT_EQUAL(IRON_TOK_VAL,        toks[0].kind);
    TEST_ASSERT_EQUAL(IRON_TOK_IDENTIFIER, toks[1].kind);
    TEST_ASSERT_EQUAL_STRING("x",          toks[1].value);
    TEST_ASSERT_EQUAL(IRON_TOK_ASSIGN,     toks[2].kind);
    TEST_ASSERT_EQUAL(IRON_TOK_INTEGER,    toks[3].kind);
    TEST_ASSERT_EQUAL_STRING("10",         toks[3].value);
    TEST_ASSERT_EQUAL(IRON_TOK_EOF,        toks[4].kind);
    arrfree(toks);
}

void test_multiline_string(void) {
    /* Triple-quoted string. */
    Iron_Token *toks = lex("\"\"\"multi\nline\"\"\"");
    TEST_ASSERT_EQUAL(IRON_TOK_STRING, toks[0].kind);
    TEST_ASSERT_NOT_NULL(toks[0].value);
    arrfree(toks);
}

void test_minus_vs_comment(void) {
    /* "x - --comment" -> [IDENTIFIER, MINUS, NEWLINE, EOF] */
    Iron_Token *toks = lex("x - --comment");
    TEST_ASSERT_EQUAL(IRON_TOK_IDENTIFIER, toks[0].kind);
    TEST_ASSERT_EQUAL(IRON_TOK_MINUS,      toks[1].kind);
    TEST_ASSERT_EQUAL(IRON_TOK_NEWLINE,    toks[2].kind);
    TEST_ASSERT_EQUAL(IRON_TOK_EOF,        toks[3].kind);
    arrfree(toks);
}

/* ── Main ────────────────────────────────────────────────────────────────── */

int main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_keyword_val);
    RUN_TEST(test_keyword_func);
    RUN_TEST(test_all_37_keywords);
    RUN_TEST(test_identifier_not_keyword);

    RUN_TEST(test_integer_literal);
    RUN_TEST(test_float_literal);
    RUN_TEST(test_string_literal);
    RUN_TEST(test_interpolated_string);
    RUN_TEST(test_bool_true);
    RUN_TEST(test_bool_false);
    RUN_TEST(test_null_literal);

    RUN_TEST(test_all_operators);
    RUN_TEST(test_arrow_operator);
    RUN_TEST(test_dotdot_operator);
    RUN_TEST(test_compound_assign);

    RUN_TEST(test_all_delimiters);

    RUN_TEST(test_comment_skipped);
    RUN_TEST(test_comment_only_line);

    RUN_TEST(test_span_line_col);
    RUN_TEST(test_span_multiline);

    RUN_TEST(test_unterminated_string_error);
    RUN_TEST(test_invalid_char_error);
    RUN_TEST(test_three_independent_errors);

    RUN_TEST(test_val_x_assign_10);
    RUN_TEST(test_multiline_string);
    RUN_TEST(test_minus_vs_comment);

    return UNITY_END();
}
