/* test_ir_optimize.c — Unity tests for the IR optimization passes (Phase 15).
 *
 * Tests hand-build IR modules using the IR constructor API, run
 * iron_ir_optimize(), and verify the optimizer's behavior in isolation.
 *
 * Tests:
 *   1. test_instr_is_pure_classification       — pure vs side-effecting kinds
 *   2. test_copy_propagation_single_store_alloca — single-store alloca: LOAD eliminated
 *   3. test_copy_propagation_multi_store_skipped — multi-store alloca: LOAD kept
 *   4. test_dce_removes_unused_pure_instruction  — unused ADD removed from block
 *   5. test_dce_preserves_side_effecting         — CALL never removed even if unused
 *   6. test_constant_folding_add                 — CONST_INT(3)+CONST_INT(4) -> CONST_INT(7)
 *   7. test_constant_folding_div_by_zero_skipped — DIV(10,0) not folded
 *   8. test_fixpoint_copy_prop_then_dce          — copy-prop enables DCE in same run
 *   9. test_optimize_empty_function              — empty function: no crash
 *  10. test_optimize_extern_function_skipped     — extern fn: no crash, skipped cleanly
 */

#include "unity.h"
#include "ir/ir.h"
#include "ir/ir_optimize.h"
#include "ir/verify.h"
#include "analyzer/types.h"
#include "diagnostics/diagnostics.h"
#include "util/arena.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>

/* ── Fixtures ────────────────────────────────────────────────────────────── */

static Iron_Arena g_arena;

void setUp(void) {
    g_arena = iron_arena_create(131072);
    iron_types_init(&g_arena);
}

void tearDown(void) {
    iron_arena_free(&g_arena);
}

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static Iron_Span test_span(void) {
    return iron_span_make("test.iron", 1, 1, 1, 1);
}

/* Count instructions of a specific kind in a block */
static int count_kind_in_block(IronIR_Block *blk, IronIR_InstrKind kind) {
    int n = 0;
    for (int i = 0; i < blk->instr_count; i++) {
        if (blk->instrs[i]->kind == kind) n++;
    }
    return n;
}

/* ── Test 1: iron_ir_instr_is_pure classification ───────────────────────── */

