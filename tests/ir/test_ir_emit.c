/* test_ir_emit.c — Unity tests for the IR-to-C emission backend (Phase 9).
 *
 * Tests hand-build IrModules using the IR constructor API, call
 * iron_ir_emit_c(), and verify the output C string contains expected patterns.
 *
 * Tests:
 *   1. test_emit_hello_world        — Iron_main with const_string + call
 *   2. test_emit_arithmetic         — Function with CONST_INT + ADD + RETURN
 *   3. test_emit_control_flow       — Function with BRANCH terminator
 *   4. test_emit_alloca_load_store  — Function with ALLOCA + STORE + LOAD
 *   5. test_emit_type_decl_object   — Module with an object type_decl
 *   6. test_emit_phi_elimination    — Function with PHI instruction
 */

#include "unity.h"
#include "ir/emit_c.h"
#include "ir/ir_optimize.h"
#include "ir/ir.h"
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

    IronIR_Module *mod = iron_ir_module_create(&ir_arena, "test_hello");

    /* func Iron_main() -> void */
    IronIR_Func *fn = iron_ir_func_create(mod, "Iron_main", NULL, 0, NULL);
    IronIR_Block *entry = iron_ir_block_create(fn, "entry");

    Iron_Span sp = test_span();
    Iron_Type *str_type = iron_type_make_primitive(IRON_TYPE_STRING);

    /* %1 = const_string "Hello" */
    IronIR_Instr *s = iron_ir_const_string(fn, entry, "Hello", str_type, sp);
    (void)s;

    /* return void */
    iron_ir_return(fn, entry, IRON_IR_VALUE_INVALID, true, NULL, sp);

    /* Emit — skip new passes (copy-prop/const-fold/DCE) so the unused
     * const_string survives in the IR and iron_string_from_literal is emitted. */
    Iron_Arena out_arena = iron_arena_create(131072);
    IronIR_OptimizeInfo opt_info_1;
    iron_ir_optimize(mod, &opt_info_1, &out_arena, false, true);
    const char *result = iron_ir_emit_c(mod, &out_arena, &g_diags, &opt_info_1);

    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_NOT_NULL(strstr(result, "#include \"runtime/iron_runtime.h\""));
    TEST_ASSERT_NOT_NULL(strstr(result, "Iron_main"));
    TEST_ASSERT_NOT_NULL(strstr(result, "int main("));
    TEST_ASSERT_NOT_NULL(strstr(result, "iron_runtime_init()"));
    TEST_ASSERT_NOT_NULL(strstr(result, "iron_runtime_shutdown()"));
    TEST_ASSERT_NOT_NULL(strstr(result, "iron_string_from_literal"));

    iron_ir_optimize_info_free(&opt_info_1);
    iron_arena_free(&out_arena);
    iron_ir_module_destroy(mod);
    iron_arena_free(&ir_arena);
}

/* ── Test 2: Arithmetic — CONST_INT + ADD + RETURN ───────────────────────── */

void test_emit_arithmetic(void) {
    Iron_Arena ir_arena = iron_arena_create(65536);
    iron_types_init(&ir_arena);

    IronIR_Module *mod = iron_ir_module_create(&ir_arena, "test_arith");

    Iron_Type *int_type = iron_type_make_primitive(IRON_TYPE_INT);

    /* func add_ints() -> Int */
    IronIR_Func *fn = iron_ir_func_create(mod, "Iron_add_ints", NULL, 0,
                                          int_type);
    IronIR_Block *entry = iron_ir_block_create(fn, "entry");
    Iron_Span sp = test_span();

    /* %1 = const_int 10 */
    IronIR_Instr *c1 = iron_ir_const_int(fn, entry, 10, int_type, sp);
    /* %2 = const_int 20 */
    IronIR_Instr *c2 = iron_ir_const_int(fn, entry, 20, int_type, sp);
    /* %3 = add %1, %2 */
    IronIR_Instr *sum = iron_ir_binop(fn, entry, IRON_IR_ADD,
                                       c1->id, c2->id, int_type, sp);
    /* return %3 */
    iron_ir_return(fn, entry, sum->id, false, int_type, sp);

    /* Skip new passes (copy-prop/const-fold/DCE) so constant folding doesn't
     * eliminate the ADD before emission — this test is checking emitter output. */
    Iron_Arena out_arena = iron_arena_create(131072);
    IronIR_OptimizeInfo opt_info_2;
    iron_ir_optimize(mod, &opt_info_2, &out_arena, false, true);
    const char *result = iron_ir_emit_c(mod, &out_arena, &g_diags, &opt_info_2);

    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_NOT_NULL(strstr(result, "int64_t"));
    TEST_ASSERT_NOT_NULL(strstr(result, "+"));
    TEST_ASSERT_NOT_NULL(strstr(result, "_v"));
    /* No main wrapper since no Iron_main */
    TEST_ASSERT_NULL(strstr(result, "int main("));

    iron_ir_optimize_info_free(&opt_info_2);
    iron_arena_free(&out_arena);
    iron_ir_module_destroy(mod);
    iron_arena_free(&ir_arena);
}

