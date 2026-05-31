/* Phase 34 Plan 02 -- hover memory-model annotation block driver.
 *
 * Flips the 7 WILL_FAIL hover slots reserved in Plan 34-01 to real
 * assertions. For each tests/lsp/fixtures/hover_*.iron fixture:
 *
 *   1. Read the fixture text.
 *   2. Parse the `// HOVER@<line>:<col>` marker (1-based line + col).
 *   3. Open a synthetic IronLsp_Document.
 *   4. Call ilsp_facade_hover with the parsed position.
 *   5. Capture the response markdown.
 *   6. Read the paired .expected file.
 *   7. Compare via TEST_ASSERT_EQUAL_STRING (trailing-whitespace tolerant).
 *
 * The driver routes through the same ilsp_facade_compile_for_nav path
 * the production hover handler uses; CORE-22 invariant intact (no
 * secondary iron_analyze_buffer call site). */

#include "unity.h"

#include "lsp/facade/nav/nav_core.h"
#include "lsp/store/document.h"
#include "lsp/server/server.h"
#include "lsp/facade/types.h"
#include "util/arena.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void setUp(void)    {}
void tearDown(void) {}

/* ── Filesystem helpers ───────────────────────────────────────────── */

static char *slurp(const char *path, size_t *out_len) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    fseek(fp, 0, SEEK_END);
    long n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (n < 0) { fclose(fp); return NULL; }
    char *buf = (char *)malloc((size_t)n + 1);
    if (!buf) { fclose(fp); return NULL; }
    size_t got = fread(buf, 1, (size_t)n, fp);
    fclose(fp);
    buf[got] = '\0';
    if (out_len) *out_len = got;
    return buf;
}

/* Trim a single trailing '\n' (and optional '\r') from in-place buf so
 * the comparison is tolerant of editor newline conventions on .expected
 * files without forcing the hover output to emit a terminator it
 * naturally doesn't. */
static void rtrim_newline(char *s) {
    if (!s) return;
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r')) {
        s[--n] = '\0';
    }
}

/* Parse `// HOVER@<line>:<col>` out of the fixture body. Returns true on
 * success with 0-based line + 0-based UTF-16 character set in *out_line /
 * *out_col (LSP positions are 0-based even though the fixture marker uses
 * 1-based source coordinates for human readability). */
static bool parse_hover_marker(const char *src,
                                  int *out_line,
                                  int *out_col) {
    const char *m = strstr(src, "HOVER@");
    if (!m) return false;
    int line1 = 0, col1 = 0;
    if (sscanf(m, "HOVER@%d:%d", &line1, &col1) != 2) return false;
    if (line1 < 1 || col1 < 1) return false;
    *out_line = line1 - 1;
    *out_col  = col1 - 1;
    return true;
}

/* ── Per-fixture runner ────────────────────────────────────────────── */

/* Run a single hover_<slug>.iron / hover_<slug>.expected pair. On any
 * failure prints the actual markdown + expected side-by-side so the
 * failure mode is obvious in CI logs. */
static void test_hover_fixture(const char *slug) {
    char iron_path[256];
    char expected_path[256];
    snprintf(iron_path, sizeof iron_path,
             "tests/lsp/fixtures/%s.iron", slug);
    snprintf(expected_path, sizeof expected_path,
             "tests/lsp/fixtures/%s.expected", slug);

    size_t src_len = 0;
    char *src = slurp(iron_path, &src_len);
    TEST_ASSERT_NOT_NULL_MESSAGE(src, iron_path);
    char *expected = slurp(expected_path, NULL);
    TEST_ASSERT_NOT_NULL_MESSAGE(expected, expected_path);

    int hover_line = 0, hover_col = 0;
    bool ok = parse_hover_marker(src, &hover_line, &hover_col);
    TEST_ASSERT_TRUE_MESSAGE(ok, "missing HOVER@line:col marker");

    /* Drive the facade. Synthetic server + document mirrors the
     * test_hover_formatter pattern (tests/lsp/unit/test_hover_formatter.c). */
    IronLsp_Server server;
    memset(&server, 0, sizeof server);
    server.position_encoding = ILSP_ENC_UTF16;

    IronLsp_Document *doc = ilsp_document_create(iron_path, src, src_len, 1);
    TEST_ASSERT_NOT_NULL(doc);

    IronLsp_Position pos = { .line = hover_line, .character = hover_col };
    Iron_Arena arena = iron_arena_create(64 * 1024);
    IronLsp_HoverResult hr = {0};
    ilsp_facade_hover(&server, doc, pos, NULL, &arena, &hr);

    /* Normalize trailing newline on both sides. Expected files end in
     * "\n" (POSIX text-file convention); hover output naturally does
     * not. Comparing trimmed avoids forcing one or the other. */
    char actual[8192];
    if (hr.markdown) {
        snprintf(actual, sizeof actual, "%s", hr.markdown);
    } else {
        actual[0] = '\0';
    }
    rtrim_newline(actual);
    rtrim_newline(expected);

    if (strcmp(actual, expected) != 0) {
        fprintf(stderr,
                "\n--- hover[%s] MISMATCH ---\n"
                "expected:\n[%s]\n"
                "actual:\n[%s]\n"
                "---\n",
                slug, expected, actual);
    }
    TEST_ASSERT_EQUAL_STRING_MESSAGE(expected, actual, slug);

    iron_arena_free(&arena);
    ilsp_document_destroy(doc);
    free(src);
    free(expected);
}

/* ── Fixtures: 7 hover slots from Plan 34-01 ──────────────────────── */

static void test_hover_policy_heap(void)        { test_hover_fixture("hover_policy_heap"); }
static void test_hover_policy_rc(void)          { test_hover_fixture("hover_policy_rc"); }
static void test_hover_policy_weak_rc(void)     { test_hover_fixture("hover_policy_weak_rc"); }
static void test_hover_policy_stack(void)       { test_hover_fixture("hover_policy_stack"); }
static void test_hover_regime_unchecked(void)   { test_hover_fixture("hover_regime_unchecked"); }
static void test_hover_readonly_func(void)      { test_hover_fixture("hover_readonly_func"); }
static void test_hover_nocopy_object(void)      { test_hover_fixture("hover_nocopy_object"); }

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_hover_policy_heap);
    RUN_TEST(test_hover_policy_rc);
    RUN_TEST(test_hover_policy_weak_rc);
    RUN_TEST(test_hover_policy_stack);
    RUN_TEST(test_hover_regime_unchecked);
    RUN_TEST(test_hover_readonly_func);
    RUN_TEST(test_hover_nocopy_object);
    return UNITY_END();
}
