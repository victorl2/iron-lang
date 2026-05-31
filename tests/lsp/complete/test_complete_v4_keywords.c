/* Phase 34 LSP-03 Plan 34-03 Task 1 — v3/v4 keyword visibility test driver.
 *
 * Replaces the 10 WILL_FAIL stubs registered by Plan 34-01
 * (lsp_complete_complete_keyword_*). Each RUN_TEST exercises one of the
 * ten new v3/v4 keyword arms in ilsp_keyword_visible_at:
 *   heap, rc, weak, unchecked, defer, drop, copy, nocopy, leak, in
 *
 * Each fixture has both a VISIBLE and a HIDDEN case. The driver builds
 * a small in-memory IronLsp_Document mirroring the Plan-01 fixture, runs
 * iron_analyze_buffer over the source to obtain a real Iron_Program for
 * arms that consult enclosing_object_decl (drop, copy), then invokes
 * the predicate at the COMPLETE@L:C cursor position and asserts the
 * boolean.
 *
 * The driver intentionally does NOT route through src/lsp/facade/compile.c
 * — it uses iron_analyze_buffer directly. CORE-22 restricts the single
 * iron_analyze_buffer call site to src/lsp/, not tests/lsp/; the
 * structural test_core22_single_analyze invariant verifies that.
 */

#include "lsp/facade/edit/complete/keyword_filter.h"
#include "lsp/facade/edit/complete/context_classify.h"
#include "lsp/store/document.h"
#include "analyzer/analyzer.h"
#include "diagnostics/diagnostics.h"
#include "parser/ast.h"
#include "util/arena.h"

#include "unity.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

void setUp(void)    {}
void tearDown(void) {}

/* ── Helpers ──────────────────────────────────────────────────────── */

/* Run a full analyze over `source` so the test can hand the resulting
 * Iron_Program to ilsp_keyword_visible_at. Caller frees arena + diags. */
static Iron_Program *analyze_source(Iron_Arena *arena, Iron_DiagList *diags,
                                      const char *source) {
    Iron_AnalyzeResult r = iron_analyze_buffer(
        source, strlen(source),
        "test.iron",
        IRON_ANALYSIS_MODE_LSP,
        arena, diags,
        /*cancel_flag=*/NULL,
        /*user_source_start_line=*/0);
    /* program may carry errors — we only need the parsed structure for
     * enclosing_object_decl spans. NULL is acceptable; the predicate
     * arms tolerate program == NULL by refusing the v3/v4 arms that
     * strictly need a program. */
    (void)r;
    return r.program;
}

/* Convert (L, C) 1-indexed → (line0, col0) LSP-style 0-indexed. */
static void to_lsp(uint32_t line_1, uint32_t col_1,
                     uint32_t *out_line, uint32_t *out_col) {
    *out_line = line_1 > 0 ? line_1 - 1 : 0;
    *out_col  = col_1  > 0 ? col_1  - 1 : 0;
}

/* Resolve the LSP context for (line0, col0) over `text` using the
 * production classifier; lets each test reflect what the real completion
 * pipeline would observe. */
static IronLsp_CompletionContext classify_at(const char *text, size_t len,
                                              uint32_t line0, uint32_t col0) {
    /* Compute byte offset by counting newlines (no line index needed). */
    size_t byte = 0;
    uint32_t cur_line = 0;
    while (byte < len && cur_line < line0) {
        if (text[byte] == '\n') cur_line++;
        byte++;
    }
    /* Add column bytes, clamped at next newline. */
    size_t add = 0;
    while (byte + add < len && add < col0 && text[byte + add] != '\n') add++;
    byte += add;
    return ilsp_completion_context_classify_buf(text, len, byte);
}

/* Drive the predicate against a fixture-style scenario:
 *   - `source` is the full fixture content
 *   - `visible_l1`/`visible_c1` is the VISIBLE-case cursor position (1-indexed)
 *   - `hidden_l1`/`hidden_c1` is the HIDDEN-case cursor position (1-indexed)
 * Asserts both expectations.
 */
