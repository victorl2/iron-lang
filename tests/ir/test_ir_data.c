/* test_ir_data.c — Unity tests for IR data structure construction.
 *
 * Tests cover IRCORE-01 (module/func/block creation), IRCORE-02 (value ID
 * monotonically incrementing), IRCORE-03 (Iron_Type* used directly), and
 * IRCORE-04 (Iron_Span carried on every instruction).
 *
 * All modules are hand-constructed; no lowering code is used.
 */

#include "unity.h"
#include "ir/ir.h"
#include "analyzer/types.h"
#include "diagnostics/diagnostics.h"

#include <string.h>
#include <stdbool.h>
#include <stdlib.h>

/* ── Fixtures ────────────────────────────────────────────────────────────── */

void setUp(void)    {}
void tearDown(void) {}

/* ── Helpers ──────────────────────────────────────────────────────────────── */

static Iron_Span zero_span(void) {
    return iron_span_make("test", 1, 1, 1, 1);
}

/* ── Tests ────────────────────────────────────────────────────────────────── */

void test_module_create(void) {
    Iron_Arena ir_arena = iron_arena_create(4096);
    iron_types_init(&ir_arena);

    IronIR_Module *mod = iron_ir_module_create(&ir_arena, "test_module");

    TEST_ASSERT_NOT_NULL(mod);
    TEST_ASSERT_EQUAL_STRING("test_module", mod->name);
    TEST_ASSERT_EQUAL_INT(0, mod->func_count);
    TEST_ASSERT_EQUAL_INT(0, mod->type_decl_count);
    TEST_ASSERT_EQUAL_INT(0, mod->extern_decl_count);

    iron_ir_module_destroy(mod);
    iron_arena_free(&ir_arena);
}

void test_func_create(void) {
    Iron_Arena ir_arena = iron_arena_create(4096);
    iron_types_init(&ir_arena);

    IronIR_Module *mod = iron_ir_module_create(&ir_arena, "test_module");
    Iron_Type *int_type = iron_type_make_primitive(IRON_TYPE_INT);

    IronIR_Func *fn = iron_ir_func_create(mod, "my_func", NULL, 0, int_type);

    TEST_ASSERT_NOT_NULL(fn);
    TEST_ASSERT_EQUAL_STRING("my_func", fn->name);
    TEST_ASSERT_EQUAL_PTR(int_type, fn->return_type);
    TEST_ASSERT_EQUAL_INT(0, fn->param_count);
    TEST_ASSERT_EQUAL_UINT(1, fn->next_value_id);  /* starts at 1 */
    TEST_ASSERT_EQUAL_INT(1, mod->func_count);

    iron_ir_module_destroy(mod);
    iron_arena_free(&ir_arena);
}

void test_block_create(void) {
    Iron_Arena ir_arena = iron_arena_create(4096);
    iron_types_init(&ir_arena);

    IronIR_Module *mod = iron_ir_module_create(&ir_arena, "test_module");
    IronIR_Func *fn = iron_ir_func_create(mod, "fn", NULL, 0, NULL);
    IronIR_Block *block = iron_ir_block_create(fn, "entry");

    TEST_ASSERT_NOT_NULL(block);
    TEST_ASSERT_EQUAL_UINT(1, block->id);  /* first block ID = 1 */
    TEST_ASSERT_EQUAL_STRING("entry", block->label);
    TEST_ASSERT_EQUAL_INT(0, block->instr_count);

    iron_ir_module_destroy(mod);
    iron_arena_free(&ir_arena);
}

