/* Phase 5 Plan 05-01 (D-01, FMT-01): single library entry for Iron
 * source formatting. Both `iron fmt` CLI and the LSP facade route
 * through here. Mirrors src/cli/fmt.c:50-86 (lex+parse+refuse) but
 * caller-owns-arena per HARD-06. */

#include "fmt/format.h"
#include "fmt/options.h"
#include "fmt/layout.h"

#include "lexer/lexer.h"
#include "parser/parser.h"
#include "parser/printer.h"
#include "parser/ast.h"
#include "diagnostics/diagnostics.h"
#include "util/arena.h"
#include "vendor/stb_ds.h"

#include <string.h>

bool iron_fmt_same_tokens(const Iron_Token *tokens, int count, const char *text,
                          const char *filename, Iron_Arena *arena) {
    Iron_DiagList d = iron_diaglist_create();
    Iron_Lexer    lx = iron_lexer_create(text, filename, arena, &d);
    Iron_Token   *again = iron_lex_all(&lx);
    bool same = d.error_count == 0;
    int i = 0, j = 0, m = (int)arrlen(again);
    while (same) {
        while (i < count && tokens[i].kind == IRON_TOK_NEWLINE) i++;
        while (j < m && again[j].kind == IRON_TOK_NEWLINE) j++;
        if (i >= count || j >= m) { same = i >= count && j >= m; break; }
        const Iron_Token *a = &tokens[i++], *b = &again[j++];
        if (a->kind != b->kind) same = false;
        else if ((a->value == NULL) != (b->value == NULL)) same = false;
        else if (a->value && a->kind == IRON_TOK_DOC_COMMENT) {
            size_t la = strlen(a->value), lb = strlen(b->value);
            while (la > 0 && (a->value[la - 1] == ' ' || a->value[la - 1] == '\t' ||
                              a->value[la - 1] == '\r')) la--;
            while (lb > 0 && (b->value[lb - 1] == ' ' || b->value[lb - 1] == '\t' ||
                              b->value[lb - 1] == '\r')) lb--;
            same = la == lb && memcmp(a->value, b->value, la) == 0;
        } else if (a->value && strcmp(a->value, b->value) != 0) same = false;
    }
    arrfree(again);
    iron_diaglist_free(&d);
    return same;
}

IronFmtResult iron_format_source(const char           *source,
                                 const char           *filename,
                                 const IronFmtOptions *opts,
                                 Iron_Arena           *arena,
                                 Iron_DiagList        *diags) {
    IronFmtResult out;
    out.formatted     = "";
    out.formatted_len = 0;
    out.ok            = false;
    out.error_count   = 0;

    /* Defensive: caller-owns contract. Bad inputs return ok=false. */
    if (!source || !arena || !diags) return out;

    IronFmtOptions effective = opts ? *opts : iron_fmt_options_default();

    /* 1. Lex */
    Iron_Lexer  lexer  = iron_lexer_create(source, filename, arena, diags);
    Iron_Token *tokens = iron_lex_all(&lexer);

    if (diags->error_count > 0) {
        arrfree(tokens);
        out.error_count = diags->error_count;
        return out;   /* REFUSE (D-03) */
    }

    /* 2. Parse */
    int         token_count = (int)arrlen(tokens);
    Iron_Parser parser      = iron_parser_create(tokens, token_count,
                                                 source, filename,
                                                 arena, diags);
    (void)iron_parse(&parser);

    if (diags->error_count > 0) {
        arrfree(tokens);
        out.error_count = diags->error_count;
        return out;   /* REFUSE (D-03) */
    }

    /* 3. Lay out. Only whitespace changes: indentation, trailing blanks
     * and runs of blank lines. Comments and tokens stay as written (the
     * old AST printer dropped comments and lost tuples, escapes, patch
     * members, `extern`, `nocopy` and more). */
    char *formatted = iron_fmt_layout(source, &effective, arena, 0, 0);

    /* 4. The formatted text must lex to the same tokens; anything else is
     * a formatter bug, and the source is left alone. */
    if (formatted && !iron_fmt_same_tokens(tokens, token_count, formatted,
                                           filename, arena)) {
        iron_diag_emit(diags, arena, IRON_DIAG_ERROR, 0,
                       iron_span_make(filename, 1, 1, 1, 1),
                       "internal formatter error: formatting would change the "
                       "program, so the file was left as it is",
                       "please report this file");
        arrfree(tokens);
        out.error_count = diags->error_count;
        return out;
    }
    arrfree(tokens);   /* FIX-03 ownership: stb_ds header is heap-owned */

    out.formatted     = formatted ? formatted : "";
    out.formatted_len = formatted ? strlen(formatted) : 0;
    out.ok            = true;
    out.error_count   = 0;
    return out;
}
