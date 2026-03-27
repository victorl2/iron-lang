/* test_comptime.c — Unity tests for the Iron comptime evaluation pass.
 *
 * Tests cover:
 *   - Simple integer arithmetic: 2 + 3 => INT_LIT "5"
 *   - Recursive function (fib(10)) => INT_LIT "55"
 *   - Step limit enforcement (infinite recursion) => E0230
 *   - Restriction: heap inside comptime => E0231
 *   - Boolean expression: true and false => BOOL_LIT false
 *   - String literal: "hello" => STRING_LIT "hello"
 *   - read_file builtin: embeds file contents at compile time
 *   - read_file not found: emits compile error
 */

#include "unity.h"
#include "analyzer/analyzer.h"
#include "analyzer/resolve.h"
#include "analyzer/typecheck.h"
#include "analyzer/scope.h"
#include "analyzer/types.h"
#include "comptime/comptime.h"
#include "parser/ast.h"
#include "parser/parser.h"
#include "lexer/lexer.h"
#include "diagnostics/diagnostics.h"
#include "util/arena.h"

#include <string.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

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

/* ── Parse + full analysis helper ────────────────────────────────────────── */

/* Runs lex -> parse -> iron_analyze (which includes comptime evaluation). */
static Iron_Program *run_analysis(const char *src) {
    Iron_Lexer   l      = iron_lexer_create(src, "test.iron", &g_arena, &g_diags);
    Iron_Token  *tokens = iron_lex_all(&l);
    int          count  = 0;
    while (tokens[count].kind != IRON_TOK_EOF) count++;
    count++;  /* include EOF */
    Iron_Parser  p    = iron_parser_create(tokens, count, src, "test.iron",
                                            &g_arena, &g_diags);
    Iron_Node   *root = iron_parse(&p);
    Iron_Program *prog = (Iron_Program *)root;
    iron_analyze(prog, &g_arena, &g_diags, NULL, src, strlen(src), false);
    return prog;
}

/* Check if a specific error code was emitted */
static bool has_error(int code) {
    for (int i = 0; i < g_diags.count; i++) {
        if (g_diags.items[i].code == code) return true;
    }
    return false;
}

/* Find the init expression of the first top-level val/var declaration.
 * After comptime evaluation, the init should be a literal node. */
static Iron_Node *get_top_level_val_init(Iron_Program *prog) {
    if (!prog) return NULL;
    for (int i = 0; i < prog->decl_count; i++) {
        Iron_Node *decl = prog->decls[i];
        if (decl->kind == IRON_NODE_VAL_DECL) {
            return ((Iron_ValDecl *)decl)->init;
        }
        if (decl->kind == IRON_NODE_VAR_DECL) {
            return ((Iron_VarDecl *)decl)->init;
        }
    }
    return NULL;
}

/* ── Tests ───────────────────────────────────────────────────────────────── */

/* test_comptime_int_arithmetic:
 * val X = comptime (2 + 3)
 * After evaluation, X's init should be INT_LIT "5".
 * Note: parentheses are required because comptime has PREC_UNARY precedence,
 * so "comptime 2 + 3" parses as "(comptime 2) + 3". */
void test_comptime_int_arithmetic(void) {
    const char *src =
        "val X = comptime (2 + 3)\n";

    Iron_Program *prog = run_analysis(src);

    /* No errors expected */
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);

    Iron_Node *init = get_top_level_val_init(prog);
    TEST_ASSERT_NOT_NULL(init);

    /* Should have been replaced with INT_LIT */
    TEST_ASSERT_EQUAL_INT(IRON_NODE_INT_LIT, (int)init->kind);
    Iron_IntLit *lit = (Iron_IntLit *)init;
    TEST_ASSERT_NOT_NULL(lit->value);
    TEST_ASSERT_EQUAL_STRING("5", lit->value);
}

/* test_comptime_fibonacci:
 * func fib(n: Int) -> Int { if n <= 1 { return n } return fib(n-1) + fib(n-2) }
 * val X = comptime fib(10)
 * After evaluation, X's init should be INT_LIT "55". */
