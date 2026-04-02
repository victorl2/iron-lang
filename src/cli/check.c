#include "cli/check.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#ifdef _WIN32
  #include <windows.h>
#else
  #include <unistd.h>
#endif
#ifdef __APPLE__
  #include <mach-o/dyld.h>
#endif

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
#include "cli/iron_import_detect.h"

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

/* ── Runtime path resolution ─────────────────────────────────────────────── */

/* resolve_self_dir: fills buf with the directory containing this binary.
   Returns 0 on success, -1 on error. */
static int resolve_self_dir(char *buf, size_t buf_size) {
#ifdef __APPLE__
    uint32_t size = (uint32_t)buf_size;
    if (_NSGetExecutablePath(buf, &size) != 0) return -1;
#elif defined(__linux__)
    ssize_t n = readlink("/proc/self/exe", buf, buf_size - 1);
    if (n < 0) return -1;
    buf[n] = '\0';
#elif defined(_WIN32)
    DWORD n = GetModuleFileNameA(NULL, buf, (DWORD)buf_size);
    if (n == 0 || n >= (DWORD)buf_size) return -1;
#else
    return -1;
#endif
    /* Truncate at last path separator to get directory */
    char *last = strrchr(buf, '/');
#ifdef _WIN32
    char *last_win = strrchr(buf, '\\');
    if (last_win > last) last = last_win;
#endif
    if (last) *last = '\0';
    return 0;
}

/* get_iron_lib_dir: returns malloc'd path to the lib/ or src/ base directory.
   For installed builds: resolves to sibling ../lib/ of the binary directory.
   For dev builds: falls back to IRON_SOURCE_DIR compile-time define.
   Caller must free(). */
static char *get_iron_lib_dir(void) {
    char self_dir[4096];
    if (resolve_self_dir(self_dir, sizeof(self_dir)) == 0) {
        /* Try sibling ../lib/ directory (installed layout) */
        size_t dlen = strlen(self_dir);
        /* Truncate to parent dir (go up from bin/) */
        char *parent_slash = strrchr(self_dir, '/');
#ifdef _WIN32
        char *parent_slash_win = strrchr(self_dir, '\\');
        if (parent_slash_win > parent_slash) parent_slash = parent_slash_win;
#endif
        if (parent_slash) {
            *parent_slash = '\0';
            dlen = strlen(self_dir);
        }
        size_t lib_len = dlen + strlen("/lib") + 1;
        char *lib_path = (char *)malloc(lib_len);
        if (lib_path) {
            snprintf(lib_path, lib_len, "%s/lib", self_dir);
            struct stat st;
            if (stat(lib_path, &st) == 0 && S_ISDIR(st.st_mode)) {
                return lib_path;
            }
            free(lib_path);
        }
    }
    /* Fallback: compile-time IRON_SOURCE_DIR (dev builds) */
    return strdup(IRON_SOURCE_DIR);
}

/* ── Helper: build a path from a base directory and relative path ─────────── */

static char *check_make_path(const char *base, const char *rel) {
    size_t base_len = strlen(base);
    size_t rel_len  = strlen(rel);
    char *out = (char *)malloc(base_len + 1 + rel_len + 1);
    if (!out) return NULL;
    memcpy(out, base, base_len);
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
    /* Resolve runtime lib/src base directory once for this check */
    char *base_dir = get_iron_lib_dir();
    if (!base_dir) return 1;

    /* 1. Read source file */
    char *source = check_read_file(source_path);
    if (!source) { free(base_dir); return 1; }

    /* Detect stdlib imports and prepend .iron wrappers (same as build.c).
     * Use a temporary arena for the token-level import detection. */
    Iron_Arena detect_arena = iron_arena_create(32 * 1024);

    if (iron_detect_import(source, source_path, "raylib", &detect_arena)) {
        char *rl_path = check_make_path(base_dir, "stdlib/raylib.iron");
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

    if (iron_detect_import(source, source_path, "math", &detect_arena)) {
        char *path = check_make_path(base_dir, "stdlib/math.iron");
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

    if (iron_detect_import(source, source_path, "io", &detect_arena)) {
        char *path = check_make_path(base_dir, "stdlib/io.iron");
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

    if (iron_detect_import(source, source_path, "time", &detect_arena)) {
        char *path = check_make_path(base_dir, "stdlib/time.iron");
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

    if (iron_detect_import(source, source_path, "log", &detect_arena)) {
        char *path = check_make_path(base_dir, "stdlib/log.iron");
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

    iron_arena_free(&detect_arena);

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
        free(base_dir);
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
        free(base_dir);
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
    free(base_dir);
    return exit_code;
}
