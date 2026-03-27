#ifndef IRON_LEXER_H
#define IRON_LEXER_H

#include "util/arena.h"
#include "diagnostics/diagnostics.h"

#include <stdint.h>
#include <stddef.h>

/* ── Token kinds ─────────────────────────────────────────────────────────── */

typedef enum {
    /* Literals */
    IRON_TOK_INTEGER,
    IRON_TOK_FLOAT,
    IRON_TOK_STRING,
    IRON_TOK_INTERP_STRING,

    /* Keywords (38 total, alphabetical) */
    IRON_TOK_AND,
    IRON_TOK_AWAIT,
    IRON_TOK_COMPTIME,
    IRON_TOK_DEFER,
    IRON_TOK_ELIF,
    IRON_TOK_ELSE,
    IRON_TOK_ENUM,
    IRON_TOK_EXTENDS,
    IRON_TOK_EXTERN,
    IRON_TOK_FALSE,
    IRON_TOK_FOR,
    IRON_TOK_FREE,
    IRON_TOK_FUNC,
    IRON_TOK_HEAP,
    IRON_TOK_IF,
    IRON_TOK_IMPLEMENTS,
    IRON_TOK_IMPORT,
    IRON_TOK_IN,
    IRON_TOK_INTERFACE,
    IRON_TOK_IS,
    IRON_TOK_LEAK,
    IRON_TOK_MATCH,
    IRON_TOK_NOT,
    IRON_TOK_NULL_KW,
    IRON_TOK_OBJECT,
    IRON_TOK_OR,
    IRON_TOK_PARALLEL,
    IRON_TOK_POOL,
    IRON_TOK_PRIVATE,
    IRON_TOK_RC,
    IRON_TOK_RETURN,
    IRON_TOK_SELF,
    IRON_TOK_SPAWN,
    IRON_TOK_SUPER,
    IRON_TOK_TRUE,
    IRON_TOK_VAL,
    IRON_TOK_VAR,
    IRON_TOK_WHILE,

    /* Operators */
    IRON_TOK_PLUS,          /* + */
    IRON_TOK_MINUS,         /* - */
    IRON_TOK_STAR,          /* * */
    IRON_TOK_SLASH,         /* / */
    IRON_TOK_PERCENT,       /* % */
    IRON_TOK_ASSIGN,        /* = */
    IRON_TOK_EQUALS,        /* == */
    IRON_TOK_NOT_EQUALS,    /* != */
    IRON_TOK_LESS,          /* < */
    IRON_TOK_GREATER,       /* > */
    IRON_TOK_LESS_EQ,       /* <= */
    IRON_TOK_GREATER_EQ,    /* >= */
    IRON_TOK_DOT,           /* . */
    IRON_TOK_DOTDOT,        /* .. */
    IRON_TOK_COMMA,         /* , */
    IRON_TOK_COLON,         /* : */
    IRON_TOK_ARROW,         /* -> */
    IRON_TOK_QUESTION,      /* ? */
    IRON_TOK_PLUS_ASSIGN,   /* += */
    IRON_TOK_MINUS_ASSIGN,  /* -= */
    IRON_TOK_STAR_ASSIGN,   /* *= */
    IRON_TOK_SLASH_ASSIGN,  /* /= */

    /* Delimiters */
    IRON_TOK_LPAREN,        /* ( */
    IRON_TOK_RPAREN,        /* ) */
    IRON_TOK_LBRACKET,      /* [ */
    IRON_TOK_RBRACKET,      /* ] */
    IRON_TOK_LBRACE,        /* { */
    IRON_TOK_RBRACE,        /* } */
    IRON_TOK_SEMICOLON,     /* ; */

    /* Special */
    IRON_TOK_NEWLINE,
    IRON_TOK_EOF,
    IRON_TOK_ERROR,
    IRON_TOK_IDENTIFIER,

    /* Sentinel */
    IRON_TOK_COUNT
} Iron_TokenKind;

/* ── Token ───────────────────────────────────────────────────────────────── */

/* A single lexed token.
 * - kind: what type of token this is
 * - value: arena-allocated copy of the token text (NULL for punctuation/delimiters)
 * - line, col: 1-indexed source position of the first character
 * - len: byte length of the token in the source
 */
typedef struct {
    Iron_TokenKind  kind;
    const char     *value;  /* NULL for punctuation; arena-copied string for literals/identifiers/keywords */
    uint32_t        line;
    uint32_t        col;
    uint32_t        len;
} Iron_Token;

/* ── Lexer ───────────────────────────────────────────────────────────────── */

typedef struct {
    const char   *src;       /* source text (not owned) */
    size_t        src_len;   /* length in bytes */
    size_t        pos;       /* current byte position */
    uint32_t      line;      /* current 1-indexed line */
    uint32_t      col;       /* current 1-indexed column */
    const char   *filename;  /* source file name for diagnostics */
    Iron_Arena   *arena;     /* arena for token values and diagnostic messages */
    Iron_DiagList *diags;    /* diagnostic list to emit errors into */
} Iron_Lexer;

/* ── API ─────────────────────────────────────────────────────────────────── */

/* Create a lexer over `src` (not copied; must outlive the lexer). */
Iron_Lexer iron_lexer_create(const char *src, const char *filename,
                              Iron_Arena *arena, Iron_DiagList *diags);

/* Lex all tokens from the source. Returns a stb_ds dynamic array of tokens.
 * The final token is always IRON_TOK_EOF.
 * The caller owns the array; call arrfree() when done.
 */
Iron_Token *iron_lex_all(Iron_Lexer *l);

/* Return a human-readable name for the token kind, e.g. "IRON_TOK_VAL". */
const char *iron_token_kind_str(Iron_TokenKind kind);

#endif /* IRON_LEXER_H */
