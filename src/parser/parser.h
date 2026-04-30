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
    /* Phase 88: BREAK gate -- default false; flip true after Phase 89 codemod */
    bool           v3_strict_mode;
    /* Phase 93 VIS-03 stdlib carve-out: line number where the user's source
     * begins. Set by the build/check pipelines after all stdlib prepends
     * complete; copied into Iron_Program at parse exit and consulted by the
     * resolver to treat decls with span.line below this value as stdlib
     * (implicitly pub). For single-file user code with no stdlib prepend,
     * leave at 0 (no real source line satisfies the carve-out condition). */
    int            user_source_start_line;
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
