/* Phase 5 Plan 05-01 (D-01, D-02, D-15, FMT-01, FMT-05):
 *   - iron_fmt now delegates lex+parse+print to iron_format_source.
 *   - iron_fmt + iron_fmt_check both load [fmt] from iron.toml via
 *     fmt_find_iron_toml (walk-up, capped at 20 directories per
 *     RESEARCH T-05-01-03 DoS mitigation).
 *   - iron_fmt_check implements the D-15 exit-code contract
 *     (0 clean / 1 dirty / 2 syntax error / >=3 I/O error). */

#include "cli/fmt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>   /* access(), F_OK */
#include <libgen.h>   /* dirname() */

#include "lexer/lexer.h"
#include "parser/parser.h"
#include "parser/printer.h"
#include "parser/ast.h"
#include "diagnostics/diagnostics.h"
#include "util/arena.h"
#include "vendor/stb_ds.h"

/* Phase 5 Plan 05-01 includes */
#include "fmt/format.h"
#include "fmt/options.h"
#include "fmt/config_load.h"
#include "cli/toml.h"

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

/* ── Helper: walk up from source_path to find iron.toml ──────────────────── */
/* Phase 5 Plan 05-01 (D-02, D-15, T-05-01-03):
 * Search source_path's directory and ancestors for iron.toml. Returns a
 * malloc-owned absolute-or-relative path (caller frees) or NULL if none
 * is found within FMT_TOML_WALK_UP_MAX parent levels. The cap defends
 * against infinite-symlink-loop DoS per RESEARCH §V threat T-05-01-03. */
#define FMT_TOML_WALK_UP_MAX 20

static char *fmt_find_iron_toml(const char *source_path) {
    if (!source_path) return NULL;

    /* Start from the directory containing source_path. dirname mutates
     * its argument on glibc, so operate on a strdup'd copy. */
    char *dir_buf = strdup(source_path);
    if (!dir_buf) return NULL;
    char *dir = dirname(dir_buf);
    if (!dir) {
        free(dir_buf);
        return NULL;
    }

    char *current = strdup(dir);
    free(dir_buf);
    if (!current) return NULL;

    for (int i = 0; i < FMT_TOML_WALK_UP_MAX; i++) {
        size_t cur_len = strlen(current);
        /* candidate = "<current>/iron.toml" */
        size_t cand_len = cur_len + 1 /* '/' */ + 9 /* "iron.toml" */ + 1 /* NUL */;
        char  *candidate = (char *)malloc(cand_len);
        if (!candidate) {
            free(current);
            return NULL;
        }
        snprintf(candidate, cand_len, "%s/iron.toml", current);
        if (access(candidate, F_OK) == 0) {
            free(current);
            return candidate;   /* caller frees */
        }
        free(candidate);

        /* Walk up: take dirname of current. */
        char *parent_buf = strdup(current);
        if (!parent_buf) {
            free(current);
            return NULL;
        }
        char *parent = dirname(parent_buf);
        if (!parent) {
            free(parent_buf);
            free(current);
            return NULL;
        }
        /* Stop at filesystem root (parent == current). */
        if (strcmp(parent, current) == 0) {
            free(parent_buf);
            free(current);
            return NULL;
        }
        char *next = strdup(parent);
        free(parent_buf);
        free(current);
        if (!next) return NULL;
        current = next;
    }

    free(current);
    return NULL;   /* depth cap hit -- DoS guard */
}

/* ── iron_fmt: format a .iron file in-place ──────────────────────────────── */

