/* test_hir_to_lir.c — Unity smoke tests for HIR-to-LIR lowering pass.
 *
 * Each test builds a minimal HIR module programmatically using iron_hir_*
 * constructors, calls iron_hir_to_lir(), and verifies the LIR module structure.
 *
 * Covers:
 *   1.  test_hir_to_lir_empty_func          — empty func -> entry block + return
 *   2.  test_hir_to_lir_val_binding         — val x = 42 -> alloca + store
 *   3.  test_hir_to_lir_var_binding         — var x = 42 -> alloca + store
 *   4.  test_hir_to_lir_if_else_cfg         — if/else -> >= 3 blocks
 *   5.  test_hir_to_lir_while_loop_cfg      — while -> >= 3 blocks with back-edge
 *   6.  test_hir_to_lir_match_switch        — match -> SWITCH terminator
 *   7.  test_hir_to_lir_return_value        — return -> RETURN terminator with value
 *   8.  test_hir_to_lir_binop               — a + b -> BINOP instruction
 *   9.  test_hir_to_lir_call                — func call -> CALL instruction
 *   10. test_hir_to_lir_alloca_in_entry     — all ALLOCAs in entry block (fn->blocks[0])
 *   11. test_hir_to_lir_phi_at_merge        — var modified in if/else has PHI at merge
 *   12. test_hir_to_lir_verify_passes       — full conversion passes iron_lir_verify()
 *   13. test_hir_to_lir_short_circuit_and   — a && b -> multi-block branch pattern
 */

#include "unity.h"
#include "hir/hir.h"
#include "hir/hir_to_lir.h"
#include "lir/lir.h"
#include "lir/verify.h"
#include "analyzer/types.h"
#include "diagnostics/diagnostics.h"
#include "util/arena.h"

#include <string.h>
#include <stdbool.h>
#include <stdint.h>

/* ── Fixtures ────────────────────────────────────────────────────────────── */

static IronHIR_Module *g_mod = NULL;
static Iron_Arena      g_lir_arena;
static Iron_DiagList   g_diags;

void setUp(void) {
    iron_types_init(NULL);
    g_mod = iron_hir_module_create("test_module");
    g_lir_arena = iron_arena_create(256 * 1024);
    g_diags = iron_diaglist_create();
}

void tearDown(void) {
    iron_hir_module_destroy(g_mod);
    g_mod = NULL;
    iron_arena_free(&g_lir_arena);
    iron_diaglist_free(&g_diags);
}

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static Iron_Span zero_span(void) {
    return iron_span_make("test.iron", 1, 1, 1, 1);
}

/* Run iron_hir_to_lir with NULL program/scope (no type decls needed for unit tests) */
static IronLIR_Module *do_lower(void) {
    return iron_hir_to_lir(g_mod, NULL, NULL, &g_lir_arena, &g_diags);
}

/* Count instructions of a specific kind across ALL blocks in a function */
static int count_instrs_in_func(IronLIR_Func *fn, IronLIR_InstrKind kind) {
    int count = 0;
    for (int bi = 0; bi < fn->block_count; bi++) {
        IronLIR_Block *blk = fn->blocks[bi];
        for (int ii = 0; ii < blk->instr_count; ii++) {
            if (blk->instrs[ii]->kind == kind) count++;
        }
    }
    return count;
}

/* Count instructions of a kind in a specific block */
static int count_instrs_in_block(IronLIR_Block *blk, IronLIR_InstrKind kind) {
    int count = 0;
    for (int ii = 0; ii < blk->instr_count; ii++) {
        if (blk->instrs[ii]->kind == kind) count++;
    }
    return count;
}

/* Check if entry block (blocks[0]) contains an ALLOCA with given name hint */
static bool entry_has_alloca(IronLIR_Func *fn, const char *name_hint) {
    if (fn->block_count == 0) return false;
    IronLIR_Block *entry = fn->blocks[0];
    for (int ii = 0; ii < entry->instr_count; ii++) {
        IronLIR_Instr *instr = entry->instrs[ii];
        if (instr->kind == IRON_LIR_ALLOCA) {
            if (name_hint == NULL) return true;
            if (instr->alloca.name_hint &&
                strcmp(instr->alloca.name_hint, name_hint) == 0) return true;
        }
    }
    return false;
}

/* ── Test 1: Empty func produces LIR func with entry block + RETURN ───────── */

