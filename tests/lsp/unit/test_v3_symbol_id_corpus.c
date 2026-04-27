/* test_v3_symbol_id_corpus -- Phase 9 Plan 01 (Wave 0 Task 1, flipped Task 2).
 *
 * D-04/D-05 corpus regression:
 *
 *   1. test_v2_zero_churn: every Iron_Symbol path string derived from the
 *      v2 portion of tests/integration glob (excluding v3_*) is byte-identical
 *      to the pre-edit baseline at tests/lsp/fixtures/symbol_id_v2_baseline.txt.
 *      Captured BEFORE Task 2 modifies symbol_id.c so the diff is
 *      structurally meaningful (Pitfall 6).
 *
 *   2. test_v2_v3_zero_collisions: walking BOTH v2 and v3 fixtures, no
 *      two distinct (canonical_path, name_path) pairs share an FNV-1a
 *      hash AND no two distinct decls share a (canonical_path, name_path)
 *      pair.
 *
 * The same harness drives a one-shot baseline regen when run with
 * REGEN_SYMBOL_ID_BASELINE=1 in the environment. That mode dumps the
 * sorted (canonical_path \t name_path \t kind) triples to the baseline
 * file and exits successfully without running assertions.
 *
 * Wave 0: both tests TEST_IGNORE'd to register under phase-m2-invariant.
 * Task 2 flips them and adds the production assertions.
 */
#include "unity.h"

#include "lsp/facade/nav/symbol_id.h"

#include "analyzer/analyzer.h"
#include "analyzer/scope.h"
#include "diagnostics/diagnostics.h"
#include "parser/ast.h"
#include "util/arena.h"
#include "vendor/stb_ds.h"

#include <ctype.h>
#include <dirent.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

void setUp(void) {}
void tearDown(void) {}

/* ── Path resolution ─────────────────────────────────────────────────── */

/* Tests are launched from build/ via CTest. Integration fixtures live at
 * ../tests/integration/ and the baseline file at
 * ../tests/lsp/fixtures/symbol_id_v2_baseline.txt. */
static const char *INTEGRATION_DIR = "../tests/integration";
#ifndef IRON_SOURCE_TREE_ROOT
static const char *BASELINE_FILE   = "../tests/lsp/fixtures/symbol_id_v2_baseline.txt";
#endif

static bool dir_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static const char *resolve_integration_dir(void) {
    if (dir_exists(INTEGRATION_DIR)) return INTEGRATION_DIR;
#ifdef IRON_SOURCE_TREE_ROOT
    static char buf[1024];
    snprintf(buf, sizeof(buf), "%s/tests/integration", IRON_SOURCE_TREE_ROOT);
    if (dir_exists(buf)) return buf;
#endif
    return NULL;
}

static const char *resolve_baseline_file(void) {
    /* Always returns the canonical write path; readers tolerate "missing". */
#ifdef IRON_SOURCE_TREE_ROOT
    static char buf[1024];
    snprintf(buf, sizeof(buf), "%s/tests/lsp/fixtures/symbol_id_v2_baseline.txt",
             IRON_SOURCE_TREE_ROOT);
    return buf;
#else
    return BASELINE_FILE;
#endif
}

/* ── Triple emission helpers ─────────────────────────────────────────── */

static const char *kind_name(Iron_SymbolKind k) {
    switch (k) {
        case IRON_SYM_VARIABLE:     return "VARIABLE";
        case IRON_SYM_FUNCTION:     return "FUNCTION";
        case IRON_SYM_METHOD:       return "METHOD";
        case IRON_SYM_TYPE:         return "TYPE";
        case IRON_SYM_ENUM:         return "ENUM";
        case IRON_SYM_ENUM_VARIANT: return "ENUM_VARIANT";
        case IRON_SYM_INTERFACE:    return "INTERFACE";
        case IRON_SYM_PARAM:        return "PARAM";
        case IRON_SYM_FIELD:        return "FIELD";
    }
    return "UNKNOWN";
}

/* Walk every named entry in scope (current scope only -- no parent walk).
 * For each Iron_Symbol with non-NULL decl_node, derive the triple and emit
 * a "canonical\tname_path\tkind" line into out_lines (stb_ds dynamic). */
static void emit_scope_triples(Iron_Scope        *scope,
                                const char        *canonical_path,
                                Iron_Program      *program,
                                Iron_Arena        *arena,
                                char            ***out_lines) {
    if (!scope) return;
    for (ptrdiff_t i = 0; i < shlen(scope->symbols); i++) {
        Iron_Symbol *sym = scope->symbols[i].value;
        if (!sym || !sym->decl_node) continue;
        IronLsp_SymbolId id = ilsp_symbol_id_derive(sym, canonical_path,
                                                      program, arena);
        if (!id.canonical_path || !id.name_path) continue;
        size_t n = strlen(id.canonical_path) + strlen(id.name_path) +
                    strlen(kind_name(id.kind)) + 8;
        char *line = (char *)malloc(n);
        if (!line) continue;
        snprintf(line, n, "%s\t%s\t%s",
                  id.canonical_path, id.name_path, kind_name(id.kind));
        arrput(*out_lines, line);
    }
}

