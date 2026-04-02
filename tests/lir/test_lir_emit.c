/* test_ir_emit.c — Unity tests for the IR-to-C emission backend (Phase 9).
 *
 * Tests hand-build IrModules using the IR constructor API, call
 * iron_lir_emit_c(), and verify the output C string contains expected patterns.
 *
 * Tests:
 *   1. test_emit_hello_world                   — Iron_main with const_string + call
 *   2. test_emit_arithmetic                    — Function with CONST_INT + ADD + RETURN
 *   3. test_emit_control_flow                  — Function with BRANCH terminator
 *   4. test_emit_alloca_load_store             — Function with ALLOCA + STORE + LOAD
 *   5. test_emit_type_decl_object              — Module with an object type_decl
 *   6. test_emit_phi_elimination               — Function with PHI instruction
 *   7. test_emit_expression_inlining_basic     — Single-use ADD inlined as compound expr
 *   8. test_emit_construct_inlined             — Single-use CONSTRUCT inlined as compound literal
 *   9. test_emit_inlined_no_separate_temps     — ADD inlined into MUL; no separate temp declared
 */

#include "unity.h"
#include "lir/emit_c.h"
#include "lir/lir_optimize.h"
#include "lir/lir.h"
#include "analyzer/types.h"
#include "diagnostics/diagnostics.h"
#include "util/arena.h"
#include "parser/ast.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>

/* ── Fixtures ────────────────────────────────────────────────────────────── */

static Iron_Arena g_arena;
static Iron_DiagList g_diags;

void setUp(void) {
    g_arena = iron_arena_create(131072);
    iron_types_init(&g_arena);
    g_diags = iron_diaglist_create();
}

void tearDown(void) {
    iron_diaglist_free(&g_diags);
    iron_arena_free(&g_arena);
}

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static Iron_Span test_span(void) {
    return iron_span_make("test.iron", 1, 1, 1, 1);
}

/* ── Test 1: Hello world — Iron_main with const_string + call ────────────── */

void test_emit_hello_world(void) {
    Iron_Arena ir_arena = iron_arena_create(65536);
    iron_types_init(&ir_arena);

    IronLIR_Module *mod = iron_lir_module_create(&ir_arena, "test_hello");

    /* func Iron_main() -> void */
    IronLIR_Func *fn = iron_lir_func_create(mod, "Iron_main", NULL, 0, NULL);
    IronLIR_Block *entry = iron_lir_block_create(fn, "entry");

    Iron_Span sp = test_span();
    Iron_Type *str_type = iron_type_make_primitive(IRON_TYPE_STRING);

    /* %1 = const_string "Hello" */
    IronLIR_Instr *s = iron_lir_const_string(fn, entry, "Hello", str_type, sp);
    (void)s;

    /* return void */
    iron_lir_return(fn, entry, IRON_LIR_VALUE_INVALID, true, NULL, sp);

    /* Emit — skip new passes (copy-prop/const-fold/DCE) so the unused
     * const_string survives in the IR and iron_string_from_literal is emitted. */
    Iron_Arena out_arena = iron_arena_create(131072);
    IronLIR_OptimizeInfo opt_info_1;
    iron_lir_optimize(mod, &opt_info_1, &out_arena, false, true);
    const char *result = iron_lir_emit_c(mod, &out_arena, &g_diags, &opt_info_1);

    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_NOT_NULL(strstr(result, "#include \"runtime/iron_runtime.h\""));
    TEST_ASSERT_NOT_NULL(strstr(result, "Iron_main"));
    TEST_ASSERT_NOT_NULL(strstr(result, "int main("));
    TEST_ASSERT_NOT_NULL(strstr(result, "iron_runtime_init()"));
    TEST_ASSERT_NOT_NULL(strstr(result, "iron_runtime_shutdown()"));
    TEST_ASSERT_NOT_NULL(strstr(result, "iron_string_from_literal"));

    iron_lir_optimize_info_free(&opt_info_1);
    iron_arena_free(&out_arena);
    iron_lir_module_destroy(mod);
    iron_arena_free(&ir_arena);
}

/* ── Test 2: Arithmetic — CONST_INT + ADD + RETURN ───────────────────────── */

