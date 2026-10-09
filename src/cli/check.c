#include "cli/check.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "analyzer/analyzer.h"
#include "analyzer/stdlib_prepend.h"
#include "diagnostics/diagnostics.h"
#include "util/arena.h"

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

int iron_check(const char *source_path, bool verbose, bool strict_v3) {
    /* Phase 9 D-11 (Option A): the strict_v3 flag is now threaded into
     * iron_analyze_buffer via the IronAnalysisMode enum. CLI mode keeps
     * the legacy strict-v3 grammar enforcement; CLI_LENIENT honors
     * `ironc check --lenient`. Mapping below; consumed at the
     * iron_analyze_buffer call site near the bottom of this function. */
    IronAnalysisMode analysis_mode = strict_v3 ? IRON_ANALYSIS_MODE_CLI
                                                : IRON_ANALYSIS_MODE_CLI_LENIENT;
    /* The lib/ (installed) or src/ (dev build) directory holding stdlib/ */
    char *base_dir = iron_stdlib_lib_dir();
    if (!base_dir) return 1;

    /* 1. Read source file */
    char *source = check_read_file(source_path);
    if (!source) { free(base_dir); return 1; }

    /* The stdlib prelude, shared with the language server */
    int stdlib_prepended_lines = iron_stdlib_prepend(&source, source_path, base_dir);

    /* 2. Set up arena and diagnostics */
    Iron_Arena arena = iron_arena_create(64 * 1024);
    Iron_DiagList diags = iron_diaglist_create();

    /* 3. Analyze — single call, no bypass paths (HARD-01).
     * Phase 9 D-11: analysis_mode encodes strict_v3 via the
     * IronAnalysisMode enum (CLI for strict, CLI_LENIENT for lenient). */
    Iron_AnalyzeResult result = iron_analyze_buffer(
        source, strlen(source), source_path,
        analysis_mode,
        &arena, &diags,
        NULL,
        stdlib_prepended_lines + 1);

    /* 4. Print all diagnostics */
    iron_diag_print_all(&diags, source);

    /* 5. Verbose: print analysis summary */
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
