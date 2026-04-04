/* test_ir_verify.c — Unity tests for the IR structural verifier (TOOL-02).
 *
 * Each test builds a hand-constructed IR module and exercises one specific
 * verifier invariant — either expecting it to pass cleanly or to report a
 * specific error code.
 */

#include "unity.h"
#include "lir/lir.h"
#include "lir/verify.h"
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

    IronLIR_Module *mod = iron_lir_module_create(&ir_arena, "test");
    Iron_Type *int_type = iron_type_make_primitive(IRON_TYPE_INT);
    IronLIR_Func *fn = iron_lir_func_create(mod, "fn", NULL, 0, int_type);
    IronLIR_Block *entry = iron_lir_block_create(fn, "entry");
    Iron_Span span = zero_span();

    IronLIR_Instr *c = iron_lir_const_int(fn, entry, 42, int_type, span);
    iron_lir_return(fn, entry, c->id, false, int_type, span);

    Iron_DiagList diags = iron_diaglist_create();
    bool result = iron_lir_verify(mod, &diags, &ir_arena);

    TEST_ASSERT_TRUE(result);
    TEST_ASSERT_EQUAL_INT(0, diags.error_count);

    iron_diaglist_free(&diags);
    iron_lir_module_destroy(mod);
    iron_arena_free(&ir_arena);
}

void test_verify_missing_terminator(void) {
    Iron_Arena ir_arena = iron_arena_create(4096);
    iron_types_init(&ir_arena);

    IronLIR_Module *mod = iron_lir_module_create(&ir_arena, "test");
    Iron_Type *int_type = iron_type_make_primitive(IRON_TYPE_INT);
    IronLIR_Func *fn = iron_lir_func_create(mod, "fn", NULL, 0, int_type);
    IronLIR_Block *entry = iron_lir_block_create(fn, "entry");
    Iron_Span span = zero_span();

    /* const_int but NO return/jump/branch — missing terminator */
    iron_lir_const_int(fn, entry, 42, int_type, span);

    Iron_DiagList diags = iron_diaglist_create();
    bool result = iron_lir_verify(mod, &diags, &ir_arena);

    TEST_ASSERT_FALSE(result);
    TEST_ASSERT_TRUE(diags.error_count >= 1);
    TEST_ASSERT_TRUE(has_error(&diags, IRON_ERR_LIR_MISSING_TERMINATOR));

    iron_diaglist_free(&diags);
    iron_lir_module_destroy(mod);
    iron_arena_free(&ir_arena);
}

void test_verify_use_before_def(void) {
    Iron_Arena ir_arena = iron_arena_create(4096);
    iron_types_init(&ir_arena);

    IronLIR_Module *mod = iron_lir_module_create(&ir_arena, "test");
    Iron_Type *int_type = iron_type_make_primitive(IRON_TYPE_INT);
    IronLIR_Func *fn = iron_lir_func_create(mod, "fn", NULL, 0, int_type);
    IronLIR_Block *entry = iron_lir_block_create(fn, "entry");
    Iron_Span span = zero_span();

    /* binop referencing a non-existent value ID 999 */
    IronLIR_Instr *bad = iron_lir_binop(fn, entry, IRON_LIR_ADD, 999, 999, int_type, span);
    iron_lir_return(fn, entry, bad->id, false, int_type, span);

    Iron_DiagList diags = iron_diaglist_create();
    bool result = iron_lir_verify(mod, &diags, &ir_arena);

    TEST_ASSERT_FALSE(result);
    TEST_ASSERT_TRUE(has_error(&diags, IRON_ERR_LIR_USE_BEFORE_DEF));

    iron_diaglist_free(&diags);
    iron_lir_module_destroy(mod);
    iron_arena_free(&ir_arena);
}

void test_verify_invalid_branch_target(void) {
    Iron_Arena ir_arena = iron_arena_create(4096);
    iron_types_init(&ir_arena);

    IronLIR_Module *mod = iron_lir_module_create(&ir_arena, "test");
    Iron_Type *bool_type = iron_type_make_primitive(IRON_TYPE_BOOL);
    IronLIR_Func *fn = iron_lir_func_create(mod, "fn", NULL, 0, NULL);
    IronLIR_Block *entry = iron_lir_block_create(fn, "entry");
    Iron_Span span = zero_span();

    IronLIR_Instr *cond = iron_lir_const_bool(fn, entry, true, bool_type, span);
    /* Branch to non-existent block IDs 999 and 998 */
    iron_lir_branch(fn, entry, cond->id, 999, 998, span);

    Iron_DiagList diags = iron_diaglist_create();
    bool result = iron_lir_verify(mod, &diags, &ir_arena);

    TEST_ASSERT_FALSE(result);
    TEST_ASSERT_TRUE(has_error(&diags, IRON_ERR_LIR_INVALID_BRANCH_TARGET));

    iron_diaglist_free(&diags);
    iron_lir_module_destroy(mod);
    iron_arena_free(&ir_arena);
}