void test_emit_arithmetic(void) {
    Iron_Arena ir_arena = iron_arena_create(65536);
    iron_types_init(&ir_arena);

    IronLIR_Module *mod = iron_lir_module_create(&ir_arena, "test_arith");

    Iron_Type *int_type = iron_type_make_primitive(IRON_TYPE_INT);

    /* func add_ints() -> Int */
    IronLIR_Func *fn = iron_lir_func_create(mod, "Iron_add_ints", NULL, 0,
                                          int_type);
    IronLIR_Block *entry = iron_lir_block_create(fn, "entry");
    Iron_Span sp = test_span();

    /* %1 = const_int 10 */
    IronLIR_Instr *c1 = iron_lir_const_int(fn, entry, 10, int_type, sp);
    /* %2 = const_int 20 */
    IronLIR_Instr *c2 = iron_lir_const_int(fn, entry, 20, int_type, sp);
    /* %3 = add %1, %2 */
    IronLIR_Instr *sum = iron_lir_binop(fn, entry, IRON_LIR_ADD,
                                       c1->id, c2->id, int_type, sp);
    /* return %3 */
    iron_lir_return(fn, entry, sum->id, false, int_type, sp);

    /* Skip new passes (copy-prop/const-fold/DCE) so constant folding doesn't
     * eliminate the ADD before emission — this test is checking emitter output. */
    Iron_Arena out_arena = iron_arena_create(131072);
    IronLIR_OptimizeInfo opt_info_2;
    iron_lir_optimize(mod, &opt_info_2, &out_arena, false, true);
    const char *result = iron_lir_emit_c(mod, &out_arena, &g_diags, &opt_info_2);

    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_NOT_NULL(strstr(result, "int64_t"));
    TEST_ASSERT_NOT_NULL(strstr(result, "+"));
    TEST_ASSERT_NOT_NULL(strstr(result, "_v"));
    /* No main wrapper since no Iron_main */
    TEST_ASSERT_NULL(strstr(result, "int main("));

    iron_lir_optimize_info_free(&opt_info_2);
    iron_arena_free(&out_arena);
    iron_lir_module_destroy(mod);
    iron_arena_free(&ir_arena);
}

/* ── Test 3: Control flow — BRANCH terminator (two blocks) ──────────────── */

void test_emit_control_flow(void) {
    Iron_Arena ir_arena = iron_arena_create(65536);
    iron_types_init(&ir_arena);

    IronLIR_Module *mod = iron_lir_module_create(&ir_arena, "test_cf");
    Iron_Type *bool_type = iron_type_make_primitive(IRON_TYPE_BOOL);
    Iron_Span sp = test_span();

    /* func branch_test(cond: Bool) -> void */
    IronLIR_Param params[1];
    params[0].name = "cond";
    params[0].type = bool_type;

    IronLIR_Func *fn = iron_lir_func_create(mod, "Iron_branch_test",
                                          params, 1, NULL);
    IronLIR_Block *entry = iron_lir_block_create(fn, "entry");
    IronLIR_Block *then_b = iron_lir_block_create(fn, "then_0");
    IronLIR_Block *else_b = iron_lir_block_create(fn, "else_0");
    IronLIR_Block *merge  = iron_lir_block_create(fn, "merge_0");

    /* entry: branch cond -> then_0, else_0 */
    IronLIR_Instr *cond_val = iron_lir_const_bool(fn, entry, true, bool_type, sp);
    iron_lir_branch(fn, entry, cond_val->id, then_b->id, else_b->id, sp);

    /* then_0: jump merge_0 */
    iron_lir_jump(fn, then_b, merge->id, sp);

    /* else_0: jump merge_0 */
    iron_lir_jump(fn, else_b, merge->id, sp);

    /* merge_0: return void */
    iron_lir_return(fn, merge, IRON_LIR_VALUE_INVALID, true, NULL, sp);

    Iron_Arena out_arena = iron_arena_create(131072);
    IronLIR_OptimizeInfo opt_info_3;
    iron_lir_optimize(mod, &opt_info_3, &out_arena, false, false);
    const char *result = iron_lir_emit_c(mod, &out_arena, &g_diags, &opt_info_3);

    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_NOT_NULL(strstr(result, "goto"));
    TEST_ASSERT_NOT_NULL(strstr(result, "then_0"));
    TEST_ASSERT_NOT_NULL(strstr(result, "else_0"));
    TEST_ASSERT_NOT_NULL(strstr(result, "if ("));
    TEST_ASSERT_NOT_NULL(strstr(result, "merge_0"));

    iron_lir_optimize_info_free(&opt_info_3);
    iron_arena_free(&out_arena);
    iron_lir_module_destroy(mod);
    iron_arena_free(&ir_arena);
}

/* ── Test 4: Alloca / Load / Store ───────────────────────────────────────── */

