#ifndef IRON_PARSER_H
#define IRON_PARSER_H

#include "parser/ast.h"
#include "lexer/lexer.h"
#include "analyzer/analyzer.h" /* IronAnalysisMode — HARD-02 */

#include <stdatomic.h>
#include <stdbool.h>

/* ── Parser state ────────────────────────────────────────────────────────── */

typedef struct {
    Iron_Token         *tokens;       /* token array from iron_lex_all */
    int                 token_count;
    int                 pos;          /* current cursor position */
    Iron_Arena         *arena;
    Iron_DiagList      *diags;
    const char         *filename;
    const char         *source;       /* original source text for diagnostics */
    bool                in_error_recovery;
    /* BREAK gate -- default false; flip true after codemod sweep */
    bool                v3_strict_mode;
    IronAnalysisMode    mode;         /* HARD-02: gate cascade-suppression on LSP mode */
    const _Atomic bool *cancel_flag;  /* HARD-05: NULL means never cancel */
    int                 recur_depth;  /* HARD-08: current recursion depth */
    /* Stdlib carve-out: line number where the user's source begins. Set
     * by the build/check pipelines after all stdlib prepends complete;
     * copied into Iron_Program at parse exit and consulted by the
     * resolver to treat decls with span.line below this value as stdlib
     * (implicitly pub). For single-file user code with no stdlib
     * prepend, leave at 0 (no real source line satisfies the carve-out
     * condition). */
    int                 user_source_start_line;
} Iron_Parser;

/* ── API ─────────────────────────────────────────────────────────────────── */

/* Create a parser. tokens must have IRON_TOK_EOF as its last element.
 * token_count includes the EOF token.
 * The parser is initialised with mode = IRON_ANALYSIS_MODE_CLI; callers that
 * need LSP semantics must call iron_parser_set_mode() immediately after. */
Iron_Parser iron_parser_create(Iron_Token *tokens, int token_count,
                                const char *source, const char *filename,
                                Iron_Arena *arena, Iron_DiagList *diags);

/* HARD-02: set the analysis mode on an existing parser. When mode is
 * IRON_ANALYSIS_MODE_LSP the cascade-suppression effect inside iron_emit_diag
 * is disabled so every diagnostic is reported (LSP clients dedupe). */
void iron_parser_set_mode(Iron_Parser *p, IronAnalysisMode mode);

/* HARD-05: attach a caller-owned _Atomic bool *cancel flag. NULL disables
 * polling (the default). When set and observed true at any poll site
 * inside iron_parse (and the static helper walkers it dispatches into),
 * the parser emits a single NOTE-level IRON_ERR_CANCELLED and unwinds at
 * the next safepoint. */
void iron_parser_set_cancel_flag(Iron_Parser *p, const _Atomic bool *flag);

/* Parse the full token stream and return an Iron_Program node.
 * Never returns NULL — uses ErrorNode on failures. */
Iron_Node *iron_parse(Iron_Parser *p);

#endif /* IRON_PARSER_H */