void test_verify_instr_after_terminator(void) {
    Iron_Arena ir_arena = iron_arena_create(4096);
    iron_types_init(&ir_arena);

    IronLIR_Module *mod = iron_lir_module_create(&ir_arena, "test");
    Iron_Type *int_type = iron_type_make_primitive(IRON_TYPE_INT);
    IronLIR_Func *fn = iron_lir_func_create(mod, "fn", NULL, 0, NULL);
    IronLIR_Block *entry = iron_lir_block_create(fn, "entry");
    Iron_Span span = zero_span();

    /* return first, then const_int after — instruction after terminator */
    iron_lir_return(fn, entry, IRON_LIR_VALUE_INVALID, true, NULL, span);
    iron_lir_const_int(fn, entry, 42, int_type, span);

    Iron_DiagList diags = iron_diaglist_create();
    bool result = iron_lir_verify(mod, &diags, &ir_arena);

    TEST_ASSERT_FALSE(result);
    TEST_ASSERT_TRUE(has_error(&diags, IRON_ERR_LIR_INSTR_AFTER_TERMINATOR));

    iron_diaglist_free(&diags);
    iron_lir_module_destroy(mod);
    iron_arena_free(&ir_arena);
}

void test_verify_no_entry_block(void) {
    Iron_Arena ir_arena = iron_arena_create(4096);
    iron_types_init(&ir_arena);

    IronLIR_Module *mod = iron_lir_module_create(&ir_arena, "test");
    /* Create func but add NO blocks */
    iron_lir_func_create(mod, "empty_fn", NULL, 0, NULL);

    Iron_DiagList diags = iron_diaglist_create();
    bool result = iron_lir_verify(mod, &diags, &ir_arena);

    TEST_ASSERT_FALSE(result);
    TEST_ASSERT_TRUE(has_error(&diags, IRON_ERR_LIR_NO_ENTRY_BLOCK));

    iron_diaglist_free(&diags);
    iron_lir_module_destroy(mod);
    iron_arena_free(&ir_arena);
}

void test_verify_multiple_errors(void) {
    Iron_Arena ir_arena = iron_arena_create(4096);
    iron_types_init(&ir_arena);

    IronLIR_Module *mod = iron_lir_module_create(&ir_arena, "test");
    Iron_Type *int_type = iron_type_make_primitive(IRON_TYPE_INT);
    IronLIR_Func *fn = iron_lir_func_create(mod, "fn", NULL, 0, int_type);
    IronLIR_Block *entry = iron_lir_block_create(fn, "entry");
    Iron_Span span = zero_span();

    /* Use-before-def: reference value ID 999 which doesn't exist */
    iron_lir_binop(fn, entry, IRON_LIR_ADD, 999, 999, int_type, span);
    /* AND missing terminator: no return/jump after the binop */
    /* (no return added) */

    Iron_DiagList diags = iron_diaglist_create();
    bool result = iron_lir_verify(mod, &diags, &ir_arena);

    TEST_ASSERT_FALSE(result);
    /* Must report at least 2 errors — not just the first one */
    TEST_ASSERT_TRUE(diags.error_count >= 2);
    TEST_ASSERT_TRUE(has_error(&diags, IRON_ERR_LIR_USE_BEFORE_DEF));
    TEST_ASSERT_TRUE(has_error(&diags, IRON_ERR_LIR_MISSING_TERMINATOR));

    iron_diaglist_free(&diags);
    iron_lir_module_destroy(mod);
    iron_arena_free(&ir_arena);
}

