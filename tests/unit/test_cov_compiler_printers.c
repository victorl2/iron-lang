/*
 * Phase 69 Plan 04 (COV-05): targeted coverage tests for the compiler-side
 * printer modules (parser AST / HIR / LIR) plus the web_await_check pass.
 *
 *   - src/parser/printer.c          (baseline 31.98% line — 213 / 666)
 *   - src/hir/hir_print.c           (baseline 38.33% line — 184 / 480)
 *   - src/lir/print.c               (baseline 42.61% line — 219 / 514)
 *   - src/analyzer/web_await_check.c (baseline 49.66% line — 73 / 147)
 *
 * Motivating incident: Phase 69 Plan 03 baseline flagged all four files
 * below the 50% line-coverage floor mandated by COV-05.
 *
 *   - parser/printer.c: tests/unit/test_printer.c exercises only 7 shapes.
 *     The AST printer has ~50 node-kind case arms, so coverage stalls
 *     around the "common shapes" ceiling.
 *   - hir/hir_print.c and lir/print.c: no dedicated unit test exists.
 *     The existing coverage came entirely from --verbose CLI invocations
 *     in the integration test suite, which exercise only the simplest
 *     program shapes.
 *   - analyzer/web_await_check.c: tests/unit/test_web_await_check.c
 *     exercises the top-level cases but misses the call-descent
 *     branch (scan_node → IRON_NODE_CALL with an identifier callee
 *     that resolves to a function decl in the map), and several of
 *     the statement-container arms (IRON_NODE_IF elif/else, WHILE,
 *     FOR iterable, RETURN, ASSIGN, VAL_DECL init).
 *
 * Strategy: drive a single moderately-large Iron source through the full
 * compile pipeline (parse → analyze → hir_lower → hir_to_lir) and call
 * iron_print_ast / iron_hir_print / iron_lir_print on the result. The
 * source is deliberately written to exercise as many distinct node shapes
 * as possible — object decls, functions, if/elif/else, while, for,
 * match, binary ops, unary ops, field access, method calls, string
 * interpolation, enum construction, array literals — so every printer
 * hits the largest uncovered block with a single walk.
 *
 * For web_await_check we add a second small test that constructs a
 * program with `await` reachable via a `helper()` call from main (on
 * target=web). That exercises the IRON_NODE_CALL + identifier-callee
 * + dict-lookup descent path, plus the IRON_NODE_IF / IRON_NODE_WHILE /
 * IRON_NODE_RETURN / IRON_NODE_VAL_DECL walker arms, which the existing
 * test_web_await_check.c leaves cold.
 *
 * Scope: NOT 100% coverage — 50% floor per CONTEXT.md "50% floor, not
 * 100% ceiling". We don't try to hit every single case arm; we aim to
 * push each file above 50% by exercising breadth, not depth.
 */

#include "unity.h"
#include "lexer/lexer.h"
#include "parser/parser.h"
#include "parser/ast.h"
#include "parser/printer.h"
#include "analyzer/analyzer.h"
#include "analyzer/web_await_check.h"
#include "analyzer/scope.h"
#include "analyzer/types.h"
#include "hir/hir.h"
#include "hir/hir_lower.h"
#include "hir/hir_to_lir.h"
#include "lir/lir.h"
#include "lir/print.h"
#include "diagnostics/diagnostics.h"
#include "util/arena.h"
#include "cli/build.h"

#include <string.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>

/* ── Module-level fixtures ───────────────────────────────────────────────── */

static Iron_Arena    g_arena;
static Iron_Arena    g_lir_arena;
static Iron_DiagList g_diags;

void setUp(void) {
    g_arena     = iron_arena_create(1024 * 1024);
    g_lir_arena = iron_arena_create(1024 * 1024);
    g_diags     = iron_diaglist_create();
    iron_types_init(&g_arena);
}

void tearDown(void) {
    iron_arena_free(&g_arena);
    iron_arena_free(&g_lir_arena);
    iron_diaglist_free(&g_diags);
}

/* ── Compile + lower helper ──────────────────────────────────────────────── */

/* Lex → parse → analyze → hir_lower. Returns the HIR module (or NULL on
 * compile failure); the AST and global scope come back via out-params so
 * the caller can feed them into hir_to_lir. */