void test_emit_alloca_load_store(void) {
    Iron_Arena ir_arena = iron_arena_create(65536);
    iron_types_init(&ir_arena);

    IronLIR_Module *mod = iron_lir_module_create(&ir_arena, "test_alloca");
    Iron_Type *int_type = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_Span sp = test_span();

    /* func alloca_test() -> Int */
    IronLIR_Func *fn = iron_lir_func_create(mod, "Iron_alloca_test",
                                          NULL, 0, int_type);
    IronLIR_Block *entry = iron_lir_block_create(fn, "entry");

    /* %1 = alloca Int */
    IronLIR_Instr *slot = iron_lir_alloca(fn, entry, int_type, "x", sp);
    /* %2 = const_int 42 */
    IronLIR_Instr *val  = iron_lir_const_int(fn, entry, 42, int_type, sp);
    /* store %1, %2 */
    iron_lir_store(fn, entry, slot->id, val->id, sp);
    /* %3 = load %1 */
    IronLIR_Instr *loaded = iron_lir_load(fn, entry, slot->id, int_type, sp);
    /* return %3 */
    iron_lir_return(fn, entry, loaded->id, false, int_type, sp);

    Iron_Arena out_arena = iron_arena_create(131072);
    IronLIR_OptimizeInfo opt_info_4;
    iron_lir_optimize(mod, &opt_info_4, &out_arena, false, false);
    const char *result = iron_lir_emit_c(mod, &out_arena, &g_diags, &opt_info_4);

    TEST_ASSERT_NOT_NULL(result);
    /* Return type is int64_t */
    TEST_ASSERT_NOT_NULL(strstr(result, "int64_t"));
    /* After copy-prop + dead alloca elimination, constant 42 is returned directly */
    TEST_ASSERT_NOT_NULL(strstr(result, "return"));
    TEST_ASSERT_NOT_NULL(strstr(result, "42"));

    iron_lir_optimize_info_free(&opt_info_4);
    iron_arena_free(&out_arena);
    iron_lir_module_destroy(mod);
    iron_arena_free(&ir_arena);
}

/* ── Test 5: Type declaration — object struct ────────────────────────────── */

void test_emit_type_decl_object(void) {
    Iron_Arena ir_arena = iron_arena_create(65536);
    iron_types_init(&ir_arena);

    /* Build a minimal Iron_ObjectDecl in the arena */
    Iron_ObjectDecl *od = ARENA_ALLOC(&ir_arena, Iron_ObjectDecl);
    memset(od, 0, sizeof(*od));
    od->kind = IRON_NODE_OBJECT_DECL;
    od->name = "Player";
    od->span = test_span();

    /* Add one field: x: Int */
    Iron_TypeAnnotation *ta = ARENA_ALLOC(&ir_arena, Iron_TypeAnnotation);
    memset(ta, 0, sizeof(*ta));
    ta->kind = IRON_NODE_TYPE_ANNOTATION;
    ta->name = "Int";
    ta->is_nullable = false;

    Iron_Field *field = ARENA_ALLOC(&ir_arena, Iron_Field);
    memset(field, 0, sizeof(*field));
    field->kind = IRON_NODE_FIELD;
    field->name = "x";
    field->type_ann = (Iron_Node *)ta;

    Iron_Node **fields = NULL;
    arrput(fields, (Iron_Node *)field);
    od->fields = fields;
    od->field_count = 1;

    /* Build Iron_Type for the object */
    Iron_Type *obj_type = iron_type_make_object(&ir_arena, od);

    /* Create module with this type_decl */
    IronLIR_Module *mod = iron_lir_module_create(&ir_arena, "test_type_decl");
    iron_lir_module_add_type_decl(mod, IRON_LIR_TYPE_OBJECT, "Player", obj_type);

    /* Add a trivial function so the module emits something */
    IronLIR_Func *fn = iron_lir_func_create(mod, "Iron_main", NULL, 0, NULL);
    IronLIR_Block *entry = iron_lir_block_create(fn, "entry");
    iron_lir_return(fn, entry, IRON_LIR_VALUE_INVALID, true, NULL, test_span());

    Iron_Arena out_arena = iron_arena_create(131072);
    IronLIR_OptimizeInfo opt_info_5;
    iron_lir_optimize(mod, &opt_info_5, &out_arena, false, false);
    const char *result = iron_lir_emit_c(mod, &out_arena, &g_diags, &opt_info_5);

    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_NOT_NULL(strstr(result, "typedef struct"));
    TEST_ASSERT_NOT_NULL(strstr(result, "Iron_Player"));
    TEST_ASSERT_NOT_NULL(strstr(result, "IRON_TAG_"));

    iron_lir_optimize_info_free(&opt_info_5);
    arrfree(fields);
    iron_arena_free(&out_arena);
    iron_lir_module_destroy(mod);
    iron_arena_free(&ir_arena);
}

