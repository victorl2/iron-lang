/* Phase 34 LSP-13 / CORE-22: structural invariant test.
 *
 * Asserts exactly one textual reference to `iron_analyze_buffer` exists
 * under `src/lsp/` (excluding comment-only mentions). The reference MUST
 * live at `src/lsp/facade/compile.c` (the canonical CORE-22 single call
 * site). If any LSP-side code path grows a second analyze call, this
 * test fires immediately.
 *
 * Implementation: shell-out to grep for "iron_analyze_buffer(" (note the
 * trailing open-paren). Documentation mentions in this tree all appear
 * without the trailing paren ("// iron_analyze_buffer is the SINGLE …",
 * "* iron_analyze_buffer call site …"), so they are excluded by the
 * paren-suffix filter. The only literal hit with "(" is the actual call
 * at src/lsp/facade/compile.c:57.
 *
 * The test is structural (text-only). It does NOT exercise the analyzer
 * at runtime. Failure indicates a CORE-22 regression and blocks merge.
 *
 * Working directory: the CTest invocation runs from CMAKE_SOURCE_DIR
 * (see tests/lsp/invariant/CMakeLists.txt), so the relative `src/lsp/`
 * path resolves correctly regardless of where the build tree lives.
 */
#include "unity.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

void setUp(void) {}
void tearDown(void) {}

/* Count textual call-shaped references to iron_analyze_buffer under
 * src/lsp/. The trailing paren in the grep pattern filters out every
 * documentation mention; only the actual function call survives. */
static int count_call_site_references(void) {
    FILE *fp = popen(
        "grep -rn 'iron_analyze_buffer(' src/lsp/ 2>/dev/null | wc -l",
        "r");
    if (!fp) return -1;
    char buf[64] = {0};
    char *got = fgets(buf, sizeof(buf), fp);
    pclose(fp);
    if (!got) return -1;
    return atoi(buf);
}

/* Return the first source path that contains an iron_analyze_buffer(
 * call. Result is copied into out (out_cap bytes), trailing newline
 * trimmed. Returns 1 on success, 0 on failure. */
static int first_call_site_path(char *out, size_t out_cap) {
    FILE *fp = popen(
        "grep -rn 'iron_analyze_buffer(' src/lsp/ 2>/dev/null "
        "| head -1 | cut -d: -f1",
        "r");
    if (!fp) return 0;
    char buf[512] = {0};
    char *got = fgets(buf, sizeof(buf), fp);
    pclose(fp);
    if (!got) return 0;
    size_t len = strlen(buf);
    if (len > 0 && buf[len - 1] == '\n') buf[--len] = '\0';
    if (len + 1 > out_cap) return 0;
    memcpy(out, buf, len + 1);
    return 1;
}

static void test_single_analyze_call_site(void) {
    int n = count_call_site_references();
    TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(0, n,
        "grep shell-out failed; cannot enforce CORE-22 invariant");
    /* Exactly one ACTUAL call ("iron_analyze_buffer(") is allowed under
     * src/lsp/. Documentation mentions without the trailing paren are
     * filtered by the grep pattern. */
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, n,
        "CORE-22 violation: src/lsp/ has != 1 iron_analyze_buffer( call. "
        "Single call site MUST live at src/lsp/facade/compile.c.");
}

static void test_call_site_in_compile_c(void) {
    char path[512] = {0};
    int ok = first_call_site_path(path, sizeof(path));
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, ok,
        "could not locate the CORE-22 call site via grep");
    TEST_ASSERT_EQUAL_STRING_MESSAGE(
        "src/lsp/facade/compile.c", path,
        "CORE-22 call site must live at src/lsp/facade/compile.c");
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_single_analyze_call_site);
    RUN_TEST(test_call_site_in_compile_c);
    return UNITY_END();
}
