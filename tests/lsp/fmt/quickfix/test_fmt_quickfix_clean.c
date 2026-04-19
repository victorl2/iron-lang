/* Phase 5 Plan 05-05 Task 2 (FMT-06, D-07): CI gate asserting every
 * Phase 4 P1 quickfix produces iron-fmt-clean output.
 *
 * Per-fixture flow:
 *   1. Read source.iron.
 *   2. Run iron_analyze_buffer to obtain Iron_Diagnostic[].
 *   3. Locate the diagnostic this fixture targets (by code).
 *   4. Invoke the corresponding quickfix handler via ilsp_quickfix_lookup
 *      to produce a single-edit IronLsp_CodeAction.
 *   5. Apply the single TextEdit in memory to source.iron.
 *   6. Read expected_post_apply.iron.
 *   7. Assert applied == expected (byte-for-byte).
 *   8. Run iron_format_source on applied; assert the formatter returns
 *      the same bytes (iron-fmt-clean).
 *
 * Dual-labeled under phase-m3-invariant AND phase-m4-invariant (D-07)
 * so a Phase 4 quickfix regression surfaces under EITHER milestone's
 * CI run. */

#include "unity.h"

#include "analyzer/analyzer.h"
#include "diagnostics/diagnostics.h"
#include "fmt/format.h"
#include "fmt/options.h"
#include "lsp/facade/edit/codeaction/registry.h"
#include "lsp/store/document.h"
#include "util/arena.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

#ifndef QUICKFIX_FIXTURE_DIR
# define QUICKFIX_FIXTURE_DIR "tests/lsp/fmt/quickfix"
#endif

/* ── helpers ────────────────────────────────────────────────────────── */

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

/* Convert 0-indexed LSP (line, character) to a byte offset in the
 * UTF-8 buffer. Column counting is byte-based (matches Phase 5 UTF-16
 * deferral note in Plan 05-02 for ASCII-only fixtures). Clamps to
 * src_len on overflow. */
static size_t line_col_to_byte(const char *src, size_t src_len,
                                 uint32_t line, uint32_t col) {
    size_t off = 0;
    uint32_t l = 0;
    while (off < src_len && l < line) {
        if (src[off++] == '\n') l++;
    }
    /* Now at start of target line. Walk col bytes, clamped to EOL. */
    uint32_t c = 0;
    while (off < src_len && c < col && src[off] != '\n') {
        off++;
        c++;
    }
    return off;
}

/* Apply one TextEdit (LSP range + new_text) to a source buffer in
 * memory. Returns malloc'd result; caller frees. */
static char *apply_text_edit(const char *src, size_t src_len,
                              uint32_t start_line, uint32_t start_char,
                              uint32_t end_line,   uint32_t end_char,
                              const char *new_text, size_t *out_len) {
    size_t start_off = line_col_to_byte(src, src_len, start_line, start_char);
    size_t end_off   = line_col_to_byte(src, src_len, end_line,   end_char);
    if (end_off < start_off) end_off = start_off;

    size_t nlen  = new_text ? strlen(new_text) : 0;
    size_t total = start_off + nlen + (src_len - end_off);
    char *out = (char *)malloc(total + 1);
    if (!out) { *out_len = 0; return NULL; }
    memcpy(out, src, start_off);
    if (nlen) memcpy(out + start_off, new_text, nlen);
    memcpy(out + start_off + nlen, src + end_off, src_len - end_off);
    out[total] = '\0';
    *out_len = total;
    return out;
}

/* Scan diaglist for the first diagnostic with the given code. */
static const Iron_Diagnostic *find_diag(const Iron_DiagList *dl, int code) {
    for (int i = 0; i < dl->count; i++) {
        if (dl->items[i].code == code) return &dl->items[i];
    }
    return NULL;
}

/* ── per-fixture driver ─────────────────────────────────────────────── */