static void run_keyword_pair(const char *kw, const char *source,
                              uint32_t visible_l1, uint32_t visible_c1,
                              uint32_t hidden_l1,  uint32_t hidden_c1) {
    Iron_Arena    arena = iron_arena_create(64 * 1024);
    Iron_DiagList diags = iron_diaglist_create();
    Iron_Program *prog  = analyze_source(&arena, &diags, source);

    IronLsp_Document *doc = ilsp_document_create("file:///t.iron",
                                                    source, strlen(source), 1);
    TEST_ASSERT_NOT_NULL(doc);

    /* VISIBLE case. */
    {
        uint32_t line0, col0;
        to_lsp(visible_l1, visible_c1, &line0, &col0);
        IronLsp_CompletionContext ctx =
            classify_at(source, strlen(source), line0, col0);
        bool v = ilsp_keyword_visible_at(kw, doc, prog, line0, col0, ctx);
        if (!v) {
            char msg[128];
            snprintf(msg, sizeof(msg),
                      "kw=%s visible-case @%u:%u ctx=%d expected TRUE",
                      kw, visible_l1, visible_c1, (int)ctx);
            TEST_FAIL_MESSAGE(msg);
        }
    }

    /* HIDDEN case. */
    {
        uint32_t line0, col0;
        to_lsp(hidden_l1, hidden_c1, &line0, &col0);
        IronLsp_CompletionContext ctx =
            classify_at(source, strlen(source), line0, col0);
        bool v = ilsp_keyword_visible_at(kw, doc, prog, line0, col0, ctx);
        if (v) {
            char msg[128];
            snprintf(msg, sizeof(msg),
                      "kw=%s hidden-case @%u:%u ctx=%d expected FALSE",
                      kw, hidden_l1, hidden_c1, (int)ctx);
            TEST_FAIL_MESSAGE(msg);
        }
    }

    ilsp_document_destroy(doc);
    iron_diaglist_free(&diags);
    iron_arena_free(&arena);
}

/* ── Per-keyword tests ────────────────────────────────────────────── */

/* `heap` — visible at type-expression (after `:` annotation) or RHS-of-=;
 * hidden at empty/STATEMENT_HEAD lines. */
static void test_keyword_heap(void) {
    const char *src =
        "object Buffer { val size: Int }\n"
        "func main() {\n"
        "    val a: heap Buffer = heap Buffer(1024)\n"
        "    val x = 10\n"
        "}\n";
    /* VISIBLE: line 3, col 12 (in type-annotation position after `: `). */
    /* HIDDEN:  line 4, col 5  (statement-head before `val`). */
    run_keyword_pair("heap", src, 3, 12, 4, 5);
}

/* `rc` — visible at RHS-of-= (after `=`); hidden at empty/STATEMENT_HEAD. */
static void test_keyword_rc(void) {
    const char *src =
        "object Point { val x: Int val y: Int }\n"
        "func main() {\n"
        "    val p = rc Point(1, 2)\n"
        "    val y = 7\n"
        "}\n";
    /* VISIBLE: line 3 col 14 (just after `= ` on the rc line). */
    /* HIDDEN:  line 4 col 5  (statement-head). */
    run_keyword_pair("rc", src, 3, 14, 4, 5);
}

/* `weak` — visible at type-position (after `:`); hidden at /-precedes
 * EXPR_HEAD lines. */
static void test_keyword_weak(void) {
    const char *src =
        "object Point { val x: Int val y: Int }\n"
        "func main() {\n"
        "    val p: weak Point = p\n"
        "    val z = 1\n"
        "}\n";
    /* VISIBLE: line 3 col 12 (type annotation after `:`). */
    /* HIDDEN:  line 4 col 5  (statement-head). */
    run_keyword_pair("weak", src, 3, 12, 4, 5);
}

/* `unchecked` — visible after `*` on same line OR TYPE_POSITION; hidden
 * at statement-head expression positions. */