void test_instr_is_pure_classification(void) {
    /* Pure kinds — no observable side effects */
    TEST_ASSERT_TRUE(iron_ir_instr_is_pure(IRON_IR_CONST_INT));
    TEST_ASSERT_TRUE(iron_ir_instr_is_pure(IRON_IR_CONST_FLOAT));
    TEST_ASSERT_TRUE(iron_ir_instr_is_pure(IRON_IR_CONST_BOOL));
    TEST_ASSERT_TRUE(iron_ir_instr_is_pure(IRON_IR_CONST_STRING));
    TEST_ASSERT_TRUE(iron_ir_instr_is_pure(IRON_IR_CONST_NULL));

    TEST_ASSERT_TRUE(iron_ir_instr_is_pure(IRON_IR_ADD));
    TEST_ASSERT_TRUE(iron_ir_instr_is_pure(IRON_IR_SUB));
    TEST_ASSERT_TRUE(iron_ir_instr_is_pure(IRON_IR_MUL));
    TEST_ASSERT_TRUE(iron_ir_instr_is_pure(IRON_IR_DIV));
    TEST_ASSERT_TRUE(iron_ir_instr_is_pure(IRON_IR_MOD));

    TEST_ASSERT_TRUE(iron_ir_instr_is_pure(IRON_IR_EQ));
    TEST_ASSERT_TRUE(iron_ir_instr_is_pure(IRON_IR_NEQ));
    TEST_ASSERT_TRUE(iron_ir_instr_is_pure(IRON_IR_LT));
    TEST_ASSERT_TRUE(iron_ir_instr_is_pure(IRON_IR_LTE));
    TEST_ASSERT_TRUE(iron_ir_instr_is_pure(IRON_IR_GT));
    TEST_ASSERT_TRUE(iron_ir_instr_is_pure(IRON_IR_GTE));

    TEST_ASSERT_TRUE(iron_ir_instr_is_pure(IRON_IR_AND));
    TEST_ASSERT_TRUE(iron_ir_instr_is_pure(IRON_IR_OR));

    TEST_ASSERT_TRUE(iron_ir_instr_is_pure(IRON_IR_NEG));
    TEST_ASSERT_TRUE(iron_ir_instr_is_pure(IRON_IR_NOT));

    TEST_ASSERT_TRUE(iron_ir_instr_is_pure(IRON_IR_LOAD));
    TEST_ASSERT_TRUE(iron_ir_instr_is_pure(IRON_IR_CAST));
    TEST_ASSERT_TRUE(iron_ir_instr_is_pure(IRON_IR_GET_FIELD));
    TEST_ASSERT_TRUE(iron_ir_instr_is_pure(IRON_IR_GET_INDEX));
    TEST_ASSERT_TRUE(iron_ir_instr_is_pure(IRON_IR_CONSTRUCT));
    TEST_ASSERT_TRUE(iron_ir_instr_is_pure(IRON_IR_ARRAY_LIT));
    TEST_ASSERT_TRUE(iron_ir_instr_is_pure(IRON_IR_IS_NULL));
    TEST_ASSERT_TRUE(iron_ir_instr_is_pure(IRON_IR_IS_NOT_NULL));
    TEST_ASSERT_TRUE(iron_ir_instr_is_pure(IRON_IR_SLICE));
    TEST_ASSERT_TRUE(iron_ir_instr_is_pure(IRON_IR_MAKE_CLOSURE));
    TEST_ASSERT_TRUE(iron_ir_instr_is_pure(IRON_IR_FUNC_REF));

    /* Side-effecting kinds */
    TEST_ASSERT_FALSE(iron_ir_instr_is_pure(IRON_IR_CALL));
    TEST_ASSERT_FALSE(iron_ir_instr_is_pure(IRON_IR_STORE));
    TEST_ASSERT_FALSE(iron_ir_instr_is_pure(IRON_IR_RETURN));
    TEST_ASSERT_FALSE(iron_ir_instr_is_pure(IRON_IR_SET_INDEX));
    TEST_ASSERT_FALSE(iron_ir_instr_is_pure(IRON_IR_SET_FIELD));
    TEST_ASSERT_FALSE(iron_ir_instr_is_pure(IRON_IR_FREE));
    TEST_ASSERT_FALSE(iron_ir_instr_is_pure(IRON_IR_JUMP));
    TEST_ASSERT_FALSE(iron_ir_instr_is_pure(IRON_IR_BRANCH));
    TEST_ASSERT_FALSE(iron_ir_instr_is_pure(IRON_IR_SWITCH));
    TEST_ASSERT_FALSE(iron_ir_instr_is_pure(IRON_IR_SPAWN));
    TEST_ASSERT_FALSE(iron_ir_instr_is_pure(IRON_IR_PARALLEL_FOR));
    TEST_ASSERT_FALSE(iron_ir_instr_is_pure(IRON_IR_AWAIT));
    TEST_ASSERT_FALSE(iron_ir_instr_is_pure(IRON_IR_HEAP_ALLOC));
    TEST_ASSERT_FALSE(iron_ir_instr_is_pure(IRON_IR_RC_ALLOC));
}

/* ── Test 2: Copy propagation — single-store alloca ────────────────────── */

