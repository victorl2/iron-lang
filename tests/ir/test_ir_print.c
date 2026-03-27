/* test_ir_print.c — Unity tests for the IR printer (TOOL-01).
 *
 * Each test builds a hand-constructed IR module and verifies that
 * iron_ir_print() produces the expected LLVM-style text fragments.
 */

#include "unity.h"
#include "ir/ir.h"
#include "ir/print.h"
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
    return iron_span_make("test.iron", 1, 1, 1, 1);
}

static bool str_contains(const char *haystack, const char *needle) {
    if (!haystack || !needle) return false;
    return strstr(haystack, needle) != NULL;
}

/* ── Tests ────────────────────────────────────────────────────────────────── */

void test_print_empty_module(void) {
    Iron_Arena ir_arena = iron_arena_create(4096);
    iron_types_init(&ir_arena);

    IronIR_Module *mod = iron_ir_module_create(&ir_arena, "test");
    char *out = iron_ir_print(mod, false);

    TEST_ASSERT_NOT_NULL(out);
    TEST_ASSERT_TRUE(str_contains(out, "; Module: test"));

    free(out);
    iron_ir_module_destroy(mod);
    iron_arena_free(&ir_arena);
}

void test_print_const_int(void) {
    Iron_Arena ir_arena = iron_arena_create(4096);
    iron_types_init(&ir_arena);

    IronIR_Module *mod = iron_ir_module_create(&ir_arena, "test");
    Iron_Type *int_type = iron_type_make_primitive(IRON_TYPE_INT);
    IronIR_Func *fn = iron_ir_func_create(mod, "Iron_main", NULL, 0, int_type);
    IronIR_Block *entry = iron_ir_block_create(fn, "entry");
    Iron_Span span = zero_span();

    IronIR_Instr *c = iron_ir_const_int(fn, entry, 42, int_type, span);
    iron_ir_return(fn, entry, c->id, false, int_type, span);

    char *out = iron_ir_print(mod, false);

    TEST_ASSERT_NOT_NULL(out);
    TEST_ASSERT_TRUE(str_contains(out, "func @Iron_main"));
    TEST_ASSERT_TRUE(str_contains(out, "entry:"));
    TEST_ASSERT_TRUE(str_contains(out, "%1 = const_int 42 : Int"));
    TEST_ASSERT_TRUE(str_contains(out, "ret %1"));

    free(out);
    iron_ir_module_destroy(mod);
    iron_arena_free(&ir_arena);
}

void test_print_binop(void) {
    Iron_Arena ir_arena = iron_arena_create(4096);
    iron_types_init(&ir_arena);

    IronIR_Module *mod = iron_ir_module_create(&ir_arena, "test");
    Iron_Type *int_type = iron_type_make_primitive(IRON_TYPE_INT);
    IronIR_Func *fn = iron_ir_func_create(mod, "fn", NULL, 0, int_type);
    IronIR_Block *entry = iron_ir_block_create(fn, "entry");
    Iron_Span span = zero_span();

    IronIR_Instr *c1 = iron_ir_const_int(fn, entry, 10, int_type, span);
    IronIR_Instr *c2 = iron_ir_const_int(fn, entry, 20, int_type, span);
    IronIR_Instr *add = iron_ir_binop(fn, entry, IRON_IR_ADD, c1->id, c2->id, int_type, span);
    iron_ir_return(fn, entry, add->id, false, int_type, span);

    char *out = iron_ir_print(mod, false);

    TEST_ASSERT_NOT_NULL(out);
    TEST_ASSERT_TRUE(str_contains(out, "%3 = add %1, %2 : Int"));

    free(out);
    iron_ir_module_destroy(mod);
    iron_arena_free(&ir_arena);
}

void test_print_alloca_load_store(void) {
    Iron_Arena ir_arena = iron_arena_create(4096);
    iron_types_init(&ir_arena);

    IronIR_Module *mod = iron_ir_module_create(&ir_arena, "test");
    Iron_Type *int_type = iron_type_make_primitive(IRON_TYPE_INT);
    IronIR_Func *fn = iron_ir_func_create(mod, "fn", NULL, 0, NULL);
    IronIR_Block *entry = iron_ir_block_create(fn, "entry");
    Iron_Span span = zero_span();

    IronIR_Instr *ptr = iron_ir_alloca(fn, entry, int_type, NULL, span);
    IronIR_Instr *val = iron_ir_const_int(fn, entry, 42, int_type, span);
    iron_ir_store(fn, entry, ptr->id, val->id, span);
    iron_ir_load(fn, entry, ptr->id, int_type, span);
    iron_ir_return(fn, entry, IRON_IR_VALUE_INVALID, true, NULL, span);

    char *out = iron_ir_print(mod, false);

    TEST_ASSERT_NOT_NULL(out);
    TEST_ASSERT_TRUE(str_contains(out, "%1 = alloca Int"));
    TEST_ASSERT_TRUE(str_contains(out, "store %1, %2"));
    TEST_ASSERT_TRUE(str_contains(out, "%3 = load %1 : Int"));

    free(out);
    iron_ir_module_destroy(mod);
    iron_arena_free(&ir_arena);
}