static void test_keyword_unchecked(void) {
    const char *src =
        "func main() {\n"
        "    val raw: *unchecked Int = 0\n"
        "    val x = 10\n"
        "}\n";
    /* VISIBLE: line 2 col 16 (inside *unchecked after the `*`). */
    /* HIDDEN:  line 3 col 5  (statement-head). */
    run_keyword_pair("unchecked", src, 2, 16, 3, 5);
}

/* `defer` — visible only at STATEMENT_HEAD; hidden at TYPE_POSITION. */
static void test_keyword_defer(void) {
    const char *src =
        "func main() {\n"
        "    val buf = 0\n"
        "    \n"
        "    val n: Int = 42\n"
        "}\n";
    /* VISIBLE: line 3 col 5 (blank line at statement head inside body). */
    /* HIDDEN:  line 4 col 12 (after `:` in type annotation). */
    run_keyword_pair("defer", src, 3, 5, 4, 12);
}

/* `drop` — visible at decl-head inside an object body; hidden in
 * function-body statement positions. */
static void test_keyword_drop(void) {
    const char *src =
        "object FileHandle {\n"
        "    val fd: Int\n"
        "    drop {}\n"
        "}\n"
        "func main() {\n"
        "    val x = 1\n"
        "}\n";
    /* VISIBLE: line 3 col 5 (start of drop decl inside object body). */
    /* HIDDEN:  line 6 col 5 (inside func main body). */
    run_keyword_pair("drop", src, 3, 5, 6, 5);
}

/* `copy` — same shape as `drop`. */
static void test_keyword_copy(void) {
    const char *src =
        "object Point {\n"
        "    val x: Int\n"
        "    val y: Int\n"
        "    copy {}\n"
        "}\n"
        "func main() {\n"
        "    val n = 0\n"
        "}\n";
    /* VISIBLE: line 4 col 5 (decl-head inside object body). */
    /* HIDDEN:  line 7 col 5 (inside func main body — no enclosing object). */
    run_keyword_pair("copy", src, 4, 5, 7, 5);
}

/* `nocopy` — visible at module-decl-head (before `object`); hidden in
 * expression positions. */
static void test_keyword_nocopy(void) {
    const char *src =
        "nocopy object FileHandle {\n"
        "    val fd: Int\n"
        "}\n"
        "func main() {\n"
        "    val n = 1\n"
        "}\n";
    /* VISIBLE: line 1 col 1 (start of `nocopy object` decl). */
    /* HIDDEN:  line 5 col 13 (end of `val n = 1`, expression position). */
    run_keyword_pair("nocopy", src, 1, 1, 5, 13);
}

/* `leak` — visible at expression-statement positions; hidden in
 * type-annotation positions. */
static void test_keyword_leak(void) {
    const char *src =
        "func main() {\n"
        "    val buf = 1\n"
        "    \n"
        "    val n: Int = 1\n"
        "}\n";
    /* VISIBLE: line 3 col 5 (statement-head). */
    /* HIDDEN:  line 4 col 12 (after `:` type position). */
    run_keyword_pair("leak", src, 3, 5, 4, 12);
}

/* `in` — visible only after a `for` token on the current line; hidden
 * everywhere else. */
static void test_keyword_in(void) {
    const char *src =
        "func main() {\n"
        "    for i in 0..10 {\n"
        "        val n = i\n"
        "    }\n"
        "    val z = 1\n"
        "}\n";
    /* VISIBLE: line 2 col 11 (right after `for i ` waiting for `in`). */
    /* HIDDEN:  line 5 col 5  (statement-head, no `for` on line). */
    run_keyword_pair("in", src, 2, 11, 5, 5);
}

/* ── Driver ───────────────────────────────────────────────────────── */

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_keyword_heap);
    RUN_TEST(test_keyword_rc);
    RUN_TEST(test_keyword_weak);
    RUN_TEST(test_keyword_unchecked);
    RUN_TEST(test_keyword_defer);
    RUN_TEST(test_keyword_drop);
    RUN_TEST(test_keyword_copy);
    RUN_TEST(test_keyword_nocopy);
    RUN_TEST(test_keyword_leak);
    RUN_TEST(test_keyword_in);
    return UNITY_END();
}