void test_copy_propagation_single_store_alloca(void) {
    Iron_Arena ir_arena = iron_arena_create(65536);
    iron_types_init(&ir_arena);

    IronIR_Module *mod = iron_ir_module_create(&ir_arena, "test_cp_single");
    Iron_Type *int_type = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_Span sp = test_span();

    /* func test() -> Int
     *   %1 = alloca Int
     *   %2 = const_int 99
     *   store %1, %2
     *   %3 = load %1        <- this LOAD should be propagated away to %2
     *   return %3
     *
     * After copy propagation, the RETURN should reference %2 directly (not %3).
     * After DCE, the LOAD instruction (%3) should be removed from the block.
     *
     * We verify both effects:
     *   1. The block has no LOAD instructions after optimization
     *   2. The value_table entry for the original load_id is NULL (DCE'd) or
     *      the RETURN refers directly to const_int 99 (which may have been const-folded)
     */
    IronIR_Func *fn = iron_ir_func_create(mod, "Iron_test", NULL, 0, int_type);
    IronIR_Block *entry = iron_ir_block_create(fn, "entry");

    IronIR_Instr *slot   = iron_ir_alloca(fn, entry, int_type, "x", sp);
    IronIR_Instr *val99  = iron_ir_const_int(fn, entry, 99, int_type, sp);
    iron_ir_store(fn, entry, slot->id, val99->id, sp);
    IronIR_Instr *loaded = iron_ir_load(fn, entry, slot->id, int_type, sp);
    IronIR_ValueId load_id = loaded->id;
    IronIR_ValueId val99_id = val99->id;
    iron_ir_return(fn, entry, loaded->id, false, int_type, sp);

    int before = entry->instr_count;

    IronIR_OptimizeInfo info;
    iron_ir_optimize(mod, &info, &ir_arena, false, false);

    /* After copy propagation + DCE:
     * - block should have fewer instructions (LOAD eliminated)
     * - no LOAD instructions should remain in the block */
    TEST_ASSERT_LESS_THAN_INT(before, entry->instr_count);
    TEST_ASSERT_EQUAL_INT(0, count_kind_in_block(entry, IRON_IR_LOAD));

    /* The value_table entry for the load should be NULL (DCE removed it) */
    IronIR_Instr *load_after = fn->value_table[load_id];
    TEST_ASSERT_NULL(load_after);

    /* The const_int 99 should still be present (it's the returned value) */
    IronIR_Instr *val_after = fn->value_table[val99_id];
    TEST_ASSERT_NOT_NULL(val_after);
    TEST_ASSERT_EQUAL_INT(IRON_IR_CONST_INT, val_after->kind);
    TEST_ASSERT_EQUAL_INT64(99, val_after->const_int.value);

    iron_ir_optimize_info_free(&info);
    iron_ir_module_destroy(mod);
    iron_arena_free(&ir_arena);
}

/* ── Test 3: Copy propagation — multi-store alloca skipped ──────────────── */

void test_copy_propagation_multi_store_skipped(void) {
    Iron_Arena ir_arena = iron_arena_create(65536);
    iron_types_init(&ir_arena);

    IronIR_Module *mod = iron_ir_module_create(&ir_arena, "test_cp_multi");
    Iron_Type *int_type = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_Span sp = test_span();

    /* func test() -> Int
     *   %1 = alloca Int
     *   %2 = const_int 1
     *   %3 = const_int 2
     *   store %1, %2     <- store #1
     *   store %1, %3     <- store #2 — alloca has 2 stores, not eligible
     *   %4 = load %1
     *   %5 = add %4, %2
     *   return %5
     */
    IronIR_Func *fn = iron_ir_func_create(mod, "Iron_test2", NULL, 0, int_type);
    IronIR_Block *entry = iron_ir_block_create(fn, "entry");

    IronIR_Instr *slot  = iron_ir_alloca(fn, entry, int_type, "x", sp);
    IronIR_Instr *v1    = iron_ir_const_int(fn, entry, 1, int_type, sp);
    IronIR_Instr *v2    = iron_ir_const_int(fn, entry, 2, int_type, sp);
    iron_ir_store(fn, entry, slot->id, v1->id, sp);
    iron_ir_store(fn, entry, slot->id, v2->id, sp);
    IronIR_Instr *loaded = iron_ir_load(fn, entry, slot->id, int_type, sp);
    IronIR_Instr *add    = iron_ir_binop(fn, entry, IRON_IR_ADD,
                                          loaded->id, v1->id, int_type, sp);
    iron_ir_return(fn, entry, add->id, false, int_type, sp);

    IronIR_ValueId load_id = loaded->id;
    IronIR_ValueId add_id  = add->id;

    IronIR_OptimizeInfo info;
    iron_ir_optimize(mod, &info, &ir_arena, false, false);

    /* With 2 stores, copy propagation must NOT replace the LOAD.
     * The ADD's left operand should still be the LOAD result. */
    IronIR_Instr *add_after = fn->value_table[add_id];
    TEST_ASSERT_NOT_NULL(add_after);
    TEST_ASSERT_EQUAL_UINT(IRON_IR_ADD, add_after->kind);
    /* Either load_id itself is still there OR value_table[load_id] is a LOAD/CONST */
    /* The key invariant: the ADD's left did NOT become v1 or v2 directly */
    TEST_ASSERT_NOT_EQUAL_UINT(v1->id, add_after->binop.left);
    TEST_ASSERT_NOT_EQUAL_UINT(v2->id, add_after->binop.left);
    /* The left should still reference the original load_id */
    TEST_ASSERT_EQUAL_UINT(load_id, add_after->binop.left);

    iron_ir_optimize_info_free(&info);
    iron_ir_module_destroy(mod);
    iron_arena_free(&ir_arena);
}