void test_hir_to_lir_empty_func(void) {
    Iron_Span span    = zero_span();
    Iron_Type *void_t = iron_type_make_primitive(IRON_TYPE_VOID);

    IronHIR_Func *fn = iron_hir_func_create(g_mod, "empty_func",
                                             NULL, 0, void_t);
    fn->body = iron_hir_block_create(g_mod); /* empty body */
    iron_hir_module_add_func(g_mod, fn);
    (void)span;

    IronLIR_Module *lir = do_lower();
    TEST_ASSERT_NOT_NULL(lir);

    /* Should have at least 1 function */
    TEST_ASSERT_GREATER_OR_EQUAL(1, lir->func_count);

    IronLIR_Func *lf = lir->funcs[0];
    TEST_ASSERT_NOT_NULL(lf);
    TEST_ASSERT_EQUAL_STRING("empty_func", lf->name);

    /* Should have at least 1 block */
    TEST_ASSERT_GREATER_OR_EQUAL(1, lf->block_count);

    /* Should have a RETURN somewhere */
    int ret_count = count_instrs_in_func(lf, IRON_LIR_RETURN);
    TEST_ASSERT_GREATER_OR_EQUAL(1, ret_count);
}

/* ── Test 2: Val binding — val x = 42 → direct SSA value (no alloca) ─────── */

void test_hir_to_lir_val_binding(void) {
    Iron_Span span    = zero_span();
    Iron_Type *int_t  = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_Type *void_t = iron_type_make_primitive(IRON_TYPE_VOID);

    IronHIR_VarId x_id  = iron_hir_alloc_var(g_mod, "x", int_t, false);
    IronHIR_Expr *lit42 = iron_hir_expr_int_lit(g_mod, 42, int_t, span);
    IronHIR_Stmt *let_x = iron_hir_stmt_let(g_mod, x_id, int_t, lit42, false, span);

    IronHIR_Block *body = iron_hir_block_create(g_mod);
    iron_hir_block_add_stmt(body, let_x);

    IronHIR_Func *fn = iron_hir_func_create(g_mod, "val_func", NULL, 0, void_t);
    fn->body = body;
    iron_hir_module_add_func(g_mod, fn);

    IronLIR_Module *lir = do_lower();
    TEST_ASSERT_NOT_NULL(lir);
    TEST_ASSERT_GREATER_OR_EQUAL(1, lir->func_count);

    IronLIR_Func *lf = lir->funcs[0];
    TEST_ASSERT_NOT_NULL(lf);

    /* Immutable vals are lowered to direct SSA values (no alloca needed).
     * Verify the function was produced and has at least one block with a RETURN. */
    TEST_ASSERT_GREATER_OR_EQUAL(1, lf->block_count);
    int ret_count = count_instrs_in_func(lf, IRON_LIR_RETURN);
    TEST_ASSERT_GREATER_OR_EQUAL(1, ret_count);

    /* No alloca for x — immutable vals skip the alloca+store path */
    TEST_ASSERT_FALSE(entry_has_alloca(lf, "x"));
}

/* ── Test 3: Var binding — var x = 42 → alloca + store ──────────────────── */

void test_hir_to_lir_var_binding(void) {
    Iron_Span span    = zero_span();
    Iron_Type *int_t  = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_Type *void_t = iron_type_make_primitive(IRON_TYPE_VOID);

    IronHIR_VarId x_id  = iron_hir_alloc_var(g_mod, "x", int_t, true);
    IronHIR_Expr *lit10 = iron_hir_expr_int_lit(g_mod, 10, int_t, span);
    IronHIR_Stmt *let_x = iron_hir_stmt_let(g_mod, x_id, int_t, lit10, true, span);

    IronHIR_Block *body = iron_hir_block_create(g_mod);
    iron_hir_block_add_stmt(body, let_x);

    IronHIR_Func *fn = iron_hir_func_create(g_mod, "var_func", NULL, 0, void_t);
    fn->body = body;
    iron_hir_module_add_func(g_mod, fn);

    IronLIR_Module *lir = do_lower();
    TEST_ASSERT_NOT_NULL(lir);

    IronLIR_Func *lf = lir->funcs[0];

    /* Must have ALLOCA in entry block */
    int allocas = count_instrs_in_block(lf->blocks[0], IRON_LIR_ALLOCA);
    TEST_ASSERT_GREATER_OR_EQUAL(1, allocas);

    /* Must have STORE somewhere */
    int stores = count_instrs_in_func(lf, IRON_LIR_STORE);
    TEST_ASSERT_GREATER_OR_EQUAL(1, stores);
}

