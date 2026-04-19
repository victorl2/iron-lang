/* Phase 5 Plan 05-01 (D-01, FMT-01): single library entry for Iron
 * source formatting. Both `iron fmt` CLI and the LSP facade route
 * through here. Mirrors src/cli/fmt.c:50-86 (lex+parse+refuse) but
 * caller-owns-arena per HARD-06. */

#include "fmt/format.h"
#include "fmt/options.h"

#include "lexer/lexer.h"
#include "parser/parser.h"
#include "parser/printer.h"
#include "parser/ast.h"
#include "diagnostics/diagnostics.h"
#include "util/arena.h"
#include "vendor/stb_ds.h"

#include <string.h>

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
    Iron_Node  *ast = iron_parse(&parser);
    arrfree(tokens);   /* FIX-03 ownership: stb_ds header is heap-owned */

    if (diags->error_count > 0) {
        out.error_count = diags->error_count;
        return out;   /* REFUSE (D-03) */
    }

    /* 3. Print */
    char *formatted = iron_print_ast(ast, &effective, arena);

    out.formatted     = formatted ? formatted : "";
    out.formatted_len = formatted ? strlen(formatted) : 0;
    out.ok            = true;
    out.error_count   = 0;
    return out;
}
