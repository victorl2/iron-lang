#include "cli/build.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <spawn.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <libgen.h>

/* IRON_SOURCE_DIR is injected by CMake at build time — absolute path to src/ */
#ifndef IRON_SOURCE_DIR
#define IRON_SOURCE_DIR "src"
#endif

#include "lexer/lexer.h"
#include "parser/parser.h"
#include "parser/ast.h"
#include "analyzer/analyzer.h"
#include "codegen/codegen.h"
#include "diagnostics/diagnostics.h"
#include "util/arena.h"
#include "vendor/stb_ds.h"

extern char **environ;

/* ── Helper: read a file into a heap-allocated string ────────────────────── */

static char *read_file(const char *path, long *out_size) {
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
    if (out_size) *out_size = (long)read;
    return buf;
}

/* ── Helper: derive output binary name from source path ──────────────────── */

static char *derive_output_name(const char *source_path) {
    /* Make a mutable copy for basename/dirname calls */
    char *path_copy = strdup(source_path);
    if (!path_copy) return NULL;

    char *base = basename(path_copy);

    /* Strip .iron extension if present */
    size_t len = strlen(base);
    const char *ext = ".iron";
    size_t ext_len = strlen(ext);
    char *out;
    if (len > ext_len && strcmp(base + len - ext_len, ext) == 0) {
        out = (char *)malloc(len - ext_len + 1);
        if (out) {
            memcpy(out, base, len - ext_len);
            out[len - ext_len] = '\0';
        }
    } else {
        out = strdup(base);
    }
    free(path_copy);
    return out;
}

/* ── Helper: write C source to a temp file ───────────────────────────────── */

static char *write_temp_c(const char *c_src, bool debug_build,
                           const char *binary_name) {
    char *path = NULL;
    int fd = -1;

    if (debug_build) {
        /* Use .iron-build/ directory */
        if (mkdir(".iron-build", 0755) != 0 && errno != EEXIST) {
            fprintf(stderr, "error: cannot create .iron-build/: %s\n",
                    strerror(errno));
            return NULL;
        }
        size_t plen = strlen(binary_name) + 32;
        path = (char *)malloc(plen);
        if (!path) return NULL;
        snprintf(path, plen, ".iron-build/%s.c", binary_name);
        fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    } else {
        /* Use /tmp with mkstemp */
        path = strdup("/tmp/iron_XXXXXX.c");
        if (!path) return NULL;
        /* mkstemps for suffix support */
        fd = mkstemps(path, 2); /* 2 = length of ".c" suffix */
    }

    if (fd < 0) {
        fprintf(stderr, "error: cannot create temp file: %s\n",
                strerror(errno));
        free(path);
        return NULL;
    }

    size_t total = strlen(c_src);
    size_t written = 0;
    while (written < total) {
        ssize_t n = write(fd, c_src + written, total - written);
        if (n < 0) {
            fprintf(stderr, "error: write failed: %s\n", strerror(errno));
            close(fd);
            free(path);
            return NULL;
        }
        written += (size_t)n;
    }
    close(fd);
    return path;
}

/* ── Helper: build a path within IRON_SOURCE_DIR ─────────────────────────── */

static char *make_src_path(const char *rel) {
    size_t base_len = strlen(IRON_SOURCE_DIR);
    size_t rel_len  = strlen(rel);
    char *out = (char *)malloc(base_len + 1 + rel_len + 1);
    if (!out) return NULL;
    memcpy(out, IRON_SOURCE_DIR, base_len);
    out[base_len] = '/';
    memcpy(out + base_len + 1, rel, rel_len + 1);
    return out;
}

/* ── Helper: invoke clang to compile generated C with runtime sources ──────── */

