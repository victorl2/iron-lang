/* test_ir_lower.c — Unity tests for the AST-to-IR lowering pass (Phase 8).
 *
 * Tests verify that iron_ir_lower() produces correct IR for Iron programs.
 * Wave 0: placeholder scaffold — just verifies the test binary links and runs.
 * Wave 1+: expression lowering tests added in Plans 08-01 onward.
 */

#include "unity.h"
#include "ir/lower.h"
#include "ir/ir.h"
#include "ir/verify.h"
#include "ir/print.h"
#include "parser/ast.h"
#include "analyzer/types.h"
#include "diagnostics/diagnostics.h"
#include "util/arena.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>

/* ── Fixtures ────────────────────────────────────────────────────────────── */

static Iron_Arena g_ir_arena;
static Iron_DiagList g_diags;

void setUp(void) {
    g_ir_arena = iron_arena_create(65536);
    iron_types_init(&g_ir_arena);
    g_diags = iron_diaglist_create();
}

void tearDown(void) {
    iron_diaglist_free(&g_diags);
    iron_arena_free(&g_ir_arena);
}

/* ── Snapshot comparison helper ─────────────────────────────────────────── */

/* Suppress unused-function warnings: helpers are used by wave 1+ tests. */
#if defined(__GNUC__) || defined(__clang__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wunused-function"
#endif

static bool compare_snapshot(const char *ir_output, const char *expected_path) {
    FILE *f = fopen(expected_path, "r");
    if (!f) { printf("Snapshot file not found: %s\n", expected_path); return false; }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *expected = malloc((size_t)len + 1);
    fread(expected, 1, (size_t)len, f);
    expected[len] = '\0';
    fclose(f);
    bool match = strcmp(ir_output, expected) == 0;
    if (!match) { printf("Snapshot mismatch.\nGot:\n%s\nExpected:\n%s\n", ir_output, expected); }
    free(expected);
    return match;
}

/* ── AST construction helpers ───────────────────────────────────────────── */

static Iron_Span test_span(void) {
    return iron_span_make("test.iron", 1, 1, 1, 1);
}

/* Build a minimal Iron_IntLit node (value is a string like "42") */
static Iron_Node *make_int(Iron_Arena *arena, const char *value) {
    Iron_IntLit *n = ARENA_ALLOC(arena, Iron_IntLit);
    memset(n, 0, sizeof(*n));
    n->span = test_span();
    n->kind = IRON_NODE_INT_LIT;
    n->value = value;
    n->resolved_type = iron_type_make_primitive(IRON_TYPE_INT);
    return (Iron_Node *)n;
}

/* Build a minimal Iron_BoolLit node */
static Iron_Node *make_bool(Iron_Arena *arena, bool value) {
    Iron_BoolLit *n = ARENA_ALLOC(arena, Iron_BoolLit);
    memset(n, 0, sizeof(*n));
    n->span = test_span();
    n->kind = IRON_NODE_BOOL_LIT;
    n->value = value;
    n->resolved_type = iron_type_make_primitive(IRON_TYPE_BOOL);
    return (Iron_Node *)n;
}

/* Build a minimal Iron_FuncDecl node with an empty body block */
static Iron_FuncDecl *make_func_decl(Iron_Arena *arena, const char *name,
                                      Iron_Type *return_type) {
    Iron_Block *body = ARENA_ALLOC(arena, Iron_Block);
    memset(body, 0, sizeof(*body));
    body->span = test_span();
    body->kind = IRON_NODE_BLOCK;
    body->stmts = NULL;
    body->stmt_count = 0;

    Iron_FuncDecl *fd = ARENA_ALLOC(arena, Iron_FuncDecl);
    memset(fd, 0, sizeof(*fd));
    fd->span = test_span();
    fd->kind = IRON_NODE_FUNC_DECL;
    fd->name = name;
    fd->params = NULL;
    fd->param_count = 0;
    fd->body = (Iron_Node *)body;
    fd->resolved_return_type = return_type;
    return fd;
}

#if defined(__GNUC__) || defined(__clang__)
#  pragma GCC diagnostic pop
#endif

/* ── Wave 0: placeholder test ───────────────────────────────────────────── */

void test_lower_placeholder(void) {
    /* This test just verifies that the test binary links and runs correctly.
     * Real lowering tests are added in Tasks 1-3 of Plan 08-01. */
    TEST_PASS();
}

/* ── main ───────────────────────────────────────────────────────────────── */

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_lower_placeholder);
    return UNITY_END();
}
