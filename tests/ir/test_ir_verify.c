/* test_ir_verify.c — Unity tests for the IR structural verifier (TOOL-02).
 *
 * Each test builds a hand-constructed IR module and exercises one specific
 * verifier invariant — either expecting it to pass cleanly or to report a
 * specific error code.
 */

#include "unity.h"
#include "ir/ir.h"
#include "ir/verify.h"
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

/* Return true if any diagnostic in list has the given error code. */
static bool has_error(const Iron_DiagList *diags, int code) {
    for (int i = 0; i < diags->count; i++) {
        if (diags->items[i].code == code) return true;
    }
    return false;
}

/* ── Tests ────────────────────────────────────────────────────────────────── */

void test_verify_well_formed(void) {
    Iron_Arena ir_arena = iron_arena_create(4096);
    iron_types_init(&ir_arena);

    IronIR_Module *mod = iron_ir_module_create(&ir_arena, "test");
    Iron_Type *int_type = iron_type_make_primitive(IRON_TYPE_INT);
    IronIR_Func *fn = iron_ir_func_create(mod, "fn", NULL, 0, int_type);
    IronIR_Block *entry = iron_ir_block_create(fn, "entry");
    Iron_Span span = zero_span();

    IronIR_Instr *c = iron_ir_const_int(fn, entry, 42, int_type, span);
    iron_ir_return(fn, entry, c->id, false, int_type, span);

    Iron_DiagList diags = iron_diaglist_create();
    bool result = iron_ir_verify(mod, &diags, &ir_arena);

    TEST_ASSERT_TRUE(result);
    TEST_ASSERT_EQUAL_INT(0, diags.error_count);

    iron_diaglist_free(&diags);
    iron_ir_module_destroy(mod);
    iron_arena_free(&ir_arena);
}

void test_verify_missing_terminator(void) {
    Iron_Arena ir_arena = iron_arena_create(4096);
    iron_types_init(&ir_arena);

    IronIR_Module *mod = iron_ir_module_create(&ir_arena, "test");
    Iron_Type *int_type = iron_type_make_primitive(IRON_TYPE_INT);
    IronIR_Func *fn = iron_ir_func_create(mod, "fn", NULL, 0, int_type);
    IronIR_Block *entry = iron_ir_block_create(fn, "entry");
    Iron_Span span = zero_span();

    /* const_int but NO return/jump/branch — missing terminator */
    iron_ir_const_int(fn, entry, 42, int_type, span);

    Iron_DiagList diags = iron_diaglist_create();
    bool result = iron_ir_verify(mod, &diags, &ir_arena);

    TEST_ASSERT_FALSE(result);
    TEST_ASSERT_TRUE(diags.error_count >= 1);
    TEST_ASSERT_TRUE(has_error(&diags, IRON_ERR_IR_MISSING_TERMINATOR));

    iron_diaglist_free(&diags);
    iron_ir_module_destroy(mod);
    iron_arena_free(&ir_arena);
}

void test_verify_use_before_def(void) {
    Iron_Arena ir_arena = iron_arena_create(4096);
    iron_types_init(&ir_arena);

    IronIR_Module *mod = iron_ir_module_create(&ir_arena, "test");
    Iron_Type *int_type = iron_type_make_primitive(IRON_TYPE_INT);
    IronIR_Func *fn = iron_ir_func_create(mod, "fn", NULL, 0, int_type);
    IronIR_Block *entry = iron_ir_block_create(fn, "entry");
    Iron_Span span = zero_span();

    /* binop referencing a non-existent value ID 999 */
    IronIR_Instr *bad = iron_ir_binop(fn, entry, IRON_IR_ADD, 999, 999, int_type, span);
    iron_ir_return(fn, entry, bad->id, false, int_type, span);

    Iron_DiagList diags = iron_diaglist_create();
    bool result = iron_ir_verify(mod, &diags, &ir_arena);

    TEST_ASSERT_FALSE(result);
    TEST_ASSERT_TRUE(has_error(&diags, IRON_ERR_IR_USE_BEFORE_DEF));

    iron_diaglist_free(&diags);
    iron_ir_module_destroy(mod);
    iron_arena_free(&ir_arena);
}

void test_verify_invalid_branch_target(void) {
    Iron_Arena ir_arena = iron_arena_create(4096);
    iron_types_init(&ir_arena);

    IronIR_Module *mod = iron_ir_module_create(&ir_arena, "test");
    Iron_Type *bool_type = iron_type_make_primitive(IRON_TYPE_BOOL);
    IronIR_Func *fn = iron_ir_func_create(mod, "fn", NULL, 0, NULL);
    IronIR_Block *entry = iron_ir_block_create(fn, "entry");
    Iron_Span span = zero_span();

    IronIR_Instr *cond = iron_ir_const_bool(fn, entry, true, bool_type, span);
    /* Branch to non-existent block IDs 999 and 998 */
    iron_ir_branch(fn, entry, cond->id, 999, 998, span);

    Iron_DiagList diags = iron_diaglist_create();
    bool result = iron_ir_verify(mod, &diags, &ir_arena);

    TEST_ASSERT_FALSE(result);
    TEST_ASSERT_TRUE(has_error(&diags, IRON_ERR_IR_INVALID_BRANCH_TARGET));

    iron_diaglist_free(&diags);
    iron_ir_module_destroy(mod);
    iron_arena_free(&ir_arena);
}