static int invoke_clang(const char *c_file, const char *output,
                         const char *src_dir) {
    (void)src_dir;

    /* Build -I flag for headers */
    size_t src_i_len = strlen("-I") + strlen(IRON_SOURCE_DIR) + 1;
    char *src_i_flag = (char *)malloc(src_i_len);
    if (!src_i_flag) return 1;
    snprintf(src_i_flag, src_i_len, "-I%s", IRON_SOURCE_DIR);

    /* Also add vendor dir for stb_ds.h */
    size_t vendor_i_len = strlen("-I") + strlen(IRON_SOURCE_DIR) + strlen("/vendor") + 1;
    char *vendor_i_flag = (char *)malloc(vendor_i_len);
    if (!vendor_i_flag) { free(src_i_flag); return 1; }
    snprintf(vendor_i_flag, vendor_i_len, "-I%s/vendor", IRON_SOURCE_DIR);

    /* Runtime and stdlib source files to compile alongside generated C.
     * These are compiled together in one clang invocation to produce a single
     * self-contained binary without needing pre-built libraries. */
    char *rt_stb     = make_src_path("util/stb_ds_impl.c");
    char *rt_arena   = make_src_path("util/arena.c");
    char *rt_strbuf  = make_src_path("util/strbuf.c");
    char *rt_string  = make_src_path("runtime/iron_string.c");
    char *rt_rc      = make_src_path("runtime/iron_rc.c");
    char *rt_builtin = make_src_path("runtime/iron_builtins.c");
    char *rt_threads = make_src_path("runtime/iron_threads.c");
    char *rt_collect = make_src_path("runtime/iron_collections.c");
    char *sl_math    = make_src_path("stdlib/iron_math.c");
    char *sl_io      = make_src_path("stdlib/iron_io.c");
    char *sl_time    = make_src_path("stdlib/iron_time.c");
    char *sl_log     = make_src_path("stdlib/iron_log.c");

    if (!rt_stb || !rt_arena || !rt_strbuf || !rt_string || !rt_rc ||
        !rt_builtin || !rt_threads || !rt_collect ||
        !sl_math || !sl_io || !sl_time || !sl_log) {
        free(src_i_flag); free(vendor_i_flag);
        free(rt_stb); free(rt_arena); free(rt_strbuf);
        free(rt_string); free(rt_rc); free(rt_builtin);
        free(rt_threads); free(rt_collect);
        free(sl_math); free(sl_io); free(sl_time); free(sl_log);
        return 1;
    }

    const char *argv[] = {
        "clang",
        "-std=c11",
        "-O2",
        "-o", output,
        c_file,
        rt_stb,
        rt_arena,
        rt_strbuf,
        rt_string,
        rt_rc,
        rt_builtin,
        rt_threads,
        rt_collect,
        sl_math,
        sl_io,
        sl_time,
        sl_log,
        src_i_flag,
        vendor_i_flag,
        "-lm",
        "-lpthread",
        NULL
    };

    pid_t pid;
    int status = posix_spawnp(&pid, "clang", NULL, NULL,
                               (char *const *)argv, environ);

    free(src_i_flag); free(vendor_i_flag);
    free(rt_stb); free(rt_arena); free(rt_strbuf);
    free(rt_string); free(rt_rc); free(rt_builtin);
    free(rt_threads); free(rt_collect);
    free(sl_math); free(sl_io); free(sl_time); free(sl_log);

    if (status != 0) {
        fprintf(stderr, "error: failed to spawn clang: %s\n",
                strerror(status));
        return 1;
    }
    if (waitpid(pid, &status, 0) < 0) {
        fprintf(stderr, "error: waitpid failed: %s\n", strerror(errno));
        return 1;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        fprintf(stderr, "error: clang exited with code %d\n", code);
        return 1;
    }
    return 0;
}

/* ── Main build function ─────────────────────────────────────────────────── */

