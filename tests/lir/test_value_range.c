#include "unity.h"
#include "lir/lir.h"
#include "lir/value_range.h"
#include "analyzer/types.h"
#include "util/arena.h"

#include <limits.h>
#include <string.h>

static Iron_Arena g_arena;

void setUp(void) {
    g_arena = iron_arena_create(131072);
    iron_types_init(&g_arena);
}

void tearDown(void) {
    iron_arena_free(&g_arena);
}

static Iron_Span sp(void) {
    return iron_span_make("range.iron", 1, 1, 1, 1);
}

static ValueRangeAnalysis analyze(IronLIR_Module *mod) {
    ValueRangeAnalysis vra = {0};
    vra.arena = &g_arena;
    iron_vr_analyze(&vra, mod, NULL);
    return vra;
}

void test_local_constant_and_arithmetic_ladders(void) {
    IronLIR_Module *mod = iron_lir_module_create(&g_arena, "range_ladders");
    Iron_Type *it = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_Type *ut = iron_type_make_primitive(IRON_TYPE_UINT);
    IronLIR_Func *fn = iron_lir_func_create(mod, "Iron_ranges", NULL, 0, it);
    IronLIR_Block *entry = iron_lir_block_create(fn, "entry");
    IronLIR_Instr *i100 = iron_lir_const_int(fn, entry, 100, it, sp());
    IronLIR_Instr *i20 = iron_lir_const_int(fn, entry, 20, it, sp());
    IronLIR_Instr *sum = iron_lir_binop(fn, entry, IRON_LIR_ADD,
                                        i100->id, i20->id, it, sp());
    IronLIR_Instr *neg129 = iron_lir_const_int(fn, entry, -129, it, sp());
    IronLIR_Instr *u255 = iron_lir_const_int(fn, entry, 255, ut, sp());
    IronLIR_Instr *u256 = iron_lir_const_int(fn, entry, 256, ut, sp());
    iron_lir_return(fn, entry, sum->id, false, it, sp());

    ValueRangeAnalysis vra = analyze(mod);
    TEST_ASSERT_EQUAL_STRING("int8_t",
        iron_vr_get_local_narrowed_type(&vra, fn, i100->id));
    TEST_ASSERT_EQUAL_STRING("int8_t",
        iron_vr_get_local_narrowed_type(&vra, fn, sum->id));
    TEST_ASSERT_EQUAL_STRING("int16_t",
        iron_vr_get_local_narrowed_type(&vra, fn, neg129->id));
    TEST_ASSERT_EQUAL_STRING("uint8_t",
        iron_vr_get_local_narrowed_type(&vra, fn, u255->id));
    TEST_ASSERT_EQUAL_STRING("uint16_t",
        iron_vr_get_local_narrowed_type(&vra, fn, u256->id));
    iron_vr_free(&vra);
    iron_lir_module_destroy(mod);
}

void test_local_branch_union_narrows_slot_and_load(void) {
    IronLIR_Module *mod = iron_lir_module_create(&g_arena, "range_union");
    Iron_Type *it = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_Type *bt = iron_type_make_primitive(IRON_TYPE_BOOL);
    IronLIR_Func *fn = iron_lir_func_create(mod, "Iron_union", NULL, 0, it);
    IronLIR_Block *entry = iron_lir_block_create(fn, "entry");
    IronLIR_Block *yes = iron_lir_block_create(fn, "yes");
    IronLIR_Block *no = iron_lir_block_create(fn, "no");
    IronLIR_Block *merge = iron_lir_block_create(fn, "merge");
    IronLIR_Instr *slot = iron_lir_alloca(fn, entry, it, "x", sp());
    IronLIR_Instr *cond = iron_lir_const_bool(fn, entry, true, bt, sp());
    IronLIR_Instr *ten = iron_lir_const_int(fn, yes, 10, it, sp());
    IronLIR_Instr *twenty = iron_lir_const_int(fn, no, 20, it, sp());
    iron_lir_branch(fn, entry, cond->id, yes->id, no->id, sp());
    iron_lir_store(fn, yes, slot->id, ten->id, sp());
    iron_lir_jump(fn, yes, merge->id, sp());
    iron_lir_store(fn, no, slot->id, twenty->id, sp());
    iron_lir_jump(fn, no, merge->id, sp());
    IronLIR_Instr *load = iron_lir_load(fn, merge, slot->id, it, sp());
    iron_lir_return(fn, merge, load->id, false, it, sp());

    ValueRangeAnalysis vra = analyze(mod);
    TEST_ASSERT_EQUAL_STRING("int8_t",
        iron_vr_get_local_narrowed_type(&vra, fn, slot->id));
    TEST_ASSERT_EQUAL_STRING("int8_t",
        iron_vr_get_local_narrowed_type(&vra, fn, load->id));
    iron_vr_free(&vra);
    iron_lir_module_destroy(mod);
}