void test_verify_return_type_mismatch(void) {
    Iron_Arena ir_arena = iron_arena_create(4096);
    iron_types_init(&ir_arena);

    IronLIR_Module *mod = iron_lir_module_create(&ir_arena, "test");
    Iron_Type *int_type  = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_Type *bool_type = iron_type_make_primitive(IRON_TYPE_BOOL);
    /* Function declared to return Int */
    IronLIR_Func *fn = iron_lir_func_create(mod, "fn", NULL, 0, int_type);
    IronLIR_Block *entry = iron_lir_block_create(fn, "entry");
    Iron_Span span = zero_span();

    /* Return a Bool value from an Int function -- type mismatch */
    IronLIR_Instr *b = iron_lir_const_bool(fn, entry, true, bool_type, span);
    iron_lir_return(fn, entry, b->id, false, bool_type, span);

    Iron_DiagList diags = iron_diaglist_create();
    bool result = iron_lir_verify(mod, &diags, &ir_arena);

    TEST_ASSERT_FALSE(result);
    TEST_ASSERT_TRUE(has_error(&diags, IRON_ERR_LIR_RETURN_TYPE_MISMATCH));

    iron_diaglist_free(&diags);
    iron_lir_module_destroy(mod);
    iron_arena_free(&ir_arena);
}

void test_verify_phi_type_mismatch(void) {
    Iron_Arena ir_arena = iron_arena_create(4096);
    iron_types_init(&ir_arena);

    IronLIR_Module *mod = iron_lir_module_create(&ir_arena, "test");
    Iron_Type *int_type  = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_Type *bool_type = iron_type_make_primitive(IRON_TYPE_BOOL);
    /* Function returns bool (to match the PHI type for return) */
    IronLIR_Func *fn = iron_lir_func_create(mod, "fn", NULL, 0, bool_type);
    IronLIR_Block *entry = iron_lir_block_create(fn, "entry");
    IronLIR_Block *merge = iron_lir_block_create(fn, "merge");
    Iron_Span span = zero_span();

    /* entry: const_int 42 (Int type), then jump to merge */
    IronLIR_Instr *c_int = iron_lir_const_int(fn, entry, 42, int_type, span);
    iron_lir_jump(fn, entry, merge->id, span);
    arrput(entry->succs, merge->id);
    arrput(merge->preds, entry->id);

    /* merge: PHI with Bool result type, but incoming value is Int -- mismatch */
    IronLIR_Instr *phi = iron_lir_phi(fn, merge, bool_type, span);
    iron_lir_phi_add_incoming(phi, c_int->id, entry->id);

    /* merge: return the PHI value */
    iron_lir_return(fn, merge, phi->id, false, bool_type, span);

    Iron_DiagList diags = iron_diaglist_create();
    bool result = iron_lir_verify(mod, &diags, &ir_arena);

    TEST_ASSERT_FALSE(result);
    TEST_ASSERT_TRUE(has_error(&diags, IRON_ERR_LIR_PHI_TYPE_MISMATCH));

    iron_diaglist_free(&diags);
    iron_lir_module_destroy(mod);
    iron_arena_free(&ir_arena);
}

void test_verify_phi_well_formed(void) {
    Iron_Arena ir_arena = iron_arena_create(4096);
    iron_types_init(&ir_arena);

    IronLIR_Module *mod = iron_lir_module_create(&ir_arena, "test");
    Iron_Type *int_type  = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_Type *bool_type = iron_type_make_primitive(IRON_TYPE_BOOL);
    IronLIR_Func *fn = iron_lir_func_create(mod, "fn", NULL, 0, int_type);
    IronLIR_Block *entry = iron_lir_block_create(fn, "entry");
    IronLIR_Block *alt   = iron_lir_block_create(fn, "alt");
    IronLIR_Block *merge = iron_lir_block_create(fn, "merge");
    Iron_Span span = zero_span();

    /* entry: const_bool (condition), const_int 1 (value), branch to alt/merge */
    IronLIR_Instr *cond = iron_lir_const_bool(fn, entry, true, bool_type, span);
    IronLIR_Instr *val1 = iron_lir_const_int(fn, entry, 1, int_type, span);
    iron_lir_branch(fn, entry, cond->id, alt->id, merge->id, span);
    arrput(entry->succs, alt->id);
    arrput(entry->succs, merge->id);
    arrput(alt->preds, entry->id);
    arrput(merge->preds, entry->id);

    /* alt: const_int 2 (value), jump to merge */
    IronLIR_Instr *val2 = iron_lir_const_int(fn, alt, 2, int_type, span);
    iron_lir_jump(fn, alt, merge->id, span);
    arrput(alt->succs, merge->id);
    arrput(merge->preds, alt->id);

    /* merge: PHI with int_type, incoming from entry (val1) and alt (val2) -- both Int */
    IronLIR_Instr *phi = iron_lir_phi(fn, merge, int_type, span);
    iron_lir_phi_add_incoming(phi, val1->id, entry->id);
    iron_lir_phi_add_incoming(phi, val2->id, alt->id);

    /* merge: return the PHI value */
    iron_lir_return(fn, merge, phi->id, false, int_type, span);

    Iron_DiagList diags = iron_diaglist_create();
    bool result = iron_lir_verify(mod, &diags, &ir_arena);

    TEST_ASSERT_TRUE(result);
    TEST_ASSERT_EQUAL_INT(0, diags.error_count);
    TEST_ASSERT_FALSE(has_error(&diags, IRON_ERR_LIR_PHI_TYPE_MISMATCH));

    iron_diaglist_free(&diags);
    iron_lir_module_destroy(mod);
    iron_arena_free(&ir_arena);
}