/* ── Test 3: Control flow — BRANCH terminator (two blocks) ──────────────── */

void test_emit_control_flow(void) {
    Iron_Arena ir_arena = iron_arena_create(65536);
    iron_types_init(&ir_arena);

    IronIR_Module *mod = iron_ir_module_create(&ir_arena, "test_cf");
    Iron_Type *bool_type = iron_type_make_primitive(IRON_TYPE_BOOL);
    Iron_Span sp = test_span();

    /* func branch_test(cond: Bool) -> void */
    IronIR_Param params[1];
    params[0].name = "cond";
    params[0].type = bool_type;

    IronIR_Func *fn = iron_ir_func_create(mod, "Iron_branch_test",
                                          params, 1, NULL);
    IronIR_Block *entry = iron_ir_block_create(fn, "entry");
    IronIR_Block *then_b = iron_ir_block_create(fn, "then_0");
    IronIR_Block *else_b = iron_ir_block_create(fn, "else_0");
    IronIR_Block *merge  = iron_ir_block_create(fn, "merge_0");

    /* entry: branch cond -> then_0, else_0 */
    IronIR_Instr *cond_val = iron_ir_const_bool(fn, entry, true, bool_type, sp);
    iron_ir_branch(fn, entry, cond_val->id, then_b->id, else_b->id, sp);

    /* then_0: jump merge_0 */
    iron_ir_jump(fn, then_b, merge->id, sp);

    /* else_0: jump merge_0 */
    iron_ir_jump(fn, else_b, merge->id, sp);

    /* merge_0: return void */
    iron_ir_return(fn, merge, IRON_IR_VALUE_INVALID, true, NULL, sp);

    Iron_Arena out_arena = iron_arena_create(131072);
    IronIR_OptimizeInfo opt_info_3;
    iron_ir_optimize(mod, &opt_info_3, &out_arena, false, false);
    const char *result = iron_ir_emit_c(mod, &out_arena, &g_diags, &opt_info_3);

    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_NOT_NULL(strstr(result, "goto"));
    TEST_ASSERT_NOT_NULL(strstr(result, "then_0"));
    TEST_ASSERT_NOT_NULL(strstr(result, "else_0"));
    TEST_ASSERT_NOT_NULL(strstr(result, "if ("));
    TEST_ASSERT_NOT_NULL(strstr(result, "merge_0"));

    iron_ir_optimize_info_free(&opt_info_3);
    iron_arena_free(&out_arena);
    iron_ir_module_destroy(mod);
    iron_arena_free(&ir_arena);
}

/* ── Test 4: Alloca / Load / Store ───────────────────────────────────────── */