void test_const_int_creates_instruction(void) {
    Iron_Arena ir_arena = iron_arena_create(4096);
    iron_types_init(&ir_arena);

    IronIR_Module *mod = iron_ir_module_create(&ir_arena, "test_module");
    Iron_Type *int_type = iron_type_make_primitive(IRON_TYPE_INT);
    IronIR_Func *fn = iron_ir_func_create(mod, "fn", NULL, 0, int_type);
    IronIR_Block *block = iron_ir_block_create(fn, "entry");
    Iron_Span span = iron_span_make("test.iron", 1, 1, 1, 5);

    IronIR_Instr *instr = iron_ir_const_int(fn, block, 42, int_type, span);

    TEST_ASSERT_NOT_NULL(instr);
    TEST_ASSERT_EQUAL_INT(IRON_IR_CONST_INT, instr->kind);
    TEST_ASSERT_EQUAL_UINT(1, instr->id);              /* IRCORE-02: first value = 1 */
    TEST_ASSERT_EQUAL_INT64(42, instr->const_int.value);
    TEST_ASSERT_EQUAL_PTR(int_type, instr->type);      /* IRCORE-03: uses Iron_Type* directly */
    TEST_ASSERT_EQUAL_UINT(1, instr->span.line);       /* IRCORE-04: carries Iron_Span */
    TEST_ASSERT_EQUAL_INT(1, block->instr_count);

    iron_ir_module_destroy(mod);
    iron_arena_free(&ir_arena);
}

void test_value_id_incrementing(void) {
    /* IRCORE-02: value IDs increment monotonically per function */
    Iron_Arena ir_arena = iron_arena_create(4096);
    iron_types_init(&ir_arena);

    IronIR_Module *mod = iron_ir_module_create(&ir_arena, "test_module");
    Iron_Type *int_type = iron_type_make_primitive(IRON_TYPE_INT);
    IronIR_Func *fn = iron_ir_func_create(mod, "fn", NULL, 0, int_type);
    IronIR_Block *block = iron_ir_block_create(fn, "entry");
    Iron_Span span = zero_span();

    IronIR_Instr *i1 = iron_ir_const_int(fn, block, 1, int_type, span);
    IronIR_Instr *i2 = iron_ir_const_int(fn, block, 2, int_type, span);

    TEST_ASSERT_EQUAL_UINT(1, i1->id);
    TEST_ASSERT_EQUAL_UINT(2, i2->id);

    iron_ir_module_destroy(mod);
    iron_arena_free(&ir_arena);
}

void test_binop_add(void) {
    Iron_Arena ir_arena = iron_arena_create(4096);
    iron_types_init(&ir_arena);

    IronIR_Module *mod = iron_ir_module_create(&ir_arena, "test_module");
    Iron_Type *int_type = iron_type_make_primitive(IRON_TYPE_INT);
    IronIR_Func *fn = iron_ir_func_create(mod, "fn", NULL, 0, int_type);
    IronIR_Block *block = iron_ir_block_create(fn, "entry");
    Iron_Span span = zero_span();

    IronIR_Instr *c1 = iron_ir_const_int(fn, block, 10, int_type, span);
    IronIR_Instr *c2 = iron_ir_const_int(fn, block, 20, int_type, span);
    IronIR_Instr *add = iron_ir_binop(fn, block, IRON_IR_ADD, c1->id, c2->id, int_type, span);

    TEST_ASSERT_NOT_NULL(add);
    TEST_ASSERT_EQUAL_INT(IRON_IR_ADD, add->kind);
    TEST_ASSERT_EQUAL_UINT(c1->id, add->binop.left);
    TEST_ASSERT_EQUAL_UINT(c2->id, add->binop.right);

    iron_ir_module_destroy(mod);
    iron_arena_free(&ir_arena);
}

void test_void_instructions_have_invalid_id(void) {
    Iron_Arena ir_arena = iron_arena_create(4096);
    iron_types_init(&ir_arena);

    IronIR_Module *mod = iron_ir_module_create(&ir_arena, "test_module");
    Iron_Type *int_type = iron_type_make_primitive(IRON_TYPE_INT);
    IronIR_Func *fn = iron_ir_func_create(mod, "fn", NULL, 0, NULL);
    IronIR_Block *block = iron_ir_block_create(fn, "entry");
    Iron_Span span = zero_span();

    IronIR_Instr *ptr = iron_ir_const_int(fn, block, 0, int_type, span);
    IronIR_Instr *val = iron_ir_const_int(fn, block, 42, int_type, span);
    IronIR_Instr *st  = iron_ir_store(fn, block, ptr->id, val->id, span);

    TEST_ASSERT_EQUAL_UINT(IRON_IR_VALUE_INVALID, st->id);

    iron_ir_module_destroy(mod);
    iron_arena_free(&ir_arena);
}