/* qsort comparator over char* */
static int qcmp_str(const void *a, const void *b) {
    const char *sa = *(const char *const *)a;
    const char *sb = *(const char *const *)b;
    return strcmp(sa, sb);
}

/* Walk tests/integration .iron files whose basename does NOT start with
 * v3_, analyze each file, and accumulate triples into out_lines. */
static void collect_v2_triples(const char *integration_dir, char ***out_lines) {
    DIR *d = opendir(integration_dir);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        const char *name = de->d_name;
        if (!name || name[0] == '.') continue;
        size_t n = strlen(name);
        if (n < 5 || strcmp(name + n - 5, ".iron") != 0) continue;
        /* Skip v3_* fixtures. */
        if (strncmp(name, "v3_", 3) == 0) continue;

        char full[1024];
        snprintf(full, sizeof(full), "%s/%s", integration_dir, name);
        FILE *f = fopen(full, "rb");
        if (!f) continue;
        if (fseek(f, 0, SEEK_END) != 0) { fclose(f); continue; }
        long sz = ftell(f);
        rewind(f);
        char *src = (char *)malloc((size_t)sz + 1);
        if (!src) { fclose(f); continue; }
        size_t got = fread(src, 1, (size_t)sz, f);
        fclose(f);
        src[got] = '\0';

        Iron_Arena arena = iron_arena_create(64 * 1024);
        Iron_DiagList diags = iron_diaglist_create();
        Iron_AnalyzeResult r = iron_analyze_buffer(src, got, full,
                                                    IRON_ANALYSIS_MODE_LSP,
                                                    &arena, &diags, NULL);
        if (r.global_scope && r.program) {
            emit_scope_triples(r.global_scope, full, r.program,
                                &arena, out_lines);
        }
        iron_diaglist_free(&diags);
        iron_arena_free(&arena);
        free(src);
    }
    closedir(d);
}

/* Write sorted triples to baseline file. */
static int write_baseline(char **lines, const char *path) {
    /* Sort lexicographically. */
    if (arrlen(lines) > 0) {
        qsort(lines, (size_t)arrlen(lines), sizeof(char *), qcmp_str);
    }
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    for (ptrdiff_t i = 0; i < arrlen(lines); i++) {
        fputs(lines[i], f);
        fputc('\n', f);
    }
    fclose(f);
    return 0;
}

/* Free triples list. */
static void free_lines(char **lines) {
    for (ptrdiff_t i = 0; i < arrlen(lines); i++) {
        free(lines[i]);
    }
    arrfree(lines);
}

/* ── Wave 0 stub tests ───────────────────────────────────────────────── */

static void test_v2_zero_churn(void) {
    TEST_IGNORE_MESSAGE("Phase 9 Plan 01 Task 2 implementation pending");
}

static void test_v2_v3_zero_collisions(void) {
    TEST_IGNORE_MESSAGE("Phase 9 Plan 01 Task 2 implementation pending");
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((unused))
#endif
static void wave0_helpers_alive(void) {
    (void)resolve_integration_dir;
    (void)resolve_baseline_file;
    (void)collect_v2_triples;
    (void)write_baseline;
    (void)free_lines;
    (void)kind_name;
}

int main(void) {
    /* REGEN_SYMBOL_ID_BASELINE=1 — one-shot baseline regen. */
    const char *regen = getenv("REGEN_SYMBOL_ID_BASELINE");
    if (regen && *regen && strcmp(regen, "0") != 0) {
        const char *idir = resolve_integration_dir();
        if (!idir) {
            fprintf(stderr,
                "REGEN: could not locate tests/integration directory\n");
            return 1;
        }
        const char *out = resolve_baseline_file();
        char **lines = NULL;
        collect_v2_triples(idir, &lines);
        if (write_baseline(lines, out) != 0) {
            fprintf(stderr, "REGEN: failed to write %s\n", out);
            free_lines(lines);
            return 1;
        }
        fprintf(stdout,
            "REGEN: wrote %td v2 triples to %s\n", arrlen(lines), out);
        free_lines(lines);
        return 0;
    }

    UNITY_BEGIN();
    RUN_TEST(test_v2_zero_churn);
    RUN_TEST(test_v2_v3_zero_collisions);
    return UNITY_END();
}