/* ── Call argument validation tests ───────────────────────────────────────── */

void test_verify_call_arg_count_mismatch(void) {
    Iron_Arena ir_arena = iron_arena_create(4096);
    iron_types_init(&ir_arena);

    IronLIR_Module *mod = iron_lir_module_create(&ir_arena, "test");
    Iron_Type *int_type = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_Span span = zero_span();

    /* callee: takes 2 Int params, returns Int */
    IronLIR_Param callee_params[2] = {
        { .name = "a", .type = int_type },
        { .name = "b", .type = int_type }
    };
    IronLIR_Func *callee = iron_lir_func_create(mod, "callee", callee_params, 2, int_type);
    IronLIR_Block *callee_entry = iron_lir_block_create(callee, "entry");
    IronLIR_Instr *callee_ret_val = iron_lir_const_int(callee, callee_entry, 0, int_type, span);
    iron_lir_return(callee, callee_entry, callee_ret_val->id, false, int_type, span);

    /* caller: passes only 1 arg (count mismatch) */
    IronLIR_Func *caller = iron_lir_func_create(mod, "caller", NULL, 0, NULL);
    IronLIR_Block *caller_entry = iron_lir_block_create(caller, "entry");
    IronLIR_Instr *arg1 = iron_lir_const_int(caller, caller_entry, 42, int_type, span);

    Iron_FuncDecl fake_decl = {0};
    fake_decl.name = "callee";
    fake_decl.param_count = 2;

    IronLIR_ValueId args[] = { arg1->id };
    iron_lir_call(caller, caller_entry, &fake_decl, IRON_LIR_VALUE_INVALID,
                  args, 1, int_type, span);
    iron_lir_return(caller, caller_entry, IRON_LIR_VALUE_INVALID, true, NULL, span);

    Iron_DiagList diags = iron_diaglist_create();
    bool result = iron_lir_verify(mod, &diags, &ir_arena);

    TEST_ASSERT_FALSE(result);
    TEST_ASSERT_TRUE(has_error(&diags, IRON_ERR_LIR_CALL_TYPE_MISMATCH));

    iron_diaglist_free(&diags);
    iron_lir_module_destroy(mod);
    iron_arena_free(&ir_arena);
}

void test_verify_call_arg_type_mismatch(void) {
    Iron_Arena ir_arena = iron_arena_create(4096);
    iron_types_init(&ir_arena);

    IronLIR_Module *mod = iron_lir_module_create(&ir_arena, "test");
    Iron_Type *int_type  = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_Type *bool_type = iron_type_make_primitive(IRON_TYPE_BOOL);
    Iron_Span span = zero_span();

    /* callee: takes 1 Int param, returns Int */
    IronLIR_Param callee_params[1] = {
        { .name = "x", .type = int_type }
    };
    IronLIR_Func *callee = iron_lir_func_create(mod, "callee", callee_params, 1, int_type);
    IronLIR_Block *callee_entry = iron_lir_block_create(callee, "entry");
    IronLIR_Instr *callee_ret_val = iron_lir_const_int(callee, callee_entry, 0, int_type, span);
    iron_lir_return(callee, callee_entry, callee_ret_val->id, false, int_type, span);

    /* caller: passes 1 Bool arg (type mismatch, correct count) */
    IronLIR_Func *caller = iron_lir_func_create(mod, "caller", NULL, 0, NULL);
    IronLIR_Block *caller_entry = iron_lir_block_create(caller, "entry");
    IronLIR_Instr *arg1 = iron_lir_const_bool(caller, caller_entry, true, bool_type, span);

    Iron_FuncDecl fake_decl = {0};
    fake_decl.name = "callee";
    fake_decl.param_count = 1;

    IronLIR_ValueId args[] = { arg1->id };
    iron_lir_call(caller, caller_entry, &fake_decl, IRON_LIR_VALUE_INVALID,
                  args, 1, int_type, span);
    iron_lir_return(caller, caller_entry, IRON_LIR_VALUE_INVALID, true, NULL, span);

    Iron_DiagList diags = iron_diaglist_create();
    bool result = iron_lir_verify(mod, &diags, &ir_arena);

    TEST_ASSERT_FALSE(result);
    TEST_ASSERT_TRUE(has_error(&diags, IRON_ERR_LIR_CALL_TYPE_MISMATCH));

    iron_diaglist_free(&diags);
    iron_lir_module_destroy(mod);
    iron_arena_free(&ir_arena);
}