int iron_build(const char *source_path, const char *output_path,
               IronBuildOpts opts) {
    /* 1. Read source file */
    long src_size = 0;
    char *source = read_file(source_path, &src_size);
    if (!source) return 1;
    (void)src_size;

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
    Iron_AnalyzeResult analysis = iron_analyze((Iron_Program *)ast, &arena,
                                               &diags);

    if (diags.error_count > 0 || analysis.has_errors) {
        iron_diag_print_all(&diags, source);
        iron_diaglist_free(&diags);
        iron_arena_free(&arena);
        free(source);
        return 1;
    }

    /* 6. Codegen */
    const char *c_src = iron_codegen((Iron_Program *)ast,
                                     analysis.global_scope,
                                     &arena, &diags);

    if (!c_src || diags.error_count > 0) {
        iron_diag_print_all(&diags, source);
        iron_diaglist_free(&diags);
        iron_arena_free(&arena);
        free(source);
        return 1;
    }

    /* 7. Verbose: print generated C */
    if (opts.verbose) {
        fprintf(stdout, "=== Generated C ===\n%s\n=== End Generated C ===\n",
                c_src);
    }

    /* 8. Determine output binary name */
    char *derived_output = NULL;
    const char *binary_name;
    if (output_path) {
        binary_name = output_path;
    } else {
        derived_output = derive_output_name(source_path);
        if (!derived_output) {
            iron_diaglist_free(&diags);
            iron_arena_free(&arena);
            free(source);
            return 1;
        }
        binary_name = derived_output;
    }

    /* 9. Write generated C to temp file */
    char *c_file_path = write_temp_c(c_src, opts.debug_build, binary_name);
    if (!c_file_path) {
        free(derived_output);
        iron_diaglist_free(&diags);
        iron_arena_free(&arena);
        free(source);
        return 1;
    }

    /* 10. Invoke clang */
    int ret = invoke_clang(c_file_path, binary_name, "src");

    /* 11. Clean up temp file unless --debug-build */
    if (!opts.debug_build) {
        unlink(c_file_path);
    }
    free(c_file_path);

    /* 12. Clean up compiler resources */
    iron_diaglist_free(&diags);
    iron_arena_free(&arena);
    free(source);

    if (ret != 0) {
        free(derived_output);
        return 1;
    }

    /* 13. Run if requested (iron run) */
    if (opts.run_after) {
        /* If binary_name has no '/', it's in the current directory.
         * posix_spawnp searches PATH, which won't find ./binary.
         * Prefix with "./" so the shell-like search works correctly. */
        char *exec_path = NULL;
        bool needs_free = false;
        if (strchr(binary_name, '/') == NULL) {
            size_t plen = strlen("./") + strlen(binary_name) + 1;
            exec_path = (char *)malloc(plen);
            if (exec_path) {
                snprintf(exec_path, plen, "./%s", binary_name);
                needs_free = true;
            } else {
                exec_path = (char *)binary_name;
            }
        } else {
            exec_path = (char *)binary_name;
        }

        /* Build argv */
        int total_args = 1 + opts.run_arg_count;
        char **run_argv = (char **)malloc(
            (size_t)(total_args + 1) * sizeof(char *));
        if (!run_argv) {
            if (needs_free) free(exec_path);
            free(derived_output);
            return 1;
        }
        run_argv[0] = exec_path;
        for (int i = 0; i < opts.run_arg_count; i++) {
            run_argv[i + 1] = (char *)opts.run_args[i];
        }
        run_argv[total_args] = NULL;

        /* Use posix_spawn (not posix_spawnp) so path is used directly */
        pid_t pid;
        int spawn_status = posix_spawn(&pid, exec_path, NULL, NULL,
                                        run_argv, environ);
        free(run_argv);
        if (needs_free) free(exec_path);
        if (spawn_status != 0) {
            fprintf(stderr, "error: failed to run '%s': %s\n",
                    binary_name, strerror(spawn_status));
            free(derived_output);
            return 1;
        }
        int wstatus;
        if (waitpid(pid, &wstatus, 0) < 0) {
            fprintf(stderr, "error: waitpid failed: %s\n", strerror(errno));
            free(derived_output);
            return 1;
        }
        free(derived_output);
        return WIFEXITED(wstatus) ? WEXITSTATUS(wstatus) : 1;
    }

    /* 14. Success message */
    fprintf(stderr, "Built: %s\n", binary_name);
    free(derived_output);
    return 0;
}