void test_emit_alloca_load_store(void) {
    Iron_Arena ir_arena = iron_arena_create(65536);
    iron_types_init(&ir_arena);

    IronIR_Module *mod = iron_ir_module_create(&ir_arena, "test_alloca");
    Iron_Type *int_type = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_Span sp = test_span();

    /* func alloca_test() -> Int */
    IronIR_Func *fn = iron_ir_func_create(mod, "Iron_alloca_test",
                                          NULL, 0, int_type);
    IronIR_Block *entry = iron_ir_block_create(fn, "entry");

    /* %1 = alloca Int */
    IronIR_Instr *slot = iron_ir_alloca(fn, entry, int_type, "x", sp);
    /* %2 = const_int 42 */
    IronIR_Instr *val  = iron_ir_const_int(fn, entry, 42, int_type, sp);
    /* store %1, %2 */
    iron_ir_store(fn, entry, slot->id, val->id, sp);
    /* %3 = load %1 */
    IronIR_Instr *loaded = iron_ir_load(fn, entry, slot->id, int_type, sp);
    /* return %3 */
    iron_ir_return(fn, entry, loaded->id, false, int_type, sp);

    Iron_Arena out_arena = iron_arena_create(131072);
    IronIR_OptimizeInfo opt_info_4;
    iron_ir_optimize(mod, &opt_info_4, &out_arena, false, false);
    const char *result = iron_ir_emit_c(mod, &out_arena, &g_diags, &opt_info_4);

    TEST_ASSERT_NOT_NULL(result);
    /* Alloca emits a variable declaration */
    TEST_ASSERT_NOT_NULL(strstr(result, "int64_t"));
    /* Store emits assignment: _vN = _vM */
    TEST_ASSERT_NOT_NULL(strstr(result, " = "));
    /* Load emits copy: _vK = _vN */
    TEST_ASSERT_NOT_NULL(strstr(result, "return"));

    iron_ir_optimize_info_free(&opt_info_4);
    iron_arena_free(&out_arena);
    iron_ir_module_destroy(mod);
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
    IronIR_Module *mod = iron_ir_module_create(&ir_arena, "test_type_decl");
    iron_ir_module_add_type_decl(mod, IRON_IR_TYPE_OBJECT, "Player", obj_type);

    /* Add a trivial function so the module emits something */
    IronIR_Func *fn = iron_ir_func_create(mod, "Iron_main", NULL, 0, NULL);
    IronIR_Block *entry = iron_ir_block_create(fn, "entry");
    iron_ir_return(fn, entry, IRON_IR_VALUE_INVALID, true, NULL, test_span());

    Iron_Arena out_arena = iron_arena_create(131072);
    IronIR_OptimizeInfo opt_info_5;
    iron_ir_optimize(mod, &opt_info_5, &out_arena, false, false);
    const char *result = iron_ir_emit_c(mod, &out_arena, &g_diags, &opt_info_5);

    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_NOT_NULL(strstr(result, "typedef struct"));
    TEST_ASSERT_NOT_NULL(strstr(result, "Iron_Player"));
    TEST_ASSERT_NOT_NULL(strstr(result, "IRON_TAG_"));

    iron_ir_optimize_info_free(&opt_info_5);
    arrfree(fields);
    iron_arena_free(&out_arena);
    iron_ir_module_destroy(mod);
    iron_arena_free(&ir_arena);
}

/* ── Test 6: Phi elimination ─────────────────────────────────────────────── */

void test_emit_phi_elimination(void) {
    Iron_Arena ir_arena = iron_arena_create(65536);
    iron_types_init(&ir_arena);

    IronIR_Module *mod = iron_ir_module_create(&ir_arena, "test_phi");
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
    IronIR_Param params[2];
    params[0].name = "a";
    params[0].type = int_type;
    params[1].name = "b";
    params[1].type = int_type;

    IronIR_Func *fn = iron_ir_func_create(mod, "Iron_phi_test",
                                          params, 2, int_type);
    IronIR_Block *entry   = iron_ir_block_create(fn, "entry");
    IronIR_Block *then_b  = iron_ir_block_create(fn, "then_0");
    IronIR_Block *else_b  = iron_ir_block_create(fn, "else_0");
    IronIR_Block *merge_b = iron_ir_block_create(fn, "merge_0");

    /* entry: create two values and a branch */
    IronIR_Instr *va = iron_ir_const_int(fn, entry, 10, int_type, sp);
    IronIR_Instr *vb = iron_ir_const_int(fn, entry, 20, int_type, sp);
    IronIR_Instr *cmp = iron_ir_binop(fn, entry, IRON_IR_GT,
                                       va->id, vb->id, bool_type, sp);
    iron_ir_branch(fn, entry, cmp->id, then_b->id, else_b->id, sp);

    /* then_0: jump merge_0 */
    iron_ir_jump(fn, then_b, merge_b->id, sp);

    /* else_0: jump merge_0 */
    iron_ir_jump(fn, else_b, merge_b->id, sp);

    /* merge_0: phi + return */
    IronIR_Instr *phi = iron_ir_phi(fn, merge_b, int_type, sp);
    iron_ir_phi_add_incoming(phi, va->id, then_b->id);
    iron_ir_phi_add_incoming(phi, vb->id, else_b->id);
    iron_ir_return(fn, merge_b, phi->id, false, int_type, sp);

    Iron_Arena out_arena = iron_arena_create(131072);
    IronIR_OptimizeInfo opt_info_6;
    iron_ir_optimize(mod, &opt_info_6, &out_arena, false, false);
    const char *result = iron_ir_emit_c(mod, &out_arena, &g_diags, &opt_info_6);

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

    iron_ir_optimize_info_free(&opt_info_6);
    iron_arena_free(&out_arena);
    iron_ir_module_destroy(mod);
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
    return UNITY_END();
}