static void run_fixture(const char *fixture_name, int diag_code) {
    char src_path[512], exp_path[512];
    snprintf(src_path, sizeof(src_path), "%s/%s/source.iron",
             QUICKFIX_FIXTURE_DIR, fixture_name);
    snprintf(exp_path, sizeof(exp_path), "%s/%s/expected_post_apply.iron",
             QUICKFIX_FIXTURE_DIR, fixture_name);

    size_t src_len = 0, exp_len = 0;
    char *src = slurp(src_path, &src_len);
    char *expected = slurp(exp_path, &exp_len);

    char msg[1024];
    snprintf(msg, sizeof(msg), "fixture %s: source.iron missing at %s",
             fixture_name, src_path);
    TEST_ASSERT_NOT_NULL_MESSAGE(src, msg);
    snprintf(msg, sizeof(msg),
             "fixture %s: expected_post_apply.iron missing at %s",
             fixture_name, exp_path);
    TEST_ASSERT_NOT_NULL_MESSAGE(expected, msg);

    /* Analyze source to get diagnostics. CLI mode -- fmt does not need
     * LSP-mode comptime gating and the quickfix seeds happen at emit
     * time in either mode. */
    Iron_Arena    arena = iron_arena_create(64 * 1024);
    Iron_DiagList diags = iron_diaglist_create();
    (void)iron_analyze_buffer(src, src_len, src_path,
                               IRON_ANALYSIS_MODE_CLI,
                               &arena, &diags, NULL);

    const Iron_Diagnostic *target = find_diag(&diags, diag_code);
    if (!target) {
        /* Dump diagnostic codes observed, to help debug misalignment. */
        char dbg[512] = {0};
        size_t dp = 0;
        for (int i = 0; i < diags.count && dp < sizeof(dbg) - 16; i++) {
            dp += (size_t)snprintf(dbg + dp, sizeof(dbg) - dp, "%d ",
                                    diags.items[i].code);
        }
        snprintf(msg, sizeof(msg),
                 "fixture %s: no diagnostic with code %d found. Observed: %s",
                 fixture_name, diag_code, dbg);
        TEST_FAIL_MESSAGE(msg);
    }

    /* Look up the quickfix handler from the public registry API. */
    IronLsp_QuickfixFn handler = ilsp_quickfix_lookup(diag_code);
    snprintf(msg, sizeof(msg),
             "fixture %s: no quickfix handler registered for code %d",
             fixture_name, diag_code);
    TEST_ASSERT_NOT_NULL_MESSAGE(handler, msg);

    /* Synthesize a real IronLsp_Document (not a stack stub). The
     * missing_return + unused_import handlers read doc->text +
     * doc->line_idx to derive indent / blank-line state. */
    IronLsp_Document *doc =
        ilsp_document_create(src_path, src, src_len, /* version */ 1);
    snprintf(msg, sizeof(msg),
             "fixture %s: ilsp_document_create returned NULL", fixture_name);
    TEST_ASSERT_NOT_NULL_MESSAGE(doc, msg);

    /* Invoke the handler. Output is a single-file single-edit shape
     * (IronLsp_CodeAction.edit_{start,end}_{line,char} + edit_new_text
     * per registry.h:56-65). */
    IronLsp_CodeAction action;
    memset(&action, 0, sizeof(action));
    handler(target, doc, /* wi */ NULL, &arena, &action);

    snprintf(msg, sizeof(msg),
             "fixture %s: quickfix produced no edit (edit_new_text NULL)",
             fixture_name);
    TEST_ASSERT_NOT_NULL_MESSAGE(action.edit_new_text, msg);

    /* Apply the single TextEdit and compare bytes. */
    size_t applied_len = 0;
    char *applied = apply_text_edit(src, src_len,
                                      action.edit_start_line,
                                      action.edit_start_char,
                                      action.edit_end_line,
                                      action.edit_end_char,
                                      action.edit_new_text,
                                      &applied_len);
    snprintf(msg, sizeof(msg),
             "fixture %s: apply_text_edit returned NULL", fixture_name);
    TEST_ASSERT_NOT_NULL_MESSAGE(applied, msg);

    if (applied_len != exp_len
        || memcmp(applied, expected, exp_len) != 0) {
        snprintf(msg, sizeof(msg),
                 "fixture %s: applied != expected.\n"
                 "  applied_len=%zu expected_len=%zu\n"
                 "  applied=\n---\n%s---\n  expected=\n---\n%s---",
                 fixture_name, applied_len, exp_len, applied, expected);
        TEST_FAIL_MESSAGE(msg);
    }

    /* Assert iron-fmt-clean: iron_format_source on applied must produce
     * byte-identical output. */
    Iron_Arena     fmt_arena = iron_arena_create(64 * 1024);
    Iron_DiagList  fmt_diags = iron_diaglist_create();
    IronFmtOptions opts      = iron_fmt_options_default();
    IronFmtResult  r = iron_format_source(applied, src_path, &opts,
                                            &fmt_arena, &fmt_diags);

    snprintf(msg, sizeof(msg),
             "fixture %s: iron_format_source refused applied output (parse error)",
             fixture_name);
    TEST_ASSERT_TRUE_MESSAGE(r.ok, msg);

    if (r.formatted_len != applied_len
        || memcmp(r.formatted, applied, applied_len) != 0) {
        snprintf(msg, sizeof(msg),
                 "fixture %s: quickfix output is NOT iron-fmt-clean.\n"
                 "  applied_len=%zu formatted_len=%zu\n"
                 "  applied=\n---\n%s---\n  formatted=\n---\n%.*s---",
                 fixture_name, applied_len, r.formatted_len,
                 applied, (int)r.formatted_len, r.formatted);
        TEST_FAIL_MESSAGE(msg);
    }

    iron_diaglist_free(&fmt_diags);
    iron_arena_free(&fmt_arena);
    free(applied);
    ilsp_document_destroy(doc);
    iron_diaglist_free(&diags);
    iron_arena_free(&arena);
    free(src);
    free(expected);
}

