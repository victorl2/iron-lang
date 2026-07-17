/* Phase 20 Wave 0 (Plan 20-02a): TDD scaffold for PTR-07 — auto-address
 * insertion at call sites for `*T` / `*var T` parameters, with PARM-03
 * IRON_ERR_PARM_VAR_SLOT_NEEDS_MUT=267 reuse for val→var-slot mismatch
 * and IRON_ERR_PTR_AMP_ON_RVALUE=270 for rvalue arguments.
 *
 * Authored RED first; flips GREEN once Plan 20-02a Task 2 lands the
 * is_auto_address_target flags + IRON_NODE_CALL/IRON_NODE_METHOD_CALL
 * arg-loop extension in src/analyzer/typecheck.c.
 *
 * Pattern: lex → parse → analyze pipeline; AST walk to inspect flags.
 */
#include "unity.h"
#include "analyzer/analyzer.h"
#include "diagnostics/diagnostics.h"
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

static int count_with_code(int code) {
    int n = 0;
    for (int i = 0; i < arrlen(diags.items); i++) {
        if (diags.items[i].code == code) n++;
    }
    return n;
}

/* Visitor: find first call to a function named `target_name`. */
typedef struct {
    const char     *target_name;
    Iron_CallExpr  *out;
} FindCallCtx;

static bool find_call_visit(Iron_Visitor *v, Iron_Node *n) {
    FindCallCtx *ctx = (FindCallCtx *)v->ctx;
    if (ctx->out) return false;
    if (n && n->kind == IRON_NODE_CALL) {
        Iron_CallExpr *ce = (Iron_CallExpr *)n;
        if (ce->callee && ce->callee->kind == IRON_NODE_IDENT) {
            Iron_Ident *id = (Iron_Ident *)ce->callee;
            if (id->name && ctx->target_name &&
                strcmp(id->name, ctx->target_name) == 0) {
                ctx->out = ce;
                return false;
            }
        }
    }
    return true;
}

static Iron_CallExpr *find_call(Iron_Program *prog, const char *name) {
    FindCallCtx ctx = { .target_name = name, .out = NULL };
    Iron_Visitor v = { .ctx = &ctx,
                       .visit_node = find_call_visit,
                       .post_visit = NULL };
    iron_ast_walk((Iron_Node *)prog, &v);
    return ctx.out;
}

/* Helper: read is_auto_address_target from any of the lvalue AST kinds. */
static bool arg_auto_addr_target(Iron_Node *arg) {
    if (!arg) return false;
    switch ((int)arg->kind) {
        case IRON_NODE_IDENT:        return ((Iron_Ident *)arg)->is_auto_address_target;
        case IRON_NODE_FIELD_ACCESS: return ((Iron_FieldAccess *)arg)->is_auto_address_target;
        case IRON_NODE_INDEX:        return ((Iron_IndexExpr *)arg)->is_auto_address_target;
        default:                     return false;
    }
}

/* Case 1: PTR-07 — `g(pt)` to `g(p: *Point)` typechecks; arg has
 * is_auto_address_target=true. */
void test_auto_address_ident_to_ptr(void) {
    Iron_AnalyzeResult r = analyze_src(
        "object Point {\n"
        "    var x: Int\n"
        "    init() { self.x = 0 }\n"
        "}\n"
        "func sink(p: *Point) {}\n"
        "func main() {\n"
        "    val pt = Point()\n"
        "    sink(pt)\n"
        "}\n");
    TEST_ASSERT_EQUAL_INT(0, total_errors());
    Iron_CallExpr *ce = find_call(r.program, "sink");
    TEST_ASSERT_NOT_NULL(ce);
    TEST_ASSERT_EQUAL_INT(1, ce->arg_count);
    TEST_ASSERT_TRUE_MESSAGE(arg_auto_addr_target(ce->args[0]),
                             "arg should be auto-address target");
}

/* Case 2: PTR-07 + PARM-03 — `g(pt)` where pt is val and g expects *var Point
 * emits IRON_ERR_PARM_VAR_SLOT_NEEDS_MUT=267. */
void test_auto_address_val_to_var_ptr_rejected(void) {
    analyze_src(
        "object Point {\n"
        "    var x: Int\n"
        "    init() { self.x = 0 }\n"
        "}\n"
        "func mutator(p: *var Point) {}\n"
        "func main() {\n"
        "    val pt = Point()\n"
        "    mutator(pt)\n"
        "}\n");
    TEST_ASSERT_GREATER_THAN_INT(0,
        count_with_code(IRON_ERR_PARM_VAR_SLOT_NEEDS_MUT));
}