static IronHIR_Module *compile_to_hir(const char *src,
                                       Iron_Program **out_prog,
                                       Iron_Scope **out_scope) {
    Iron_Lexer   l      = iron_lexer_create(src, "test.iron", &g_arena, &g_diags);
    Iron_Token  *tokens = iron_lex_all(&l);
    int          count  = 0;
    while (tokens[count].kind != IRON_TOK_EOF) count++;
    count++;

    Iron_Parser   p   = iron_parser_create(tokens, count, src, "test.iron",
                                            &g_arena, &g_diags);
    p.v3_strict_mode = false;  /* dense fixture pre-dates v3-strict default */
    Iron_Node    *root = iron_parse(&p);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, g_diags.error_count,
                                   "parse errors in compile_to_hir");

    Iron_Program *prog = (Iron_Program *)root;
    Iron_AnalyzeResult res = iron_analyze(prog, &g_arena, &g_diags,
                                           NULL, src, strlen(src),
                                           /*force_comptime*/ false,
                                           IRON_TARGET_NATIVE);
    TEST_ASSERT_FALSE_MESSAGE(res.has_errors,
                               "analyze errors in compile_to_hir");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, g_diags.error_count,
                                   "diag errors after analyze");

    IronHIR_Module *hir = iron_hir_lower(prog, res.global_scope, NULL,
                                          &g_diags);
    TEST_ASSERT_NOT_NULL_MESSAGE(hir, "iron_hir_lower returned NULL");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, g_diags.error_count,
                                   "diag errors after hir_lower");

    *out_prog  = prog;
    *out_scope = res.global_scope;
    return hir;
}

/* ── Test 1: broad-shape AST pretty-printer coverage ─────────────────────── */

/* A deliberately dense program that hits many parser-printer case arms:
 *   - val/var declarations with and without type annotations
 *   - object decl with fields
 *   - enum decl with two variants
 *   - func with params and return type
 *   - if/elif/else
 *   - while loop
 *   - for loop (range)
 *   - binary expressions, comparisons, unary negation
 *   - field access
 *   - array literal
 *   - string interpolation
 *   - return with value
 * We then call iron_print_ast on the whole program and check that
 * representative tokens appear in the output. This alone pushes
 * parser/printer.c coverage over the 50% line.
 */
static const char *k_dense_src =
    "interface Greeter {\n"
    "  func greet() -> String\n"
    "}\n"
    "\n"
    "object Point impl Greeter {\n"
    "  var x: Int\n"
    "  var y: Int\n"
    "}\n"
    "\n"
    "func Point.greet() -> String {\n"
    "  return \"hi\"\n"
    "}\n"
    "\n"
    "enum Color {\n"
    "  Red,\n"
    "  Blue,\n"
    "}\n"
    "\n"
    "func clamp(v: Int, lo: Int, hi: Int) -> Int {\n"
    "  if v < lo {\n"
    "    return lo\n"
    "  } elif v > hi {\n"
    "    return hi\n"
    "  } else {\n"
    "    return v\n"
    "  }\n"
    "}\n"
    "\n"
    "func sum_range(n: Int) -> Int {\n"
    "  var total = 0\n"
    "  var i = 0\n"
    "  while i < n {\n"
    "    total = total + i\n"
    "    i = i + 1\n"
    "  }\n"
    "  return total\n"
    "}\n"
    "\n"
    "func describe(c: Color) -> String {\n"
    "  match c {\n"
    "    Color.Red  -> return \"red\"\n"
    "    Color.Blue -> return \"blue\"\n"
    "  }\n"
    "  return \"?\"\n"
    "}\n"
    "\n"
    "func main() {\n"
    "  val p = Point(1, 2)\n"
    "  val msg = p.greet()\n"
    "  val c = Color.Red\n"
    "  val d = describe(c)\n"
    "  val xs = [10, 20, 30, 40]\n"
    "  val first = xs[0]\n"
    "  val clamped = clamp(-5, 0, 100)\n"
    "  val total = sum_range(10)\n"
    "  val name = \"world\"\n"
    "  val greeting = \"hello {name}\"\n"
    "  val pi = 3.14\n"
    "  val flag = true\n"
    "  val neg = -clamped\n"
    "  val and_v = flag and true\n"
    "  val or_v = flag or false\n"
    "  var n = 10\n"
    "  for i in 3 {\n"
    "    n = n + 0\n"
    "  }\n"
    "  println(greeting)\n"
    "}\n";

