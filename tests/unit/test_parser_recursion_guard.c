/* HARD-08: parser recursion-depth guard.
 *
 * Feeds pathological deeply-nested input (~2000 parens) and asserts the
 * parser emits IRON_ERR_PARSE_DEPTH_EXCEEDED (code 107, reserved by
 * Plan 01) rather than crashing via SIGSEGV from C-stack exhaustion.
 * Also asserts that moderate nesting (100 parens) parses cleanly — the
 * guard must not fire on any legitimate input.
 *
 * The guard lives in src/parser/parser.c as IRON_PARSER_MAX_DEPTH=1000
 * with a thin wrapper around iron_parse_expr_prec /
 * iron_parse_type_annotation / iron_parse_stmt / iron_parse_block /
 * iron_parse_decl. Every call through one of those entry points
 * increments p->recur_depth on entry and decrements on return; the
 * helper iron_parser_depth_exceeded trips at the ceiling, emits a
 * diagnostic, and the caller returns an ErrorNode.
 */

#include "unity.h"
#include "analyzer/analyzer.h"
#include "parser/parser.h"
#include "lexer/lexer.h"
#include "util/arena.h"
#include "diagnostics/diagnostics.h"
#include "stb_ds.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static Iron_Arena    arena;
static Iron_DiagList diags;

void setUp(void)    {
    arena = iron_arena_create(4 * 1024 * 1024);
    diags = iron_diaglist_create();
}
void tearDown(void) {
    iron_diaglist_free(&diags);
    iron_arena_free(&arena);
}

/* HARD-08: ~2000 open parens inside an expression trigger the
 * recursion-depth guard and emit IRON_ERR_PARSE_DEPTH_EXCEEDED; we
 * must reach this test's asserts without crashing. */
void test_pathological_nesting_emits_depth_exceeded_diagnostic(void) {
    const int N = 2000;
    size_t cap = (size_t)N * 4 + 256;
    char *buf = (char *)malloc(cap);
    TEST_ASSERT_NOT_NULL(buf);
    size_t pos = 0;
    int w;
    w = snprintf(buf + pos, cap - pos, "func f() -> Int { return ");
    pos += (size_t)w;
    for (int i = 0; i < N; i++) buf[pos++] = '(';
    buf[pos++] = '1';
    for (int i = 0; i < N; i++) buf[pos++] = ')';
    w = snprintf(buf + pos, cap - pos, "\n}\n");
    pos += (size_t)w;
    buf[pos] = '\0';

    iron_analyze_buffer(
        buf, pos, "nested.iron",
        IRON_ANALYSIS_MODE_CLI,
        &arena, &diags, NULL);

    /* Expect at least one IRON_ERR_PARSE_DEPTH_EXCEEDED diagnostic, and
     * critically: we REACHED this assertion without crashing. */
    int saw = 0;
    for (int i = 0; i < diags.count; i++) {
        if (diags.items[i].code == IRON_ERR_PARSE_DEPTH_EXCEEDED) { saw = 1; break; }
    }
    TEST_ASSERT_TRUE_MESSAGE(saw,
        "expected IRON_ERR_PARSE_DEPTH_EXCEEDED diagnostic on 2000-deep nesting");
    free(buf);
}

/* HARD-08: 100-paren input parses cleanly — the guard must not fire on
 * legitimate moderate nesting. Integration fixtures max out at depth 8
 * (grep of tests/integration `*.iron`), so 100 is a 10x-safety margin. */
void test_moderate_nesting_parses_cleanly(void) {
    const int N = 100;
    size_t cap = (size_t)N * 4 + 256;
    char *buf = (char *)malloc(cap);
    TEST_ASSERT_NOT_NULL(buf);
    size_t pos = 0;
    int w;
    w = snprintf(buf + pos, cap - pos, "func f() -> Int { return ");
    pos += (size_t)w;
    for (int i = 0; i < N; i++) buf[pos++] = '(';
    buf[pos++] = '1';
    for (int i = 0; i < N; i++) buf[pos++] = ')';
    w = snprintf(buf + pos, cap - pos, "\n}\n");
    pos += (size_t)w;
    buf[pos] = '\0';

    iron_analyze_buffer(
        buf, pos, "moderate.iron",
        IRON_ANALYSIS_MODE_CLI,
        &arena, &diags, NULL);

    for (int i = 0; i < diags.count; i++) {
        TEST_ASSERT_NOT_EQUAL_MESSAGE(
            IRON_ERR_PARSE_DEPTH_EXCEEDED,
            diags.items[i].code,
            "moderate nesting should not trip depth guard");
    }
    free(buf);
}

/* HARD-08: deeply-nested *blocks* (braces) also trip the guard — the
 * block-parse wrapper is instrumented the same way. */
void test_pathological_block_nesting_emits_depth_exceeded(void) {
    const int N = 1500;
    size_t cap = (size_t)N * 4 + 256;
    char *buf = (char *)malloc(cap);
    TEST_ASSERT_NOT_NULL(buf);
    size_t pos = 0;
    int w;
    w = snprintf(buf + pos, cap - pos, "func f() -> Int ");
    pos += (size_t)w;
    for (int i = 0; i < N; i++) buf[pos++] = '{';
    w = snprintf(buf + pos, cap - pos, " return 1 ");
    pos += (size_t)w;
    for (int i = 0; i < N; i++) buf[pos++] = '}';
    w = snprintf(buf + pos, cap - pos, "\n");
    pos += (size_t)w;
    buf[pos] = '\0';

    iron_analyze_buffer(
        buf, pos, "blocks.iron",
        IRON_ANALYSIS_MODE_CLI,
        &arena, &diags, NULL);

    int saw = 0;
    for (int i = 0; i < diags.count; i++) {
        if (diags.items[i].code == IRON_ERR_PARSE_DEPTH_EXCEEDED) { saw = 1; break; }
    }
    TEST_ASSERT_TRUE_MESSAGE(saw,
        "expected IRON_ERR_PARSE_DEPTH_EXCEEDED diagnostic on 1500 nested blocks");
    free(buf);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_pathological_nesting_emits_depth_exceeded_diagnostic);
    RUN_TEST(test_moderate_nesting_parses_cleanly);
    RUN_TEST(test_pathological_block_nesting_emits_depth_exceeded);
    return UNITY_END();
}