/* Case 3: PTR-07 mutable source accepted — `g(pt)` where pt is var and g
 * expects *var Point typechecks; is_auto_address_target=true. */
void test_auto_address_var_to_var_ptr_accepted(void) {
    Iron_AnalyzeResult r = analyze_src(
        "object Point {\n"
        "    var x: Int\n"
        "    init() { self.x = 0 }\n"
        "}\n"
        "func mutator(p: *var Point) {}\n"
        "func main() {\n"
        "    var pt = Point()\n"
        "    mutator(pt)\n"
        "}\n");
    TEST_ASSERT_EQUAL_INT(0,
        count_with_code(IRON_ERR_PARM_VAR_SLOT_NEEDS_MUT));
    Iron_CallExpr *ce = find_call(r.program, "mutator");
    TEST_ASSERT_NOT_NULL(ce);
    TEST_ASSERT_TRUE(arg_auto_addr_target(ce->args[0]));
}

/* Case 4: PTR-07 rvalue rejection — `g(Point())` where g expects *Point
 * emits IRON_ERR_PTR_AMP_ON_RVALUE=270 (function-call result is rvalue). */
void test_auto_address_rvalue_rejected(void) {
    analyze_src(
        "object Point {\n"
        "    var x: Int\n"
        "    init() { self.x = 0 }\n"
        "}\n"
        "func sink(p: *Point) {}\n"
        "func make() -> Point { return Point() }\n"
        "func main() {\n"
        "    sink(make())\n"
        "}\n");
    TEST_ASSERT_GREATER_THAN_INT(0,
        count_with_code(IRON_ERR_PTR_AMP_ON_RVALUE));
}

/* Case 5: Explicit & still works — `g(&pt)` where g expects *Point;
 * the unary AMP path is taken (not auto-address), so is_auto_address_target
 * remains false on the IRON_NODE_IDENT operand. */
void test_explicit_amp_no_double_address(void) {
    Iron_AnalyzeResult r = analyze_src(
        "object Point {\n"
        "    var x: Int\n"
        "    init() { self.x = 0 }\n"
        "}\n"
        "func sink(p: *Point) {}\n"
        "func main() {\n"
        "    val pt = Point()\n"
        "    sink(&pt)\n"
        "}\n");
    TEST_ASSERT_EQUAL_INT(0, total_errors());
    Iron_CallExpr *ce = find_call(r.program, "sink");
    TEST_ASSERT_NOT_NULL(ce);
    /* arg is an Iron_UnaryExpr (not an IDENT), so the auto-address flag
     * predicate returns false. */
    TEST_ASSERT_EQUAL_INT(IRON_NODE_UNARY, ce->args[0]->kind);
    TEST_ASSERT_FALSE(arg_auto_addr_target(ce->args[0]));
}

/* Case 6: Element auto-address — `g(arr[0])` where g expects *Int and
 * arr is mutable. is_auto_address_target=true on the IRON_NODE_INDEX node.
 *
 * Uses unsized `[Int]` array form because array literals produce a
 * dynamic-size array (size=-1), and the typechecker requires the val
 * type-annotation to match that shape. (Sized `[Int; 3]` is a
 * pre-existing analyzer mismatch unrelated to PTR-07.) */
void test_auto_address_index_to_ptr(void) {
    Iron_AnalyzeResult r = analyze_src(
        "func sink(p: *Int) {}\n"
        "func main() {\n"
        "    val arr: [Int] = [1, 2, 3]\n"
        "    sink(arr[0])\n"
        "}\n");
    TEST_ASSERT_EQUAL_INT(0, total_errors());
    Iron_CallExpr *ce = find_call(r.program, "sink");
    TEST_ASSERT_NOT_NULL(ce);
    TEST_ASSERT_EQUAL_INT(1, ce->arg_count);
    TEST_ASSERT_EQUAL_INT(IRON_NODE_INDEX, ce->args[0]->kind);
    TEST_ASSERT_TRUE_MESSAGE(arg_auto_addr_target(ce->args[0]),
                             "INDEX arg should be auto-address target");
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_auto_address_ident_to_ptr);
    RUN_TEST(test_auto_address_val_to_var_ptr_rejected);
    RUN_TEST(test_auto_address_var_to_var_ptr_accepted);
    RUN_TEST(test_auto_address_rvalue_rejected);
    RUN_TEST(test_explicit_amp_no_double_address);
    RUN_TEST(test_auto_address_index_to_ptr);
    return UNITY_END();
}
