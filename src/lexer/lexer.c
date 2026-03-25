#include "lexer/lexer.h"
#include "stb_ds.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ── Keyword table ───────────────────────────────────────────────────────── */

typedef struct {
    const char    *name;
    Iron_TokenKind kind;
} KeywordEntry;

/* Must remain sorted for bsearch. */
static const KeywordEntry kw_table[] = {
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

#define KW_TABLE_SIZE ((int)(sizeof(kw_table) / sizeof(kw_table[0])))

static int kw_compare(const void *key, const void *entry) {
    return strcmp((const char *)key, ((const KeywordEntry *)entry)->name);
}

static Iron_TokenKind keyword_lookup(const char *name) {
    const KeywordEntry *e = bsearch(name, kw_table, (size_t)KW_TABLE_SIZE,
                                    sizeof(KeywordEntry), kw_compare);
    return e ? e->kind : IRON_TOK_IDENTIFIER;
}

/* ── Token kind names ────────────────────────────────────────────────────── */

static const char *kw_kind_names[IRON_TOK_COUNT] = {
    [IRON_TOK_INTEGER]       = "IRON_TOK_INTEGER",
    [IRON_TOK_FLOAT]         = "IRON_TOK_FLOAT",
    [IRON_TOK_STRING]        = "IRON_TOK_STRING",
    [IRON_TOK_INTERP_STRING] = "IRON_TOK_INTERP_STRING",
    [IRON_TOK_AND]           = "IRON_TOK_AND",
    [IRON_TOK_AWAIT]         = "IRON_TOK_AWAIT",
    [IRON_TOK_COMPTIME]      = "IRON_TOK_COMPTIME",
    [IRON_TOK_DEFER]         = "IRON_TOK_DEFER",
    [IRON_TOK_ELIF]          = "IRON_TOK_ELIF",
    [IRON_TOK_ELSE]          = "IRON_TOK_ELSE",
    [IRON_TOK_ENUM]          = "IRON_TOK_ENUM",
    [IRON_TOK_EXTENDS]       = "IRON_TOK_EXTENDS",
    [IRON_TOK_FALSE]         = "IRON_TOK_FALSE",
    [IRON_TOK_FOR]           = "IRON_TOK_FOR",
    [IRON_TOK_FREE]          = "IRON_TOK_FREE",
    [IRON_TOK_FUNC]          = "IRON_TOK_FUNC",
    [IRON_TOK_HEAP]          = "IRON_TOK_HEAP",
    [IRON_TOK_IF]            = "IRON_TOK_IF",
    [IRON_TOK_IMPLEMENTS]    = "IRON_TOK_IMPLEMENTS",
    [IRON_TOK_IMPORT]        = "IRON_TOK_IMPORT",
    [IRON_TOK_IN]            = "IRON_TOK_IN",
    [IRON_TOK_INTERFACE]     = "IRON_TOK_INTERFACE",
    [IRON_TOK_IS]            = "IRON_TOK_IS",
    [IRON_TOK_LEAK]          = "IRON_TOK_LEAK",
    [IRON_TOK_MATCH]         = "IRON_TOK_MATCH",
    [IRON_TOK_NOT]           = "IRON_TOK_NOT",
    [IRON_TOK_NULL_KW]       = "IRON_TOK_NULL_KW",
    [IRON_TOK_OBJECT]        = "IRON_TOK_OBJECT",
    [IRON_TOK_OR]            = "IRON_TOK_OR",
    [IRON_TOK_PARALLEL]      = "IRON_TOK_PARALLEL",
    [IRON_TOK_POOL]          = "IRON_TOK_POOL",
    [IRON_TOK_PRIVATE]       = "IRON_TOK_PRIVATE",
    [IRON_TOK_RC]            = "IRON_TOK_RC",
    [IRON_TOK_RETURN]        = "IRON_TOK_RETURN",
    [IRON_TOK_SELF]          = "IRON_TOK_SELF",
    [IRON_TOK_SPAWN]         = "IRON_TOK_SPAWN",
    [IRON_TOK_SUPER]         = "IRON_TOK_SUPER",
    [IRON_TOK_TRUE]          = "IRON_TOK_TRUE",
    [IRON_TOK_VAL]           = "IRON_TOK_VAL",
    [IRON_TOK_VAR]           = "IRON_TOK_VAR",
    [IRON_TOK_WHILE]         = "IRON_TOK_WHILE",
    [IRON_TOK_PLUS]          = "IRON_TOK_PLUS",
    [IRON_TOK_MINUS]         = "IRON_TOK_MINUS",
    [IRON_TOK_STAR]          = "IRON_TOK_STAR",
    [IRON_TOK_SLASH]         = "IRON_TOK_SLASH",
    [IRON_TOK_PERCENT]       = "IRON_TOK_PERCENT",
    [IRON_TOK_ASSIGN]        = "IRON_TOK_ASSIGN",
    [IRON_TOK_EQUALS]        = "IRON_TOK_EQUALS",
    [IRON_TOK_NOT_EQUALS]    = "IRON_TOK_NOT_EQUALS",
    [IRON_TOK_LESS]          = "IRON_TOK_LESS",
    [IRON_TOK_GREATER]       = "IRON_TOK_GREATER",
    [IRON_TOK_LESS_EQ]       = "IRON_TOK_LESS_EQ",
    [IRON_TOK_GREATER_EQ]    = "IRON_TOK_GREATER_EQ",
    [IRON_TOK_DOT]           = "IRON_TOK_DOT",
    [IRON_TOK_DOTDOT]        = "IRON_TOK_DOTDOT",
    [IRON_TOK_COMMA]         = "IRON_TOK_COMMA",
    [IRON_TOK_COLON]         = "IRON_TOK_COLON",
    [IRON_TOK_ARROW]         = "IRON_TOK_ARROW",
    [IRON_TOK_QUESTION]      = "IRON_TOK_QUESTION",
    [IRON_TOK_PLUS_ASSIGN]   = "IRON_TOK_PLUS_ASSIGN",
    [IRON_TOK_MINUS_ASSIGN]  = "IRON_TOK_MINUS_ASSIGN",
    [IRON_TOK_STAR_ASSIGN]   = "IRON_TOK_STAR_ASSIGN",
    [IRON_TOK_SLASH_ASSIGN]  = "IRON_TOK_SLASH_ASSIGN",
    [IRON_TOK_LPAREN]        = "IRON_TOK_LPAREN",
    [IRON_TOK_RPAREN]        = "IRON_TOK_RPAREN",
    [IRON_TOK_LBRACKET]      = "IRON_TOK_LBRACKET",
    [IRON_TOK_RBRACKET]      = "IRON_TOK_RBRACKET",
    [IRON_TOK_LBRACE]        = "IRON_TOK_LBRACE",
    [IRON_TOK_RBRACE]        = "IRON_TOK_RBRACE",
    [IRON_TOK_SEMICOLON]     = "IRON_TOK_SEMICOLON",
    [IRON_TOK_NEWLINE]       = "IRON_TOK_NEWLINE",
    [IRON_TOK_EOF]           = "IRON_TOK_EOF",
    [IRON_TOK_ERROR]         = "IRON_TOK_ERROR",
    [IRON_TOK_IDENTIFIER]    = "IRON_TOK_IDENTIFIER",
};

const char *iron_token_kind_str(Iron_TokenKind kind) {
    if (kind < 0 || kind >= IRON_TOK_COUNT) return "IRON_TOK_UNKNOWN";
    const char *s = kw_kind_names[kind];
    return s ? s : "IRON_TOK_UNKNOWN";
}

/* ── Lexer creation ──────────────────────────────────────────────────────── */

Iron_Lexer iron_lexer_create(const char *src, const char *filename,
                              Iron_Arena *arena, Iron_DiagList *diags) {
    Iron_Lexer l;
    l.src      = src;
    l.src_len  = src ? strlen(src) : 0;
    l.pos      = 0;
    l.line     = 1;
    l.col      = 1;
    l.filename = filename;
    l.arena    = arena;
    l.diags    = diags;
    return l;
}

/* ── Low-level helpers ───────────────────────────────────────────────────── */

static char iron_peek_char(Iron_Lexer *l) {
    if (l->pos >= l->src_len) return '\0';
    return l->src[l->pos];
}

static char iron_peek_next(Iron_Lexer *l) {
    if (l->pos + 1 >= l->src_len) return '\0';
    return l->src[l->pos + 1];
}

static char iron_advance_char(Iron_Lexer *l) {
    if (l->pos >= l->src_len) return '\0';
    char c = l->src[l->pos++];
    l->col++;
    return c;
}

/* Skip spaces and tabs only. Newlines are significant tokens. */
static void iron_skip_whitespace(Iron_Lexer *l) {
    while (l->pos < l->src_len) {
        char c = l->src[l->pos];
        if (c == ' ' || c == '\t' || c == '\r') {
            l->pos++;
            l->col++;
        } else {
            break;
        }
    }
}

static Iron_Token iron_make_token(Iron_Lexer *l, Iron_TokenKind kind,
                                   const char *value,
                                   uint32_t start_line, uint32_t start_col,
                                   uint32_t len) {
    Iron_Token t;
    t.kind  = kind;
    t.value = value;
    t.line  = start_line;
    t.col   = start_col;
    t.len   = len;
    (void)l; /* suppress unused warning */
    return t;
}

/* ── String lexing ───────────────────────────────────────────────────────── */

static Iron_Token iron_lex_string(Iron_Lexer *l) {
    uint32_t start_line = l->line;
    uint32_t start_col  = l->col;
    size_t   start_pos  = l->pos;

    /* Consume opening quote(s). */
    iron_advance_char(l); /* first " */

    int multiline = 0;
    if (iron_peek_char(l) == '"' && iron_peek_next(l) == '"') {
        iron_advance_char(l); /* second " */
        iron_advance_char(l); /* third " */
        multiline = 1;
    }

    /* Buffer for string content (stored in arena later). */
    size_t  buf_cap  = 256;
    size_t  buf_len  = 0;
    char   *buf      = (char *)iron_arena_alloc(l->arena, buf_cap, 1);
    if (!buf) buf_cap = 0;

    int has_interp = 0;

    /* Reserve space helper (inline growth). Since arena is bump-only, we
     * pre-allocate a generous block and track usage. For very long strings we
     * just extend the arena. The simpler approach: build into a local large
     * arena allocation. We use 4096 initial capacity. */
    (void)buf_cap;
    /* Restart: allocate 4 KB up front. The arena won't reclaim it so we accept
     * the waste for now. */
    buf_len = 0;
    buf     = (char *)iron_arena_alloc(l->arena, 4096, 1);
    if (!buf) {
        /* OOM — return an error token. */
        Iron_Span span = iron_span_make(l->filename, start_line, start_col,
                                        l->line, l->col);
        iron_diag_emit(l->diags, l->arena, IRON_DIAG_ERROR,
                       IRON_ERR_UNTERMINATED_STRING, span,
                       "out of memory", NULL);
        return iron_make_token(l, IRON_TOK_ERROR, NULL,
                               start_line, start_col, 0);
    }
    size_t buf_max = 4096;

#define PUSH_CHAR(ch) do { \
    if (buf_len < buf_max - 1) { buf[buf_len++] = (ch); } \
} while (0)

    for (;;) {
        char c = iron_peek_char(l);

        if (c == '\0') {
            /* EOF without closing quote. */
            Iron_Span span = iron_span_make(l->filename, start_line, start_col,
                                             l->line, l->col);
            iron_diag_emit(l->diags, l->arena, IRON_DIAG_ERROR,
                           IRON_ERR_UNTERMINATED_STRING, span,
                           "unterminated string literal",
                           "add a closing `\"` at the end of the string");
            return iron_make_token(l, IRON_TOK_ERROR, NULL,
                                   start_line, start_col,
                                   (uint32_t)(l->pos - start_pos));
        }

        if (!multiline && c == '\n') {
            /* Newline inside non-multiline string — unterminated. */
            Iron_Span span = iron_span_make(l->filename, start_line, start_col,
                                             l->line, l->col);
            iron_diag_emit(l->diags, l->arena, IRON_DIAG_ERROR,
                           IRON_ERR_UNTERMINATED_STRING, span,
                           "unterminated string literal",
                           "add a closing `\"` at the end of the string");
            return iron_make_token(l, IRON_TOK_ERROR, NULL,
                                   start_line, start_col,
                                   (uint32_t)(l->pos - start_pos));
        }

        if (c == '\\') {
            iron_advance_char(l);
            char esc = iron_advance_char(l);
            switch (esc) {
                case 'n':  PUSH_CHAR('\n'); break;
                case 't':  PUSH_CHAR('\t'); break;
                case '\\': PUSH_CHAR('\\'); break;
                case '"':  PUSH_CHAR('"');  break;
                case '{':  PUSH_CHAR('{');  break;
                default:   PUSH_CHAR('\\'); PUSH_CHAR(esc); break;
            }
            continue;
        }

        if (c == '{') {
            has_interp = 1;
        }

        if (multiline) {
            if (c == '"' && iron_peek_next(l) == '"') {
                /* Check if third " */
                if (l->pos + 2 < l->src_len && l->src[l->pos + 2] == '"') {
                    iron_advance_char(l); /* " */
                    iron_advance_char(l); /* " */
                    iron_advance_char(l); /* " */
                    break;
                }
            }
        } else {
            if (c == '"') {
                iron_advance_char(l);
                break;
            }
        }

        /* Handle newlines in multiline strings. */
        if (c == '\n') {
            l->pos++;
            l->line++;
            l->col = 1;
            PUSH_CHAR('\n');
            continue;
        }

        iron_advance_char(l);
        PUSH_CHAR(c);
    }

#undef PUSH_CHAR

    buf[buf_len] = '\0';
    /* Copy final string into arena (fresh minimal allocation). */
    const char *value = iron_arena_strdup(l->arena, buf, buf_len);
    uint32_t tok_len = (uint32_t)(l->pos - start_pos);
    Iron_TokenKind kind = has_interp ? IRON_TOK_INTERP_STRING : IRON_TOK_STRING;
    return iron_make_token(l, kind, value, start_line, start_col, tok_len);
}