void test_print_branch(void) {
    Iron_Arena ir_arena = iron_arena_create(4096);
    iron_types_init(&ir_arena);

    IronIR_Module *mod = iron_ir_module_create(&ir_arena, "test");
    Iron_Type *bool_type = iron_type_make_primitive(IRON_TYPE_BOOL);
    IronIR_Func *fn = iron_ir_func_create(mod, "fn", NULL, 0, NULL);
    IronIR_Block *entry  = iron_ir_block_create(fn, "entry");
    IronIR_Block *then_b = iron_ir_block_create(fn, "then");
    IronIR_Block *else_b = iron_ir_block_create(fn, "else");
    Iron_Span span = zero_span();

    IronIR_Instr *cond = iron_ir_const_bool(fn, entry, true, bool_type, span);
    iron_ir_branch(fn, entry, cond->id, then_b->id, else_b->id, span);

    iron_ir_return(fn, then_b, IRON_IR_VALUE_INVALID, true, NULL, span);
    iron_ir_return(fn, else_b, IRON_IR_VALUE_INVALID, true, NULL, span);

    char *out = iron_ir_print(mod, false);

    TEST_ASSERT_NOT_NULL(out);
    TEST_ASSERT_TRUE(str_contains(out, "branch %1, then, else"));

    free(out);
    iron_ir_module_destroy(mod);
    iron_arena_free(&ir_arena);
}

void test_print_annotations_on(void) {
    Iron_Arena ir_arena = iron_arena_create(4096);
    iron_types_init(&ir_arena);

    IronIR_Module *mod = iron_ir_module_create(&ir_arena, "test");
    Iron_Type *int_type = iron_type_make_primitive(IRON_TYPE_INT);
    IronIR_Func *fn = iron_ir_func_create(mod, "fn", NULL, 0, NULL);
    IronIR_Block *entry = iron_ir_block_create(fn, "entry");
    Iron_Span span = zero_span();

    IronIR_Instr *val = iron_ir_const_int(fn, entry, 1, int_type, span);
    /* heap_alloc with auto_free=true */
    iron_ir_heap_alloc(fn, entry, val->id, true, false, int_type, span);
    iron_ir_return(fn, entry, IRON_IR_VALUE_INVALID, true, NULL, span);

    char *out = iron_ir_print(mod, true);

    TEST_ASSERT_NOT_NULL(out);
    TEST_ASSERT_TRUE(str_contains(out, "; auto_free"));

    free(out);
    iron_ir_module_destroy(mod);
    iron_arena_free(&ir_arena);
}

void test_print_annotations_off(void) {
    Iron_Arena ir_arena = iron_arena_create(4096);
    iron_types_init(&ir_arena);

    IronIR_Module *mod = iron_ir_module_create(&ir_arena, "test");
    Iron_Type *int_type = iron_type_make_primitive(IRON_TYPE_INT);
    IronIR_Func *fn = iron_ir_func_create(mod, "fn", NULL, 0, NULL);
    IronIR_Block *entry = iron_ir_block_create(fn, "entry");
    Iron_Span span = zero_span();

    IronIR_Instr *val = iron_ir_const_int(fn, entry, 1, int_type, span);
    iron_ir_heap_alloc(fn, entry, val->id, true, false, int_type, span);
    iron_ir_return(fn, entry, IRON_IR_VALUE_INVALID, true, NULL, span);

    /* With annotations OFF, "; auto_free" should NOT appear */
    char *out = iron_ir_print(mod, false);

    TEST_ASSERT_NOT_NULL(out);
    TEST_ASSERT_FALSE(str_contains(out, "; auto_free"));

    free(out);
    iron_ir_module_destroy(mod);
    iron_arena_free(&ir_arena);
}

void test_print_function_params(void) {
    Iron_Arena ir_arena = iron_arena_create(4096);
    iron_types_init(&ir_arena);

    IronIR_Module *mod = iron_ir_module_create(&ir_arena, "test");
    Iron_Type *int_type   = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_Type *float_type = iron_type_make_primitive(IRON_TYPE_FLOAT);

    IronIR_Param params[2] = {
        { "x", int_type   },
        { "y", float_type }
    };

    IronIR_Func *fn = iron_ir_func_create(mod, "add", params, 2, int_type);
    IronIR_Block *entry = iron_ir_block_create(fn, "entry");
    Iron_Span span = zero_span();

    IronIR_Instr *c = iron_ir_const_int(fn, entry, 0, int_type, span);
    iron_ir_return(fn, entry, c->id, false, int_type, span);

    char *out = iron_ir_print(mod, false);

    TEST_ASSERT_NOT_NULL(out);
    TEST_ASSERT_TRUE(str_contains(out, "func @add("));
    TEST_ASSERT_TRUE(str_contains(out, "x: Int"));
    TEST_ASSERT_TRUE(str_contains(out, "y: Float"));

    free(out);
    iron_ir_module_destroy(mod);
    iron_arena_free(&ir_arena);
}

/* ── Main ─────────────────────────────────────────────────────────────────── */

int main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_print_empty_module);
    RUN_TEST(test_print_const_int);
    RUN_TEST(test_print_binop);
    RUN_TEST(test_print_alloca_load_store);
    RUN_TEST(test_print_branch);
    RUN_TEST(test_print_annotations_on);
    RUN_TEST(test_print_annotations_off);
    RUN_TEST(test_print_function_params);

    return UNITY_END();
}
