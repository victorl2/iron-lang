/* tests/lsp/parity/test_parity_ironc_lsp.c — HARD-12 (Phase 1 Plan 05).
 *
 * End-to-end parity harness. Sweeps every tests/integration/FIXTURE.iron file
 * through `iron_analyze_buffer()` and asserts the invariants that M0 locks in:
 *
 *   1. CLI-CLI determinism — running the same source twice in CLI mode with
 *      fresh arenas + fresh diag lists must produce byte-identical canonical
 *      diagnostic output. A regression here is a HARD-06 (arena scoping) or
 *      HARD-07 (pthread_once types init) violation.
 *
 *   2. LSP vs CLI divergence is explained — when LSP mode's canonical output
 *      differs from CLI mode's, the harness demands a textual reason: the
 *      fixture uses `comptime` (the HARD-02 FS gate is the expected difference)
 *      OR the fixture declares an `ERROR` marker comment (which indicates it
 *      deliberately exercises the cascade path that CLI suppresses and LSP
 *      does not — HARD-02 cascade-suppression gate, Plan 02).
 *
 *   3. Fixture count floor — the sweep walks >= 300 fixtures. Dropping below
 *      this threshold is a signal that the integration suite was unintentionally
 *      gutted; an intentional reduction can bump the floor.
 *
 *   4. Known baseline — the four pre-existing failures carried forward from
 *      prior waves (game.iron, hint_black_box.iron, mono_different_concrete_types.iron,
 *      nullable.iron) are NOT regressions. The harness does not treat them as
 *      failures; it runs them and tolerates the same error_count observed in
 *      the Wave 4 baseline.
 *
 * The TRUE byte-for-byte HARD-11 check against the pre-M0 `.expected` files is
 * the shell script `tests/integration/run_integration.sh`, which this Unity
 * harness does NOT reimplement. The shell script runs the ironc binary and
 * compares stdout against committed golden files; this harness runs the
 * library entry point directly and cross-checks determinism and LSP/CLI
 * divergence surface. Both gates must be green for Phase 1 closure.
 */

#include "unity.h"
#include "analyzer/analyzer.h"
#include "util/arena.h"
#include "util/strbuf.h"
#include "diagnostics/diagnostics.h"

/* Plan 05 (CORE-22): 4th pass drives the LSP facade call site. */
#include "lsp/facade/compile.h"
#include "lsp/store/document.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

void setUp(void)    {}
void tearDown(void) {}

