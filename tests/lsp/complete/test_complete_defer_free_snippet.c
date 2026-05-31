/* Phase 34 LSP-04 Plan 34-03 Task 2 — `defer free <binding>` snippet
 * test driver.
 *
 * Retires the 3 lsp_complete_complete_defer_free_* WILL_FAIL stubs from
 * Plan 34-01:
 *   - complete_defer_free_heap → snippet body `defer free ${1:buffer}$0`
 *   - complete_defer_free_rc   → snippet body `defer free ${1:shared}$0`
 *   - complete_defer_free_none → no snippet offered
 *
 * Three layers exercised:
 *   1. ilsp_collect_recent_heap_rc_bindings — the AST backward-scan
 *      returns the correct binding names in most-recent-first order.
 *   2. ilsp_snippet_render(ILSP_SNIPPET_DEFER_FREE, ...) — produces the
 *      LSP-snippet body with `${1:<name>}$0`, escaping `$`/`}`/`\` in
 *      hostile identifiers (PITFALL D guard).
 *   3. Multi-binding ranking — when two heap/rc bindings precede the
 *      cursor, the most-recent comes first.
 */

#include "lsp/facade/edit/complete/defer_free.h"
#include "lsp/facade/edit/complete/snippet.h"
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

static Iron_Program *analyze_src(Iron_Arena *arena, Iron_DiagList *diags,
                                    const char *source) {
    Iron_AnalyzeResult r = iron_analyze_buffer(
        source, strlen(source),
        "test.iron",
        IRON_ANALYSIS_MODE_LSP,
        arena, diags,
        /*cancel_flag=*/NULL,
        /*user_source_start_line=*/0);
    return r.program;
}

/* ── Test 1: heap binding → snippet for that binding ──────────────── */

static void test_defer_free_heap_single_binding(void) {
    /* Mirror fixtures/complete_defer_free_heap.iron — one heap binding,
     * then a blank line at statement-head. */
    const char *src =
        "object Buffer { val size: Int }\n"
        "func main() {\n"
        "    val buffer = heap Buffer(1024)\n"
        "    \n"
        "}\n";

    Iron_Arena    arena = iron_arena_create(64 * 1024);
    Iron_DiagList diags = iron_diaglist_create();
    Iron_Program *prog  = analyze_src(&arena, &diags, src);
    TEST_ASSERT_NOT_NULL(prog);

    const char *names[ILSP_DEFER_FREE_MAX_CANDIDATES] = {0};
    /* Cursor at line 4 (the blank line, 1-indexed). */
    size_t cnt = ilsp_collect_recent_heap_rc_bindings(
        prog, /*cursor_line_1=*/4, &arena, names,
        ILSP_DEFER_FREE_MAX_CANDIDATES);
    TEST_ASSERT_EQUAL_size_t(1, cnt);
    TEST_ASSERT_NOT_NULL(names[0]);
    TEST_ASSERT_EQUAL_STRING("buffer", names[0]);

    /* Render the snippet for that binding. */
    IronLsp_SnippetMeta meta = { .name = names[0] };
    const char *body = ilsp_snippet_render(ILSP_SNIPPET_DEFER_FREE,
                                              &meta, &arena);
    TEST_ASSERT_NOT_NULL(body);
    TEST_ASSERT_EQUAL_STRING("defer free ${1:buffer}$0", body);

    iron_diaglist_free(&diags);
    iron_arena_free(&arena);
}

/* ── Test 2: rc binding → snippet for that binding ────────────────── */

static void test_defer_free_rc_single_binding(void) {
    const char *src =
        "object Point { val x: Int val y: Int }\n"
        "func main() {\n"
        "    val shared = rc Point(1, 2)\n"
        "    \n"
        "}\n";

    Iron_Arena    arena = iron_arena_create(64 * 1024);
    Iron_DiagList diags = iron_diaglist_create();
    Iron_Program *prog  = analyze_src(&arena, &diags, src);
    TEST_ASSERT_NOT_NULL(prog);

    const char *names[ILSP_DEFER_FREE_MAX_CANDIDATES] = {0};
    size_t cnt = ilsp_collect_recent_heap_rc_bindings(
        prog, /*cursor_line_1=*/4, &arena, names,
        ILSP_DEFER_FREE_MAX_CANDIDATES);
    TEST_ASSERT_EQUAL_size_t(1, cnt);
    TEST_ASSERT_EQUAL_STRING("shared", names[0]);

    IronLsp_SnippetMeta meta = { .name = names[0] };
    const char *body = ilsp_snippet_render(ILSP_SNIPPET_DEFER_FREE,
                                              &meta, &arena);
    TEST_ASSERT_NOT_NULL(body);
    TEST_ASSERT_EQUAL_STRING("defer free ${1:shared}$0", body);

    iron_diaglist_free(&diags);
    iron_arena_free(&arena);
}

/* ── Test 3: no heap/rc binding → no snippet offered ──────────────── */

static void test_defer_free_no_eligible_binding(void) {
    const char *src =
        "func main() {\n"
        "    val count = 42\n"
        "    \n"
        "}\n";

    Iron_Arena    arena = iron_arena_create(64 * 1024);
    Iron_DiagList diags = iron_diaglist_create();
    Iron_Program *prog  = analyze_src(&arena, &diags, src);
    TEST_ASSERT_NOT_NULL(prog);

    const char *names[ILSP_DEFER_FREE_MAX_CANDIDATES] = {0};
    size_t cnt = ilsp_collect_recent_heap_rc_bindings(
        prog, /*cursor_line_1=*/3, &arena, names,
        ILSP_DEFER_FREE_MAX_CANDIDATES);
    TEST_ASSERT_EQUAL_size_t(0, cnt);

    iron_diaglist_free(&diags);
    iron_arena_free(&arena);
}