void test_local_unknown_cycle_alias_overflow_and_explicit_type_fallback(void) {
    IronLIR_Module *mod = iron_lir_module_create(&g_arena, "range_fallbacks");
    Iron_Type *it = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_Type *i32 = iron_type_make_primitive(IRON_TYPE_INT32);
    IronLIR_Param params[1] = {{ .name = "unknown", .type = it }};
    IronLIR_Func *fn = iron_lir_func_create(mod, "Iron_fallbacks", params, 1, it);
    fn->next_value_id = 2; /* reserve synthetic parameter %1 */
    IronLIR_Block *entry = iron_lir_block_create(fn, "entry");

    IronLIR_Instr *unknown_slot = iron_lir_alloca(fn, entry, it, "unknown", sp());
    iron_lir_store(fn, entry, unknown_slot->id, 1, sp());

    IronLIR_Instr *cycle_slot = iron_lir_alloca(fn, entry, it, "cycle", sp());
    IronLIR_Instr *zero = iron_lir_const_int(fn, entry, 0, it, sp());
    iron_lir_store(fn, entry, cycle_slot->id, zero->id, sp());
    IronLIR_Instr *cycle_load = iron_lir_load(fn, entry, cycle_slot->id, it, sp());
    IronLIR_Instr *one = iron_lir_const_int(fn, entry, 1, it, sp());
    IronLIR_Instr *next = iron_lir_binop(fn, entry, IRON_LIR_ADD,
                                         cycle_load->id, one->id, it, sp());
    iron_lir_store(fn, entry, cycle_slot->id, next->id, sp());

    IronLIR_Instr *aliased = iron_lir_alloca(fn, entry, it, "aliased", sp());
    iron_lir_store(fn, entry, aliased->id, one->id, sp());
    IronLIR_Instr *addr = iron_lir_addr_of(fn, entry, aliased->id,
                                           IRON_LIR_GEN_STACK, it, sp());
    (void)iron_lir_ptr_load(fn, entry, addr->id, IRON_LIR_GEN_STACK, it, sp());

    IronLIR_Instr *imax = iron_lir_const_int(fn, entry, INT64_MAX, it, sp());
    IronLIR_Instr *overflow = iron_lir_binop(fn, entry, IRON_LIR_ADD,
                                             imax->id, one->id, it, sp());
    IronLIR_Instr *explicit32 = iron_lir_const_int(fn, entry, 7, i32, sp());
    iron_lir_return(fn, entry, overflow->id, false, it, sp());

    ValueRangeAnalysis vra = analyze(mod);
    TEST_ASSERT_NULL(iron_vr_get_local_narrowed_type(&vra, fn,
                                                     unknown_slot->id));
    TEST_ASSERT_NULL(iron_vr_get_local_narrowed_type(&vra, fn,
                                                     cycle_slot->id));
    TEST_ASSERT_NULL(iron_vr_get_local_narrowed_type(&vra, fn,
                                                     cycle_load->id));
    TEST_ASSERT_NULL(iron_vr_get_local_narrowed_type(&vra, fn, aliased->id));
    TEST_ASSERT_NULL(iron_vr_get_local_narrowed_type(&vra, fn, overflow->id));
    TEST_ASSERT_NULL(iron_vr_get_local_narrowed_type(&vra, fn, explicit32->id));
    iron_vr_free(&vra);
    iron_lir_module_destroy(mod);
}