/* ── Test 4: DCE removes unused pure instruction ────────────────────────── */

void test_dce_removes_unused_pure_instruction(void) {
    Iron_Arena ir_arena = iron_arena_create(65536);
    iron_types_init(&ir_arena);

    IronIR_Module *mod = iron_ir_module_create(&ir_arena, "test_dce_rm");
    Iron_Type *int_type = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_Span sp = test_span();

    /* func test() -> void
     *   %1 = const_int 42
     *   %2 = const_int 10
     *   %3 = add %1, %2   <- result never used; should be eliminated
     *   return void
     */
    IronIR_Func *fn = iron_ir_func_create(mod, "Iron_dce_test", NULL, 0, NULL);
    IronIR_Block *entry = iron_ir_block_create(fn, "entry");

    IronIR_Instr *c1  = iron_ir_const_int(fn, entry, 42, int_type, sp);
    IronIR_Instr *c2  = iron_ir_const_int(fn, entry, 10, int_type, sp);
    /* add result is never referenced */
    iron_ir_binop(fn, entry, IRON_IR_ADD, c1->id, c2->id, int_type, sp);
    iron_ir_return(fn, entry, IRON_IR_VALUE_INVALID, true, NULL, sp);

    int before = entry->instr_count;

    IronIR_OptimizeInfo info;
    iron_ir_optimize(mod, &info, &ir_arena, false, false);

    /* After DCE the block should have fewer instructions (ADD was removed).
     * Only the RETURN and possibly no live constants should remain. */
    TEST_ASSERT_LESS_THAN_INT(before, entry->instr_count);
    TEST_ASSERT_EQUAL_INT(0, count_kind_in_block(entry, IRON_IR_ADD));

    iron_ir_optimize_info_free(&info);
    iron_ir_module_destroy(mod);
    iron_arena_free(&ir_arena);
}

/* ── Test 5: DCE preserves side-effecting instructions ──────────────────── */

void test_dce_preserves_side_effecting(void) {
    Iron_Arena ir_arena = iron_arena_create(65536);
    iron_types_init(&ir_arena);

    IronIR_Module *mod = iron_ir_module_create(&ir_arena, "test_dce_keep");
    Iron_Type *int_type = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_Span sp = test_span();

    /* func test() -> void
     *   %1 = func_ref "some_func"     <- function pointer
     *   %2 = call %1()                <- side-effecting even if result unused
     *   return void
     */
    IronIR_Func *fn = iron_ir_func_create(mod, "Iron_dce_keep", NULL, 0, NULL);
    IronIR_Block *entry = iron_ir_block_create(fn, "entry");

    /* func_ref is pure but the CALL itself is side-effecting */
    Iron_Type *fn_type = iron_type_make_primitive(IRON_TYPE_INT);
    IronIR_Instr *fref = iron_ir_func_ref(fn, entry, "some_func", fn_type, sp);

    /* Indirect call via func_ptr — result is int but never used */
    iron_ir_call(fn, entry, NULL, fref->id, NULL, 0, int_type, sp);
    iron_ir_return(fn, entry, IRON_IR_VALUE_INVALID, true, NULL, sp);

    IronIR_OptimizeInfo info;
    iron_ir_optimize(mod, &info, &ir_arena, false, false);

    /* CALL must still be present */
    TEST_ASSERT_EQUAL_INT(1, count_kind_in_block(entry, IRON_IR_CALL));

    iron_ir_optimize_info_free(&info);
    iron_ir_module_destroy(mod);
    iron_arena_free(&ir_arena);
}

/* ── Test 6: Constant folding — add two constants ───────────────────────── */