/* ── Number lexing ───────────────────────────────────────────────────────── */

static Iron_Token iron_lex_number(Iron_Lexer *l) {
    uint32_t start_line = l->line;
    uint32_t start_col  = l->col;
    size_t   start_pos  = l->pos;
    int      is_float   = 0;

    while (l->pos < l->src_len && isdigit((unsigned char)l->src[l->pos])) {
        iron_advance_char(l);
    }

    /* Check for float: digit '.' digit */
    if (iron_peek_char(l) == '.' && l->pos + 1 < l->src_len &&
        isdigit((unsigned char)l->src[l->pos + 1])) {
        is_float = 1;
        iron_advance_char(l); /* . */
        while (l->pos < l->src_len && isdigit((unsigned char)l->src[l->pos])) {
            iron_advance_char(l);
        }
    }

    uint32_t tok_len = (uint32_t)(l->pos - start_pos);

    /* Invalid suffix: letter immediately after number. */
    if (l->pos < l->src_len && isalpha((unsigned char)l->src[l->pos])) {
        Iron_Span span = iron_span_make(l->filename, start_line, start_col,
                                         l->line, l->col);
        iron_diag_emit(l->diags, l->arena, IRON_DIAG_ERROR,
                       IRON_ERR_INVALID_NUMBER, span,
                       "invalid numeric literal: unexpected character after number",
                       NULL);
        /* Consume the bad suffix to aid recovery. */
        while (l->pos < l->src_len && isalnum((unsigned char)l->src[l->pos])) {
            iron_advance_char(l);
        }
        tok_len = (uint32_t)(l->pos - start_pos);
        return iron_make_token(l, IRON_TOK_ERROR, NULL,
                               start_line, start_col, tok_len);
    }

    const char *value = iron_arena_strdup(l->arena, l->src + start_pos, tok_len);
    Iron_TokenKind kind = is_float ? IRON_TOK_FLOAT : IRON_TOK_INTEGER;
    return iron_make_token(l, kind, value, start_line, start_col, tok_len);
}

