#include "cli/check.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "lexer/lexer.h"
#include "parser/parser.h"
#include "parser/ast.h"
#include "analyzer/analyzer.h"
#include "diagnostics/diagnostics.h"
#include "util/arena.h"
#include "vendor/stb_ds.h"

/* ── Helper: read a file into a heap-allocated string ────────────────────── */

static char *check_read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "error: cannot open '%s': %s\n", path, strerror(errno));
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        fprintf(stderr, "error: cannot seek '%s': %s\n", path, strerror(errno));
        return NULL;
    }
    long size = ftell(f);
    rewind(f);
    char *buf = (char *)malloc((size_t)(size + 1));
    if (!buf) {
        fclose(f);
        fprintf(stderr, "error: out of memory reading '%s'\n", path);
        return NULL;
    }
    size_t read = fread(buf, 1, (size_t)size, f);
    fclose(f);
    buf[read] = '\0';
    return buf;
}

/* ── Check: lex + parse + analyze, no codegen ────────────────────────────── */

int iron_check(const char *source_path, bool verbose) {
    /* 1. Read source file */
    char *source = check_read_file(source_path);
    if (!source) return 1;

    /* 2. Set up arena and diagnostics */
    Iron_Arena arena = iron_arena_create(32 * 1024);
    Iron_DiagList diags = iron_diaglist_create();

    /* 3. Lex */
    Iron_Lexer lexer = iron_lexer_create(source, source_path, &arena, &diags);
    Iron_Token *tokens = iron_lex_all(&lexer);

    if (diags.error_count > 0) {
        iron_diag_print_all(&diags, source);
        arrfree(tokens);
        iron_diaglist_free(&diags);
        iron_arena_free(&arena);
        free(source);
        return 1;
    }

    /* 4. Parse */
    int token_count = (int)arrlen(tokens);
    Iron_Parser parser = iron_parser_create(tokens, token_count,
                                            source, source_path,
                                            &arena, &diags);
    Iron_Node *ast = iron_parse(&parser);
    arrfree(tokens);

    if (diags.error_count > 0) {
        iron_diag_print_all(&diags, source);
        iron_diaglist_free(&diags);
        iron_arena_free(&arena);
        free(source);
        return 1;
    }

    /* 5. Analyze */
    Iron_AnalyzeResult result = iron_analyze((Iron_Program *)ast, &arena,
                                             &diags);

    /* 6. Print all diagnostics */
    iron_diag_print_all(&diags, source);

    /* 7. Verbose: print analysis summary */
    if (verbose) {
        fprintf(stderr, "check: %s\n", source_path);
        fprintf(stderr, "  errors:   %d\n", diags.error_count);
        fprintf(stderr, "  warnings: %d\n", diags.warning_count);
        if (result.global_scope) {
            fprintf(stderr, "  analysis: complete (global scope established)\n");
        }
    }

    int exit_code = (diags.error_count > 0 || result.has_errors) ? 1 : 0;

    iron_diaglist_free(&diags);
    iron_arena_free(&arena);
    free(source);
    return exit_code;
}