void test_print_ast_dense_program(void) {
    Iron_Lexer   l      = iron_lexer_create(k_dense_src, "test.iron",
                                             &g_arena, &g_diags);
    Iron_Token  *tokens = iron_lex_all(&l);
    int          count  = 0;
    while (tokens[count].kind != IRON_TOK_EOF) count++;
    count++;
    Iron_Parser  p = iron_parser_create(tokens, count, k_dense_src,
                                         "test.iron", &g_arena, &g_diags);
    p.v3_strict_mode = false;  /* dense fixture pre-dates v3-strict default */
    Iron_Node   *root = iron_parse(&p);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);

    /* AST printer runs on the parser's output directly — no analysis needed. */
    char *printed = iron_print_ast(root, &g_arena);
    TEST_ASSERT_NOT_NULL(printed);

    /* Sample several shapes: if present in output, at least the
     * corresponding case arms in printer.c fired. */
    TEST_ASSERT_NOT_NULL(strstr(printed, "object Point"));
    TEST_ASSERT_NOT_NULL(strstr(printed, "enum Color"));
    TEST_ASSERT_NOT_NULL(strstr(printed, "func clamp"));
    TEST_ASSERT_NOT_NULL(strstr(printed, "func sum_range"));
    TEST_ASSERT_NOT_NULL(strstr(printed, "func main"));
    TEST_ASSERT_NOT_NULL(strstr(printed, "if"));
    TEST_ASSERT_NOT_NULL(strstr(printed, "elif"));
    TEST_ASSERT_NOT_NULL(strstr(printed, "else"));
    TEST_ASSERT_NOT_NULL(strstr(printed, "while"));
    TEST_ASSERT_NOT_NULL(strstr(printed, "return"));
    /* Iron's printer emits "var" for var declarations */
    TEST_ASSERT_NOT_NULL(strstr(printed, "var"));
    TEST_ASSERT_NOT_NULL(strstr(printed, "val"));
}

/* ── Test 2: HIR + LIR pretty-printer coverage ───────────────────────────── */

/* A second dense program — kept separate from test 1 because compile_to_hir
 * actually runs the analyzer, which requires more plumbing than a bare
 * parser run. The source here is a subset of k_dense_src that hir_lower
 * and hir_to_lir can handle cleanly (no enums or interpolated strings
 * that might stress HIR lowering). Complete-enough to exercise the main
 * HIR/LIR printer case arms: func body, let stmts, binop/unop exprs,
 * if/while, return, calls, literal kinds. */
static const char *k_hir_src =
    "object Pair {\n"
    "  var a: Int\n"
    "  var b: Int\n"
    "}\n"
    "\n"
    "func Pair.sum() -> Int {\n"
    "  return self.a + self.b\n"
    "}\n"
    "\n"
    "func add(a: Int, b: Int) -> Int {\n"
    "  return a + b\n"
    "}\n"
    "\n"
    "func fib(n: Int) -> Int {\n"
    "  if n < 2 {\n"
    "    return n\n"
    "  } else {\n"
    "    return fib(n - 1) + fib(n - 2)\n"
    "  }\n"
    "}\n"
    "\n"
    "func arith(x: Int, y: Int) -> Int {\n"
    "  -- Exercise many binop arms: *, /, %, ==, !=, <=, >=, &, |, ^, <<, >>\n"
    "  val m = x * y\n"
    "  val d = x / y\n"
    "  val r = x % y\n"
    "  val eq = x == y\n"
    "  val ne = x != y\n"
    "  val le = x <= y\n"
    "  val ge = x >= y\n"
    "  val ba = x & y\n"
    "  val bo = x | y\n"
    "  val bx = x ^ y\n"
    "  val sl = x << 2\n"
    "  val sr = x >> 1\n"
    "  val not_b = not eq\n"
    "  return m + d + r + ba + bo + bx + sl + sr\n"
    "}\n"
    "\n"
    "func main() {\n"
    "  val a = 5\n"
    "  val b = 7\n"
    "  val s = add(a, b)\n"
    "  val f = fib(6)\n"
    "  val t = arith(10, 3)\n"
    "  val p = Pair(1, 2)\n"
    "  val ps = p.sum()\n"
    "  val xs = [10, 20, 30, 40]\n"
    "  val first = xs[0]\n"
    "  val f_val = 3.14\n"
    "  val b_val = true\n"
    "  val name = \"hello\"\n"
    "  var i = 0\n"
    "  while i < 3 {\n"
    "    i = i + 1\n"
    "  }\n"
    "  val neg = -a\n"
    "  val cond = a > b\n"
    "}\n";

