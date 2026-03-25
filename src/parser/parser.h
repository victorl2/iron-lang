#ifndef IRON_PARSER_H
#define IRON_PARSER_H

#include "parser/ast.h"
#include "lexer/lexer.h"

/* ── Parser state ────────────────────────────────────────────────────────── */

typedef struct {
    Iron_Token    *tokens;       /* token array from iron_lex_all */
    int            token_count;
    int            pos;          /* current cursor position */
    Iron_Arena    *arena;
    Iron_DiagList *diags;
    const char    *filename;
    const char    *source;       /* original source text for diagnostics */
    bool           in_error_recovery;
} Iron_Parser;

/* ── API ─────────────────────────────────────────────────────────────────── */

/* Create a parser. tokens must have IRON_TOK_EOF as its last element.
 * token_count includes the EOF token. */
Iron_Parser iron_parser_create(Iron_Token *tokens, int token_count,
                                const char *source, const char *filename,
                                Iron_Arena *arena, Iron_DiagList *diags);

/* Parse the full token stream and return an Iron_Program node.
 * Never returns NULL — uses ErrorNode on failures. */
Iron_Node *iron_parse(Iron_Parser *p);

#endif /* IRON_PARSER_H */
