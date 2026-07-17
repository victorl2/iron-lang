/* Phase 20 Wave 0 (Plan 20-02a): TDD scaffold for PTR-04 — `&` operator
 * lowering to Iron_FatPtr.
 *
 * Plan 20-02a (analyzer side): case 1 only — `&x` where x is a stack-local
 * resolves to *T at the analyzer level. Cases 2-4 (HIR/LIR opcodes,
 * gen_source tagging) live in Plan 20-02b.
 *
 * This test file is registered with the additional CTest LABEL
 * `phase20-pending-20-02b` so it is EXCLUDED from the default
 * `ctest -L phase20-invariant -LE phase20-pending-20-02b` sweep until
 * Plan 20-02b ships the HIR/LIR opcodes (IRON_HIR_EXPR_ADDR_OF /
 * IRON_LIR_ADDR_OF / IRON_LIR_PTR_LOAD / IRON_LIR_PTR_STORE) and the
 * label is dropped from this entry's set_tests_properties line.
 *
 * Pattern: lex → parse → analyze; AST walk to find the IRON_NODE_UNARY
 * with op==IRON_TOK_AMP and inspect resolved_type.
 */
#include "unity.h"
#include "analyzer/analyzer.h"
#include "analyzer/types.h"
#include "diagnostics/diagnostics.h"
#include "lexer/lexer.h"
#include "parser/ast.h"
#include "util/arena.h"
#include "stb_ds.h"

#include <string.h>

static Iron_Arena    arena;
static Iron_DiagList diags;

void setUp(void) {
    arena = iron_arena_create(131072);
    diags = iron_diaglist_create();
}

void tearDown(void) {
    iron_diaglist_free(&diags);
    iron_arena_free(&arena);
}

static Iron_AnalyzeResult analyze_src(const char *src) {
    return iron_analyze_buffer(
        src, strlen(src), "test.iron",
        IRON_ANALYSIS_MODE_CLI,
        &arena, &diags, NULL,
        0);
}

static int total_errors(void) {
    int n = 0;
    for (int i = 0; i < arrlen(diags.items); i++) {
        if (diags.items[i].level == IRON_DIAG_ERROR) n++;
    }
    return n;
}

/* Visitor: find the first Iron_UnaryExpr with op==IRON_TOK_AMP. */
typedef struct {
    Iron_UnaryExpr *out;
} FindAmpCtx;

static bool find_amp_visit(Iron_Visitor *v, Iron_Node *n) {
    FindAmpCtx *ctx = (FindAmpCtx *)v->ctx;
    if (ctx->out) return false;
    if (n && n->kind == IRON_NODE_UNARY) {
        Iron_UnaryExpr *ue = (Iron_UnaryExpr *)n;
        if (ue->op == (Iron_OpKind)IRON_TOK_AMP) {
            ctx->out = ue;
            return false;
        }
    }
    return true;
}

static Iron_UnaryExpr *find_amp(Iron_Program *prog) {
    FindAmpCtx ctx = { .out = NULL };
    Iron_Visitor v = { .ctx = &ctx,
                       .visit_node = find_amp_visit,
                       .post_visit = NULL };
    iron_ast_walk((Iron_Node *)prog, &v);
    return ctx.out;
}

/* Case 1: `&x` for `val x: Int` resolves to *Int (analyzer side; GREEN
 * by end of Plan 20-02a Task 2). */
void test_amp_on_local_resolves_to_ptr(void) {
    Iron_AnalyzeResult r = analyze_src(
        "func sink(p: *Int) {}\n"
        "func main() {\n"
        "    val x: Int = 42\n"
        "    sink(&x)\n"
        "}\n");
    TEST_ASSERT_EQUAL_INT(0, total_errors());
    Iron_UnaryExpr *ue = find_amp(r.program);
    TEST_ASSERT_NOT_NULL_MESSAGE(ue, "expected to find &x unary expression");
    TEST_ASSERT_NOT_NULL(ue->resolved_type);
    TEST_ASSERT_EQUAL_INT(IRON_TYPE_PTR, ue->resolved_type->kind);
    TEST_ASSERT_NOT_NULL(ue->resolved_type->ptr.pointee);
    TEST_ASSERT_EQUAL_INT(IRON_TYPE_INT, ue->resolved_type->ptr.pointee->kind);
    /* `val x` source is non-mutable; pointer type should be `*Int`,
     * not `*var Int`. */
    TEST_ASSERT_FALSE(ue->resolved_type->ptr.is_var);
}

/* Case 2: `&y` for `var y: Int` resolves to *var Int.
 * GREEN in 20-02a (analyzer-side). */
void test_amp_on_var_local_resolves_to_var_ptr(void) {
    Iron_AnalyzeResult r = analyze_src(
        "func sink(p: *var Int) {}\n"
        "func main() {\n"
        "    var y: Int = 42\n"
        "    sink(&y)\n"
        "}\n");
    TEST_ASSERT_EQUAL_INT(0, total_errors());
    Iron_UnaryExpr *ue = find_amp(r.program);
    TEST_ASSERT_NOT_NULL(ue);
    TEST_ASSERT_NOT_NULL(ue->resolved_type);
    TEST_ASSERT_EQUAL_INT(IRON_TYPE_PTR, ue->resolved_type->kind);
    TEST_ASSERT_TRUE(ue->resolved_type->ptr.is_var);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_amp_on_local_resolves_to_ptr);
    RUN_TEST(test_amp_on_var_local_resolves_to_var_ptr);
    return UNITY_END();
}
