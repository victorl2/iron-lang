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
#include <inttypes.h>
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

/* Path resolution uses the compile-time IRON_SOURCE_TREE_ROOT macro so
 * canonical_path strings are CWD-independent. Both the regen path and
 * the regression test path produce byte-identical paths regardless of
 * whether the binary is launched from build/ or build/tests/lsp/unit/.
 */
#ifndef IRON_SOURCE_TREE_ROOT
#  error "IRON_SOURCE_TREE_ROOT must be defined at compile time"
#endif

static bool dir_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static const char *resolve_integration_dir(void) {
    static char buf[1024];
    snprintf(buf, sizeof(buf), "%s/tests/integration", IRON_SOURCE_TREE_ROOT);
    if (dir_exists(buf)) return buf;
    return NULL;
}

static const char *resolve_baseline_file(void) {
    static char buf[1024];
    snprintf(buf, sizeof(buf), "%s/tests/lsp/fixtures/symbol_id_v2_baseline.txt",
             IRON_SOURCE_TREE_ROOT);
    return buf;
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
 * a "canonical\tname_path\tkind" line into out_lines (stb_ds dynamic).
 *
 * The baseline file uses the BASENAME of canonical_path (not the full path)
 * so the byte-identity assertion is portable across checkout locations.
 * Without this, a baseline captured on /home/dev/iron-lang/ would not
 * match a comparison run on /Users/runner/work/iron-lang/iron-lang/.
 * The full path is still passed to ilsp_symbol_id_derive so production
 * symbol-id derivation logic is exercised unchanged. */
static void emit_scope_triples(Iron_Scope        *scope,
                                const char        *canonical_path,
                                Iron_Program      *program,
                                Iron_Arena        *arena,
                                char            ***out_lines) {
    if (!scope) return;
    /* Strip path prefix so the baseline is portable. */
    const char *basename = canonical_path;
    const char *slash = strrchr(canonical_path, '/');
    if (slash) basename = slash + 1;
    for (ptrdiff_t i = 0; i < shlen(scope->symbols); i++) {
        Iron_Symbol *sym = scope->symbols[i].value;
        if (!sym || !sym->decl_node) continue;
        IronLsp_SymbolId id = ilsp_symbol_id_derive(sym, canonical_path,
                                                      program, arena);
        if (!id.canonical_path || !id.name_path) continue;
        size_t n = strlen(basename) + strlen(id.name_path) +
                    strlen(kind_name(id.kind)) + 8;
        char *line = (char *)malloc(n);
        if (!line) continue;
        snprintf(line, n, "%s\t%s\t%s",
                  basename, id.name_path, kind_name(id.kind));
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
                                                    &arena, &diags, NULL,
        0);
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

/* ── Walk every fixture (v2 + v3) ───────────────────────────────────── */

typedef struct {
    char     *canonical;   /* heap */
    char     *name_path;   /* heap */
    uint64_t  hash;
    int       kind;
} TripleRow;

/* Collect (canonical, name_path, hash, kind) tuples from every .iron
 * fixture in the integration dir. include_v3=true to include v3_*. */
static TripleRow *collect_triples(const char *integration_dir,
                                    bool include_v3) {
    TripleRow *rows = NULL;
    DIR *d = opendir(integration_dir);
    if (!d) return rows;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        const char *name = de->d_name;
        if (!name || name[0] == '.') continue;
        size_t n = strlen(name);
        if (n < 5 || strcmp(name + n - 5, ".iron") != 0) continue;
        bool is_v3 = (strncmp(name, "v3_", 3) == 0);
        if (is_v3 && !include_v3) continue;

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
                                                    &arena, &diags, NULL,
        0);
        if (r.global_scope && r.program) {
            for (ptrdiff_t i = 0; i < shlen(r.global_scope->symbols); i++) {
                Iron_Symbol *sym = r.global_scope->symbols[i].value;
                if (!sym || !sym->decl_node) continue;
                IronLsp_SymbolId id = ilsp_symbol_id_derive(sym, full,
                                                              r.program, &arena);
                if (!id.canonical_path || !id.name_path) continue;
                TripleRow row = {0};
                row.canonical = strdup(id.canonical_path);
                row.name_path = strdup(id.name_path);
                row.hash      = id.hash;
                row.kind      = (int)id.kind;
                arrput(rows, row);
            }
        }
        iron_diaglist_free(&diags);
        iron_arena_free(&arena);
        free(src);
    }
    closedir(d);
    return rows;
}

static void free_rows(TripleRow *rows) {
    for (ptrdiff_t i = 0; i < arrlen(rows); i++) {
        free(rows[i].canonical);
        free(rows[i].name_path);
    }
    arrfree(rows);
}