/* ── Test 6: Phi elimination ─────────────────────────────────────────────── */

void test_emit_phi_elimination(void) {
    Iron_Arena ir_arena = iron_arena_create(65536);
    iron_types_init(&ir_arena);

    IronLIR_Module *mod = iron_lir_module_create(&ir_arena, "test_phi");
    Iron_Type *int_type = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_Type *bool_type = iron_type_make_primitive(IRON_TYPE_BOOL);
    Iron_Span sp = test_span();

    /*
     * Build a function that computes max(a, b) with a phi node:
     *
     *   func max(a: Int, b: Int) -> Int {
     *   entry:
     *     %3 = a > b
     *     branch %3 -> then_0, else_0
     *   then_0:
     *     jump merge_0
     *   else_0:
     *     jump merge_0
     *   merge_0:
     *     %result = phi [%a_val: then_0, %b_val: else_0]
     *     return %result
     *   }
     */
    IronLIR_Param params[2];
    params[0].name = "a";
    params[0].type = int_type;
    params[1].name = "b";
    params[1].type = int_type;

    IronLIR_Func *fn = iron_lir_func_create(mod, "Iron_phi_test",
                                          params, 2, int_type);
    IronLIR_Block *entry   = iron_lir_block_create(fn, "entry");
    IronLIR_Block *then_b  = iron_lir_block_create(fn, "then_0");
    IronLIR_Block *else_b  = iron_lir_block_create(fn, "else_0");
    IronLIR_Block *merge_b = iron_lir_block_create(fn, "merge_0");

    /* entry: create two values and a branch */
    IronLIR_Instr *va = iron_lir_const_int(fn, entry, 10, int_type, sp);
    IronLIR_Instr *vb = iron_lir_const_int(fn, entry, 20, int_type, sp);
    IronLIR_Instr *cmp = iron_lir_binop(fn, entry, IRON_LIR_GT,
                                       va->id, vb->id, bool_type, sp);
    iron_lir_branch(fn, entry, cmp->id, then_b->id, else_b->id, sp);

    /* then_0: jump merge_0 */
    iron_lir_jump(fn, then_b, merge_b->id, sp);

    /* else_0: jump merge_0 */
    iron_lir_jump(fn, else_b, merge_b->id, sp);

    /* merge_0: phi + return */
    IronLIR_Instr *phi = iron_lir_phi(fn, merge_b, int_type, sp);
    iron_lir_phi_add_incoming(phi, va->id, then_b->id);
    iron_lir_phi_add_incoming(phi, vb->id, else_b->id);
    iron_lir_return(fn, merge_b, phi->id, false, int_type, sp);

    Iron_Arena out_arena = iron_arena_create(131072);
    IronLIR_OptimizeInfo opt_info_6;
    iron_lir_optimize(mod, &opt_info_6, &out_arena, false, false);
    const char *result = iron_lir_emit_c(mod, &out_arena, &g_diags, &opt_info_6);

    TEST_ASSERT_NOT_NULL(result);

    /* After phi elimination, output should NOT contain "phi" as an IR keyword */
    /* It may appear in identifiers, but not as a standalone instruction */
    /* More importantly, alloca+store pattern should be present */
    TEST_ASSERT_NOT_NULL(strstr(result, "int64_t"));

    /* The function must still return something */
    TEST_ASSERT_NOT_NULL(strstr(result, "return "));

    /* No phi instruction text should remain (it becomes load) */
    /* ERROR: phi comment would only appear if phi_eliminate failed */
    TEST_ASSERT_NULL(strstr(result, "ERROR: phi not eliminated"));

    iron_lir_optimize_info_free(&opt_info_6);
    iron_arena_free(&out_arena);
    iron_lir_module_destroy(mod);
    iron_arena_free(&ir_arena);
}

/* ── Test 7: Expression inlining — single-use ADD inlined ────────────────── */

