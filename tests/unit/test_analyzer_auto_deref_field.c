/* Phase 20 Wave 0 (Plan 20-02a): TDD scaffold for PTR-06 — auto-deref at
 * `.field` / `.method()` on `*T` receivers.
 *
 * Authored RED first; flips GREEN once Plan 20-02a Task 2 lands the
 * Iron_FieldAccess.is_auto_deref + Iron_MethodCallExpr.is_auto_deref AST
 * flags and the IRON_NODE_FIELD_ACCESS / IRON_NODE_METHOD_CALL handlers in
 * src/analyzer/typecheck.c that walk through IRON_TYPE_PTR receivers and
 * resolve the field/method against the pointee type.
 *
 * Pattern: lex → parse → analyze pipeline via iron_analyze_buffer; then
 * walk the resulting Iron_Program AST to inspect is_auto_deref flags.
 */
#include "unity.h"
#include "analyzer/analyzer.h"
#include "analyzer/types.h"
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

/* Visitor that finds the first IRON_NODE_FIELD_ACCESS whose field name
 * matches `target_field` AND whose object is an IDENT named `target_obj`
 * (or any IDENT when target_obj is NULL). The IDENT-name filter
 * disambiguates `p.x` from `self.x` inside an init body. */
typedef struct {
    const char       *target_field;
    const char       *target_obj;  /* NULL = any IDENT */
    Iron_FieldAccess *out;
} FindFieldCtx;

static bool find_field_visit(Iron_Visitor *v, Iron_Node *n) {
    FindFieldCtx *ctx = (FindFieldCtx *)v->ctx;
    if (ctx->out) return false;  /* already found; stop */
    if (n && n->kind == IRON_NODE_FIELD_ACCESS) {
        Iron_FieldAccess *fa = (Iron_FieldAccess *)n;
        if (!fa->field || !ctx->target_field) return true;
        if (strcmp(fa->field, ctx->target_field) != 0) return true;
        if (ctx->target_obj) {
            if (!fa->object || fa->object->kind != IRON_NODE_IDENT) return true;
            Iron_Ident *id = (Iron_Ident *)fa->object;
            if (!id->name || strcmp(id->name, ctx->target_obj) != 0) return true;
        }
        ctx->out = fa;
        return false;
    }
    return true;
}

static Iron_FieldAccess *find_field_access_on(Iron_Program *prog,
                                                const char *field_name,
                                                const char *obj_name) {
    FindFieldCtx ctx = { .target_field = field_name,
                         .target_obj   = obj_name,
                         .out          = NULL };
    Iron_Visitor v = { .ctx = &ctx,
                       .visit_node = find_field_visit,
                       .post_visit = NULL };
    iron_ast_walk((Iron_Node *)prog, &v);
    return ctx.out;
}

/* Visitor variant for IRON_NODE_METHOD_CALL. */
typedef struct {
    const char           *target_method;
    Iron_MethodCallExpr  *out;
} FindMethodCtx;

static bool find_method_visit(Iron_Visitor *v, Iron_Node *n) {
    FindMethodCtx *ctx = (FindMethodCtx *)v->ctx;
    if (ctx->out) return false;
    if (n && n->kind == IRON_NODE_METHOD_CALL) {
        Iron_MethodCallExpr *mc = (Iron_MethodCallExpr *)n;
        if (mc->method && ctx->target_method &&
            strcmp(mc->method, ctx->target_method) == 0) {
            ctx->out = mc;
            return false;
        }
    }
    return true;
}

static Iron_MethodCallExpr *find_method_call(Iron_Program *prog,
                                              const char *method_name) {
    FindMethodCtx ctx = { .target_method = method_name, .out = NULL };
    Iron_Visitor v = { .ctx = &ctx,
                       .visit_node = find_method_visit,
                       .post_visit = NULL };
    iron_ast_walk((Iron_Node *)prog, &v);
    return ctx.out;
}

/* Case 1: PTR-06 — `func f(p: *Point) -> Int { return p.x }` typechecks
 * with is_auto_deref=true on the IRON_NODE_FIELD_ACCESS for `p.x`. */
void test_auto_deref_field_on_ptr_typechecks(void) {
    Iron_AnalyzeResult r = analyze_src(
        "object Point {\n"
        "    var x: Int\n"
        "    init() { self.x = 0 }\n"
        "}\n"
        "func get_x(p: *Point) -> Int {\n"
        "    return p.x\n"
        "}\n"
        "func main() {}\n");
    TEST_ASSERT_EQUAL_INT(0, total_errors());
    Iron_FieldAccess *fa = find_field_access_on(r.program, "x", "p");
    TEST_ASSERT_NOT_NULL_MESSAGE(fa, "expected to find p.x field-access");
    TEST_ASSERT_TRUE_MESSAGE(fa->is_auto_deref,
                             "is_auto_deref must be true for *T receiver");
    /* Resolved type must be Int (the pointee's field type). */
    TEST_ASSERT_NOT_NULL(fa->resolved_type);
    TEST_ASSERT_EQUAL_INT(IRON_TYPE_INT, fa->resolved_type->kind);
}

/* Case 2: PTR-06 + OQ-A write side — `func f(p: *var Point) { p.x = 42 }`
 * typechecks with is_auto_deref=true on the field-access LHS. The codegen
 * lowering lands in Plan 20-02b; this plan only flags the AST. */