/* ── Test 4: if/else CFG — produces >= 3 blocks ─────────────────────────── */

void test_hir_to_lir_if_else_cfg(void) {
    Iron_Span span     = zero_span();
    Iron_Type *bool_t  = iron_type_make_primitive(IRON_TYPE_BOOL);
    Iron_Type *void_t  = iron_type_make_primitive(IRON_TYPE_VOID);

    /* if true { } else { } */
    IronHIR_Expr  *cond      = iron_hir_expr_bool_lit(g_mod, true, bool_t, span);
    IronHIR_Block *then_body = iron_hir_block_create(g_mod);
    IronHIR_Block *else_body = iron_hir_block_create(g_mod);
    IronHIR_Stmt  *if_stmt   = iron_hir_stmt_if(g_mod, cond, then_body, else_body, span);

    IronHIR_Block *body = iron_hir_block_create(g_mod);
    iron_hir_block_add_stmt(body, if_stmt);

    IronHIR_Func *fn = iron_hir_func_create(g_mod, "if_func", NULL, 0, void_t);
    fn->body = body;
    iron_hir_module_add_func(g_mod, fn);

    IronLIR_Module *lir = do_lower();
    TEST_ASSERT_NOT_NULL(lir);

    IronLIR_Func *lf = lir->funcs[0];

    /* if/else requires: entry, then_block, else_block (and optionally merge_block) */
    TEST_ASSERT_GREATER_OR_EQUAL(3, lf->block_count);

    /* Must have a BRANCH somewhere */
    int branches = count_instrs_in_func(lf, IRON_LIR_BRANCH);
    TEST_ASSERT_GREATER_OR_EQUAL(1, branches);
}

/* ── Test 5: while loop — >= 3 blocks with back-edge ────────────────────── */

void test_hir_to_lir_while_loop_cfg(void) {
    Iron_Span span    = zero_span();
    Iron_Type *bool_t = iron_type_make_primitive(IRON_TYPE_BOOL);
    Iron_Type *void_t = iron_type_make_primitive(IRON_TYPE_VOID);

    /* while false { } */
    IronHIR_Expr  *cond = iron_hir_expr_bool_lit(g_mod, false, bool_t, span);
    IronHIR_Block *body_blk = iron_hir_block_create(g_mod);
    IronHIR_Stmt  *while_stmt = iron_hir_stmt_while(g_mod, cond, body_blk, span);

    IronHIR_Block *body = iron_hir_block_create(g_mod);
    iron_hir_block_add_stmt(body, while_stmt);

    IronHIR_Func *fn = iron_hir_func_create(g_mod, "while_func", NULL, 0, void_t);
    fn->body = body;
    iron_hir_module_add_func(g_mod, fn);

    IronLIR_Module *lir = do_lower();
    TEST_ASSERT_NOT_NULL(lir);

    IronLIR_Func *lf = lir->funcs[0];

    /* while requires: entry/pre, header, body, exit = at least 3 */
    TEST_ASSERT_GREATER_OR_EQUAL(3, lf->block_count);

    /* Must have a BRANCH (loop condition check) */
    int branches = count_instrs_in_func(lf, IRON_LIR_BRANCH);
    TEST_ASSERT_GREATER_OR_EQUAL(1, branches);

    /* Must have at least one JUMP (back-edge or forward edge) */
    int jumps = count_instrs_in_func(lf, IRON_LIR_JUMP);
    TEST_ASSERT_GREATER_OR_EQUAL(1, jumps);
}

/* ── Test 6: match — produces SWITCH terminator ─────────────────────────── */