static IronLIR_Instr *build_induction_loop(IronLIR_Module *mod,
                                            const char *header_label,
                                            bool descending) {
    Iron_Type *it = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_Type *bt = iron_type_make_primitive(IRON_TYPE_BOOL);
    IronLIR_Func *fn = iron_lir_func_create(mod, "Iron_induction", NULL, 0, it);
    IronLIR_Block *entry = iron_lir_block_create(fn, "entry");
    IronLIR_Block *header = iron_lir_block_create(fn, header_label);
    IronLIR_Block *body = iron_lir_block_create(fn, "while_body_0");
    IronLIR_Block *exit_b = iron_lir_block_create(fn, "while_exit_0");
    IronLIR_Instr *slot = iron_lir_alloca(fn, entry, it, "i", sp());
    IronLIR_Instr *init = iron_lir_const_int(fn, entry,
                                             descending ? 10 : 0, it, sp());
    iron_lir_store(fn, entry, slot->id, init->id, sp());
    iron_lir_jump(fn, entry, header->id, sp());
    arrput(entry->succs, header->id);
    arrput(header->preds, entry->id);
    IronLIR_Instr *hload = iron_lir_load(fn, header, slot->id, it, sp());
    IronLIR_Instr *bound = iron_lir_const_int(fn, header,
                                              descending ? 0 : 100, it, sp());
    IronLIR_Instr *cmp = iron_lir_binop(fn, header,
        descending ? IRON_LIR_GT : IRON_LIR_LT,
        hload->id, bound->id, bt, sp());
    iron_lir_branch(fn, header, cmp->id, body->id, exit_b->id, sp());
    arrput(header->succs, body->id);
    arrput(header->succs, exit_b->id);
    arrput(body->preds, header->id);
    arrput(exit_b->preds, header->id);
    IronLIR_Instr *bload = iron_lir_load(fn, body, slot->id, it, sp());
    IronLIR_Instr *one = iron_lir_const_int(fn, body, 1, it, sp());
    IronLIR_Instr *next = iron_lir_binop(fn, body,
        descending ? IRON_LIR_SUB : IRON_LIR_ADD,
        bload->id, one->id, it, sp());
    iron_lir_store(fn, body, slot->id, next->id, sp());
    iron_lir_jump(fn, body, header->id, sp());
    arrput(body->succs, header->id);
    arrput(header->preds, body->id);
    IronLIR_Instr *result = iron_lir_load(fn, exit_b, slot->id, it, sp());
    iron_lir_return(fn, exit_b, result->id, false, it, sp());
    return slot;
}

void test_local_bounded_induction_and_noncanonical_fallback(void) {
    IronLIR_Module *ascending = iron_lir_module_create(&g_arena, "induction_up");
    IronLIR_Instr *up_slot = build_induction_loop(
        ascending, "while_header_0", false);
    IronLIR_Func *up_fn = ascending->funcs[0];
    ValueRangeAnalysis vra = analyze(ascending);
    TEST_ASSERT_EQUAL_STRING("int8_t",
        iron_vr_get_local_narrowed_type(&vra, up_fn, up_slot->id));
    iron_vr_free(&vra);
    iron_lir_module_destroy(ascending);

    IronLIR_Module *descending = iron_lir_module_create(&g_arena, "induction_down");
    IronLIR_Instr *down_slot = build_induction_loop(
        descending, "for_header_0", true);
    IronLIR_Func *down_fn = descending->funcs[0];
    vra = analyze(descending);
    TEST_ASSERT_EQUAL_STRING("int8_t",
        iron_vr_get_local_narrowed_type(&vra, down_fn, down_slot->id));
    iron_vr_free(&vra);
    iron_lir_module_destroy(descending);

    IronLIR_Module *fallback = iron_lir_module_create(&g_arena, "induction_fallback");
    IronLIR_Instr *fallback_slot = build_induction_loop(
        fallback, "hand_built_header", false);
    IronLIR_Func *fallback_fn = fallback->funcs[0];
    vra = analyze(fallback);
    TEST_ASSERT_NULL_MESSAGE(
        iron_vr_get_local_narrowed_type(&vra, fallback_fn, fallback_slot->id),
        "noncanonical cyclic CFG must conservatively remain full width");
    iron_vr_free(&vra);
    iron_lir_module_destroy(fallback);
}