/* ── test entries (one per P1 quickfix) ─────────────────────────────── */

/* Diagnostic codes from src/diagnostics/diagnostics.h:
 *   IRON_ERR_UNDEFINED_VAR         200
 *   IRON_ERR_TYPE_MISMATCH_LITERAL 235
 *   IRON_ERR_MISSING_RETURN        236
 *   IRON_WARN_UNUSED_IMPORT        611
 *   IRON_WARN_REDUNDANT_CAST       612
 */

static void test_quickfix_undefined_var_is_fmt_clean(void) {
    run_fixture("undefined_var", IRON_ERR_UNDEFINED_VAR);
}

static void test_quickfix_unused_import_is_fmt_clean(void) {
    run_fixture("unused_import", IRON_WARN_UNUSED_IMPORT);
}

static void test_quickfix_missing_return_is_fmt_clean(void) {
    run_fixture("missing_return", IRON_ERR_MISSING_RETURN);
}

static void test_quickfix_type_mismatch_literal_is_fmt_clean(void) {
    run_fixture("type_mismatch_literal", IRON_ERR_TYPE_MISMATCH_LITERAL);
}

static void test_quickfix_redundant_cast_is_fmt_clean(void) {
    run_fixture("redundant_cast", IRON_WARN_REDUNDANT_CAST);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_quickfix_undefined_var_is_fmt_clean);
    RUN_TEST(test_quickfix_unused_import_is_fmt_clean);
    RUN_TEST(test_quickfix_missing_return_is_fmt_clean);
    RUN_TEST(test_quickfix_type_mismatch_literal_is_fmt_clean);
    RUN_TEST(test_quickfix_redundant_cast_is_fmt_clean);
    return UNITY_END();
}