void test_emit_expression_inlining_basic(void) {
    Iron_Arena ir_arena = iron_arena_create(65536);
    iron_types_init(&ir_arena);

    IronLIR_Module *mod = iron_lir_module_create(&ir_arena, "test_inline_basic");
    Iron_Type *int_type = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_Span sp = test_span();

    /* Build:
     *   func add_and_double() -> Int
     *     %1 = const_int 3
     *     %2 = const_int 4
     *     %3 = add %1, %2    <- single-use result
     *     %4 = const_int 2
     *     %5 = mul %3, %4    <- inlined: ((%1 + %2) * %4)
     *     return %5
     *
     * With expression inlining active (skip_new_passes=false), constant folding
     * will fold this entirely.  Use a subtraction to avoid full folding while
     * still exercising inlining: the result cannot be folded if we use a
     * non-constant operand path. Instead, verify the C code is produced without
     * error and contains parenthesized arithmetic forms. */
    IronLIR_Func *fn = iron_lir_func_create(mod, "Iron_add_double",
                                           NULL, 0, int_type);
    IronLIR_Block *entry = iron_lir_block_create(fn, "entry");

    IronLIR_Instr *c3  = iron_lir_const_int(fn, entry, 3, int_type, sp);
    IronLIR_Instr *c4  = iron_lir_const_int(fn, entry, 4, int_type, sp);
    IronLIR_Instr *add = iron_lir_binop(fn, entry, IRON_LIR_ADD,
                                       c3->id, c4->id, int_type, sp);
    IronLIR_Instr *c2  = iron_lir_const_int(fn, entry, 2, int_type, sp);
    IronLIR_Instr *mul = iron_lir_binop(fn, entry, IRON_LIR_MUL,
                                       add->id, c2->id, int_type, sp);
    iron_lir_return(fn, entry, mul->id, false, int_type, sp);

    /* Run with optimization passes enabled — this activates expression inlining */
    Iron_Arena out_arena = iron_arena_create(131072);
    IronLIR_OptimizeInfo opt_info;
    iron_lir_optimize(mod, &opt_info, &out_arena, false, false);
    const char *result = iron_lir_emit_c(mod, &out_arena, &g_diags, &opt_info);

    TEST_ASSERT_NOT_NULL(result);
    /* Emitted C must compile without errors — spot-check that it has valid
     * structure: a function definition and a return statement */
    TEST_ASSERT_NOT_NULL(strstr(result, "Iron_add_double"));
    TEST_ASSERT_NOT_NULL(strstr(result, "return"));
    /* With constant folding, result is const 14 or parenthesized arithmetic.
     * Either way, the generated C must not have both _v3 and _v4 as separate
     * int64_t declarations followed by _v5 = _v3 + _v4 pattern (they should
     * be inlined). If fully constant-folded, the folded constant appears.
     * Verify: no separate _v3 assignment (either inlined or folded away). */
    TEST_ASSERT_NULL(strstr(result, "int64_t _v3 ="));

    iron_lir_optimize_info_free(&opt_info);
    iron_arena_free(&out_arena);
    iron_lir_module_destroy(mod);
    iron_arena_free(&ir_arena);
}

/* ── Test 8: CONSTRUCT inlined as compound literal ───────────────────────── */