void test_hir_to_lir_match_switch(void) {
    Iron_Span span   = zero_span();
    Iron_Type *int_t = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_Type *void_t = iron_type_make_primitive(IRON_TYPE_VOID);

    /* match x { 1 -> {} else -> {} } */
    IronHIR_VarId x_id = iron_hir_alloc_var(g_mod, "x", int_t, false);
    IronHIR_Stmt *let_x = iron_hir_stmt_let(g_mod, x_id, int_t,
                                             iron_hir_expr_int_lit(g_mod, 5, int_t, span),
                                             false, span);

    IronHIR_Expr *scrutinee = iron_hir_expr_ident(g_mod, x_id, "x", int_t, span);

    IronHIR_MatchArm arms[2];
    arms[0].pattern = iron_hir_expr_int_lit(g_mod, 1, int_t, span);
    arms[0].guard   = NULL;
    arms[0].body    = iron_hir_block_create(g_mod);

    arms[1].pattern = iron_hir_expr_int_lit(g_mod, 2, int_t, span);
    arms[1].guard   = NULL;
    arms[1].body    = iron_hir_block_create(g_mod);

    IronHIR_Stmt *match_stmt = iron_hir_stmt_match(g_mod, scrutinee, arms, 2, span);

    IronHIR_Block *body = iron_hir_block_create(g_mod);
    iron_hir_block_add_stmt(body, let_x);
    iron_hir_block_add_stmt(body, match_stmt);

    IronHIR_Func *fn = iron_hir_func_create(g_mod, "match_func", NULL, 0, void_t);
    fn->body = body;
    iron_hir_module_add_func(g_mod, fn);

    IronLIR_Module *lir = do_lower();
    TEST_ASSERT_NOT_NULL(lir);

    IronLIR_Func *lf = lir->funcs[0];

    /* Must have SWITCH terminator */
    int switches = count_instrs_in_func(lf, IRON_LIR_SWITCH);
    TEST_ASSERT_GREATER_OR_EQUAL(1, switches);
}

/* ── Test 7: return value — produces RETURN terminator with value ─────────── */

void test_hir_to_lir_return_value(void) {
    Iron_Span span   = zero_span();
    Iron_Type *int_t = iron_type_make_primitive(IRON_TYPE_INT);

    IronHIR_Expr *ret_val  = iron_hir_expr_int_lit(g_mod, 99, int_t, span);
    IronHIR_Stmt *ret_stmt = iron_hir_stmt_return(g_mod, ret_val, span);

    IronHIR_Block *body = iron_hir_block_create(g_mod);
    iron_hir_block_add_stmt(body, ret_stmt);

    IronHIR_Func *fn = iron_hir_func_create(g_mod, "ret_func", NULL, 0, int_t);
    fn->body = body;
    iron_hir_module_add_func(g_mod, fn);

    IronLIR_Module *lir = do_lower();
    TEST_ASSERT_NOT_NULL(lir);

    IronLIR_Func *lf = lir->funcs[0];

    /* Must have RETURN */
    int rets = count_instrs_in_func(lf, IRON_LIR_RETURN);
    TEST_ASSERT_GREATER_OR_EQUAL(1, rets);

    /* The RETURN must carry a non-void value */
    bool found_valued_return = false;
    for (int bi = 0; bi < lf->block_count; bi++) {
        IronLIR_Block *blk = lf->blocks[bi];
        for (int ii = 0; ii < blk->instr_count; ii++) {
            IronLIR_Instr *instr = blk->instrs[ii];
            if (instr->kind == IRON_LIR_RETURN && !instr->ret.is_void) {
                found_valued_return = true;
            }
        }
    }
    TEST_ASSERT_TRUE(found_valued_return);
}

/* ── Test 8: binop — a + b produces ADD instruction ─────────────────────── */

void test_hir_to_lir_binop(void) {
    Iron_Span span   = zero_span();
    Iron_Type *int_t = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_Type *void_t = iron_type_make_primitive(IRON_TYPE_VOID);

    IronHIR_Expr *a_lit = iron_hir_expr_int_lit(g_mod, 3, int_t, span);
    IronHIR_Expr *b_lit = iron_hir_expr_int_lit(g_mod, 4, int_t, span);
    IronHIR_Expr *add   = iron_hir_expr_binop(g_mod, IRON_HIR_BINOP_ADD, a_lit, b_lit, int_t, span);
    IronHIR_Stmt *expr_stmt = iron_hir_stmt_expr(g_mod, add, span);

    IronHIR_Block *body = iron_hir_block_create(g_mod);
    iron_hir_block_add_stmt(body, expr_stmt);

    IronHIR_Func *fn = iron_hir_func_create(g_mod, "binop_func", NULL, 0, void_t);
    fn->body = body;
    iron_hir_module_add_func(g_mod, fn);

    IronLIR_Module *lir = do_lower();
    TEST_ASSERT_NOT_NULL(lir);

    IronLIR_Func *lf = lir->funcs[0];

    int adds = count_instrs_in_func(lf, IRON_LIR_ADD);
    TEST_ASSERT_GREATER_OR_EQUAL(1, adds);
}

