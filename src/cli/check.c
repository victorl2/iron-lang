#include "cli/check.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

/* IRON_SOURCE_DIR is injected by CMake at build time — absolute path to src/ */
#ifndef IRON_SOURCE_DIR
#define IRON_SOURCE_DIR "src"
#endif

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

/* ── Helper: build a path within IRON_SOURCE_DIR ─────────────────────────── */

static char *check_make_src_path(const char *rel) {
    size_t base_len = strlen(IRON_SOURCE_DIR);
    size_t rel_len  = strlen(rel);
    char *out = (char *)malloc(base_len + 1 + rel_len + 1);
    if (!out) return NULL;
    memcpy(out, IRON_SOURCE_DIR, base_len);
    out[base_len] = '/';
    memcpy(out + base_len + 1, rel, rel_len + 1);
    return out;
}

/* ── Helper: read a file with size output ────────────────────────────────── */

static char *check_read_stdlib(const char *path, long *out_size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long size = ftell(f);
    rewind(f);
    char *buf = (char *)malloc((size_t)(size + 1));
    if (!buf) { fclose(f); return NULL; }
    size_t nread = fread(buf, 1, (size_t)size, f);
    fclose(f);
    buf[nread] = '\0';
    if (out_size) *out_size = (long)nread;
    return buf;
}

/* ── Check: lex + parse + analyze, no codegen ────────────────────────────── */

int iron_check(const char *source_path, bool verbose) {
    /* 1. Read source file */
    char *source = check_read_file(source_path);
    if (!source) return 1;

    /* Detect stdlib imports and prepend .iron wrappers (same as build.c) */
    if (strstr(source, "import raylib") != NULL) {
        char *rl_path = check_make_src_path("stdlib/raylib.iron");
        if (rl_path) {
            long rl_size = 0;
            char *rl_src = check_read_stdlib(rl_path, &rl_size);
            free(rl_path);
            if (rl_src) {
                size_t combined_len = (size_t)rl_size + 1 + strlen(source) + 1;
                char *combined = (char *)malloc(combined_len);
                if (combined) {
                    memcpy(combined, rl_src, (size_t)rl_size);
                    combined[rl_size] = '\n';
                    strcpy(combined + rl_size + 1, source);
                    free(source);
                    source = combined;
                }
                free(rl_src);
            }
        }
    }

    if (strstr(source, "import math") != NULL) {
        char *path = check_make_src_path("stdlib/math.iron");
        if (path) {
            long sz = 0;
            char *src = check_read_stdlib(path, &sz);
            free(path);
            if (src) {
                size_t combined_len = (size_t)sz + 1 + strlen(source) + 1;
                char *combined = (char *)malloc(combined_len);
                if (combined) {
                    memcpy(combined, src, (size_t)sz);
                    combined[sz] = '\n';
                    strcpy(combined + sz + 1, source);
                    free(source);
                    source = combined;
                }
                free(src);
            }
        }
    }

    if (strstr(source, "import io") != NULL) {
        char *path = check_make_src_path("stdlib/io.iron");
        if (path) {
            long sz = 0;
            char *src = check_read_stdlib(path, &sz);
            free(path);
            if (src) {
                size_t combined_len = (size_t)sz + 1 + strlen(source) + 1;
                char *combined = (char *)malloc(combined_len);
                if (combined) {
                    memcpy(combined, src, (size_t)sz);
                    combined[sz] = '\n';
                    strcpy(combined + sz + 1, source);
                    free(source);
                    source = combined;
                }
                free(src);
            }
        }
    }

    if (strstr(source, "import time") != NULL) {
        char *path = check_make_src_path("stdlib/time.iron");
        if (path) {
            long sz = 0;
            char *src = check_read_stdlib(path, &sz);
            free(path);
            if (src) {
                size_t combined_len = (size_t)sz + 1 + strlen(source) + 1;
                char *combined = (char *)malloc(combined_len);
                if (combined) {
                    memcpy(combined, src, (size_t)sz);
                    combined[sz] = '\n';
                    strcpy(combined + sz + 1, source);
                    free(source);
                    source = combined;
                }
                free(src);
            }
        }
    }

    if (strstr(source, "import log") != NULL) {
        char *path = check_make_src_path("stdlib/log.iron");
        if (path) {
            long sz = 0;
            char *src = check_read_stdlib(path, &sz);
            free(path);
            if (src) {
                size_t combined_len = (size_t)sz + 1 + strlen(source) + 1;
                char *combined = (char *)malloc(combined_len);
                if (combined) {
                    memcpy(combined, src, (size_t)sz);
                    combined[sz] = '\n';
                    strcpy(combined + sz + 1, source);
                    free(source);
                    source = combined;
                }
                free(src);
            }
        }
    }

    /* 2. Set up arena and diagnostics */
    Iron_Arena arena = iron_arena_create(64 * 1024);
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
                                             &diags,
                                             NULL  /* source_file_dir */,
                                             NULL  /* source_text */,
                                             0     /* source_len */,
                                             false /* force_comptime */);

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
