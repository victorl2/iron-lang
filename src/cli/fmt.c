#include "cli/fmt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "lexer/lexer.h"
#include "parser/parser.h"
#include "parser/printer.h"
#include "parser/ast.h"
#include "diagnostics/diagnostics.h"
#include "util/arena.h"
#include "vendor/stb_ds.h"

/* ── Helper: read a file into a heap-allocated string ────────────────────── */

static char *fmt_read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "iron fmt: cannot open '%s': %s\n", path, strerror(errno));
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        fprintf(stderr, "iron fmt: cannot seek '%s': %s\n", path, strerror(errno));
        return NULL;
    }
    long size = ftell(f);
    rewind(f);
    char *buf = (char *)malloc((size_t)(size + 1));
    if (!buf) {
        fclose(f);
        fprintf(stderr, "iron fmt: out of memory reading '%s'\n", path);
        return NULL;
    }
    size_t nread = fread(buf, 1, (size_t)size, f);
    fclose(f);
    buf[nread] = '\0';
    return buf;
}

/* ── iron_fmt: format a .iron file in-place ──────────────────────────────── */

int iron_fmt(const char *source_path) {
    /* 1. Read source file */
    char *source = fmt_read_file(source_path);
    if (!source) return 1;

    /* 2. Set up arena and diagnostics */
    Iron_Arena arena = iron_arena_create(64 * 1024);
    Iron_DiagList diags = iron_diaglist_create();

    /* 3. Lex */
    Iron_Lexer lexer = iron_lexer_create(source, source_path, &arena, &diags);
    Iron_Token *tokens = iron_lex_all(&lexer);

    if (diags.error_count > 0) {
        iron_diag_print_all(&diags, source);
        fprintf(stderr, "iron fmt: refusing to format file with syntax errors\n");
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
        fprintf(stderr, "iron fmt: refusing to format file with syntax errors\n");
        iron_diaglist_free(&diags);
        iron_arena_free(&arena);
        free(source);
        return 1;
    }

    /* 5. Pretty-print the AST to formatted source */
    char *formatted = iron_print_ast(ast, &arena);

    /* 6. Write formatted output to a temporary file */
    /* Construct temp path: <source_path>.iron.tmp */
    size_t path_len = strlen(source_path);
    char *tmp_path = (char *)malloc(path_len + 10); /* ".iron.tmp" + NUL */
    if (!tmp_path) {
        fprintf(stderr, "iron fmt: out of memory\n");
        iron_diaglist_free(&diags);
        iron_arena_free(&arena);
        free(source);
        return 1;
    }
    memcpy(tmp_path, source_path, path_len);
    memcpy(tmp_path + path_len, ".iron.tmp", 10);

    FILE *tmp_f = fopen(tmp_path, "wb");
    if (!tmp_f) {
        fprintf(stderr, "iron fmt: cannot create temp file '%s': %s\n",
                tmp_path, strerror(errno));
        free(tmp_path);
        iron_diaglist_free(&diags);
        iron_arena_free(&arena);
        free(source);
        return 1;
    }

    size_t fmt_len = strlen(formatted);
    size_t written = fwrite(formatted, 1, fmt_len, tmp_f);
    int flush_err = fflush(tmp_f);
    fclose(tmp_f);

    /* 7. Verify temp file was written successfully */
    if (written != fmt_len || flush_err != 0) {
        fprintf(stderr, "iron fmt: failed to write temp file '%s'\n", tmp_path);
        remove(tmp_path);
        free(tmp_path);
        iron_diaglist_free(&diags);
        iron_arena_free(&arena);
        free(source);
        return 1;
    }

    /* 8. Atomically rename temp file to original */
    if (rename(tmp_path, source_path) != 0) {
        fprintf(stderr, "iron fmt: cannot replace '%s': %s\n",
                source_path, strerror(errno));
        remove(tmp_path);
        free(tmp_path);
        iron_diaglist_free(&diags);
        iron_arena_free(&arena);
        free(source);
        return 1;
    }

    /* 9. Clean up */
    free(tmp_path);
    iron_diaglist_free(&diags);
    iron_arena_free(&arena);
    free(source);
    return 0;
}