/* ── Test 4: multiple bindings → most-recent ranks first ──────────── */

static void test_defer_free_multi_binding_ranking(void) {
    /* Two heap/rc bindings — `r2` declared after `r1`. Expect r2 first. */
    const char *src =
        "object Buffer { val size: Int }\n"
        "object Other  { val k: Int }\n"
        "func main() {\n"
        "    val r1 = heap Buffer(1024)\n"
        "    val r2 = heap Other(7)\n"
        "    \n"
        "}\n";

    Iron_Arena    arena = iron_arena_create(64 * 1024);
    Iron_DiagList diags = iron_diaglist_create();
    Iron_Program *prog  = analyze_src(&arena, &diags, src);
    TEST_ASSERT_NOT_NULL(prog);

    const char *names[ILSP_DEFER_FREE_MAX_CANDIDATES] = {0};
    size_t cnt = ilsp_collect_recent_heap_rc_bindings(
        prog, /*cursor_line_1=*/6, &arena, names,
        ILSP_DEFER_FREE_MAX_CANDIDATES);
    TEST_ASSERT_EQUAL_size_t(2, cnt);
    TEST_ASSERT_EQUAL_STRING("r2", names[0]);  /* most-recent first */
    TEST_ASSERT_EQUAL_STRING("r1", names[1]);

    iron_diaglist_free(&diags);
    iron_arena_free(&arena);
}

/* ── Test 5: 5-candidate cap honored ─────────────────────────────── */

static void test_defer_free_cap_at_5(void) {
    /* Seven heap bindings preceding the cursor — only 5 surface. */
    const char *src =
        "object T { val k: Int }\n"
        "func main() {\n"
        "    val a = heap T(1)\n"
        "    val b = heap T(2)\n"
        "    val c = heap T(3)\n"
        "    val d = heap T(4)\n"
        "    val e = heap T(5)\n"
        "    val f = heap T(6)\n"
        "    val g = heap T(7)\n"
        "    \n"
        "}\n";

    Iron_Arena    arena = iron_arena_create(64 * 1024);
    Iron_DiagList diags = iron_diaglist_create();
    Iron_Program *prog  = analyze_src(&arena, &diags, src);
    TEST_ASSERT_NOT_NULL(prog);

    const char *names[ILSP_DEFER_FREE_MAX_CANDIDATES] = {0};
    size_t cnt = ilsp_collect_recent_heap_rc_bindings(
        prog, /*cursor_line_1=*/10, &arena, names,
        ILSP_DEFER_FREE_MAX_CANDIDATES);
    TEST_ASSERT_EQUAL_size_t(5, cnt);
    /* Most-recent five: g, f, e, d, c. */
    TEST_ASSERT_EQUAL_STRING("g", names[0]);
    TEST_ASSERT_EQUAL_STRING("f", names[1]);
    TEST_ASSERT_EQUAL_STRING("e", names[2]);
    TEST_ASSERT_EQUAL_STRING("d", names[3]);
    TEST_ASSERT_EQUAL_STRING("c", names[4]);

    iron_diaglist_free(&diags);
    iron_arena_free(&arena);
}

/* ── Test 6: cursor outside any function body → no snippet ────────── */

static void test_defer_free_cursor_outside_function(void) {
    const char *src =
        "func main() {\n"
        "    val buffer = 1\n"
        "}\n"
        "\n";

    Iron_Arena    arena = iron_arena_create(64 * 1024);
    Iron_DiagList diags = iron_diaglist_create();
    Iron_Program *prog  = analyze_src(&arena, &diags, src);
    TEST_ASSERT_NOT_NULL(prog);

    const char *names[ILSP_DEFER_FREE_MAX_CANDIDATES] = {0};
    /* Cursor at line 4 — past the closing brace, outside any function. */
    size_t cnt = ilsp_collect_recent_heap_rc_bindings(
        prog, /*cursor_line_1=*/4, &arena, names,
        ILSP_DEFER_FREE_MAX_CANDIDATES);
    TEST_ASSERT_EQUAL_size_t(0, cnt);

    iron_diaglist_free(&diags);
    iron_arena_free(&arena);
}

/* ── Test 7: PITFALL D — hostile identifier `${USER}` is escaped ──── */

static void test_defer_free_snippet_escapes_hostile_identifier(void) {
    /* The backward-scan can't produce `${USER}` from real Iron source
     * (the parser rejects `$`/`{`/`}` in identifiers), but the renderer
     * must still escape such a name if a caller hands it one — that's
     * the load-bearing PITFALL D guard. Drive the renderer directly. */
    Iron_Arena arena = iron_arena_create(16 * 1024);
    IronLsp_SnippetMeta meta = { .name = "${USER}\\bad}" };
    const char *body = ilsp_snippet_render(ILSP_SNIPPET_DEFER_FREE,
                                              &meta, &arena);
    TEST_ASSERT_NOT_NULL(body);
    /* Each of `$`, `}`, `\` becomes `\$`, `\}`, `\\` inside the placeholder. */
    TEST_ASSERT_EQUAL_STRING("defer free ${1:\\${USER\\}\\\\bad\\}}$0", body);
    iron_arena_free(&arena);
}

/* ── Driver ───────────────────────────────────────────────────────── */

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_defer_free_heap_single_binding);
    RUN_TEST(test_defer_free_rc_single_binding);
    RUN_TEST(test_defer_free_no_eligible_binding);
    RUN_TEST(test_defer_free_multi_binding_ranking);
    RUN_TEST(test_defer_free_cap_at_5);
    RUN_TEST(test_defer_free_cursor_outside_function);
    RUN_TEST(test_defer_free_snippet_escapes_hostile_identifier);
    return UNITY_END();
}