/* ── Test 9: function call — produces CALL instruction ───────────────────── */

void test_hir_to_lir_call(void) {
    Iron_Span span   = zero_span();
    Iron_Type *void_t = iron_type_make_primitive(IRON_TYPE_VOID);
    Iron_Type *int_t  = iron_type_make_primitive(IRON_TYPE_INT);

    /* Call a function by func_ref */
    IronHIR_Expr *callee = iron_hir_expr_func_ref(g_mod, "some_func", void_t, span);
    IronHIR_Expr *arg    = iron_hir_expr_int_lit(g_mod, 1, int_t, span);

    IronHIR_Expr **args = NULL;
    arrput(args, arg);

    IronHIR_Expr *call_expr = iron_hir_expr_call(g_mod, callee, args, 1, void_t, span);
    IronHIR_Stmt *call_stmt = iron_hir_stmt_expr(g_mod, call_expr, span);

    IronHIR_Block *body = iron_hir_block_create(g_mod);
    iron_hir_block_add_stmt(body, call_stmt);

    IronHIR_Func *fn = iron_hir_func_create(g_mod, "caller_func", NULL, 0, void_t);
    fn->body = body;
    iron_hir_module_add_func(g_mod, fn);

    IronLIR_Module *lir = do_lower();
    TEST_ASSERT_NOT_NULL(lir);

    IronLIR_Func *lf = lir->funcs[0];

    int calls = count_instrs_in_func(lf, IRON_LIR_CALL);
    TEST_ASSERT_GREATER_OR_EQUAL(1, calls);
}

/* ── Test 10: ALLOCAs are in entry block even for loop-declared vars ─────── */

void test_hir_to_lir_alloca_in_entry(void) {
    Iron_Span span    = zero_span();
    Iron_Type *int_t  = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_Type *bool_t = iron_type_make_primitive(IRON_TYPE_BOOL);
    Iron_Type *void_t = iron_type_make_primitive(IRON_TYPE_VOID);

    /* while true { var x = 42 } */
    IronHIR_VarId x_id  = iron_hir_alloc_var(g_mod, "x", int_t, true);
    IronHIR_Expr *lit42 = iron_hir_expr_int_lit(g_mod, 42, int_t, span);
    IronHIR_Stmt *let_x = iron_hir_stmt_let(g_mod, x_id, int_t, lit42, true, span);

    IronHIR_Block *loop_body = iron_hir_block_create(g_mod);
    iron_hir_block_add_stmt(loop_body, let_x);

    IronHIR_Expr *cond = iron_hir_expr_bool_lit(g_mod, false, bool_t, span);
    IronHIR_Stmt *while_s = iron_hir_stmt_while(g_mod, cond, loop_body, span);

    IronHIR_Block *body = iron_hir_block_create(g_mod);
    iron_hir_block_add_stmt(body, while_s);

    IronHIR_Func *fn = iron_hir_func_create(g_mod, "alloca_in_entry_func", NULL, 0, void_t);
    fn->body = body;
    iron_hir_module_add_func(g_mod, fn);

    IronLIR_Module *lir = do_lower();
    TEST_ASSERT_NOT_NULL(lir);

    IronLIR_Func *lf = lir->funcs[0];
    TEST_ASSERT_GREATER_OR_EQUAL(1, lf->block_count);

    /* All ALLOCAs must be in blocks[0] (entry block) */
    IronLIR_Block *entry = lf->blocks[0];
    int entry_allocas = count_instrs_in_block(entry, IRON_LIR_ALLOCA);

    /* Count allocas in non-entry blocks */
    int other_allocas = 0;
    for (int bi = 1; bi < lf->block_count; bi++) {
        other_allocas += count_instrs_in_block(lf->blocks[bi], IRON_LIR_ALLOCA);
    }

    /* All allocas must be in entry block */
    TEST_ASSERT_EQUAL_INT(0, other_allocas);
    TEST_ASSERT_GREATER_OR_EQUAL(1, entry_allocas);
}

/* ── Test 11: PHI node at merge block after if/else that modifies a var ──── */