void test_verify_instr_after_terminator(void) {
    Iron_Arena ir_arena = iron_arena_create(4096);
    iron_types_init(&ir_arena);

    IronIR_Module *mod = iron_ir_module_create(&ir_arena, "test");
    Iron_Type *int_type = iron_type_make_primitive(IRON_TYPE_INT);
    IronIR_Func *fn = iron_ir_func_create(mod, "fn", NULL, 0, NULL);
    IronIR_Block *entry = iron_ir_block_create(fn, "entry");
    Iron_Span span = zero_span();

    /* return first, then const_int after — instruction after terminator */
    iron_ir_return(fn, entry, IRON_IR_VALUE_INVALID, true, NULL, span);
    iron_ir_const_int(fn, entry, 42, int_type, span);

    Iron_DiagList diags = iron_diaglist_create();
    bool result = iron_ir_verify(mod, &diags, &ir_arena);

    TEST_ASSERT_FALSE(result);
    TEST_ASSERT_TRUE(has_error(&diags, IRON_ERR_IR_INSTR_AFTER_TERMINATOR));

    iron_diaglist_free(&diags);
    iron_ir_module_destroy(mod);
    iron_arena_free(&ir_arena);
}

void test_verify_no_entry_block(void) {
    Iron_Arena ir_arena = iron_arena_create(4096);
    iron_types_init(&ir_arena);

    IronIR_Module *mod = iron_ir_module_create(&ir_arena, "test");
    /* Create func but add NO blocks */
    iron_ir_func_create(mod, "empty_fn", NULL, 0, NULL);

    Iron_DiagList diags = iron_diaglist_create();
    bool result = iron_ir_verify(mod, &diags, &ir_arena);

    TEST_ASSERT_FALSE(result);
    TEST_ASSERT_TRUE(has_error(&diags, IRON_ERR_IR_NO_ENTRY_BLOCK));

    iron_diaglist_free(&diags);
    iron_ir_module_destroy(mod);
    iron_arena_free(&ir_arena);
}

void test_verify_multiple_errors(void) {
    Iron_Arena ir_arena = iron_arena_create(4096);
    iron_types_init(&ir_arena);

    IronIR_Module *mod = iron_ir_module_create(&ir_arena, "test");
    Iron_Type *int_type = iron_type_make_primitive(IRON_TYPE_INT);
    IronIR_Func *fn = iron_ir_func_create(mod, "fn", NULL, 0, int_type);
    IronIR_Block *entry = iron_ir_block_create(fn, "entry");
    Iron_Span span = zero_span();

    /* Use-before-def: reference value ID 999 which doesn't exist */
    iron_ir_binop(fn, entry, IRON_IR_ADD, 999, 999, int_type, span);
    /* AND missing terminator: no return/jump after the binop */
    /* (no return added) */

    Iron_DiagList diags = iron_diaglist_create();
    bool result = iron_ir_verify(mod, &diags, &ir_arena);

    TEST_ASSERT_FALSE(result);
    /* Must report at least 2 errors — not just the first one */
    TEST_ASSERT_TRUE(diags.error_count >= 2);
    TEST_ASSERT_TRUE(has_error(&diags, IRON_ERR_IR_USE_BEFORE_DEF));
    TEST_ASSERT_TRUE(has_error(&diags, IRON_ERR_IR_MISSING_TERMINATOR));

    iron_diaglist_free(&diags);
    iron_ir_module_destroy(mod);
    iron_arena_free(&ir_arena);
}

void test_verify_return_type_mismatch(void) {
    Iron_Arena ir_arena = iron_arena_create(4096);
    iron_types_init(&ir_arena);

    IronIR_Module *mod = iron_ir_module_create(&ir_arena, "test");
    Iron_Type *int_type  = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_Type *bool_type = iron_type_make_primitive(IRON_TYPE_BOOL);
    /* Function declared to return Int */
    IronIR_Func *fn = iron_ir_func_create(mod, "fn", NULL, 0, int_type);
    IronIR_Block *entry = iron_ir_block_create(fn, "entry");
    Iron_Span span = zero_span();

    /* Return a Bool value from an Int function -- type mismatch */
    IronIR_Instr *b = iron_ir_const_bool(fn, entry, true, bool_type, span);
    iron_ir_return(fn, entry, b->id, false, bool_type, span);

    Iron_DiagList diags = iron_diaglist_create();
    bool result = iron_ir_verify(mod, &diags, &ir_arena);

    TEST_ASSERT_FALSE(result);
    TEST_ASSERT_TRUE(has_error(&diags, IRON_ERR_IR_RETURN_TYPE_MISMATCH));

    iron_diaglist_free(&diags);
    iron_ir_module_destroy(mod);
    iron_arena_free(&ir_arena);
}

/* ── Main ─────────────────────────────────────────────────────────────────── */

int main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_verify_well_formed);
    RUN_TEST(test_verify_missing_terminator);
    RUN_TEST(test_verify_use_before_def);
    RUN_TEST(test_verify_invalid_branch_target);
    RUN_TEST(test_verify_instr_after_terminator);
    RUN_TEST(test_verify_no_entry_block);
    RUN_TEST(test_verify_multiple_errors);
    RUN_TEST(test_verify_return_type_mismatch);

    return UNITY_END();
}