void test_constant_folding_add(void) {
    Iron_Arena ir_arena = iron_arena_create(65536);
    iron_types_init(&ir_arena);

    IronIR_Module *mod = iron_ir_module_create(&ir_arena, "test_cf_add");
    Iron_Type *int_type = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_Span sp = test_span();

    /* func test() -> Int
     *   %1 = const_int 3
     *   %2 = const_int 4
     *   %3 = add %1, %2   <- should be folded to const_int 7
     *   return %3
     */
    IronIR_Func *fn = iron_ir_func_create(mod, "Iron_cf_add", NULL, 0, int_type);
    IronIR_Block *entry = iron_ir_block_create(fn, "entry");

    IronIR_Instr *c3  = iron_ir_const_int(fn, entry, 3, int_type, sp);
    IronIR_Instr *c4  = iron_ir_const_int(fn, entry, 4, int_type, sp);
    IronIR_Instr *add = iron_ir_binop(fn, entry, IRON_IR_ADD,
                                       c3->id, c4->id, int_type, sp);
    IronIR_ValueId add_id = add->id;
    iron_ir_return(fn, entry, add_id, false, int_type, sp);

    IronIR_OptimizeInfo info;
    iron_ir_optimize(mod, &info, &ir_arena, false, false);

    /* The ADD instruction should have been replaced by CONST_INT(7) in-place */
    IronIR_Instr *after = fn->value_table[add_id];
    TEST_ASSERT_NOT_NULL(after);
    TEST_ASSERT_EQUAL_INT(IRON_IR_CONST_INT, after->kind);
    TEST_ASSERT_EQUAL_INT64(7, after->const_int.value);

    iron_ir_optimize_info_free(&info);
    iron_ir_module_destroy(mod);
    iron_arena_free(&ir_arena);
}

/* ── Test 7: Constant folding — division by zero skipped ────────────────── */

void test_constant_folding_div_by_zero_skipped(void) {
    Iron_Arena ir_arena = iron_arena_create(65536);
    iron_types_init(&ir_arena);

    IronIR_Module *mod = iron_ir_module_create(&ir_arena, "test_cf_div0");
    Iron_Type *int_type = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_Span sp = test_span();

    /* func test() -> Int
     *   %1 = const_int 10
     *   %2 = const_int 0
     *   %3 = div %1, %2   <- must NOT be folded (div by zero)
     *   return %3
     */
    IronIR_Func *fn = iron_ir_func_create(mod, "Iron_cf_div0", NULL, 0, int_type);
    IronIR_Block *entry = iron_ir_block_create(fn, "entry");

    IronIR_Instr *c10  = iron_ir_const_int(fn, entry, 10, int_type, sp);
    IronIR_Instr *c0   = iron_ir_const_int(fn, entry, 0,  int_type, sp);
    IronIR_Instr *div  = iron_ir_binop(fn, entry, IRON_IR_DIV,
                                        c10->id, c0->id, int_type, sp);
    IronIR_ValueId div_id = div->id;
    iron_ir_return(fn, entry, div_id, false, int_type, sp);

    IronIR_OptimizeInfo info;
    iron_ir_optimize(mod, &info, &ir_arena, false, false);

    /* DIV must still be present — not folded */
    IronIR_Instr *after = fn->value_table[div_id];
    TEST_ASSERT_NOT_NULL(after);
    TEST_ASSERT_EQUAL_INT(IRON_IR_DIV, after->kind);

    iron_ir_optimize_info_free(&info);
    iron_ir_module_destroy(mod);
    iron_arena_free(&ir_arena);
}

/* ── Test 8: Fixpoint — copy prop enables DCE ───────────────────────────── */