void test_auto_deref_field_assign_on_var_ptr(void) {
    Iron_AnalyzeResult r = analyze_src(
        "object Point {\n"
        "    var x: Int\n"
        "    init() { self.x = 0 }\n"
        "}\n"
        "func set_x(p: *var Point) {\n"
        "    p.x = 42\n"
        "}\n"
        "func main() {}\n");
    TEST_ASSERT_EQUAL_INT(0, total_errors());
    Iron_FieldAccess *fa = find_field_access_on(r.program, "x", "p");
    TEST_ASSERT_NOT_NULL_MESSAGE(fa, "expected to find p.x field-access");
    TEST_ASSERT_TRUE_MESSAGE(fa->is_auto_deref,
                             "is_auto_deref must be true for *var T LHS");
}

/* Case 3: Multi-level rejection — `func f(pp: **Point) -> Int { return pp.x }`
 * emits a compile error (single-level auto-deref only per CONTEXT.md). */
void test_auto_deref_multi_level_rejected(void) {
    analyze_src(
        "object Point {\n"
        "    var x: Int\n"
        "    init() { self.x = 0 }\n"
        "}\n"
        "func bad(pp: **Point) -> Int {\n"
        "    return pp.x\n"
        "}\n"
        "func main() {}\n");
    /* Some error must fire — exact code is planner discretion. */
    TEST_ASSERT_GREATER_THAN_INT(0, total_errors());
}

/* Case 4: PTR-13 narrowing — `if p != null { p.x }` for `p: ?*Point`
 * typechecks; the inner field-access has is_auto_deref=true. */
void test_auto_deref_via_nullable_ptr_narrowing(void) {
    Iron_AnalyzeResult r = analyze_src(
        "object Point {\n"
        "    var x: Int\n"
        "    init() { self.x = 0 }\n"
        "}\n"
        "func maybe_get(p: ?*Point) -> Int {\n"
        "    if p != null {\n"
        "        return p.x\n"
        "    } else {\n"
        "        return 0\n"
        "    }\n"
        "}\n"
        "func main() {}\n");
    TEST_ASSERT_EQUAL_INT(0, total_errors());
    Iron_FieldAccess *fa = find_field_access_on(r.program, "x", "p");
    TEST_ASSERT_NOT_NULL_MESSAGE(fa, "expected to find p.x");
    TEST_ASSERT_TRUE_MESSAGE(fa->is_auto_deref,
                             "narrowed ?*T -> *T should still auto-deref");
}

/* Case 5: PTR-13 NO narrowing — `func f(p: ?*Point) -> Int { return p.x }`
 * emits an error (existing nullable-field-access diagnostic). */
void test_no_auto_deref_on_unnarrowed_nullable_ptr(void) {
    analyze_src(
        "object Point {\n"
        "    var x: Int\n"
        "    init() { self.x = 0 }\n"
        "}\n"
        "func unsafe_get(p: ?*Point) -> Int {\n"
        "    return p.x\n"
        "}\n"
        "func main() {}\n");
    TEST_ASSERT_GREATER_THAN_INT(0, total_errors());
}

/* Case 6: Method auto-deref AST flag set — verifies the typechecker sets
 * is_auto_deref on Iron_MethodCallExpr when the receiver is a `*T` /
 * `*var T` pointer and the analyzer dispatches the call against the
 * pointee's ObjectDecl. We test the FLAG-SETTING step only (analyzer
 * surface). The full mutability-vs-method semantics interaction is the
 * domain of Phase 22 (readonly/pure tier on methods); this Phase 20
 * test isolates the auto-deref signal.
 *
 * Pattern: a *var Point receiver paired with a non-(self) method whose
 * mutability check passes (parameter is `*var T`, so the binding is
 * mutable through the pointer per OQ-A). The receiver expression is
 * IDENT(p) -> resolved_type=*var Point. mc->is_auto_deref must be true
 * regardless of whether the dispatch produces a non-Void return type. */
void test_method_auto_deref_on_var_ptr(void) {
    Iron_AnalyzeResult r = analyze_src(
        "object Point {\n"
        "    var x: Int\n"
        "    init() { self.x = 0 }\n"
        "    func bump() { self.x = self.x + 1 }\n"
        "}\n"
        "func mutate(p: *var Point) {\n"
        "    p.bump()\n"
        "}\n"
        "func main() {}\n");
    /* The dispatch may surface its own Void/return-type diagnostics for
     * non-receiver-form methods invoked through a *var T receiver — that's
     * a separate v3 method-mutability issue tracked outside this plan.
     * We assert ONLY the analyzer surface signal (is_auto_deref flag on
     * the AST node), not the absence of dispatch-path errors. */
    Iron_MethodCallExpr *mc = find_method_call(r.program, "bump");
    TEST_ASSERT_NOT_NULL_MESSAGE(mc, "expected to find p.bump()");
    TEST_ASSERT_TRUE_MESSAGE(mc->is_auto_deref,
                             "method receiver of *var T should auto-deref");
    (void)r;
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_auto_deref_field_on_ptr_typechecks);
    RUN_TEST(test_auto_deref_field_assign_on_var_ptr);
    RUN_TEST(test_auto_deref_multi_level_rejected);
    RUN_TEST(test_auto_deref_via_nullable_ptr_narrowing);
    RUN_TEST(test_no_auto_deref_on_unnarrowed_nullable_ptr);
    RUN_TEST(test_method_auto_deref_on_var_ptr);
    return UNITY_END();
}