int iron_fmt(const char *source_path) {
    /* 1. Read source file */
    char *source = fmt_read_file(source_path);
    if (!source) return 1;

    /* 2. Set up arena and diagnostics */
    Iron_Arena    arena = iron_arena_create(64 * 1024);
    Iron_DiagList diags = iron_diaglist_create();

    /* 3. Load [fmt] from iron.toml if reachable from source_path. */
    IronFmtOptions opts          = iron_fmt_options_default();
    char          *iron_toml_path = fmt_find_iron_toml(source_path);
    IronProject   *proj           = NULL;
    if (iron_toml_path) {
        proj = iron_toml_parse(iron_toml_path);
        if (proj) opts = iron_fmt_options_from_toml(proj);
        free(iron_toml_path);
    }

    /* 4. Format via the single library entry point (D-01). */
    IronFmtResult r = iron_format_source(source, source_path, &opts, &arena, &diags);
    if (!r.ok) {
        iron_diag_print_all(&diags, source);
        fprintf(stderr, "iron fmt: refusing to format file with syntax errors\n");
        iron_diaglist_free(&diags);
        iron_arena_free(&arena);
        free(source);
        if (proj) iron_toml_free(proj);
        return 1;
    }

    /* 5. Write formatted output to a temporary file */
    /* Construct temp path: <source_path>.iron.tmp */
    size_t path_len = strlen(source_path);
    char *tmp_path = (char *)malloc(path_len + 10); /* ".iron.tmp" + NUL */
    if (!tmp_path) {
        fprintf(stderr, "iron fmt: out of memory\n");
        iron_diaglist_free(&diags);
        iron_arena_free(&arena);
        free(source);
        if (proj) iron_toml_free(proj);
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
        if (proj) iron_toml_free(proj);
        return 1;
    }

    size_t written   = fwrite(r.formatted, 1, r.formatted_len, tmp_f);
    int    flush_err = fflush(tmp_f);
    fclose(tmp_f);

    /* 6. Verify temp file was written successfully */
    if (written != r.formatted_len || flush_err != 0) {
        fprintf(stderr, "iron fmt: failed to write temp file '%s'\n", tmp_path);
        remove(tmp_path);
        free(tmp_path);
        iron_diaglist_free(&diags);
        iron_arena_free(&arena);
        free(source);
        if (proj) iron_toml_free(proj);
        return 1;
    }

    /* 7. Atomically rename temp file to original */
    if (rename(tmp_path, source_path) != 0) {
        fprintf(stderr, "iron fmt: cannot replace '%s': %s\n",
                source_path, strerror(errno));
        remove(tmp_path);
        free(tmp_path);
        iron_diaglist_free(&diags);
        iron_arena_free(&arena);
        free(source);
        if (proj) iron_toml_free(proj);
        return 1;
    }

    /* 8. Clean up */
    free(tmp_path);
    iron_diaglist_free(&diags);
    iron_arena_free(&arena);
    free(source);
    if (proj) iron_toml_free(proj);
    return 0;
}

/* ── iron_fmt_check: D-15 non-mutating check mode ─────────────────────────── */
/* Exit codes (D-15):
 *   0 -- file is already iron-fmt-clean
 *   1 -- file would be reformatted (diff-visible); "would reformat <path>"
 *        emitted to stderr in gofmt -l style
 *   2 -- file has lexer/parser errors (refused, diags printed)
 *   3 -- I/O error (file not found, not readable, etc.) */
int iron_fmt_check(const char *source_path) {
    char *source = fmt_read_file(source_path);
    if (!source) return 3;    /* I/O error */

    Iron_Arena    arena = iron_arena_create(64 * 1024);
    Iron_DiagList diags = iron_diaglist_create();

    IronFmtOptions opts          = iron_fmt_options_default();
    char          *iron_toml_path = fmt_find_iron_toml(source_path);
    IronProject   *proj           = NULL;
    if (iron_toml_path) {
        proj = iron_toml_parse(iron_toml_path);
        if (proj) opts = iron_fmt_options_from_toml(proj);
        free(iron_toml_path);
    }

    IronFmtResult r = iron_format_source(source, source_path, &opts, &arena, &diags);

    int    rc;
    size_t source_len = strlen(source);
    if (!r.ok) {
        iron_diag_print_all(&diags, source);
        rc = 2;     /* syntax error */
    } else if (source_len == r.formatted_len &&
               memcmp(source, r.formatted, r.formatted_len) == 0) {
        rc = 0;     /* clean */
    } else {
        fprintf(stderr, "would reformat %s\n", source_path);
        rc = 1;     /* dirty */
    }

    iron_diaglist_free(&diags);
    iron_arena_free(&arena);
    free(source);
    if (proj) iron_toml_free(proj);
    return rc;
}