/* ── Identifier / keyword lexing ─────────────────────────────────────────── */

static Iron_Token iron_lex_identifier(Iron_Lexer *l) {
    uint32_t start_line = l->line;
    uint32_t start_col  = l->col;
    size_t   start_pos  = l->pos;

    while (l->pos < l->src_len) {
        char c = l->src[l->pos];
        if (isalnum((unsigned char)c) || c == '_') {
            iron_advance_char(l);
        } else {
            break;
        }
    }

    uint32_t tok_len = (uint32_t)(l->pos - start_pos);
    const char *text = iron_arena_strdup(l->arena, l->src + start_pos, tok_len);

    Iron_TokenKind kind = keyword_lookup(text);
    return iron_make_token(l, kind, text, start_line, start_col, tok_len);
}

/* ── Punctuation / operator lexing ───────────────────────────────────────── */

static Iron_Token iron_lex_punctuation(Iron_Lexer *l) {
    uint32_t start_line = l->line;
    uint32_t start_col  = l->col;
    size_t   start_pos  = l->pos;

    char c = iron_advance_char(l);

    switch (c) {
        case '+':
            if (iron_peek_char(l) == '=') {
                iron_advance_char(l);
                return iron_make_token(l, IRON_TOK_PLUS_ASSIGN, NULL,
                                       start_line, start_col, 2);
            }
            return iron_make_token(l, IRON_TOK_PLUS, NULL,
                                   start_line, start_col, 1);

        case '-':
            if (iron_peek_char(l) == '>') {
                iron_advance_char(l);
                return iron_make_token(l, IRON_TOK_ARROW, NULL,
                                       start_line, start_col, 2);
            }
            if (iron_peek_char(l) == '-') {
                /* Comment: consume to end of line. */
                while (l->pos < l->src_len && l->src[l->pos] != '\n') {
                    l->pos++;
                    l->col++;
                }
                /* Return NEWLINE token (comment consumed). */
                if (l->pos < l->src_len && l->src[l->pos] == '\n') {
                    l->pos++;
                    l->line++;
                    l->col = 1;
                }
                return iron_make_token(l, IRON_TOK_NEWLINE, NULL,
                                       start_line, start_col,
                                       (uint32_t)(l->pos - start_pos));
            }
            if (iron_peek_char(l) == '=') {
                iron_advance_char(l);
                return iron_make_token(l, IRON_TOK_MINUS_ASSIGN, NULL,
                                       start_line, start_col, 2);
            }
            return iron_make_token(l, IRON_TOK_MINUS, NULL,
                                   start_line, start_col, 1);

        case '*':
            if (iron_peek_char(l) == '=') {
                iron_advance_char(l);
                return iron_make_token(l, IRON_TOK_STAR_ASSIGN, NULL,
                                       start_line, start_col, 2);
            }
            return iron_make_token(l, IRON_TOK_STAR, NULL,
                                   start_line, start_col, 1);

        case '/':
            if (iron_peek_char(l) == '=') {
                iron_advance_char(l);
                return iron_make_token(l, IRON_TOK_SLASH_ASSIGN, NULL,
                                       start_line, start_col, 2);
            }
            return iron_make_token(l, IRON_TOK_SLASH, NULL,
                                   start_line, start_col, 1);

        case '%':
            return iron_make_token(l, IRON_TOK_PERCENT, NULL,
                                   start_line, start_col, 1);

        case '=':
            if (iron_peek_char(l) == '=') {
                iron_advance_char(l);
                return iron_make_token(l, IRON_TOK_EQUALS, NULL,
                                       start_line, start_col, 2);
            }
            return iron_make_token(l, IRON_TOK_ASSIGN, NULL,
                                   start_line, start_col, 1);

        case '!':
            if (iron_peek_char(l) == '=') {
                iron_advance_char(l);
                return iron_make_token(l, IRON_TOK_NOT_EQUALS, NULL,
                                       start_line, start_col, 2);
            }
            /* Bare '!' is not valid Iron — fall through to error. */
            {
                char msg[64];
                snprintf(msg, sizeof(msg), "invalid character '!'");
                const char *arena_msg = iron_arena_strdup(l->arena, msg, strlen(msg));
                Iron_Span span = iron_span_make(l->filename, start_line, start_col,
                                                 start_line, start_col + 1);
                iron_diag_emit(l->diags, l->arena, IRON_DIAG_ERROR,
                               IRON_ERR_INVALID_CHAR, span, arena_msg, NULL);
                return iron_make_token(l, IRON_TOK_ERROR, NULL,
                                       start_line, start_col,
                                       (uint32_t)(l->pos - start_pos));
            }

        case '<':
            if (iron_peek_char(l) == '=') {
                iron_advance_char(l);
                return iron_make_token(l, IRON_TOK_LESS_EQ, NULL,
                                       start_line, start_col, 2);
            }
            return iron_make_token(l, IRON_TOK_LESS, NULL,
                                   start_line, start_col, 1);

        case '>':
            if (iron_peek_char(l) == '=') {
                iron_advance_char(l);
                return iron_make_token(l, IRON_TOK_GREATER_EQ, NULL,
                                       start_line, start_col, 2);
            }
            return iron_make_token(l, IRON_TOK_GREATER, NULL,
                                   start_line, start_col, 1);

        case '.':
            if (iron_peek_char(l) == '.') {
                iron_advance_char(l);
                return iron_make_token(l, IRON_TOK_DOTDOT, NULL,
                                       start_line, start_col, 2);
            }
            return iron_make_token(l, IRON_TOK_DOT, NULL,
                                   start_line, start_col, 1);

        case ',':
            return iron_make_token(l, IRON_TOK_COMMA, NULL,
                                   start_line, start_col, 1);
        case ':':
            return iron_make_token(l, IRON_TOK_COLON, NULL,
                                   start_line, start_col, 1);
        case '?':
            return iron_make_token(l, IRON_TOK_QUESTION, NULL,
                                   start_line, start_col, 1);
        case '(':
            return iron_make_token(l, IRON_TOK_LPAREN, NULL,
                                   start_line, start_col, 1);
        case ')':
            return iron_make_token(l, IRON_TOK_RPAREN, NULL,
                                   start_line, start_col, 1);
        case '[':
            return iron_make_token(l, IRON_TOK_LBRACKET, NULL,
                                   start_line, start_col, 1);
        case ']':
            return iron_make_token(l, IRON_TOK_RBRACKET, NULL,
                                   start_line, start_col, 1);
        case '{':
            return iron_make_token(l, IRON_TOK_LBRACE, NULL,
                                   start_line, start_col, 1);
        case '}':
            return iron_make_token(l, IRON_TOK_RBRACE, NULL,
                                   start_line, start_col, 1);
        case ';':
            return iron_make_token(l, IRON_TOK_SEMICOLON, NULL,
                                   start_line, start_col, 1);

        default: {
            /* Unrecognized character — emit E0002 and return ERROR token. */
            char msg[64];
            if (c >= 32 && c < 127) {
                snprintf(msg, sizeof(msg), "invalid character '%c'", c);
            } else {
                snprintf(msg, sizeof(msg), "invalid character (0x%02x)", (unsigned char)c);
            }
            const char *arena_msg = iron_arena_strdup(l->arena, msg, strlen(msg));
            Iron_Span span = iron_span_make(l->filename, start_line, start_col,
                                             start_line, start_col + 1);
            iron_diag_emit(l->diags, l->arena, IRON_DIAG_ERROR,
                           IRON_ERR_INVALID_CHAR, span, arena_msg, NULL);
            return iron_make_token(l, IRON_TOK_ERROR, NULL,
                                   start_line, start_col,
                                   (uint32_t)(l->pos - start_pos));
        }
    }
}