void test_comptime_fibonacci(void) {
    const char *src =
        "func fib(n: Int) -> Int {\n"
        "  if n <= 1 { return n }\n"
        "  return fib(n - 1) + fib(n - 2)\n"
        "}\n"
        "val X = comptime fib(10)\n";

    Iron_Program *prog = run_analysis(src);

    /* No errors expected */
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);

    Iron_Node *init = get_top_level_val_init(prog);
    TEST_ASSERT_NOT_NULL(init);

    TEST_ASSERT_EQUAL_INT(IRON_NODE_INT_LIT, (int)init->kind);
    Iron_IntLit *lit = (Iron_IntLit *)init;
    TEST_ASSERT_NOT_NULL(lit->value);
    TEST_ASSERT_EQUAL_STRING("55", lit->value);
}

/* test_comptime_step_limit:
 * func infinite(n: Int) -> Int { return infinite(n) }
 * val X = comptime infinite(1)
 * Should emit IRON_ERR_COMPTIME_STEP_LIMIT (E0230). */
void test_comptime_step_limit(void) {
    const char *src =
        "func infinite(n: Int) -> Int {\n"
        "  return infinite(n)\n"
        "}\n"
        "val X = comptime infinite(1)\n";

    run_analysis(src);

    TEST_ASSERT_TRUE(has_error(IRON_ERR_COMPTIME_STEP_LIMIT));
}

/* test_comptime_no_heap:
 * Tests that the IRON_ERR_COMPTIME_RESTRICTION error is emitted when a
 * comptime expression contains a heap node.
 *
 * We test this by directly invoking the comptime evaluator on a synthetically
 * constructed AST with a HEAP node, bypassing the full pipeline (which would
 * run escape analysis first and block comptime). */
void test_comptime_no_heap(void) {
    /* Build a minimal comptime context */
    Iron_Scope *global = iron_scope_create(&g_arena, NULL, IRON_SCOPE_GLOBAL);
    iron_types_init(&g_arena);

    Iron_ComptimeCtx ctx;
    Iron_Span zero_span = iron_span_make("test.iron", 1, 1, 1, 1);
    ctx.arena           = &g_arena;
    ctx.diags           = &g_diags;
    ctx.global_scope    = global;
    ctx.steps           = 0;
    ctx.step_limit      = 1000000;
    ctx.call_stack      = NULL;
    ctx.call_spans      = NULL;
    ctx.call_depth      = 0;
    ctx.source_file_dir = NULL;
    ctx.had_error       = false;
    ctx.local_frames    = NULL;
    ctx.frame_depth     = 0;
    ctx.had_return      = false;
    ctx.return_val      = NULL;

    /* Build a synthetic HEAP node */
    Iron_HeapExpr *heap_node = iron_arena_alloc(&g_arena, sizeof(Iron_HeapExpr),
                                                  _Alignof(Iron_HeapExpr));
    heap_node->span          = zero_span;
    heap_node->kind          = IRON_NODE_HEAP;
    heap_node->resolved_type = NULL;
    heap_node->inner         = NULL;
    heap_node->auto_free     = false;
    heap_node->escapes       = false;

    /* Evaluate — should emit RESTRICTION error */
    iron_comptime_eval_expr(&ctx, (Iron_Node *)heap_node);

    TEST_ASSERT_TRUE(has_error(IRON_ERR_COMPTIME_RESTRICTION));
}

/* test_comptime_bool:
 * val X = comptime (true and false)
 * After evaluation, X's init should be BOOL_LIT false.
 * Note: parentheses are required because comptime has PREC_UNARY precedence,
 * so "comptime true and false" parses as "(comptime true) and false". */
void test_comptime_bool(void) {
    const char *src =
        "val X = comptime (true and false)\n";

    Iron_Program *prog = run_analysis(src);

    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);

    Iron_Node *init = get_top_level_val_init(prog);
    TEST_ASSERT_NOT_NULL(init);

    TEST_ASSERT_EQUAL_INT(IRON_NODE_BOOL_LIT, (int)init->kind);
    Iron_BoolLit *lit = (Iron_BoolLit *)init;
    TEST_ASSERT_FALSE(lit->value);
}

/* test_comptime_string:
 * val X = comptime "hello"
 * After evaluation, X's init should be STRING_LIT "hello". */