void test_value_table_lookup(void) {
    Iron_Arena ir_arena = iron_arena_create(4096);
    iron_types_init(&ir_arena);

    IronIR_Module *mod = iron_ir_module_create(&ir_arena, "test_module");
    Iron_Type *int_type = iron_type_make_primitive(IRON_TYPE_INT);
    IronIR_Func *fn = iron_ir_func_create(mod, "fn", NULL, 0, int_type);
    IronIR_Block *block = iron_ir_block_create(fn, "entry");
    Iron_Span span = zero_span();

    IronIR_Instr *instr = iron_ir_const_int(fn, block, 99, int_type, span);

    TEST_ASSERT_EQUAL_PTR(instr, fn->value_table[instr->id]);

    iron_ir_module_destroy(mod);
    iron_arena_free(&ir_arena);
}

void test_block_braun_fields_default_false(void) {
    Iron_Arena ir_arena = iron_arena_create(4096);
    iron_types_init(&ir_arena);

    IronIR_Module *mod = iron_ir_module_create(&ir_arena, "test_module");
    IronIR_Func *fn = iron_ir_func_create(mod, "fn", NULL, 0, NULL);
    IronIR_Block *block = iron_ir_block_create(fn, "entry");

    TEST_ASSERT_FALSE(block->is_sealed);
    TEST_ASSERT_FALSE(block->is_filled);
    TEST_ASSERT_NULL(block->var_defs);

    iron_ir_module_destroy(mod);
    iron_arena_free(&ir_arena);
}

void test_terminator_detection(void) {
    TEST_ASSERT_TRUE(iron_ir_is_terminator(IRON_IR_JUMP));
    TEST_ASSERT_TRUE(iron_ir_is_terminator(IRON_IR_BRANCH));
    TEST_ASSERT_TRUE(iron_ir_is_terminator(IRON_IR_RETURN));
    TEST_ASSERT_TRUE(iron_ir_is_terminator(IRON_IR_SWITCH));
    TEST_ASSERT_FALSE(iron_ir_is_terminator(IRON_IR_ADD));
    TEST_ASSERT_FALSE(iron_ir_is_terminator(IRON_IR_CONST_INT));
    TEST_ASSERT_FALSE(iron_ir_is_terminator(IRON_IR_STORE));
}

void test_module_destroy_no_crash(void) {
    Iron_Arena ir_arena = iron_arena_create(4096);
    iron_types_init(&ir_arena);

    IronIR_Module *mod = iron_ir_module_create(&ir_arena, "test_module");
    Iron_Type *int_type = iron_type_make_primitive(IRON_TYPE_INT);
    IronIR_Func *fn = iron_ir_func_create(mod, "fn", NULL, 0, int_type);
    IronIR_Block *block = iron_ir_block_create(fn, "entry");
    Iron_Span span = zero_span();

    iron_ir_const_int(fn, block, 1, int_type, span);
    iron_ir_const_int(fn, block, 2, int_type, span);
    iron_ir_return(fn, block, 1, false, int_type, span);

    /* Should not crash */
    iron_ir_module_destroy(mod);
    iron_arena_free(&ir_arena);

    TEST_PASS();
}

/* ── Main ─────────────────────────────────────────────────────────────────── */

int main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_module_create);
    RUN_TEST(test_func_create);
    RUN_TEST(test_block_create);
    RUN_TEST(test_const_int_creates_instruction);
    RUN_TEST(test_value_id_incrementing);
    RUN_TEST(test_binop_add);
    RUN_TEST(test_void_instructions_have_invalid_id);
    RUN_TEST(test_value_table_lookup);
    RUN_TEST(test_block_braun_fields_default_false);
    RUN_TEST(test_terminator_detection);
    RUN_TEST(test_module_destroy_no_crash);

    return UNITY_END();
}