/* ── Main lex loop ───────────────────────────────────────────────────────── */

Iron_Token *iron_lex_all(Iron_Lexer *l) {
    Iron_Token *tokens = NULL; /* stb_ds array */

    for (;;) {
        iron_skip_whitespace(l);

        char c = iron_peek_char(l);

        /* EOF */
        if (c == '\0') {
            Iron_Token eof = iron_make_token(l, IRON_TOK_EOF, NULL,
                                              l->line, l->col, 0);
            arrput(tokens, eof);
            break;
        }

        /* Newline */
        if (c == '\n') {
            uint32_t nl_line = l->line;
            uint32_t nl_col  = l->col;
            l->pos++;
            l->line++;
            l->col = 1;
            Iron_Token nl = iron_make_token(l, IRON_TOK_NEWLINE, NULL,
                                             nl_line, nl_col, 1);
            arrput(tokens, nl);
            continue;
        }

        Iron_Token tok;

        /* String literal */
        if (c == '"') {
            tok = iron_lex_string(l);
        }
        /* Number */
        else if (isdigit((unsigned char)c)) {
            tok = iron_lex_number(l);
        }
        /* Identifier or keyword */
        else if (isalpha((unsigned char)c) || c == '_') {
            tok = iron_lex_identifier(l);
        }
        /* Punctuation / operators */
        else {
            tok = iron_lex_punctuation(l);
        }

        arrput(tokens, tok);

        /* After a comment (which returned NEWLINE), we already advanced past
         * the '\n', so just continue normally. */
    }

    return tokens;
}