void test_sparse_parameter_only_branch_is_safe(void) {
    IronLIR_Module *mod = iron_lir_module_create(&g_arena, "sparse_param");
    Iron_Type *bt = iron_type_make_primitive(IRON_TYPE_BOOL);
    IronLIR_Param params[1] = {{ .name = "condition", .type = bt }};
    IronLIR_Func *fn = iron_lir_func_create(mod, "Iron_sparse_param",
                                             params, 1, NULL);
    fn->next_value_id = 2; /* %1 is a parameter, not an instruction producer. */
    IronLIR_Block *entry = iron_lir_block_create(fn, "entry");
    IronLIR_Block *yes = iron_lir_block_create(fn, "yes");
    IronLIR_Block *no = iron_lir_block_create(fn, "no");
    iron_lir_branch(fn, entry, 1, yes->id, no->id, sp());
    iron_lir_return(fn, yes, IRON_LIR_VALUE_INVALID, true, NULL, sp());
    iron_lir_return(fn, no, IRON_LIR_VALUE_INVALID, true, NULL, sp());

    ValueRangeAnalysis vra = analyze(mod);
    TEST_ASSERT_NULL_MESSAGE(
        iron_vr_get_local_narrowed_type(&vra, fn, 1),
        "parameter-only conditions have no local narrowing proof");
    iron_vr_free(&vra);
    iron_lir_module_destroy(mod);
}

void test_address_observable_values_keep_full_width(void) {
    for (int mode = 0; mode < 4; mode++) {
        IronLIR_Module *mod = iron_lir_module_create(&g_arena, "address_width");
        Iron_Type *it = iron_type_make_primitive(IRON_TYPE_INT);
        IronLIR_Func *fn = iron_lir_func_create(mod, "Iron_address_width", NULL, 0, it);
        IronLIR_Block *entry = iron_lir_block_create(fn, "entry");
        IronLIR_Instr *constant = iron_lir_const_int(fn, entry, 42, it, sp());
        IronLIR_Instr *slot = iron_lir_alloca(fn, entry, it, "x", sp());
        iron_lir_store(fn, entry, slot->id, constant->id, sp());
        IronLIR_Instr *load = iron_lir_load(fn, entry, slot->id, it, sp());
        IronLIR_ValueId observed = mode < 2 ? constant->id
                                  : mode == 2 ? slot->id : load->id;
        bool by_addr[] = {true};
        if (mode == 0) {
            iron_lir_addr_of(fn, entry, observed, IRON_LIR_GEN_STACK, it, sp());
        } else {
            IronLIR_Instr *ref = iron_lir_func_ref(fn, entry, "observe", NULL, sp());
            IronLIR_Instr *call = iron_lir_call(fn, entry, NULL, ref->id,
                                                &observed, 1, NULL, sp());
            if (mode == 1) call->call.args_by_addr = by_addr;
            else call->call.self_by_addr = true;
        }
        IronLIR_Instr *neighbor = iron_lir_const_int(fn, entry, 9, it, sp());
        iron_lir_return(fn, entry, neighbor->id, false, it, sp());
        ValueRangeAnalysis vra = analyze(mod);
        TEST_ASSERT_NULL_MESSAGE(iron_vr_get_local_narrowed_type(&vra, fn, observed),
            "address-observable temporaries and receiver slots must retain ABI width");
        TEST_ASSERT_EQUAL_STRING("int8_t",
            iron_vr_get_local_narrowed_type(&vra, fn, neighbor->id));
        iron_vr_free(&vra);
        iron_lir_module_destroy(mod);
    }
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_local_constant_and_arithmetic_ladders);
    RUN_TEST(test_local_branch_union_narrows_slot_and_load);
    RUN_TEST(test_local_unknown_cycle_alias_overflow_and_explicit_type_fallback);
    RUN_TEST(test_local_bounded_induction_and_noncanonical_fallback);
    RUN_TEST(test_sparse_parameter_only_branch_is_safe);
    RUN_TEST(test_address_observable_values_keep_full_width);
    return UNITY_END();
}