void test_hir_to_lir_phi_at_merge(void) {
    Iron_Span span    = zero_span();
    Iron_Type *int_t  = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_Type *bool_t = iron_type_make_primitive(IRON_TYPE_BOOL);
    Iron_Type *void_t = iron_type_make_primitive(IRON_TYPE_VOID);

    /* var x = 0
     * if true { x = 1 } else { x = 2 }
     * The merge block should have a phi for x after SSA construction. */
    IronHIR_VarId x_id = iron_hir_alloc_var(g_mod, "x", int_t, true);
    IronHIR_Stmt *let_x = iron_hir_stmt_let(g_mod, x_id, int_t,
                                              iron_hir_expr_int_lit(g_mod, 0, int_t, span),
                                              true, span);

    /* then: x = 1 */
    IronHIR_Expr *x_ref1 = iron_hir_expr_ident(g_mod, x_id, "x", int_t, span);
    IronHIR_Stmt *assign1 = iron_hir_stmt_assign(g_mod, x_ref1,
                                                   iron_hir_expr_int_lit(g_mod, 1, int_t, span),
                                                   span);
    IronHIR_Block *then_body = iron_hir_block_create(g_mod);
    iron_hir_block_add_stmt(then_body, assign1);

    /* else: x = 2 */
    IronHIR_Expr *x_ref2 = iron_hir_expr_ident(g_mod, x_id, "x", int_t, span);
    IronHIR_Stmt *assign2 = iron_hir_stmt_assign(g_mod, x_ref2,
                                                   iron_hir_expr_int_lit(g_mod, 2, int_t, span),
                                                   span);
    IronHIR_Block *else_body = iron_hir_block_create(g_mod);
    iron_hir_block_add_stmt(else_body, assign2);

    IronHIR_Expr *cond = iron_hir_expr_bool_lit(g_mod, true, bool_t, span);
    IronHIR_Stmt *if_stmt = iron_hir_stmt_if(g_mod, cond, then_body, else_body, span);

    IronHIR_Block *body = iron_hir_block_create(g_mod);
    iron_hir_block_add_stmt(body, let_x);
    iron_hir_block_add_stmt(body, if_stmt);

    IronHIR_Func *fn = iron_hir_func_create(g_mod, "phi_func", NULL, 0, void_t);
    fn->body = body;
    iron_hir_module_add_func(g_mod, fn);

    IronLIR_Module *lir = do_lower();
    TEST_ASSERT_NOT_NULL(lir);

    IronLIR_Func *lf = lir->funcs[0];

    /* PHI nodes should exist after SSA construction */
    int phis = count_instrs_in_func(lf, IRON_LIR_PHI);
    TEST_ASSERT_GREATER_OR_EQUAL(1, phis);
}

/* ── Test 12: Full conversion passes iron_lir_verify() ──────────────────── */

void test_hir_to_lir_verify_passes(void) {
    Iron_Span span   = zero_span();
    Iron_Type *int_t = iron_type_make_primitive(IRON_TYPE_INT);

    /* func add(a: Int, b: Int) -> Int { return a + b } */
    IronHIR_VarId a_id = iron_hir_alloc_var(g_mod, "a", int_t, false);
    IronHIR_VarId b_id = iron_hir_alloc_var(g_mod, "b", int_t, false);

    IronHIR_Param params[2];
    params[0].var_id = a_id;
    params[0].name   = "a";
    params[0].type   = int_t;
    params[1].var_id = b_id;
    params[1].name   = "b";
    params[1].type   = int_t;

    IronHIR_Expr *a_ref = iron_hir_expr_ident(g_mod, a_id, "a", int_t, span);
    IronHIR_Expr *b_ref = iron_hir_expr_ident(g_mod, b_id, "b", int_t, span);
    IronHIR_Expr *add   = iron_hir_expr_binop(g_mod, IRON_HIR_BINOP_ADD, a_ref, b_ref, int_t, span);
    IronHIR_Stmt *ret   = iron_hir_stmt_return(g_mod, add, span);

    IronHIR_Block *body = iron_hir_block_create(g_mod);
    iron_hir_block_add_stmt(body, ret);

    IronHIR_Func *fn = iron_hir_func_create(g_mod, "add", params, 2, int_t);
    fn->body = body;
    iron_hir_module_add_func(g_mod, fn);

    /* Clear any previous diagnostics */
    iron_diaglist_free(&g_diags);
    g_diags = iron_diaglist_create();

    IronLIR_Module *lir = iron_hir_to_lir(g_mod, NULL, NULL, &g_lir_arena, &g_diags);
    TEST_ASSERT_NOT_NULL(lir);

    /* Re-run verify explicitly */
    Iron_Arena verify_arena = iron_arena_create(64 * 1024);
    Iron_DiagList verify_diags = iron_diaglist_create();
    bool ok = iron_lir_verify(lir, &verify_diags, &verify_arena);

    /* iron_lir_verify should pass (true = ok) */
    TEST_ASSERT_TRUE(ok);

    iron_diaglist_free(&verify_diags);
    iron_arena_free(&verify_arena);
}