void test_print_hir_and_lir_dense_program(void) {
    Iron_Program *prog  = NULL;
    Iron_Scope   *scope = NULL;
    IronHIR_Module *hir = compile_to_hir(k_hir_src, &prog, &scope);

    /* HIR printer — this is the first dedicated coverage of iron_hir_print. */
    char *hir_text = iron_hir_print(hir);
    TEST_ASSERT_NOT_NULL(hir_text);
    /* Should contain func names from the source. */
    TEST_ASSERT_NOT_NULL(strstr(hir_text, "FuncDecl(add)"));
    TEST_ASSERT_NOT_NULL(strstr(hir_text, "FuncDecl(fib)"));
    TEST_ASSERT_NOT_NULL(strstr(hir_text, "FuncDecl(main)"));
    free(hir_text);

    /* Lower to LIR and run the LIR printer. */
    IronLIR_Module *lir = iron_hir_to_lir(hir, prog, scope,
                                           &g_lir_arena, &g_diags);
    TEST_ASSERT_NOT_NULL(lir);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);

    char *lir_text_ann = iron_lir_print(lir, /*show_annotations*/ true);
    TEST_ASSERT_NOT_NULL(lir_text_ann);
    /* LIR module emits "; Module: ..." header and per-func blocks. */
    TEST_ASSERT_NOT_NULL(strstr(lir_text_ann, "Module:"));
    free(lir_text_ann);

    /* And the show_annotations=false branch */
    char *lir_text_plain = iron_lir_print(lir, /*show_annotations*/ false);
    TEST_ASSERT_NOT_NULL(lir_text_plain);
    free(lir_text_plain);

    iron_hir_module_destroy(hir);
}

/* ── Test 3: iron_hir_print NULL-module guard ────────────────────────────── */

void test_print_hir_null_module(void) {
    char *out = iron_hir_print(NULL);
    TEST_ASSERT_NULL(out);
}

/* ── Test 4: iron_lir_print NULL-module guard ────────────────────────────── */

void test_print_lir_null_module(void) {
    char *out = iron_lir_print(NULL, false);
    TEST_ASSERT_NULL(out);
}

/* ── Test 5: web_await_check deeper BFS coverage ─────────────────────────── */

/* The existing tests/unit/test_web_await_check.c only exercises:
 *   (a) await directly in main on target=web     => E0501
 *   (b) same program on target=native            => no error
 *   (c) no-await program on target=web           => no error
 *
 * What it leaves cold is the scan_node call-descent branch:
 *   func main() { helper() }
 *   func helper() { await 0 }
 * On target=web the pass should follow main→helper via the IRON_NODE_CALL
 * arm (identifier callee + map lookup), detect the await inside helper,
 * and emit E0501 with the call chain recorded in the message. This also
 * exercises the IRON_NODE_VAL_DECL, IRON_NODE_BLOCK, and identifier-lookup
 * arms of scan_node. Easiest way to build this reliably is via the real
 * parser (compile_to_hir isn't needed — resolve+typecheck are enough).
 *
 * We hand-roll a tiny parse-then-call-web-await pipeline because the
 * tight coupling between iron_analyze and the web_await_check pass would
 * hide this branch (iron_analyze already runs web_await_check when
 * target=IRON_TARGET_WEB, so we'd double-call). Instead: parse, do a
 * minimal resolve to satisfy AST invariants, then drive web_await_check
 * directly with target=web.
 */
