/* Phase 5 Plan 05-02 (D-08, FMT-02) -- CLI <-> LSP formatter parity.
 *
 * For every .iron fixture under tests/integration that parses cleanly:
 *
 *   1. Run iron_format_source directly (the CLI path both `iron fmt`
 *      and the Unity driver use).
 *   2. Run ilsp_facade_format_full with an in-memory IronLsp_Document
 *      whose buffer points at the same source (the LSP path clients
 *      see through textDocument/formatting).
 *   3. Assert the LSP path returned exactly one TextEdit.
 *   4. Assert the TextEdit's new_text is byte-for-byte equal to the
 *      CLI path's formatted bytes (after the facade's trailing-
 *      newline / BOM normalization, which is source-driven so it
 *      applies identically to the CLI buffer we're comparing to --
 *      any divergence is a bug to surface).
 *
 * Both paths call the same iron_format_source primitive underneath,
 * so the only way this can fail is if the LSP facade adds post-
 * processing the CLI does not mirror. That's exactly the divergence
 * gate this test is meant to catch.
 *
 * Fixtures whose source has lexer or parser errors are skipped --
 * the formatter refuses on those (D-03) and both paths return empty,
 * which is already tested in unit coverage and produces no meaningful
 * parity signal. */

#include "unity.h"

#include "diagnostics/diagnostics.h"
#include "fmt/format.h"
#include "fmt/options.h"
#include "lsp/facade/fmt/format.h"
#include "lsp/facade/types.h"
#include "lsp/store/document.h"
#include "util/arena.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

void setUp(void)    {}
void tearDown(void) {}

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

/* Apply the same trailing-newline / BOM normalization the LSP facade
 * performs (see src/lsp/facade/fmt/format.c normalize_result). We mirror
 * the logic here so we can compare the CLI-formatted bytes against the
 * LSP-facade newText on equal footing. Any divergence means the facade
 * did something the CLI did not. */
static char *normalize_cli(const char *source, size_t source_len,
                             const char *formatted, size_t formatted_len,
                             size_t *out_len) {
    int src_has_bom = source_len >= 3
                      && (unsigned char)source[0] == 0xEF
                      && (unsigned char)source[1] == 0xBB
                      && (unsigned char)source[2] == 0xBF;
    int src_has_final = source_len > 0 && source[source_len - 1] == '\n';

    int need_final = src_has_final
                     && (formatted_len == 0
                         || formatted[formatted_len - 1] != '\n');
    int need_bom   = src_has_bom
                     && !(formatted_len >= 3
                          && (unsigned char)formatted[0] == 0xEF
                          && (unsigned char)formatted[1] == 0xBB
                          && (unsigned char)formatted[2] == 0xBF);

    size_t extra = (need_bom ? 3u : 0u) + (need_final ? 1u : 0u);
    size_t total = formatted_len + extra;
    char *buf = (char *)malloc(total + 1);
    if (!buf) { *out_len = 0; return NULL; }

    size_t o = 0;
    if (need_bom) {
        buf[o++] = (char)0xEF;
        buf[o++] = (char)0xBB;
        buf[o++] = (char)0xBF;
    }
    memcpy(buf + o, formatted, formatted_len); o += formatted_len;
    if (need_final) buf[o++] = '\n';
    buf[o] = '\0';
    *out_len = o;
    return buf;
}

static int checked    = 0;
static int skipped    = 0;
static int mismatches = 0;

static void parity_one_file(const char *path) {
    size_t src_len = 0;
    char *src = slurp(path, &src_len);
    if (!src) return;

    /* CLI path: direct iron_format_source call. */
    Iron_Arena     cli_arena = iron_arena_create(64 * 1024);
    Iron_DiagList  cli_diags = iron_diaglist_create();
    IronFmtOptions opts      = iron_fmt_options_default();
    IronFmtResult  cli_r = iron_format_source(src, path, &opts,
                                                &cli_arena, &cli_diags);

    if (!cli_r.ok) {
        /* Both paths refuse identically on parse errors; skip. */
        iron_diaglist_free(&cli_diags);
        iron_arena_free(&cli_arena);
        free(src);
        skipped++;
        return;
    }

    /* LSP facade path: synthesize an IronLsp_Document. */
    Iron_Arena       lsp_arena = iron_arena_create(64 * 1024);
    IronLsp_Document doc;
    memset(&doc, 0, sizeof(doc));
    doc.uri      = (char *)path;
    doc.text     = src;
    doc.text_len = src_len;
    doc.version  = 1;

    IronLsp_TextEditList result = ilsp_facade_format_full(
        &doc, /* ws */ NULL, &opts, &lsp_arena, /* cancel */ NULL);

    char msg[1024];
    if (result.count != 1) {
        snprintf(msg, sizeof(msg),
                 "fixture %s: expected 1 TextEdit, got %zu",
                 path, result.count);
        TEST_FAIL_MESSAGE(msg);
    }

    /* Normalize CLI bytes the same way the facade normalizes, so the
     * comparison is between like and like. Both are source-driven; any
     * residual difference is a real divergence. */
    size_t cli_norm_len = 0;
    char *cli_norm = normalize_cli(src, src_len,
                                     cli_r.formatted, cli_r.formatted_len,
                                     &cli_norm_len);
    TEST_ASSERT_NOT_NULL_MESSAGE(cli_norm, path);

    const char *lsp_text = result.edits[0].new_text
                            ? result.edits[0].new_text
                            : "";
    size_t lsp_len = strlen(lsp_text);

    if (lsp_len != cli_norm_len
        || memcmp(cli_norm, lsp_text, cli_norm_len) != 0) {
        snprintf(msg, sizeof(msg),
                 "fixture %s: CLI %zu bytes vs LSP %zu bytes -- divergence",
                 path, cli_norm_len, lsp_len);
        mismatches++;
        TEST_FAIL_MESSAGE(msg);
    }

    free(cli_norm);
    iron_diaglist_free(&cli_diags);
    iron_arena_free(&cli_arena);
    iron_arena_free(&lsp_arena);
    free(src);
    checked++;
}

void test_parity_corpus(void) {
    /* TESTS_INTEGRATION_DIR is baked at compile time via CMake. */
    const char *dir_path = TESTS_INTEGRATION_DIR;
    DIR *d = opendir(dir_path);
    TEST_ASSERT_NOT_NULL_MESSAGE(d, dir_path);

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        size_t nlen = strlen(ent->d_name);
        if (nlen < 5) continue;
        if (strcmp(ent->d_name + nlen - 5, ".iron") != 0) continue;

        char full[4096];
        snprintf(full, sizeof(full), "%s/%s", dir_path, ent->d_name);

        struct stat st;
        if (stat(full, &st) != 0) continue;
        if (!S_ISREG(st.st_mode)) continue;

        parity_one_file(full);
    }
    closedir(d);

    char msg[128];
    snprintf(msg, sizeof(msg),
             "parity: checked=%d skipped=%d mismatches=%d",
             checked, skipped, mismatches);
    TEST_ASSERT_TRUE_MESSAGE(checked > 0, msg);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, mismatches, msg);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_parity_corpus);
    return UNITY_END();
}