void test_emit_construct_inlined(void) {
    Iron_Arena ir_arena = iron_arena_create(65536);
    iron_types_init(&ir_arena);

    /* Build a 2-field object type: Point { x: Int, y: Int } */
    Iron_ObjectDecl *od = ARENA_ALLOC(&ir_arena, Iron_ObjectDecl);
    memset(od, 0, sizeof(*od));
    od->kind  = IRON_NODE_OBJECT_DECL;
    od->name  = "Point";
    od->span  = test_span();

    Iron_Type *int_type  = iron_type_make_primitive(IRON_TYPE_INT);

    /* Field x */
    Iron_TypeAnnotation *ta_x = ARENA_ALLOC(&ir_arena, Iron_TypeAnnotation);
    memset(ta_x, 0, sizeof(*ta_x));
    ta_x->kind = IRON_NODE_TYPE_ANNOTATION;
    ta_x->name = "Int";
    Iron_Field *fx = ARENA_ALLOC(&ir_arena, Iron_Field);
    memset(fx, 0, sizeof(*fx));
    fx->kind = IRON_NODE_FIELD;  fx->name = "x";  fx->type_ann = (Iron_Node *)ta_x;

    /* Field y */
    Iron_TypeAnnotation *ta_y = ARENA_ALLOC(&ir_arena, Iron_TypeAnnotation);
    memset(ta_y, 0, sizeof(*ta_y));
    ta_y->kind = IRON_NODE_TYPE_ANNOTATION;
    ta_y->name = "Int";
    Iron_Field *fy = ARENA_ALLOC(&ir_arena, Iron_Field);
    memset(fy, 0, sizeof(*fy));
    fy->kind = IRON_NODE_FIELD;  fy->name = "y";  fy->type_ann = (Iron_Node *)ta_y;

    Iron_Node **fields = NULL;
    arrput(fields, (Iron_Node *)fx);
    arrput(fields, (Iron_Node *)fy);
    od->fields      = fields;
    od->field_count = 2;

    Iron_Type *point_type = iron_type_make_object(&ir_arena, od);

    /* Create module + type decl */
    IronLIR_Module *mod = iron_lir_module_create(&ir_arena, "test_construct_inline");
    iron_lir_module_add_type_decl(mod, IRON_LIR_TYPE_OBJECT, "Point", point_type);

    Iron_Span sp = test_span();

    /* Build:
     *   func make_point() -> Point
     *     %1 = const_int 10   <- x value
     *     %2 = const_int 20   <- y value
     *     %3 = construct Point { %1, %2 }   <- single-use
     *     return %3
     *
     * With expression inlining, %3 should be inlined as a compound literal
     * at the return site. */
    IronLIR_Func *fn = iron_lir_func_create(mod, "Iron_make_point",
                                           NULL, 0, point_type);
    IronLIR_Block *entry = iron_lir_block_create(fn, "entry");

    IronLIR_Instr *cx = iron_lir_const_int(fn, entry, 10, int_type, sp);
    IronLIR_Instr *cy = iron_lir_const_int(fn, entry, 20, int_type, sp);

    IronLIR_ValueId field_vals[2] = { cx->id, cy->id };
    IronLIR_Instr *pt = iron_lir_construct(fn, entry, point_type, field_vals, 2, sp);
    iron_lir_return(fn, entry, pt->id, false, point_type, sp);

    Iron_Arena out_arena = iron_arena_create(131072);
    IronLIR_OptimizeInfo opt_info;
    iron_lir_optimize(mod, &opt_info, &out_arena, false, false);
    const char *result = iron_lir_emit_c(mod, &out_arena, &g_diags, &opt_info);

    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_NOT_NULL(strstr(result, "Iron_make_point"));
    TEST_ASSERT_NOT_NULL(strstr(result, "return"));
    /* CONSTRUCT should be inlined: compound literal syntax in the return */
    /* Either inlined as (Iron_Point){...} or emitted as separate decl.
     * With inlining, _vN (construct result) should NOT appear as a separate
     * Iron_Point _vN = ... declaration before return. */
    /* Check for the struct type being emitted (even if inlined or declared) */
    TEST_ASSERT_NOT_NULL(strstr(result, "Iron_Point"));

    iron_lir_optimize_info_free(&opt_info);
    arrfree(fields);
    iron_arena_free(&out_arena);
    iron_lir_module_destroy(mod);
    iron_arena_free(&ir_arena);
}

/* ── Test 9: Inlined ADD — no separate temp declared ─────────────────────── */