static const char *k_await_chain_src =
    "func helper() {\n"
    "  await 0\n"
    "}\n"
    "\n"
    "func main() {\n"
    "  val x = 1\n"
    "  if x > 0 {\n"
    "    helper()\n"
    "  } else {\n"
    "    helper()\n"
    "  }\n"
    "}\n";

void test_web_await_check_follows_call_chain_to_helper(void) {
    Iron_Lexer   l      = iron_lexer_create(k_await_chain_src, "test.iron",
                                             &g_arena, &g_diags);
    Iron_Token  *tokens = iron_lex_all(&l);
    int          count  = 0;
    while (tokens[count].kind != IRON_TOK_EOF) count++;
    count++;

    Iron_Parser   p    = iron_parser_create(tokens, count, k_await_chain_src,
                                             "test.iron", &g_arena, &g_diags);
    Iron_Node    *root = iron_parse(&p);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);

    /* Drive web_await_check directly on the bare AST with target=web.
     * The pass only needs the decl table and call-name identifiers
     * (which come from the parser) — it doesn't consult resolved types. */
    iron_web_await_check((Iron_Program *)root, &g_arena, &g_diags,
                         IRON_TARGET_WEB, NULL);

    /* Expect E0501 because await is reachable from main via helper(). */
    bool found_501 = false;
    for (int i = 0; i < g_diags.count; i++) {
        if (g_diags.items[i].code == 501) { found_501 = true; break; }
    }
    TEST_ASSERT_TRUE_MESSAGE(found_501,
                              "web_await_check must flag await reachable "
                              "via helper() call chain on target=web");

    /* And the chain string should mention both `main` and `helper`. */
    bool chain_ok = false;
    for (int i = 0; i < g_diags.count; i++) {
        const char *m = g_diags.items[i].message;
        if (m && strstr(m, "main") && strstr(m, "helper")) {
            chain_ok = true;
            break;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(chain_ok,
                              "E0501 message must record main→helper chain");
}

/* ── Test 6: web_await_check WHILE / RETURN / ASSIGN arms ────────────────── */

/* A separate input exercising the container-node descent arms: WHILE
 * body, ASSIGN expr, RETURN value. No await anywhere, so the expected
 * outcome is: no E0501 emitted. Main value here is coverage of the
 * dead-leaf descent paths (they recurse into children that cannot
 * possibly contain an await, but the recursion still adds lines). */
static const char *k_container_arms_src =
    "func work(n: Int) -> Int {\n"
    "  var total = 0\n"
    "  var i = 0\n"
    "  while i < n {\n"
    "    total = total + i\n"
    "    i = i + 1\n"
    "  }\n"
    "  return total\n"
    "}\n"
    "\n"
    "func main() {\n"
    "  val result = work(5)\n"
    "}\n";

void test_web_await_check_container_arms_no_await(void) {
    Iron_Lexer   l      = iron_lexer_create(k_container_arms_src,
                                             "test.iron", &g_arena, &g_diags);
    Iron_Token  *tokens = iron_lex_all(&l);
    int          count  = 0;
    while (tokens[count].kind != IRON_TOK_EOF) count++;
    count++;

    Iron_Parser  p = iron_parser_create(tokens, count, k_container_arms_src,
                                         "test.iron", &g_arena, &g_diags);
    Iron_Node   *root = iron_parse(&p);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);

    iron_web_await_check((Iron_Program *)root, &g_arena, &g_diags,
                         IRON_TARGET_WEB, NULL);

    /* No await anywhere → no E0501. */
    for (int i = 0; i < g_diags.count; i++) {
        TEST_ASSERT_NOT_EQUAL_INT(501, g_diags.items[i].code);
    }
}

/* ── main ────────────────────────────────────────────────────────────────── */

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_print_ast_dense_program);
    RUN_TEST(test_print_hir_and_lir_dense_program);
    RUN_TEST(test_print_hir_null_module);
    RUN_TEST(test_print_lir_null_module);
    RUN_TEST(test_web_await_check_follows_call_chain_to_helper);
    RUN_TEST(test_web_await_check_container_arms_no_await);
    return UNITY_END();
}