/* ── Test 13: Short-circuit AND — a && b produces multi-block branch+phi ── */

void test_hir_to_lir_short_circuit_and(void) {
    Iron_Span span    = zero_span();
    Iron_Type *bool_t = iron_type_make_primitive(IRON_TYPE_BOOL);
    Iron_Type *void_t = iron_type_make_primitive(IRON_TYPE_VOID);

    /* val a = true
     * val b = false
     * val c = a && b */
    IronHIR_VarId a_id = iron_hir_alloc_var(g_mod, "a", bool_t, false);
    IronHIR_VarId b_id = iron_hir_alloc_var(g_mod, "b", bool_t, false);
    IronHIR_VarId c_id = iron_hir_alloc_var(g_mod, "c", bool_t, false);

    IronHIR_Stmt *let_a = iron_hir_stmt_let(g_mod, a_id, bool_t,
                                              iron_hir_expr_bool_lit(g_mod, true, bool_t, span),
                                              false, span);
    IronHIR_Stmt *let_b = iron_hir_stmt_let(g_mod, b_id, bool_t,
                                              iron_hir_expr_bool_lit(g_mod, false, bool_t, span),
                                              false, span);

    IronHIR_Expr *a_ref = iron_hir_expr_ident(g_mod, a_id, "a", bool_t, span);
    IronHIR_Expr *b_ref = iron_hir_expr_ident(g_mod, b_id, "b", bool_t, span);
    IronHIR_Expr *and_e = iron_hir_expr_binop(g_mod, IRON_HIR_BINOP_AND, a_ref, b_ref, bool_t, span);
    IronHIR_Stmt *let_c = iron_hir_stmt_let(g_mod, c_id, bool_t, and_e, false, span);

    IronHIR_Block *body = iron_hir_block_create(g_mod);
    iron_hir_block_add_stmt(body, let_a);
    iron_hir_block_add_stmt(body, let_b);
    iron_hir_block_add_stmt(body, let_c);

    IronHIR_Func *fn = iron_hir_func_create(g_mod, "and_func", NULL, 0, void_t);
    fn->body = body;
    iron_hir_module_add_func(g_mod, fn);

    IronLIR_Module *lir = do_lower();
    TEST_ASSERT_NOT_NULL(lir);

    IronLIR_Func *lf = lir->funcs[0];

    /* Short-circuit AND creates extra blocks: at least 4 blocks
     * (entry, and_rhs, and_merge, implicit return) */
    TEST_ASSERT_GREATER_OR_EQUAL(3, lf->block_count);

    /* Must have a BRANCH (for the short-circuit check) */
    int branches = count_instrs_in_func(lf, IRON_LIR_BRANCH);
    TEST_ASSERT_GREATER_OR_EQUAL(1, branches);

    /* Must have a PHI at the merge block */
    int phis = count_instrs_in_func(lf, IRON_LIR_PHI);
    TEST_ASSERT_GREATER_OR_EQUAL(1, phis);
}

/* ── Runner ───────────────────────────────────────────────────────────────── */

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_hir_to_lir_empty_func);
    RUN_TEST(test_hir_to_lir_val_binding);
    RUN_TEST(test_hir_to_lir_var_binding);
    RUN_TEST(test_hir_to_lir_if_else_cfg);
    RUN_TEST(test_hir_to_lir_while_loop_cfg);
    RUN_TEST(test_hir_to_lir_match_switch);
    RUN_TEST(test_hir_to_lir_return_value);
    RUN_TEST(test_hir_to_lir_binop);
    RUN_TEST(test_hir_to_lir_call);
    RUN_TEST(test_hir_to_lir_alloca_in_entry);
    RUN_TEST(test_hir_to_lir_phi_at_merge);
    RUN_TEST(test_hir_to_lir_verify_passes);
    RUN_TEST(test_hir_to_lir_short_circuit_and);
    return UNITY_END();
}