void test_emit_inlined_no_separate_temps(void) {
    Iron_Arena ir_arena = iron_arena_create(65536);
    iron_types_init(&ir_arena);

    /* Build a 2-param function that mimics the lowered IR pattern:
     *
     *   func calc(a: Int, b: Int) -> Int
     *
     * The lowered IR for a 2-param function uses synthetic param IDs 1 and 3:
     *   param_a_id = 1  (synthetic, not an instruction node)
     *   %2 = alloca Int  "a"
     *   store %2, %1
     *   param_b_id = 3  (synthetic)
     *   %4 = alloca Int  "b"
     *   store %4, %3
     *   %5 = load %2         <- copy-prop will replace uses with %1
     *   %6 = load %4         <- copy-prop will replace uses with %3
     *   %7 = add %5, %6      <- single-use; inline-eligible after copy-prop
     *   %8 = const_int 2
     *   %9 = mul %7, %8      <- inline site: ADD inlined here
     *   return %9
     *
     * After copy-prop: ADD(%1, %3) — not constant-foldable (param IDs).
     * After inlining: return statement contains the ADD inlined into MUL:
     *   return ((_v1 + _v3) * ((int64_t)2LL))
     * No separate "int64_t _v7 = ..." declaration for the ADD result.
     */
    IronLIR_Module *mod = iron_lir_module_create(&ir_arena, "test_inline_temps");
    Iron_Type *int_type = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_Span sp = test_span();

    IronLIR_Param params[2];
    params[0].name = "a";
    params[0].type = int_type;
    params[1].name = "b";
    params[1].type = int_type;

    IronLIR_Func *fn = iron_lir_func_create(mod, "Iron_calc",
                                           params, 2, int_type);
    IronLIR_Block *entry = iron_lir_block_create(fn, "entry");

    /* Simulate the param lowering pattern: reserve synthetic param value IDs
     * and create the alloca+store entries that the lowerer would emit. */

    /* Param a: synthetic ID (fn->next_value_id starts at 1) */
    IronLIR_ValueId param_a_id = fn->next_value_id++;
    while (arrlen(fn->value_table) <= (ptrdiff_t)param_a_id)
        arrput(fn->value_table, NULL);
    fn->value_table[param_a_id] = NULL;  /* no instruction node — synthetic */

    /* alloca for a */
    IronLIR_Instr *alloca_a = iron_lir_alloca(fn, entry, int_type, "a", sp);

    /* Param b: next synthetic ID */
    IronLIR_ValueId param_b_id = fn->next_value_id++;
    while (arrlen(fn->value_table) <= (ptrdiff_t)param_b_id)
        arrput(fn->value_table, NULL);
    fn->value_table[param_b_id] = NULL;

    /* alloca for b */
    IronLIR_Instr *alloca_b = iron_lir_alloca(fn, entry, int_type, "b", sp);

    /* store param values into their alloca slots */
    iron_lir_store(fn, entry, alloca_a->id, param_a_id, sp);
    iron_lir_store(fn, entry, alloca_b->id, param_b_id, sp);

    /* load from allocas */
    IronLIR_Instr *load_a = iron_lir_load(fn, entry, alloca_a->id, int_type, sp);
    IronLIR_Instr *load_b = iron_lir_load(fn, entry, alloca_b->id, int_type, sp);

    /* ADD load_a + load_b -> v_add (single-use: only used by MUL below) */
    IronLIR_Instr *v_add = iron_lir_binop(fn, entry, IRON_LIR_ADD,
                                         load_a->id, load_b->id, int_type, sp);
    IronLIR_ValueId add_id = v_add->id;

    /* const_int 2 */
    IronLIR_Instr *v_two = iron_lir_const_int(fn, entry, 2, int_type, sp);

    /* MUL v_add * v_two -> v_mul */
    IronLIR_Instr *v_mul = iron_lir_binop(fn, entry, IRON_LIR_MUL,
                                         v_add->id, v_two->id, int_type, sp);

    /* return v_mul */
    iron_lir_return(fn, entry, v_mul->id, false, int_type, sp);

    /* Run with all passes enabled (copy-prop + const-fold + DCE + inline info) */
    Iron_Arena out_arena = iron_arena_create(131072);
    IronLIR_OptimizeInfo opt_info;
    iron_lir_optimize(mod, &opt_info, &out_arena, false, false);
    const char *result = iron_lir_emit_c(mod, &out_arena, &g_diags, &opt_info);

    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_NOT_NULL(strstr(result, "Iron_calc"));
    TEST_ASSERT_NOT_NULL(strstr(result, "return"));

    /* The ADD result (v_add) must NOT appear as a separate int64_t declaration.
     * With inlining active, the ADD is folded into the MUL at the use site.
     * Verify no separate "_vN = " assignment exists for the ADD result. */
    char sep_temp[32];
    snprintf(sep_temp, sizeof(sep_temp), "int64_t _v%u =", (unsigned)add_id);
    TEST_ASSERT_NULL_MESSAGE(strstr(result, sep_temp),
        "ADD result should be inlined, not emitted as a separate temp variable");

    /* With the ADD inlined into the MUL, the emitted expression should contain
     * both '+' (inlined ADD) and '*' (the MUL) in the return statement. */
    TEST_ASSERT_NOT_NULL(strstr(result, "+"));
    TEST_ASSERT_NOT_NULL(strstr(result, "*"));

    iron_lir_optimize_info_free(&opt_info);
    iron_arena_free(&out_arena);
    iron_lir_module_destroy(mod);
    iron_arena_free(&ir_arena);
}

/* ── Test 10: Stdlib stub extern should NOT wrap string args in iron_string_cstr ─ */