void test_comptime_string(void) {
    const char *src =
        "val X = comptime \"hello\"\n";

    Iron_Program *prog = run_analysis(src);

    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);

    Iron_Node *init = get_top_level_val_init(prog);
    TEST_ASSERT_NOT_NULL(init);

    TEST_ASSERT_EQUAL_INT(IRON_NODE_STRING_LIT, (int)init->kind);
    Iron_StringLit *lit = (Iron_StringLit *)init;
    TEST_ASSERT_NOT_NULL(lit->value);
    TEST_ASSERT_EQUAL_STRING("hello", lit->value);
}

/* ── Analysis helper with custom source_file_dir ─────────────────────────── */

static Iron_Program *run_analysis_with_dir(const char *src,
                                            const char *source_file_dir) {
    Iron_Lexer   l      = iron_lexer_create(src, "test.iron", &g_arena, &g_diags);
    Iron_Token  *tokens = iron_lex_all(&l);
    int          count  = 0;
    while (tokens[count].kind != IRON_TOK_EOF) count++;
    count++;  /* include EOF */
    Iron_Parser  p    = iron_parser_create(tokens, count, src, "test.iron",
                                            &g_arena, &g_diags);
    Iron_Node   *root = iron_parse(&p);
    Iron_Program *prog = (Iron_Program *)root;
    iron_analyze(prog, &g_arena, &g_diags,
                 source_file_dir, src, strlen(src), false);
    return prog;
}

/* ── Test: read_file embeds file contents as a string literal ────────────── */

void test_comptime_read_file(void) {
    /* Write a temp file with known contents */
    char tmp_dir[] = "/tmp/iron_test_XXXXXX";
    char *dir = mkdtemp(tmp_dir);
    TEST_ASSERT_NOT_NULL_MESSAGE(dir, "mkdtemp failed");

    char tmp_path[256];
    snprintf(tmp_path, sizeof(tmp_path), "%s/data.txt", dir);
    FILE *f = fopen(tmp_path, "w");
    TEST_ASSERT_NOT_NULL_MESSAGE(f, "fopen failed for temp file");
    fputs("hello world", f);
    fclose(f);

    /* Parse source that uses read_file with the filename only */
    const char *src =
        "val CONTENT = comptime read_file(\"data.txt\")\n"
        "func main() { println(CONTENT) }\n";

    Iron_Program *prog = run_analysis_with_dir(src, dir);

    /* Expect no errors */
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, g_diags.error_count,
                                  "expected no errors from read_file");

    /* The val init should have been replaced with STRING_LIT "hello world" */
    Iron_Node *init = get_top_level_val_init(prog);
    TEST_ASSERT_NOT_NULL(init);
    TEST_ASSERT_EQUAL_INT(IRON_NODE_STRING_LIT, (int)init->kind);
    Iron_StringLit *lit = (Iron_StringLit *)init;
    TEST_ASSERT_NOT_NULL(lit->value);
    TEST_ASSERT_EQUAL_STRING("hello world", lit->value);

    /* Clean up */
    remove(tmp_path);
    rmdir(dir);
}

/* ── Test: read_file with nonexistent path emits compile error ───────────── */

void test_comptime_read_file_not_found(void) {
    const char *src =
        "val X = comptime read_file(\"nonexistent_file_that_does_not_exist.txt\")\n"
        "func main() {}\n";

    /* Pass NULL for source_file_dir — file lookup will be in cwd / not found */
    run_analysis_with_dir(src, "");

    /* Expect a compile error (IRON_ERR_COMPTIME_ERROR = E0229) */
    TEST_ASSERT_TRUE_MESSAGE(g_diags.error_count > 0,
                             "expected compile error for missing file");
}

/* ── Test runner ─────────────────────────────────────────────────────────── */

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_comptime_int_arithmetic);
    RUN_TEST(test_comptime_fibonacci);
    RUN_TEST(test_comptime_step_limit);
    RUN_TEST(test_comptime_no_heap);
    RUN_TEST(test_comptime_bool);
    RUN_TEST(test_comptime_string);
    RUN_TEST(test_comptime_read_file);
    RUN_TEST(test_comptime_read_file_not_found);
    return UNITY_END();
}