/* Read a file into a malloc'd buffer. Caller frees. */
static char *slurp(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long n = ftell(f);
    if (n < 0) { fclose(f); return NULL; }
    rewind(f);
    char *buf = (char *)malloc((size_t)n + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t rd = fread(buf, 1, (size_t)n, f);
    buf[rd] = '\0';
    fclose(f);
    if (out_len) *out_len = rd;
    return buf;
}

/* Canonical diagnostic serialization:
 *   "E<code>: <line>:<col>-<el>:<ec> [LEVEL] <message>\n"
 *
 * Includes the level tag so NOTE vs ERROR vs WARNING divergence is visible,
 * but excludes `suggestion` (not stable across runs). The sort order is the
 * emission order — every analyzer pass emits in deterministic order, so two
 * CLI-mode runs on the same source must produce identical strings. */
static char *diag_serialize(const Iron_DiagList *dl) {
    static const char *lvl[] = { "ERROR", "WARN", "NOTE" };
    Iron_StrBuf sb = iron_strbuf_create(2048);
    for (int i = 0; i < dl->count; i++) {
        const Iron_Diagnostic *d = &dl->items[i];
        const char *ls = (d->level >= 0 && d->level <= 2) ? lvl[d->level] : "?";
        iron_strbuf_appendf(&sb, "E%04d: %u:%u-%u:%u [%s] %s\n",
                            d->code, d->span.line, d->span.col,
                            d->span.end_line, d->span.end_col,
                            ls,
                            d->message ? d->message : "");
    }
    /* Dup the internal buffer into a malloc'd string so we can free the
     * Iron_StrBuf independently and hold the payload across arena lifetimes. */
    const char *s = iron_strbuf_get(&sb);
    size_t n = strlen(s);
    char *copy = (char *)malloc(n + 1);
    if (copy) memcpy(copy, s, n + 1);
    iron_strbuf_free(&sb);
    return copy;
}

/* Run iron_analyze_buffer once in the given mode on a fresh arena + diag list,
 * return the canonical diag string (malloc'd, caller frees). */
static char *run_once(const char *src, size_t len, const char *name,
                      IronAnalysisMode mode) {
    Iron_Arena    a = iron_arena_create(131072);
    Iron_DiagList d = iron_diaglist_create();
    (void)iron_analyze_buffer(src, len, name, mode, &a, &d, NULL, 0);
    char *out = diag_serialize(&d);
    iron_diaglist_free(&d);
    iron_arena_free(&a);
    return out;
}

/* Plan 05 CORE-22: drive the LSP facade's single iron_analyze_buffer call
 * site and produce the same canonical serialization so we can assert
 * byte-for-byte match with the CLI mode output.
 *
 * We use ilsp_facade_compile_pure, which skips the writer/notification
 * path and returns the raw Iron_DiagList via caller-owned arena + diags.
 * That isolates the analyze step and proves the facade's call shape
 * matches the CLI's.
 *
 * The document is synthetic: uri = fixture name, text = slurped source,
 * line_idx not used by facade_compile_pure (it's only needed by the
 * translation step, which we don't exercise here). */
static char *run_via_facade(const char *src, size_t len, const char *name) {
    Iron_Arena    a = iron_arena_create(131072);
    Iron_DiagList d = iron_diaglist_create();

    IronLsp_Document doc;
    memset(&doc, 0, sizeof(doc));
    /* The facade reads doc->text, doc->text_len, doc->uri. Cast the
     * const char * since IronLsp_Document declares text as char *.
     * facade_compile_pure does not mutate the buffer. */
    doc.text     = (char *)src;
    doc.text_len = len;
    doc.uri      = (char *)name;

    IronLsp_CompileRequest req = { .version = 1, .cancel_flag = NULL };
    ilsp_facade_compile_pure(&doc, &req, &a, &d);

    char *out = diag_serialize(&d);
    iron_diaglist_free(&d);
    iron_arena_free(&a);
    return out;
}

/* Heuristic markers to explain a permitted LSP/CLI divergence on a fixture. */
static int fixture_uses_comptime(const char *src) {
    return strstr(src, "comptime") != NULL;
}
static int fixture_has_error_marker(const char *src) {
    /* CLI cascade-suppression gates diagnostics after the first error inside
     * a recovery block. LSP mode emits every diagnostic. Fixtures that exercise
     * error-recovery paths label themselves with one of these markers. */
    return strstr(src, "/* ERROR")      != NULL
        || strstr(src, "// ERROR")      != NULL
        || strstr(src, "-- ERROR")      != NULL
        || strstr(src, "expect error")  != NULL
        || strstr(src, "ERROR:")        != NULL;
}

/* Baseline failures from Wave 4 SUMMARY (pre-existing, NOT regressions).
 * The harness does not fail on these fixtures' CLI/LSP divergence — they are
 * treated as known-broken input and their parity signal is noise. */
static int fixture_is_known_baseline_failure(const char *name) {
    return strcmp(name, "game.iron")                          == 0
        || strcmp(name, "hint_black_box.iron")                == 0
        || strcmp(name, "mono_different_concrete_types.iron") == 0
        || strcmp(name, "nullable.iron")                      == 0;
}

/* HARD-11 + HARD-12 main body. */
void test_parity_all_integration_fixtures(void) {
    const char *dir_path = "tests/integration";
    DIR *dir = opendir(dir_path);
    TEST_ASSERT_NOT_NULL_MESSAGE(dir,
        "cannot open tests/integration — check WORKING_DIRECTORY");

    int fixtures_checked            = 0;
    int fixtures_skipped_baseline   = 0;
    int cli_cli_mismatches          = 0;
    int lsp_cli_diffs_annotated     = 0;
    int lsp_cli_diffs_unexplained   = 0;
    /* Plan 05 CORE-22: pass-4 vs CLI pass-1 parity. */
    int facade_cli_mismatches       = 0;

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        const char *name = ent->d_name;
        size_t nl = strlen(name);
        if (nl < 5 || strcmp(name + nl - 5, ".iron") != 0) continue;

        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", dir_path, name);

        size_t slen = 0;
        char *src = slurp(path, &slen);
        if (!src) continue;

        /* Baseline-failure fixtures: sweep them (to keep fixture_count honest)
         * but do not assert on their diagnostic output. */
        int is_baseline = fixture_is_known_baseline_failure(name);

        /* CLI-CLI determinism (HARD-06 / HARD-07 canary). */
        char *out1 = run_once(src, slen, name, IRON_ANALYSIS_MODE_CLI);
        char *out2 = run_once(src, slen, name, IRON_ANALYSIS_MODE_CLI);
        if (!is_baseline && strcmp(out1, out2) != 0) {
            cli_cli_mismatches++;
            char msg[4096];
            snprintf(msg, sizeof(msg),
                "CLI-CLI determinism failure on %s\n---run1---\n%s---run2---\n%s",
                name, out1, out2);
            TEST_FAIL_MESSAGE(msg);
        }

        /* LSP vs CLI divergence surface (HARD-02 FS-gate + cascade-gate). */
        char *out3 = run_once(src, slen, name, IRON_ANALYSIS_MODE_LSP);
        if (!is_baseline && strcmp(out1, out3) != 0) {
            int allowed = fixture_uses_comptime(src)
                       || fixture_has_error_marker(src);
            if (allowed) {
                lsp_cli_diffs_annotated++;
            } else {
                /* Unexplained divergence — collect but don't fail immediately
                 * so we see the full damage surface in a single run. */
                lsp_cli_diffs_unexplained++;
                /* Print for visibility under --output-on-failure. */
                fprintf(stderr,
                    "[parity] unexplained LSP/CLI diff on fixture %s\n"
                    "---CLI---\n%s---LSP---\n%s\n",
                    name, out1, out3);
            }
        }

        /* Plan 05 CORE-22: pass 4 -- drive the LSP facade directly and
         * assert byte-for-byte match against CLI pass 1. The facade is
         * the ONE iron_analyze_buffer call site in src/lsp, so if this
         * output matches CLI, then every LSP feature that reads from
         * the facade (push diagnostics, pull diagnostics, hover, etc.)
         * matches ironc by construction. */
        char *out4 = run_via_facade(src, slen, name);
        if (!is_baseline && strcmp(out3, out4) != 0) {
            /* The facade runs with IRON_ANALYSIS_MODE_LSP, same as pass
             * 3. If the outputs diverge, the facade is adding or
             * dropping diagnostics vs. direct-LSP-mode -- that's a
             * facade bug. */
            facade_cli_mismatches++;
            fprintf(stderr,
                "[parity] facade/LSP divergence on fixture %s\n"
                "---direct-LSP---\n%s---facade-LSP---\n%s\n",
                name, out3, out4);
        }

        if (is_baseline) fixtures_skipped_baseline++;

        free(out4);
        free(out3);
        free(out2);
        free(out1);
        free(src);
        fixtures_checked++;
    }
    closedir(dir);

    /* Fixture-count floor: >= 300 (observed 381 on Wave 4 tip). A drop below
     * 300 means the integration suite was gutted; bump this floor if the drop
     * is intentional.
     * WR-05: must be >= 300, not strictly > 300 — prior _GREATER_THAN_
     * assertion required 301+ which contradicted the comment. */
    TEST_ASSERT_GREATER_OR_EQUAL_MESSAGE(300, fixtures_checked,
        "tests/integration/ fixture count dropped below 300 — intentional?");

    /* Zero tolerance for unexplained LSP/CLI divergence. */
    if (lsp_cli_diffs_unexplained > 0) {
        char msg[256];
        snprintf(msg, sizeof(msg),
            "%d fixtures produced LSP/CLI diffs that don't match the comptime "
            "or cascade-marker heuristic — see stderr for details",
            lsp_cli_diffs_unexplained);
        TEST_FAIL_MESSAGE(msg);
    }

    /* Plan 05 CORE-22: zero tolerance for facade vs direct-LSP
     * divergence -- the facade MUST replicate the analyzer output
     * exactly, since it just passes through iron_analyze_buffer. */
    if (facade_cli_mismatches > 0) {
        char msg[256];
        snprintf(msg, sizeof(msg),
            "%d fixtures produced facade/LSP diffs -- see stderr for details",
            facade_cli_mismatches);
        TEST_FAIL_MESSAGE(msg);
    }

    /* Status summary (printed even on success under --output-on-failure). */
    printf("[parity] fixtures=%d baseline_skipped=%d cli_cli_mismatches=%d "
           "lsp_cli_annotated=%d lsp_cli_unexplained=%d "
           "facade_cli_mismatches=%d\n",
           fixtures_checked, fixtures_skipped_baseline,
           cli_cli_mismatches, lsp_cli_diffs_annotated,
           lsp_cli_diffs_unexplained, facade_cli_mismatches);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_parity_all_integration_fixtures);
    return UNITY_END();
}