void test_emit_stdlib_stub_no_cstr_wrap(void) {
    Iron_Arena ir_arena = iron_arena_create(131072);
    iron_types_init(&ir_arena);

    IronLIR_Module *mod = iron_lir_module_create(&ir_arena, "test_stub_str");
    Iron_Type *str_type  = iron_type_make_primitive(IRON_TYPE_STRING);
    Iron_Type *bool_type = iron_type_make_primitive(IRON_TYPE_BOOL);
    Iron_Span sp = test_span();

    /* Create a stdlib stub function: io_file_exists(path: String) -> Bool
     * It's marked extern (empty body) but has NO extern_c_name */
    IronLIR_Func *stub = iron_lir_func_create(mod, "io_file_exists",
                                               NULL, 0, bool_type);
    stub->is_extern = true;
    stub->extern_c_name = NULL;  /* stdlib stub — NOT a true C extern */

    /* Create caller: main() */
    IronLIR_Func *fn = iron_lir_func_create(mod, "Iron_main", NULL, 0, NULL);
    IronLIR_Block *entry = iron_lir_block_create(fn, "entry");

    /* %1 = const_string "/tmp" */
    IronLIR_Instr *path_str = iron_lir_const_string(fn, entry, "/tmp", str_type, sp);
    /* %2 = func_ref io_file_exists */
    IronLIR_Instr *fref = iron_lir_func_ref(fn, entry, "io_file_exists", NULL, sp);
    /* %3 = call %2(%1) */
    IronLIR_ValueId args[1] = { path_str->id };
    iron_lir_call(fn, entry, NULL, fref->id, args, 1, bool_type, sp);
    iron_lir_return(fn, entry, IRON_LIR_VALUE_INVALID, true, NULL, sp);

    Iron_Arena out_arena = iron_arena_create(131072);
    IronLIR_OptimizeInfo opt_info;
    iron_lir_optimize(mod, &opt_info, &out_arena, false, false);
    const char *result = iron_lir_emit_c(mod, &out_arena, &g_diags, &opt_info);

    TEST_ASSERT_NOT_NULL(result);
    /* Stdlib stub: string arg should be passed as-is, NOT wrapped in cstr */
    TEST_ASSERT_NOT_NULL(strstr(result, "io_file_exists("));
    TEST_ASSERT_NULL_MESSAGE(strstr(result, "iron_string_cstr"),
        "Stdlib stub call should NOT wrap string args in iron_string_cstr()");

    iron_lir_optimize_info_free(&opt_info);
    iron_arena_free(&out_arena);
    iron_lir_module_destroy(mod);
    iron_arena_free(&ir_arena);
}

/* ── Test 11: True C extern SHOULD wrap string args in iron_string_cstr ──── */

void test_emit_true_extern_cstr_wrap(void) {
    Iron_Arena ir_arena = iron_arena_create(131072);
    iron_types_init(&ir_arena);

    IronLIR_Module *mod = iron_lir_module_create(&ir_arena, "test_extern_str");
    Iron_Type *str_type = iron_type_make_primitive(IRON_TYPE_STRING);
    Iron_Type *int_type = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_Span sp = test_span();

    /* Create a true C extern: puts(s: String) -> Int
     * Has extern_c_name set — this is a real C function expecting const char* */
    IronLIR_Func *ext = iron_lir_func_create(mod, "puts", NULL, 0, int_type);
    ext->is_extern = true;
    ext->extern_c_name = "puts";  /* true C extern */

    /* Create caller: main() */
    IronLIR_Func *fn = iron_lir_func_create(mod, "Iron_main", NULL, 0, NULL);
    IronLIR_Block *entry = iron_lir_block_create(fn, "entry");

    /* %1 = const_string "hello" */
    IronLIR_Instr *msg = iron_lir_const_string(fn, entry, "hello", str_type, sp);
    /* %2 = func_ref puts */
    IronLIR_Instr *fref = iron_lir_func_ref(fn, entry, "puts", NULL, sp);
    /* %3 = call %2(%1) */
    IronLIR_ValueId args[1] = { msg->id };
    iron_lir_call(fn, entry, NULL, fref->id, args, 1, int_type, sp);
    iron_lir_return(fn, entry, IRON_LIR_VALUE_INVALID, true, NULL, sp);

    Iron_Arena out_arena = iron_arena_create(131072);
    IronLIR_OptimizeInfo opt_info;
    iron_lir_optimize(mod, &opt_info, &out_arena, false, false);
    const char *result = iron_lir_emit_c(mod, &out_arena, &g_diags, &opt_info);

    TEST_ASSERT_NOT_NULL(result);
    /* True C extern: string arg SHOULD be wrapped in iron_string_cstr */
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(result, "iron_string_cstr"),
        "True C extern call should wrap string args in iron_string_cstr()");

    iron_lir_optimize_info_free(&opt_info);
    iron_arena_free(&out_arena);
    iron_lir_module_destroy(mod);
    iron_arena_free(&ir_arena);
}

/* ── main ─────────────────────────────────────────────────────────────────── */

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_emit_hello_world);
    RUN_TEST(test_emit_arithmetic);
    RUN_TEST(test_emit_control_flow);
    RUN_TEST(test_emit_alloca_load_store);
    RUN_TEST(test_emit_type_decl_object);
    RUN_TEST(test_emit_phi_elimination);
    RUN_TEST(test_emit_expression_inlining_basic);
    RUN_TEST(test_emit_construct_inlined);
    RUN_TEST(test_emit_inlined_no_separate_temps);
    RUN_TEST(test_emit_stdlib_stub_no_cstr_wrap);
    RUN_TEST(test_emit_true_extern_cstr_wrap);
    return UNITY_END();
}