void test_verify_call_well_formed(void) {
    Iron_Arena ir_arena = iron_arena_create(4096);
    iron_types_init(&ir_arena);

    IronLIR_Module *mod = iron_lir_module_create(&ir_arena, "test");
    Iron_Type *int_type = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_Span span = zero_span();

    /* callee: takes 1 Int param, returns Int */
    IronLIR_Param callee_params[1] = {
        { .name = "x", .type = int_type }
    };
    IronLIR_Func *callee = iron_lir_func_create(mod, "callee", callee_params, 1, int_type);
    IronLIR_Block *callee_entry = iron_lir_block_create(callee, "entry");
    IronLIR_Instr *callee_ret_val = iron_lir_const_int(callee, callee_entry, 0, int_type, span);
    iron_lir_return(callee, callee_entry, callee_ret_val->id, false, int_type, span);

    /* caller: passes 1 Int arg (correct count and type) */
    IronLIR_Func *caller = iron_lir_func_create(mod, "caller", NULL, 0, NULL);
    IronLIR_Block *caller_entry = iron_lir_block_create(caller, "entry");
    IronLIR_Instr *arg1 = iron_lir_const_int(caller, caller_entry, 42, int_type, span);

    Iron_FuncDecl fake_decl = {0};
    fake_decl.name = "callee";
    fake_decl.param_count = 1;

    IronLIR_ValueId args[] = { arg1->id };
    iron_lir_call(caller, caller_entry, &fake_decl, IRON_LIR_VALUE_INVALID,
                  args, 1, int_type, span);
    iron_lir_return(caller, caller_entry, IRON_LIR_VALUE_INVALID, true, NULL, span);

    Iron_DiagList diags = iron_diaglist_create();
    bool result = iron_lir_verify(mod, &diags, &ir_arena);

    TEST_ASSERT_TRUE(result);
    TEST_ASSERT_EQUAL_INT(0, diags.error_count);

    iron_diaglist_free(&diags);
    iron_lir_module_destroy(mod);
    iron_arena_free(&ir_arena);
}

void test_verify_call_indirect_skipped(void) {
    Iron_Arena ir_arena = iron_arena_create(4096);
    iron_types_init(&ir_arena);

    IronLIR_Module *mod = iron_lir_module_create(&ir_arena, "test");
    Iron_Type *int_type = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_Span span = zero_span();

    /* Single function with an indirect call (func_decl=NULL) */
    IronLIR_Func *fn = iron_lir_func_create(mod, "fn", NULL, 0, NULL);
    IronLIR_Block *entry = iron_lir_block_create(fn, "entry");

    /* Create a const_int to use as a fake func_ptr for the indirect call */
    IronLIR_Instr *fake_ptr = iron_lir_const_int(fn, entry, 0, int_type, span);

    iron_lir_call(fn, entry, NULL, fake_ptr->id,
                  NULL, 0, int_type, span);
    iron_lir_return(fn, entry, IRON_LIR_VALUE_INVALID, true, NULL, span);

    Iron_DiagList diags = iron_diaglist_create();
    iron_lir_verify(mod, &diags, &ir_arena);

    /* Should NOT contain IRON_ERR_LIR_CALL_TYPE_MISMATCH -- indirect calls are skipped */
    TEST_ASSERT_FALSE(has_error(&diags, IRON_ERR_LIR_CALL_TYPE_MISMATCH));

    iron_diaglist_free(&diags);
    iron_lir_module_destroy(mod);
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
    RUN_TEST(test_verify_phi_type_mismatch);
    RUN_TEST(test_verify_phi_well_formed);
    RUN_TEST(test_verify_call_arg_count_mismatch);
    RUN_TEST(test_verify_call_arg_type_mismatch);
    RUN_TEST(test_verify_call_well_formed);
    RUN_TEST(test_verify_call_indirect_skipped);

    return UNITY_END();
}