void test_fixpoint_copy_prop_then_dce(void) {
    Iron_Arena ir_arena = iron_arena_create(65536);
    iron_types_init(&ir_arena);

    IronIR_Module *mod = iron_ir_module_create(&ir_arena, "test_fixpoint");
    Iron_Type *int_type = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_Span sp = test_span();

    /* func test() -> void
     *   %1 = alloca Int
     *   %2 = const_int 42
     *   store %1, %2
     *   %3 = load %1      <- after copy-prop, all uses of %3 become %2
     *   (no uses of %3)   <- after DCE, %3 LOAD is eliminated
     *   return void
     *
     * The combined effect: after one fixpoint iteration, the LOAD (and
     * potentially the STORE and ALLOCA) should be gone.
     */
    IronIR_Func *fn = iron_ir_func_create(mod, "Iron_fixpoint", NULL, 0, NULL);
    IronIR_Block *entry = iron_ir_block_create(fn, "entry");

    iron_ir_alloca(fn, entry, int_type, "x", sp);
    IronIR_Instr *c42 = iron_ir_const_int(fn, entry, 42, int_type, sp);
    /* We don't actually store here to make the LOAD result unused directly,
     * so DCE will remove it.  We use alloca/store/load so copy-prop runs first. */
    IronIR_Instr *slot2 = iron_ir_alloca(fn, entry, int_type, "y", sp);
    iron_ir_store(fn, entry, slot2->id, c42->id, sp);
    iron_ir_load(fn, entry, slot2->id, int_type, sp); /* load result never used */
    iron_ir_return(fn, entry, IRON_IR_VALUE_INVALID, true, NULL, sp);

    int before = entry->instr_count;

    IronIR_OptimizeInfo info;
    iron_ir_optimize(mod, &info, &ir_arena, false, false);

    /* The fixpoint loop should have reduced the instruction count */
    TEST_ASSERT_LESS_THAN_INT(before, entry->instr_count);
    /* No LOAD should remain after DCE removes it */
    TEST_ASSERT_EQUAL_INT(0, count_kind_in_block(entry, IRON_IR_LOAD));

    iron_ir_optimize_info_free(&info);
    iron_ir_module_destroy(mod);
    iron_arena_free(&ir_arena);
}

/* ── Test 9: Empty function — no crash ──────────────────────────────────── */

void test_optimize_empty_function(void) {
    Iron_Arena ir_arena = iron_arena_create(65536);
    iron_types_init(&ir_arena);

    IronIR_Module *mod = iron_ir_module_create(&ir_arena, "test_empty");
    Iron_Span sp = test_span();

    /* func test() -> void  (single block with only RETURN) */
    IronIR_Func *fn = iron_ir_func_create(mod, "Iron_empty", NULL, 0, NULL);
    IronIR_Block *entry = iron_ir_block_create(fn, "entry");
    iron_ir_return(fn, entry, IRON_IR_VALUE_INVALID, true, NULL, sp);

    int before = entry->instr_count;

    IronIR_OptimizeInfo info;
    /* Must not crash */
    iron_ir_optimize(mod, &info, &ir_arena, false, false);

    /* RETURN is side-effecting; it must be preserved */
    TEST_ASSERT_EQUAL_INT(before, entry->instr_count);
    TEST_ASSERT_EQUAL_INT(1, count_kind_in_block(entry, IRON_IR_RETURN));

    iron_ir_optimize_info_free(&info);
    iron_ir_module_destroy(mod);
    iron_arena_free(&ir_arena);
}

/* ── Test 10: Extern function — skipped cleanly ─────────────────────────── */

void test_optimize_extern_function_skipped(void) {
    Iron_Arena ir_arena = iron_arena_create(65536);
    iron_types_init(&ir_arena);

    IronIR_Module *mod = iron_ir_module_create(&ir_arena, "test_extern");

    /* Create an extern function — no blocks */
    IronIR_Func *fn = iron_ir_func_create(mod, "Iron_some_extern", NULL, 0, NULL);
    fn->is_extern = true;

    IronIR_OptimizeInfo info;
    /* Must not crash even though there are no blocks */
    iron_ir_optimize(mod, &info, &ir_arena, false, false);

    /* Extern fn should remain unchanged */
    TEST_ASSERT_TRUE(fn->is_extern);
    TEST_ASSERT_EQUAL_INT(0, fn->block_count);

    iron_ir_optimize_info_free(&info);
    iron_ir_module_destroy(mod);
    iron_arena_free(&ir_arena);
}

/* ── main ─────────────────────────────────────────────────────────────────── */

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_instr_is_pure_classification);
    RUN_TEST(test_copy_propagation_single_store_alloca);
    RUN_TEST(test_copy_propagation_multi_store_skipped);
    RUN_TEST(test_dce_removes_unused_pure_instruction);
    RUN_TEST(test_dce_preserves_side_effecting);
    RUN_TEST(test_constant_folding_add);
    RUN_TEST(test_constant_folding_div_by_zero_skipped);
    RUN_TEST(test_fixpoint_copy_prop_then_dce);
    RUN_TEST(test_optimize_empty_function);
    RUN_TEST(test_optimize_extern_function_skipped);
    return UNITY_END();
}
