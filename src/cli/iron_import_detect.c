#include "cli/iron_import_detect.h"
#include "lexer/lexer.h"
#include "diagnostics/diagnostics.h"
#include "vendor/stb_ds.h"
#include <string.h>

bool iron_detect_import(const char *source, const char *filename,
                        const char *module_name, Iron_Arena *arena) {
    if (!source || !module_name) return false;
    const char *fname = filename ? filename : "<import-detect>";

    /* Create a throwaway diagnostic list — lex errors are ignored for detection */
    Iron_DiagList diags = iron_diaglist_create();

    Iron_Lexer lexer = iron_lexer_create(source, fname, arena, &diags);
    Iron_Token *tokens = iron_lex_all(&lexer);

    bool found = false;
    if (tokens) {
        ptrdiff_t count = arrlen(tokens);
        for (ptrdiff_t i = 0; i < count - 1; i++) {
            if (tokens[i].kind == IRON_TOK_IMPORT &&
                tokens[i + 1].kind == IRON_TOK_IDENTIFIER &&
                tokens[i + 1].value != NULL &&
                strcmp(tokens[i + 1].value, module_name) == 0) {
                found = true;
                break;
            }
        }
        arrfree(tokens);
    }

    iron_diaglist_free(&diags);
    return found;
}