/* ── Test 01: zero churn vs v2 baseline snapshot ─────────────────────── */
static void test_v2_zero_churn(void) {
    const char *idir = resolve_integration_dir();
    TEST_ASSERT_NOT_NULL_MESSAGE(idir, "integration dir not found");

    /* Regenerate v2 lines using the new ilsp_symbol_id_derive. */
    char **lines = NULL;
    collect_v2_triples(idir, &lines);
    TEST_ASSERT_GREATER_THAN_MESSAGE(0, arrlen(lines),
        "expected to find at least one v2 triple");
    qsort(lines, (size_t)arrlen(lines), sizeof(char *), qcmp_str);

    /* Read baseline file. */
    const char *base_path = resolve_baseline_file();
    FILE *f = fopen(base_path, "rb");
    TEST_ASSERT_NOT_NULL_MESSAGE(f, "baseline file missing — re-run with REGEN");
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); TEST_FAIL_MESSAGE("seek"); }
    long sz = ftell(f);
    rewind(f);
    char *blob = (char *)malloc((size_t)sz + 1);
    TEST_ASSERT_NOT_NULL(blob);
    size_t got = fread(blob, 1, (size_t)sz, f);
    fclose(f);
    blob[got] = '\0';

    /* Build current snapshot blob. */
    size_t cur_cap = 0;
    for (ptrdiff_t i = 0; i < arrlen(lines); i++) {
        cur_cap += strlen(lines[i]) + 1;
    }
    char *cur = (char *)malloc(cur_cap + 1);
    TEST_ASSERT_NOT_NULL(cur);
    size_t o = 0;
    for (ptrdiff_t i = 0; i < arrlen(lines); i++) {
        size_t L = strlen(lines[i]);
        memcpy(cur + o, lines[i], L); o += L;
        cur[o++] = '\n';
    }
    cur[o] = '\0';

    if (strcmp(cur, blob) != 0) {
        /* Surface a snippet of the first divergence to aid debugging. */
        size_t i = 0;
        while (i < got && i < o && blob[i] == cur[i]) i++;
        size_t lstart = i;
        while (lstart > 0 && blob[lstart - 1] != '\n') lstart--;
        char snippet[256];
        size_t snip_n = 0;
        while (snip_n < sizeof(snippet) - 1 && lstart < got &&
               blob[lstart] != '\n') {
            snippet[snip_n++] = blob[lstart++];
        }
        snippet[snip_n] = '\0';
        char msg[512];
        snprintf(msg, sizeof(msg),
            "v2 zero-churn breach: divergence at byte %zu: baseline=\"%s\"",
            i, snippet);
        free(blob); free(cur); free_lines(lines);
        TEST_FAIL_MESSAGE(msg);
    }
    free(blob);
    free(cur);
    free_lines(lines);
}

/* ── Test 02: zero hash collisions across v2 + v3 corpus ────────────── */
static void test_v2_v3_zero_collisions(void) {
    const char *idir = resolve_integration_dir();
    TEST_ASSERT_NOT_NULL_MESSAGE(idir, "integration dir not found");

    TripleRow *rows = collect_triples(idir, /*include_v3=*/true);
    TEST_ASSERT_GREATER_THAN_MESSAGE(0, arrlen(rows),
        "expected to find at least one symbol triple");

    /* Build a hash -> first-seen-row index. For every subsequent row
     * with the same hash, both the canonical and name_path strings must
     * be byte-identical (== same canonical decl). Otherwise the hash
     * collides on a structurally distinct symbol — D-05 invariant
     * violation. */
    struct { uint64_t key; int value; } *htab = NULL;
    hmdefault(htab, -1);

    int collisions = 0;
    char *first_collision_msg = NULL;
    for (ptrdiff_t i = 0; i < arrlen(rows); i++) {
        int prev = hmget(htab, rows[i].hash);
        if (prev < 0) {
            hmput(htab, rows[i].hash, (int)i);
            continue;
        }
        if (rows[prev].kind == rows[i].kind &&
            strcmp(rows[prev].canonical, rows[i].canonical) == 0 &&
            strcmp(rows[prev].name_path, rows[i].name_path) == 0) {
            /* Same canonical decl seen twice — fine, just a duplicate
             * symbol-table entry. */
            continue;
        }
        if (collisions == 0) {
            first_collision_msg = (char *)malloc(1024);
            if (first_collision_msg) {
                snprintf(first_collision_msg, 1024,
                    "hash collision: 0x%016" PRIx64 " — A=(%s|%s|%s) B=(%s|%s|%s)",
                    rows[i].hash,
                    rows[prev].canonical, rows[prev].name_path,
                    kind_name((Iron_SymbolKind)rows[prev].kind),
                    rows[i].canonical, rows[i].name_path,
                    kind_name((Iron_SymbolKind)rows[i].kind));
            }
        }
        collisions++;
    }
    hmfree(htab);

    if (collisions != 0 && first_collision_msg) {
        free_rows(rows);
        char *m = first_collision_msg;
        TEST_FAIL_MESSAGE(m);
    }
    if (first_collision_msg) free(first_collision_msg);
    free_rows(rows);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, collisions,
        "v2+v3 corpus must have zero hash collisions on distinct decls");
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
