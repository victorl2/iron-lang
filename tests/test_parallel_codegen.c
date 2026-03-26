/* test_parallel_codegen.c — Unity tests verifying parallel-for codegen emits
 * correct C patterns: capture struct, chunk function, pool_submit, barrier.
 *
 * Iron syntax for parallel-for: for VAR in ITERABLE parallel { BODY }
 * The 'parallel' keyword appears after the iterable, before the block.
 */

#include "unity.h"
#include "codegen/codegen.h"
#include "analyzer/resolve.h"
#include "analyzer/typecheck.h"
#include "analyzer/escape.h"
#include "analyzer/scope.h"
#include "analyzer/types.h"
#include "parser/ast.h"
#include "parser/parser.h"
#include "lexer/lexer.h"
#include "diagnostics/diagnostics.h"
#include "util/arena.h"

#include <string.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

/* ── Module-level fixtures ───────────────────────────────────────────────── */

static Iron_Arena    g_arena;
static Iron_DiagList g_diags;

void setUp(void) {
    g_arena = iron_arena_create(1024 * 1024);
    g_diags = iron_diaglist_create();
    iron_types_init(&g_arena);
}

void tearDown(void) {
    iron_arena_free(&g_arena);
    iron_diaglist_free(&g_diags);
}

/* ── Full pipeline helper: source -> generated C string ──────────────────── */

static const char *run_codegen(const char *src) {
    Iron_Lexer   l      = iron_lexer_create(src, "test.iron", &g_arena, &g_diags);
    Iron_Token  *tokens = iron_lex_all(&l);
    int          count  = 0;
    while (tokens[count].kind != IRON_TOK_EOF) count++;
    count++;
    Iron_Parser  p    = iron_parser_create(tokens, count, src, "test.iron",
                                            &g_arena, &g_diags);
    Iron_Node   *root = iron_parse(&p);
    Iron_Program *prog = (Iron_Program *)root;
    Iron_Scope   *global = iron_resolve(prog, &g_arena, &g_diags);
    iron_typecheck(prog, global, &g_arena, &g_diags);
    iron_escape_analyze(prog, global, &g_arena, &g_diags);
    return iron_codegen(prog, global, &g_arena, &g_diags);
}

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static bool str_contains(const char *haystack, const char *needle) {
    if (!haystack || !needle) return false;
    return strstr(haystack, needle) != NULL;
}

/* ── Test: parallel-for emits Iron_parallel_ctx_ typedef struct ──────────── */

void test_parallel_ctx_struct(void) {
    /* Syntax: for VAR in ITERABLE parallel { BODY } */
    const char *src =
        "func main() {\n"
        "  for i in 10 parallel {\n"
        "  }\n"
        "}\n";
    const char *c = run_codegen(src);
    TEST_ASSERT_NOT_NULL(c);
    /* Capture struct typedef must be present */
    TEST_ASSERT_TRUE_MESSAGE(
        str_contains(c, "Iron_parallel_ctx_"),
        "Expected 'Iron_parallel_ctx_' typedef struct in generated C");
    /* struct must contain start and end fields */
    TEST_ASSERT_TRUE_MESSAGE(
        str_contains(c, "int64_t start;"),
        "Expected 'int64_t start;' field in parallel ctx struct");
    TEST_ASSERT_TRUE_MESSAGE(
        str_contains(c, "int64_t end;"),
        "Expected 'int64_t end;' field in parallel ctx struct");
}

/* ── Test: chunk function has void(*)(void*) compatible signature ─────────── */

void test_parallel_chunk_fn(void) {
    const char *src =
        "func main() {\n"
        "  for i in 10 parallel {\n"
        "  }\n"
        "}\n";
    const char *c = run_codegen(src);
    TEST_ASSERT_NOT_NULL(c);
    /* Chunk function definition must use void* ctx_arg (single arg) */
    TEST_ASSERT_TRUE_MESSAGE(
        str_contains(c, "Iron_parallel_chunk_"),
        "Expected 'Iron_parallel_chunk_' function in generated C");
    TEST_ASSERT_TRUE_MESSAGE(
        str_contains(c, "void* ctx_arg"),
        "Expected 'void* ctx_arg' in chunk function signature");
    /* Must unpack start and end from context struct */
    TEST_ASSERT_TRUE_MESSAGE(
        str_contains(c, "_pctx->start"),
        "Expected '_pctx->start' in chunk function body");
    TEST_ASSERT_TRUE_MESSAGE(
        str_contains(c, "_pctx->end"),
        "Expected '_pctx->end' in chunk function body");
    /* Must free the context at end of chunk */
    TEST_ASSERT_TRUE_MESSAGE(
        str_contains(c, "free(ctx_arg)"),
        "Expected 'free(ctx_arg)' at end of chunk function");
}

/* ── Test: use-site emits Iron_pool_submit with chunk function ────────────── */

void test_parallel_pool_submit(void) {
    const char *src =
        "func main() {\n"
        "  for i in 10 parallel {\n"
        "  }\n"
        "}\n";
    const char *c = run_codegen(src);
    TEST_ASSERT_NOT_NULL(c);
    /* Pool submit call must reference Iron_global_pool */
    TEST_ASSERT_TRUE_MESSAGE(
        str_contains(c, "Iron_pool_submit(Iron_global_pool"),
        "Expected 'Iron_pool_submit(Iron_global_pool' in generated C");
    /* Context must be malloc'd per chunk */
    TEST_ASSERT_TRUE_MESSAGE(
        str_contains(c, "malloc(sizeof(Iron_parallel_ctx_"),
        "Expected per-chunk malloc in generated C");
}

/* ── Test: use-site emits Iron_pool_barrier after submit loop ─────────────── */

void test_parallel_barrier(void) {
    const char *src =
        "func main() {\n"
        "  for i in 10 parallel {\n"
        "  }\n"
        "}\n";
    const char *c = run_codegen(src);
    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_TRUE_MESSAGE(
        str_contains(c, "Iron_pool_barrier"),
        "Expected 'Iron_pool_barrier' call after submit loop");
}

/* ── Test: captured outer variable appears in ctx struct ─────────────────── */

void test_parallel_capture_in_struct(void) {
    /* Use a var-decl and reference it in the parallel body */
    const char *src =
        "func main() {\n"
        "  var buf: Int = 0\n"
        "  for i in 4 parallel {\n"
        "    buf = i\n"
        "  }\n"
        "}\n";
    const char *c = run_codegen(src);
    TEST_ASSERT_NOT_NULL(c);
    /* Captured 'buf' should appear as cap_buf in the struct */
    TEST_ASSERT_TRUE_MESSAGE(
        str_contains(c, "cap_buf"),
        "Expected 'cap_buf' capture field for outer variable 'buf'");
}

/* ── Main ─────────────────────────────────────────────────────────────────── */

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_parallel_ctx_struct);
    RUN_TEST(test_parallel_chunk_fn);
    RUN_TEST(test_parallel_pool_submit);
    RUN_TEST(test_parallel_barrier);
    RUN_TEST(test_parallel_capture_in_struct);
    return UNITY_END();
}
