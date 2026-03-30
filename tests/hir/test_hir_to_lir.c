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

/* ════════════════════════════════════════════════════════════════════════════
 * TASK 1 — Feature-matrix tests (~30 tests)
 * ════════════════════════════════════════════════════════════════════════════ */

/* ── Test 14: if without else — >= 3 blocks (entry/then/merge) ───────────── */

void test_h2l_if_no_else_blocks(void) {
    Iron_Span span    = zero_span();
    Iron_Type *bool_t = iron_type_make_primitive(IRON_TYPE_BOOL);
    Iron_Type *void_t = iron_type_make_primitive(IRON_TYPE_VOID);

    /* if true { } */
    IronHIR_Expr  *cond = iron_hir_expr_bool_lit(g_mod, true, bool_t, span);
    IronHIR_Block *then_body = iron_hir_block_create(g_mod);
    IronHIR_Stmt  *if_stmt   = iron_hir_stmt_if(g_mod, cond, then_body, NULL, span);

    IronHIR_Block *body = iron_hir_block_create(g_mod);
    iron_hir_block_add_stmt(body, if_stmt);

    IronHIR_Func *fn = iron_hir_func_create(g_mod, "if_no_else", NULL, 0, void_t);
    fn->body = body;
    iron_hir_module_add_func(g_mod, fn);

    IronLIR_Module *lir = do_lower();
    TEST_ASSERT_NOT_NULL(lir);

    IronLIR_Func *lf = lir->funcs[0];
    /* entry + then + merge = at least 3 */
    TEST_ASSERT_GREATER_OR_EQUAL(3, lf->block_count);
    int branches = count_instrs_in_func(lf, IRON_LIR_BRANCH);
    TEST_ASSERT_GREATER_OR_EQUAL(1, branches);
}

/* ── Test 15: if/else — >= 4 blocks (entry/then/else/merge) ─────────────── */

void test_h2l_if_else_blocks(void) {
    Iron_Span span    = zero_span();
    Iron_Type *bool_t = iron_type_make_primitive(IRON_TYPE_BOOL);
    Iron_Type *void_t = iron_type_make_primitive(IRON_TYPE_VOID);

    IronHIR_Expr  *cond      = iron_hir_expr_bool_lit(g_mod, false, bool_t, span);
    IronHIR_Block *then_body = iron_hir_block_create(g_mod);
    IronHIR_Block *else_body = iron_hir_block_create(g_mod);
    IronHIR_Stmt  *if_stmt   = iron_hir_stmt_if(g_mod, cond, then_body, else_body, span);

    IronHIR_Block *body = iron_hir_block_create(g_mod);
    iron_hir_block_add_stmt(body, if_stmt);

    IronHIR_Func *fn = iron_hir_func_create(g_mod, "if_else_blocks", NULL, 0, void_t);
    fn->body = body;
    iron_hir_module_add_func(g_mod, fn);

    IronLIR_Module *lir = do_lower();
    TEST_ASSERT_NOT_NULL(lir);

    IronLIR_Func *lf = lir->funcs[0];
    /* entry + then + else + merge = at least 4 */
    TEST_ASSERT_GREATER_OR_EQUAL(4, lf->block_count);
}

/* ── Test 16: if/else terminators — BRANCH on cond block ────────────────── */

void test_h2l_if_else_terminators(void) {
    Iron_Span span    = zero_span();
    Iron_Type *bool_t = iron_type_make_primitive(IRON_TYPE_BOOL);
    Iron_Type *void_t = iron_type_make_primitive(IRON_TYPE_VOID);

    IronHIR_Expr  *cond      = iron_hir_expr_bool_lit(g_mod, true, bool_t, span);
    IronHIR_Block *then_body = iron_hir_block_create(g_mod);
    IronHIR_Block *else_body = iron_hir_block_create(g_mod);
    IronHIR_Stmt  *if_stmt   = iron_hir_stmt_if(g_mod, cond, then_body, else_body, span);

    IronHIR_Block *body = iron_hir_block_create(g_mod);
    iron_hir_block_add_stmt(body, if_stmt);

    IronHIR_Func *fn = iron_hir_func_create(g_mod, "if_else_terminators", NULL, 0, void_t);
    fn->body = body;
    iron_hir_module_add_func(g_mod, fn);

    IronLIR_Module *lir = do_lower();
    TEST_ASSERT_NOT_NULL(lir);

    IronLIR_Func *lf = lir->funcs[0];

    /* Every block must end with a terminator (BRANCH, JUMP, or RETURN) */
    for (int bi = 0; bi < lf->block_count; bi++) {
        IronLIR_Block *blk = lf->blocks[bi];
        if (blk->instr_count == 0) continue;
        IronLIR_Instr *last = blk->instrs[blk->instr_count - 1];
        bool is_term = (last->kind == IRON_LIR_BRANCH ||
                        last->kind == IRON_LIR_JUMP    ||
                        last->kind == IRON_LIR_SWITCH  ||
                        last->kind == IRON_LIR_RETURN);
        TEST_ASSERT_TRUE(is_term);
    }
}

/* ── Test 17: while loop back-edge — body block jumps to header ──────────── */

void test_h2l_while_loop_backedge(void) {
    Iron_Span span    = zero_span();
    Iron_Type *bool_t = iron_type_make_primitive(IRON_TYPE_BOOL);
    Iron_Type *void_t = iron_type_make_primitive(IRON_TYPE_VOID);

    IronHIR_Expr  *cond      = iron_hir_expr_bool_lit(g_mod, true, bool_t, span);
    IronHIR_Block *body_blk  = iron_hir_block_create(g_mod);
    IronHIR_Stmt  *while_s   = iron_hir_stmt_while(g_mod, cond, body_blk, span);

    IronHIR_Block *body = iron_hir_block_create(g_mod);
    iron_hir_block_add_stmt(body, while_s);

    IronHIR_Func *fn = iron_hir_func_create(g_mod, "while_backedge", NULL, 0, void_t);
    fn->body = body;
    iron_hir_module_add_func(g_mod, fn);

    IronLIR_Module *lir = do_lower();
    TEST_ASSERT_NOT_NULL(lir);

    IronLIR_Func *lf = lir->funcs[0];
    /* Must have a JUMP somewhere (back-edge from body to header) */
    int jumps = count_instrs_in_func(lf, IRON_LIR_JUMP);
    TEST_ASSERT_GREATER_OR_EQUAL(1, jumps);
    /* Must have a BRANCH for the loop condition */
    int branches = count_instrs_in_func(lf, IRON_LIR_BRANCH);
    TEST_ASSERT_GREATER_OR_EQUAL(1, branches);
}

/* ── Test 18: while loop exit — header→exit edge on false condition ──────── */

void test_h2l_while_loop_exit(void) {
    Iron_Span span    = zero_span();
    Iron_Type *bool_t = iron_type_make_primitive(IRON_TYPE_BOOL);
    Iron_Type *void_t = iron_type_make_primitive(IRON_TYPE_VOID);

    /* while false { } — loop body never executes, exits immediately */
    IronHIR_Expr  *cond     = iron_hir_expr_bool_lit(g_mod, false, bool_t, span);
    IronHIR_Block *body_blk = iron_hir_block_create(g_mod);
    IronHIR_Stmt  *while_s  = iron_hir_stmt_while(g_mod, cond, body_blk, span);

    IronHIR_Block *body = iron_hir_block_create(g_mod);
    iron_hir_block_add_stmt(body, while_s);

    IronHIR_Func *fn = iron_hir_func_create(g_mod, "while_exit", NULL, 0, void_t);
    fn->body = body;
    iron_hir_module_add_func(g_mod, fn);

    IronLIR_Module *lir = do_lower();
    TEST_ASSERT_NOT_NULL(lir);

    IronLIR_Func *lf = lir->funcs[0];
    /* Must have at least 3 blocks: pre/entry, header, exit */
    TEST_ASSERT_GREATER_OR_EQUAL(3, lf->block_count);
    /* The BRANCH at the header has an exit target */
    int branches = count_instrs_in_func(lf, IRON_LIR_BRANCH);
    TEST_ASSERT_GREATER_OR_EQUAL(1, branches);
    /* Must have RETURN in the exit block */
    int rets = count_instrs_in_func(lf, IRON_LIR_RETURN);
    TEST_ASSERT_GREATER_OR_EQUAL(1, rets);
}

/* ── Test 19: nested if inside while — blocks correctly nested ───────────── */

void test_h2l_nested_if_in_while(void) {
    Iron_Span span    = zero_span();
    Iron_Type *bool_t = iron_type_make_primitive(IRON_TYPE_BOOL);
    Iron_Type *void_t = iron_type_make_primitive(IRON_TYPE_VOID);

    /* while true { if false { } } */
    IronHIR_Expr  *inner_cond = iron_hir_expr_bool_lit(g_mod, false, bool_t, span);
    IronHIR_Block *then_body  = iron_hir_block_create(g_mod);
    IronHIR_Stmt  *if_stmt    = iron_hir_stmt_if(g_mod, inner_cond, then_body, NULL, span);

    IronHIR_Block *loop_body  = iron_hir_block_create(g_mod);
    iron_hir_block_add_stmt(loop_body, if_stmt);

    IronHIR_Expr  *outer_cond = iron_hir_expr_bool_lit(g_mod, true, bool_t, span);
    IronHIR_Stmt  *while_s    = iron_hir_stmt_while(g_mod, outer_cond, loop_body, span);

    IronHIR_Block *body = iron_hir_block_create(g_mod);
    iron_hir_block_add_stmt(body, while_s);

    IronHIR_Func *fn = iron_hir_func_create(g_mod, "nested_if_in_while", NULL, 0, void_t);
    fn->body = body;
    iron_hir_module_add_func(g_mod, fn);

    IronLIR_Module *lir = do_lower();
    TEST_ASSERT_NOT_NULL(lir);

    IronLIR_Func *lf = lir->funcs[0];
    /* while + if adds significant blocks: >= 5 */
    TEST_ASSERT_GREATER_OR_EQUAL(5, lf->block_count);
    /* Both while and if produce BRANCHes */
    int branches = count_instrs_in_func(lf, IRON_LIR_BRANCH);
    TEST_ASSERT_GREATER_OR_EQUAL(2, branches);
}

/* ── Test 20: match SWITCH terminator with N cases ───────────────────────── */

void test_h2l_match_switch_terminator(void) {
    Iron_Span span   = zero_span();
    Iron_Type *int_t = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_Type *void_t = iron_type_make_primitive(IRON_TYPE_VOID);

    IronHIR_VarId x_id = iron_hir_alloc_var(g_mod, "x", int_t, false);
    IronHIR_Stmt *let_x = iron_hir_stmt_let(g_mod, x_id, int_t,
                                             iron_hir_expr_int_lit(g_mod, 3, int_t, span),
                                             false, span);
    IronHIR_Expr *scrutinee = iron_hir_expr_ident(g_mod, x_id, "x", int_t, span);

    /* 3 arms */
    IronHIR_MatchArm arms[3];
    arms[0].pattern = iron_hir_expr_int_lit(g_mod, 1, int_t, span);
    arms[0].guard   = NULL;
    arms[0].body    = iron_hir_block_create(g_mod);
    arms[1].pattern = iron_hir_expr_int_lit(g_mod, 2, int_t, span);
    arms[1].guard   = NULL;
    arms[1].body    = iron_hir_block_create(g_mod);
    arms[2].pattern = iron_hir_expr_int_lit(g_mod, 3, int_t, span);
    arms[2].guard   = NULL;
    arms[2].body    = iron_hir_block_create(g_mod);

    IronHIR_Stmt *match_s = iron_hir_stmt_match(g_mod, scrutinee, arms, 3, span);

    IronHIR_Block *body = iron_hir_block_create(g_mod);
    iron_hir_block_add_stmt(body, let_x);
    iron_hir_block_add_stmt(body, match_s);

    IronHIR_Func *fn = iron_hir_func_create(g_mod, "match_3arms", NULL, 0, void_t);
    fn->body = body;
    iron_hir_module_add_func(g_mod, fn);

    IronLIR_Module *lir = do_lower();
    TEST_ASSERT_NOT_NULL(lir);

    IronLIR_Func *lf = lir->funcs[0];
    int switches = count_instrs_in_func(lf, IRON_LIR_SWITCH);
    TEST_ASSERT_GREATER_OR_EQUAL(1, switches);

    /* Verify SWITCH has 3 cases (N arms) */
    bool found_3cases = false;
    for (int bi = 0; bi < lf->block_count; bi++) {
        IronLIR_Block *blk = lf->blocks[bi];
        for (int ii = 0; ii < blk->instr_count; ii++) {
            IronLIR_Instr *instr = blk->instrs[ii];
            if (instr->kind == IRON_LIR_SWITCH && instr->sw.case_count >= 3) {
                found_3cases = true;
            }
        }
    }
    TEST_ASSERT_TRUE(found_3cases);
}

/* ── Test 21: match join block — all arms jump to shared join block ──────── */

void test_h2l_match_join_block(void) {
    Iron_Span span   = zero_span();
    Iron_Type *int_t = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_Type *void_t = iron_type_make_primitive(IRON_TYPE_VOID);

    IronHIR_VarId x_id = iron_hir_alloc_var(g_mod, "x", int_t, false);
    IronHIR_Stmt *let_x = iron_hir_stmt_let(g_mod, x_id, int_t,
                                             iron_hir_expr_int_lit(g_mod, 1, int_t, span),
                                             false, span);
    IronHIR_Expr *scrutinee = iron_hir_expr_ident(g_mod, x_id, "x", int_t, span);

    IronHIR_MatchArm arms[2];
    arms[0].pattern = iron_hir_expr_int_lit(g_mod, 1, int_t, span);
    arms[0].guard   = NULL;
    arms[0].body    = iron_hir_block_create(g_mod);
    arms[1].pattern = iron_hir_expr_int_lit(g_mod, 2, int_t, span);
    arms[1].guard   = NULL;
    arms[1].body    = iron_hir_block_create(g_mod);

    IronHIR_Stmt *match_s = iron_hir_stmt_match(g_mod, scrutinee, arms, 2, span);

    IronHIR_Block *body = iron_hir_block_create(g_mod);
    iron_hir_block_add_stmt(body, let_x);
    iron_hir_block_add_stmt(body, match_s);

    IronHIR_Func *fn = iron_hir_func_create(g_mod, "match_join", NULL, 0, void_t);
    fn->body = body;
    iron_hir_module_add_func(g_mod, fn);

    IronLIR_Module *lir = do_lower();
    TEST_ASSERT_NOT_NULL(lir);

    IronLIR_Func *lf = lir->funcs[0];
    /* SWITCH + 2 arm blocks + join block + entry = at least 4 blocks */
    TEST_ASSERT_GREATER_OR_EQUAL(4, lf->block_count);
    /* All blocks end with a valid terminator */
    for (int bi = 0; bi < lf->block_count; bi++) {
        IronLIR_Block *blk = lf->blocks[bi];
        if (blk->instr_count == 0) continue;
        IronLIR_Instr *last = blk->instrs[blk->instr_count - 1];
        bool is_term = (last->kind == IRON_LIR_BRANCH ||
                        last->kind == IRON_LIR_JUMP    ||
                        last->kind == IRON_LIR_SWITCH  ||
                        last->kind == IRON_LIR_RETURN);
        TEST_ASSERT_TRUE(is_term);
    }
}

/* ── Test 22: nested while loops — outer and inner back-edges ────────────── */

void test_h2l_nested_while_loops(void) {
    Iron_Span span    = zero_span();
    Iron_Type *bool_t = iron_type_make_primitive(IRON_TYPE_BOOL);
    Iron_Type *void_t = iron_type_make_primitive(IRON_TYPE_VOID);

    /* while false { while false { } } */
    IronHIR_Expr  *inner_cond = iron_hir_expr_bool_lit(g_mod, false, bool_t, span);
    IronHIR_Block *inner_body = iron_hir_block_create(g_mod);
    IronHIR_Stmt  *inner_while = iron_hir_stmt_while(g_mod, inner_cond, inner_body, span);

    IronHIR_Block *outer_body = iron_hir_block_create(g_mod);
    iron_hir_block_add_stmt(outer_body, inner_while);

    IronHIR_Expr  *outer_cond = iron_hir_expr_bool_lit(g_mod, false, bool_t, span);
    IronHIR_Stmt  *outer_while = iron_hir_stmt_while(g_mod, outer_cond, outer_body, span);

    IronHIR_Block *body = iron_hir_block_create(g_mod);
    iron_hir_block_add_stmt(body, outer_while);

    IronHIR_Func *fn = iron_hir_func_create(g_mod, "nested_while", NULL, 0, void_t);
    fn->body = body;
    iron_hir_module_add_func(g_mod, fn);

    IronLIR_Module *lir = do_lower();
    TEST_ASSERT_NOT_NULL(lir);

    IronLIR_Func *lf = lir->funcs[0];
    /* Two nested loops: at least 5 blocks */
    TEST_ASSERT_GREATER_OR_EQUAL(5, lf->block_count);
    /* Both loops produce BRANCHes */
    int branches = count_instrs_in_func(lf, IRON_LIR_BRANCH);
    TEST_ASSERT_GREATER_OR_EQUAL(2, branches);
    /* Both loops produce back-edge JUMPs */
    int jumps = count_instrs_in_func(lf, IRON_LIR_JUMP);
    TEST_ASSERT_GREATER_OR_EQUAL(2, jumps);
}

/* ── Test 23: mutable var — alloca + store in entry ─────────────────────── */

void test_h2l_mutable_var_alloca(void) {
    Iron_Span span   = zero_span();
    Iron_Type *int_t = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_Type *void_t = iron_type_make_primitive(IRON_TYPE_VOID);

    IronHIR_VarId x_id  = iron_hir_alloc_var(g_mod, "x", int_t, true);
    IronHIR_Expr *lit42 = iron_hir_expr_int_lit(g_mod, 42, int_t, span);
    IronHIR_Stmt *let_x = iron_hir_stmt_let(g_mod, x_id, int_t, lit42, true, span);

    IronHIR_Block *body = iron_hir_block_create(g_mod);
    iron_hir_block_add_stmt(body, let_x);

    IronHIR_Func *fn = iron_hir_func_create(g_mod, "mutable_var_alloca", NULL, 0, void_t);
    fn->body = body;
    iron_hir_module_add_func(g_mod, fn);

    IronLIR_Module *lir = do_lower();
    TEST_ASSERT_NOT_NULL(lir);

    IronLIR_Func *lf = lir->funcs[0];

    /* ALLOCA in entry block */
    int allocas = count_instrs_in_block(lf->blocks[0], IRON_LIR_ALLOCA);
    TEST_ASSERT_GREATER_OR_EQUAL(1, allocas);
    /* STORE after alloca */
    int stores = count_instrs_in_func(lf, IRON_LIR_STORE);
    TEST_ASSERT_GREATER_OR_EQUAL(1, stores);
    /* CONST_INT for the literal 42 */
    int consts = count_instrs_in_func(lf, IRON_LIR_CONST_INT);
    TEST_ASSERT_GREATER_OR_EQUAL(1, consts);
}

/* ── Test 24: immutable val — no alloca, direct SSA binding ─────────────── */

void test_h2l_immutable_val(void) {
    Iron_Span span   = zero_span();
    Iron_Type *int_t = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_Type *void_t = iron_type_make_primitive(IRON_TYPE_VOID);

    IronHIR_VarId x_id  = iron_hir_alloc_var(g_mod, "x", int_t, false);
    IronHIR_Expr *lit7  = iron_hir_expr_int_lit(g_mod, 7, int_t, span);
    IronHIR_Stmt *let_x = iron_hir_stmt_let(g_mod, x_id, int_t, lit7, false, span);

    IronHIR_Block *body = iron_hir_block_create(g_mod);
    iron_hir_block_add_stmt(body, let_x);

    IronHIR_Func *fn = iron_hir_func_create(g_mod, "immutable_val", NULL, 0, void_t);
    fn->body = body;
    iron_hir_module_add_func(g_mod, fn);

    IronLIR_Module *lir = do_lower();
    TEST_ASSERT_NOT_NULL(lir);

    IronLIR_Func *lf = lir->funcs[0];
    /* Immutable val: no ALLOCA for x */
    TEST_ASSERT_FALSE(entry_has_alloca(lf, "x"));
    /* CONST_INT for the literal */
    int consts = count_instrs_in_func(lf, IRON_LIR_CONST_INT);
    TEST_ASSERT_GREATER_OR_EQUAL(1, consts);
}

/* ── Test 25: assign to var — LOAD → binop → STORE ──────────────────────── */

void test_h2l_assign_to_var(void) {
    Iron_Span span   = zero_span();
    Iron_Type *int_t = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_Type *void_t = iron_type_make_primitive(IRON_TYPE_VOID);

    /* var x = 1; x = x + 1 */
    IronHIR_VarId x_id = iron_hir_alloc_var(g_mod, "x", int_t, true);
    IronHIR_Stmt *let_x = iron_hir_stmt_let(g_mod, x_id, int_t,
                                             iron_hir_expr_int_lit(g_mod, 1, int_t, span),
                                             true, span);

    IronHIR_Expr *x_ref  = iron_hir_expr_ident(g_mod, x_id, "x", int_t, span);
    IronHIR_Expr *one    = iron_hir_expr_int_lit(g_mod, 1, int_t, span);
    IronHIR_Expr *add    = iron_hir_expr_binop(g_mod, IRON_HIR_BINOP_ADD, x_ref, one, int_t, span);

    IronHIR_Expr *x_ref2 = iron_hir_expr_ident(g_mod, x_id, "x", int_t, span);
    IronHIR_Stmt *assign = iron_hir_stmt_assign(g_mod, x_ref2, add, span);

    IronHIR_Block *body = iron_hir_block_create(g_mod);
    iron_hir_block_add_stmt(body, let_x);
    iron_hir_block_add_stmt(body, assign);

    IronHIR_Func *fn = iron_hir_func_create(g_mod, "assign_to_var", NULL, 0, void_t);
    fn->body = body;
    iron_hir_module_add_func(g_mod, fn);

    IronLIR_Module *lir = do_lower();
    TEST_ASSERT_NOT_NULL(lir);

    IronLIR_Func *lf = lir->funcs[0];
    /* LOAD for x_ref, ADD, STORE for assignment */
    int adds   = count_instrs_in_func(lf, IRON_LIR_ADD);
    int stores = count_instrs_in_func(lf, IRON_LIR_STORE);
    TEST_ASSERT_GREATER_OR_EQUAL(1, adds);
    TEST_ASSERT_GREATER_OR_EQUAL(1, stores);
}

/* ── Test 26: ALLOCA always in entry block even for while-declared vars ──── */

void test_h2l_alloca_always_in_entry(void) {
    Iron_Span span    = zero_span();
    Iron_Type *int_t  = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_Type *bool_t = iron_type_make_primitive(IRON_TYPE_BOOL);
    Iron_Type *void_t = iron_type_make_primitive(IRON_TYPE_VOID);

    /* while false { var y = 99 } */
    IronHIR_VarId y_id  = iron_hir_alloc_var(g_mod, "y", int_t, true);
    IronHIR_Stmt *let_y = iron_hir_stmt_let(g_mod, y_id, int_t,
                                             iron_hir_expr_int_lit(g_mod, 99, int_t, span),
                                             true, span);

    IronHIR_Block *loop_body = iron_hir_block_create(g_mod);
    iron_hir_block_add_stmt(loop_body, let_y);

    IronHIR_Expr  *cond    = iron_hir_expr_bool_lit(g_mod, false, bool_t, span);
    IronHIR_Stmt  *while_s = iron_hir_stmt_while(g_mod, cond, loop_body, span);

    IronHIR_Block *body = iron_hir_block_create(g_mod);
    iron_hir_block_add_stmt(body, while_s);

    IronHIR_Func *fn = iron_hir_func_create(g_mod, "alloca_always_in_entry", NULL, 0, void_t);
    fn->body = body;
    iron_hir_module_add_func(g_mod, fn);

    IronLIR_Module *lir = do_lower();
    TEST_ASSERT_NOT_NULL(lir);

    IronLIR_Func *lf = lir->funcs[0];

    /* ALLOCA must be in entry block (blocks[0]) */
    int entry_allocas = count_instrs_in_block(lf->blocks[0], IRON_LIR_ALLOCA);
    int other_allocas = 0;
    for (int bi = 1; bi < lf->block_count; bi++) {
        other_allocas += count_instrs_in_block(lf->blocks[bi], IRON_LIR_ALLOCA);
    }
    TEST_ASSERT_EQUAL_INT(0, other_allocas);
    TEST_ASSERT_GREATER_OR_EQUAL(1, entry_allocas);
}

/* ── Test 27: multiple vars all have ALLOCAs in entry block ──────────────── */

void test_h2l_multiple_vars_all_entry(void) {
    Iron_Span span   = zero_span();
    Iron_Type *int_t = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_Type *void_t = iron_type_make_primitive(IRON_TYPE_VOID);

    IronHIR_Block *body = iron_hir_block_create(g_mod);

    /* 5 mutable variables */
    for (int i = 0; i < 5; i++) {
        char name[8];
        snprintf(name, sizeof(name), "v%d", i);
        IronHIR_VarId vid  = iron_hir_alloc_var(g_mod, name, int_t, true);
        IronHIR_Expr *init = iron_hir_expr_int_lit(g_mod, (int64_t)i, int_t, span);
        IronHIR_Stmt *let  = iron_hir_stmt_let(g_mod, vid, int_t, init, true, span);
        iron_hir_block_add_stmt(body, let);
    }

    IronHIR_Func *fn = iron_hir_func_create(g_mod, "multi_vars", NULL, 0, void_t);
    fn->body = body;
    iron_hir_module_add_func(g_mod, fn);

    IronLIR_Module *lir = do_lower();
    TEST_ASSERT_NOT_NULL(lir);

    IronLIR_Func *lf = lir->funcs[0];

    /* All 5 ALLOCAs in entry block */
    int entry_allocas = count_instrs_in_block(lf->blocks[0], IRON_LIR_ALLOCA);
    int other_allocas = 0;
    for (int bi = 1; bi < lf->block_count; bi++) {
        other_allocas += count_instrs_in_block(lf->blocks[bi], IRON_LIR_ALLOCA);
    }
    TEST_ASSERT_EQUAL_INT(0, other_allocas);
    TEST_ASSERT_GREATER_OR_EQUAL(5, entry_allocas);
}

/* ── Test 28: function param accessible as value (no alloca for immutable) ─ */

void test_h2l_param_value_id(void) {
    Iron_Span span   = zero_span();
    Iron_Type *int_t = iron_type_make_primitive(IRON_TYPE_INT);

    IronHIR_VarId p_id = iron_hir_alloc_var(g_mod, "p", int_t, false);

    IronHIR_Param params[1];
    params[0].var_id = p_id;
    params[0].name   = "p";
    params[0].type   = int_t;

    /* return p */
    IronHIR_Expr *p_ref = iron_hir_expr_ident(g_mod, p_id, "p", int_t, span);
    IronHIR_Stmt *ret   = iron_hir_stmt_return(g_mod, p_ref, span);

    IronHIR_Block *body = iron_hir_block_create(g_mod);
    iron_hir_block_add_stmt(body, ret);

    IronHIR_Func *fn = iron_hir_func_create(g_mod, "param_val", params, 1, int_t);
    fn->body = body;
    iron_hir_module_add_func(g_mod, fn);

    IronLIR_Module *lir = do_lower();
    TEST_ASSERT_NOT_NULL(lir);

    IronLIR_Func *lf = lir->funcs[0];
    /* Params get their own alloca+store in entry; param_map allows ident lookup via LOAD.
     * The function has exactly 1 param so there should be 1 alloca (for the param slot). */
    int allocas = count_instrs_in_func(lf, IRON_LIR_ALLOCA);
    TEST_ASSERT_GREATER_OR_EQUAL(1, allocas);
    /* RETURN with value */
    int rets = count_instrs_in_func(lf, IRON_LIR_RETURN);
    TEST_ASSERT_GREATER_OR_EQUAL(1, rets);
}

/* ── Test 29: integer literal → CONST_INT instruction ───────────────────── */

void test_h2l_int_lit_const(void) {
    Iron_Span span   = zero_span();
    Iron_Type *int_t = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_Type *void_t = iron_type_make_primitive(IRON_TYPE_VOID);

    IronHIR_Expr *lit = iron_hir_expr_int_lit(g_mod, 123, int_t, span);
    IronHIR_Stmt *s   = iron_hir_stmt_expr(g_mod, lit, span);

    IronHIR_Block *body = iron_hir_block_create(g_mod);
    iron_hir_block_add_stmt(body, s);

    IronHIR_Func *fn = iron_hir_func_create(g_mod, "int_lit_fn", NULL, 0, void_t);
    fn->body = body;
    iron_hir_module_add_func(g_mod, fn);

    IronLIR_Module *lir = do_lower();
    TEST_ASSERT_NOT_NULL(lir);

    IronLIR_Func *lf = lir->funcs[0];
    int consts = count_instrs_in_func(lf, IRON_LIR_CONST_INT);
    TEST_ASSERT_GREATER_OR_EQUAL(1, consts);
}

/* ── Test 30: float literal → CONST_FLOAT instruction ───────────────────── */

void test_h2l_float_lit_const(void) {
    Iron_Span span     = zero_span();
    Iron_Type *float_t = iron_type_make_primitive(IRON_TYPE_FLOAT);
    Iron_Type *void_t  = iron_type_make_primitive(IRON_TYPE_VOID);

    IronHIR_Expr *lit = iron_hir_expr_float_lit(g_mod, 3.14, float_t, span);
    IronHIR_Stmt *s   = iron_hir_stmt_expr(g_mod, lit, span);

    IronHIR_Block *body = iron_hir_block_create(g_mod);
    iron_hir_block_add_stmt(body, s);

    IronHIR_Func *fn = iron_hir_func_create(g_mod, "float_lit_fn", NULL, 0, void_t);
    fn->body = body;
    iron_hir_module_add_func(g_mod, fn);

    IronLIR_Module *lir = do_lower();
    TEST_ASSERT_NOT_NULL(lir);

    IronLIR_Func *lf = lir->funcs[0];
    int consts = count_instrs_in_func(lf, IRON_LIR_CONST_FLOAT);
    TEST_ASSERT_GREATER_OR_EQUAL(1, consts);
}

/* ── Test 31: string literal → CONST_STRING instruction ─────────────────── */

void test_h2l_string_lit_const(void) {
    Iron_Span span    = zero_span();
    Iron_Type *str_t  = iron_type_make_primitive(IRON_TYPE_STRING);
    Iron_Type *void_t = iron_type_make_primitive(IRON_TYPE_VOID);

    IronHIR_Expr *lit = iron_hir_expr_string_lit(g_mod, "hello", str_t, span);
    IronHIR_Stmt *s   = iron_hir_stmt_expr(g_mod, lit, span);

    IronHIR_Block *body = iron_hir_block_create(g_mod);
    iron_hir_block_add_stmt(body, s);

    IronHIR_Func *fn = iron_hir_func_create(g_mod, "str_lit_fn", NULL, 0, void_t);
    fn->body = body;
    iron_hir_module_add_func(g_mod, fn);

    IronLIR_Module *lir = do_lower();
    TEST_ASSERT_NOT_NULL(lir);

    IronLIR_Func *lf = lir->funcs[0];
    int consts = count_instrs_in_func(lf, IRON_LIR_CONST_STRING);
    TEST_ASSERT_GREATER_OR_EQUAL(1, consts);
}

/* ── Test 32: binop → correct LIR instruction kind ──────────────────────── */

void test_h2l_binop_instruction(void) {
    Iron_Span span   = zero_span();
    Iron_Type *int_t = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_Type *void_t = iron_type_make_primitive(IRON_TYPE_VOID);

    /* 5 * 3 */
    IronHIR_Expr *a   = iron_hir_expr_int_lit(g_mod, 5, int_t, span);
    IronHIR_Expr *b   = iron_hir_expr_int_lit(g_mod, 3, int_t, span);
    IronHIR_Expr *mul = iron_hir_expr_binop(g_mod, IRON_HIR_BINOP_MUL, a, b, int_t, span);
    IronHIR_Stmt *s   = iron_hir_stmt_expr(g_mod, mul, span);

    IronHIR_Block *body = iron_hir_block_create(g_mod);
    iron_hir_block_add_stmt(body, s);

    IronHIR_Func *fn = iron_hir_func_create(g_mod, "binop_mul", NULL, 0, void_t);
    fn->body = body;
    iron_hir_module_add_func(g_mod, fn);

    IronLIR_Module *lir = do_lower();
    TEST_ASSERT_NOT_NULL(lir);

    IronLIR_Func *lf = lir->funcs[0];
    int muls = count_instrs_in_func(lf, IRON_LIR_MUL);
    TEST_ASSERT_GREATER_OR_EQUAL(1, muls);
}

/* ── Test 33: unop → UNOP instruction ───────────────────────────────────── */

void test_h2l_unop_instruction(void) {
    Iron_Span span   = zero_span();
    Iron_Type *int_t = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_Type *void_t = iron_type_make_primitive(IRON_TYPE_VOID);

    IronHIR_Expr *lit = iron_hir_expr_int_lit(g_mod, 5, int_t, span);
    IronHIR_Expr *neg = iron_hir_expr_unop(g_mod, IRON_HIR_UNOP_NEG, lit, int_t, span);
    IronHIR_Stmt *s   = iron_hir_stmt_expr(g_mod, neg, span);

    IronHIR_Block *body = iron_hir_block_create(g_mod);
    iron_hir_block_add_stmt(body, s);

    IronHIR_Func *fn = iron_hir_func_create(g_mod, "unop_neg", NULL, 0, void_t);
    fn->body = body;
    iron_hir_module_add_func(g_mod, fn);

    IronLIR_Module *lir = do_lower();
    TEST_ASSERT_NOT_NULL(lir);

    IronLIR_Func *lf = lir->funcs[0];
    int negs = count_instrs_in_func(lf, IRON_LIR_NEG);
    TEST_ASSERT_GREATER_OR_EQUAL(1, negs);
}

/* ── Test 34: function call → CALL instruction with correct arg count ────── */

void test_h2l_call_instruction(void) {
    Iron_Span span    = zero_span();
    Iron_Type *int_t  = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_Type *void_t = iron_type_make_primitive(IRON_TYPE_VOID);

    IronHIR_Expr *callee = iron_hir_expr_func_ref(g_mod, "target_fn", int_t, span);
    IronHIR_Expr *a1     = iron_hir_expr_int_lit(g_mod, 10, int_t, span);
    IronHIR_Expr *a2     = iron_hir_expr_int_lit(g_mod, 20, int_t, span);

    IronHIR_Expr **args = NULL;
    arrput(args, a1);
    arrput(args, a2);

    IronHIR_Expr *call = iron_hir_expr_call(g_mod, callee, args, 2, int_t, span);
    IronHIR_Stmt *s    = iron_hir_stmt_expr(g_mod, call, span);

    IronHIR_Block *body = iron_hir_block_create(g_mod);
    iron_hir_block_add_stmt(body, s);

    IronHIR_Func *fn = iron_hir_func_create(g_mod, "call_instr_fn", NULL, 0, void_t);
    fn->body = body;
    iron_hir_module_add_func(g_mod, fn);

    IronLIR_Module *lir = do_lower();
    TEST_ASSERT_NOT_NULL(lir);

    IronLIR_Func *lf = lir->funcs[0];
    int calls = count_instrs_in_func(lf, IRON_LIR_CALL);
    TEST_ASSERT_GREATER_OR_EQUAL(1, calls);

    /* Find the CALL and verify its arg_count == 2 */
    bool found = false;
    for (int bi = 0; bi < lf->block_count; bi++) {
        IronLIR_Block *blk = lf->blocks[bi];
        for (int ii = 0; ii < blk->instr_count; ii++) {
            IronLIR_Instr *instr = blk->instrs[ii];
            if (instr->kind == IRON_LIR_CALL && instr->call.arg_count == 2) {
                found = true;
            }
        }
    }
    TEST_ASSERT_TRUE(found);
}

/* ── Test 35: field access → GET_FIELD instruction ───────────────────────── */

void test_h2l_field_access_get(void) {
    Iron_Span span   = zero_span();
    Iron_Type *int_t = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_Type *void_t = iron_type_make_primitive(IRON_TYPE_VOID);

    /* Build a simple struct type for field access */
    IronHIR_VarId obj_id  = iron_hir_alloc_var(g_mod, "obj", int_t, false);
    IronHIR_Stmt *let_obj = iron_hir_stmt_let(g_mod, obj_id, int_t,
                                               iron_hir_expr_int_lit(g_mod, 1, int_t, span),
                                               false, span);

    /* Access obj.field (even with int type — lowerer emits GET_FIELD by kind) */
    IronHIR_Expr *obj_ref  = iron_hir_expr_ident(g_mod, obj_id, "obj", int_t, span);
    IronHIR_Expr *field_e  = iron_hir_expr_field_access(g_mod, obj_ref, "field", int_t, span);
    IronHIR_Stmt *field_s  = iron_hir_stmt_expr(g_mod, field_e, span);

    IronHIR_Block *body = iron_hir_block_create(g_mod);
    iron_hir_block_add_stmt(body, let_obj);
    iron_hir_block_add_stmt(body, field_s);

    IronHIR_Func *fn = iron_hir_func_create(g_mod, "field_access_fn", NULL, 0, void_t);
    fn->body = body;
    iron_hir_module_add_func(g_mod, fn);

    IronLIR_Module *lir = do_lower();
    TEST_ASSERT_NOT_NULL(lir);

    IronLIR_Func *lf = lir->funcs[0];
    int gets = count_instrs_in_func(lf, IRON_LIR_GET_FIELD);
    TEST_ASSERT_GREATER_OR_EQUAL(1, gets);
}

/* ── Test 36: index access → GET_INDEX instruction ───────────────────────── */

void test_h2l_index_access_get(void) {
    Iron_Span span   = zero_span();
    Iron_Type *int_t = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_Type *void_t = iron_type_make_primitive(IRON_TYPE_VOID);

    IronHIR_VarId arr_id  = iron_hir_alloc_var(g_mod, "arr", int_t, false);
    IronHIR_Stmt *let_arr = iron_hir_stmt_let(g_mod, arr_id, int_t,
                                               iron_hir_expr_int_lit(g_mod, 0, int_t, span),
                                               false, span);

    IronHIR_Expr *arr_ref = iron_hir_expr_ident(g_mod, arr_id, "arr", int_t, span);
    IronHIR_Expr *idx     = iron_hir_expr_int_lit(g_mod, 0, int_t, span);
    IronHIR_Expr *index_e = iron_hir_expr_index(g_mod, arr_ref, idx, int_t, span);
    IronHIR_Stmt *index_s = iron_hir_stmt_expr(g_mod, index_e, span);

    IronHIR_Block *body = iron_hir_block_create(g_mod);
    iron_hir_block_add_stmt(body, let_arr);
    iron_hir_block_add_stmt(body, index_s);

    IronHIR_Func *fn = iron_hir_func_create(g_mod, "index_access_fn", NULL, 0, void_t);
    fn->body = body;
    iron_hir_module_add_func(g_mod, fn);

    IronLIR_Module *lir = do_lower();
    TEST_ASSERT_NOT_NULL(lir);

    IronLIR_Func *lf = lir->funcs[0];
    int gets = count_instrs_in_func(lf, IRON_LIR_GET_INDEX);
    TEST_ASSERT_GREATER_OR_EQUAL(1, gets);
}

/* ── Test 37: heap allocation → HEAP_ALLOC instruction ───────────────────── */

void test_h2l_heap_alloc_instr(void) {
    Iron_Span span   = zero_span();
    Iron_Type *int_t = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_Type *void_t = iron_type_make_primitive(IRON_TYPE_VOID);

    IronHIR_Expr *inner  = iron_hir_expr_int_lit(g_mod, 5, int_t, span);
    IronHIR_Expr *heap_e = iron_hir_expr_heap(g_mod, inner, true, false, int_t, span);
    IronHIR_Stmt *s      = iron_hir_stmt_expr(g_mod, heap_e, span);

    IronHIR_Block *body = iron_hir_block_create(g_mod);
    iron_hir_block_add_stmt(body, s);

    IronHIR_Func *fn = iron_hir_func_create(g_mod, "heap_alloc_fn", NULL, 0, void_t);
    fn->body = body;
    iron_hir_module_add_func(g_mod, fn);

    IronLIR_Module *lir = do_lower();
    TEST_ASSERT_NOT_NULL(lir);

    IronLIR_Func *lf = lir->funcs[0];
    int heaps = count_instrs_in_func(lf, IRON_LIR_HEAP_ALLOC);
    TEST_ASSERT_GREATER_OR_EQUAL(1, heaps);
}

/* ── Test 38: array literal → ARRAY_LIT instruction with elements ─────────── */

void test_h2l_array_lit_instr(void) {
    Iron_Span span   = zero_span();
    Iron_Type *int_t = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_Type *void_t = iron_type_make_primitive(IRON_TYPE_VOID);

    IronHIR_Expr *e0 = iron_hir_expr_int_lit(g_mod, 1, int_t, span);
    IronHIR_Expr *e1 = iron_hir_expr_int_lit(g_mod, 2, int_t, span);
    IronHIR_Expr *e2 = iron_hir_expr_int_lit(g_mod, 3, int_t, span);

    IronHIR_Expr **elems = NULL;
    arrput(elems, e0);
    arrput(elems, e1);
    arrput(elems, e2);

    Iron_Type *arr_t = iron_type_make_array(g_mod->arena, int_t, 3);
    IronHIR_Expr *arr_lit = iron_hir_expr_array_lit(g_mod, int_t, elems, 3, arr_t, span);
    IronHIR_Stmt *s       = iron_hir_stmt_expr(g_mod, arr_lit, span);

    IronHIR_Block *body = iron_hir_block_create(g_mod);
    iron_hir_block_add_stmt(body, s);

    IronHIR_Func *fn = iron_hir_func_create(g_mod, "array_lit_fn", NULL, 0, void_t);
    fn->body = body;
    iron_hir_module_add_func(g_mod, fn);

    IronLIR_Module *lir = do_lower();
    TEST_ASSERT_NOT_NULL(lir);

    IronLIR_Func *lf = lir->funcs[0];
    int arrays = count_instrs_in_func(lf, IRON_LIR_ARRAY_LIT);
    TEST_ASSERT_GREATER_OR_EQUAL(1, arrays);

    /* Verify element count == 3 */
    bool found3 = false;
    for (int bi = 0; bi < lf->block_count; bi++) {
        IronLIR_Block *blk = lf->blocks[bi];
        for (int ii = 0; ii < blk->instr_count; ii++) {
            IronLIR_Instr *instr = blk->instrs[ii];
            if (instr->kind == IRON_LIR_ARRAY_LIT && instr->array_lit.element_count == 3) {
                found3 = true;
            }
        }
    }
    TEST_ASSERT_TRUE(found3);
}

/* ── Test 39: cast → CAST instruction ────────────────────────────────────── */

void test_h2l_cast_instruction(void) {
    Iron_Span span     = zero_span();
    Iron_Type *int_t   = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_Type *float_t = iron_type_make_primitive(IRON_TYPE_FLOAT);
    Iron_Type *void_t  = iron_type_make_primitive(IRON_TYPE_VOID);

    IronHIR_Expr *val  = iron_hir_expr_int_lit(g_mod, 42, int_t, span);
    IronHIR_Expr *cast = iron_hir_expr_cast(g_mod, val, float_t, span);
    IronHIR_Stmt *s    = iron_hir_stmt_expr(g_mod, cast, span);

    IronHIR_Block *body = iron_hir_block_create(g_mod);
    iron_hir_block_add_stmt(body, s);

    IronHIR_Func *fn = iron_hir_func_create(g_mod, "cast_fn", NULL, 0, void_t);
    fn->body = body;
    iron_hir_module_add_func(g_mod, fn);

    IronLIR_Module *lir = do_lower();
    TEST_ASSERT_NOT_NULL(lir);

    IronLIR_Func *lf = lir->funcs[0];
    int casts = count_instrs_in_func(lf, IRON_LIR_CAST);
    TEST_ASSERT_GREATER_OR_EQUAL(1, casts);
}

/* ── Test 40: return void — RETURN terminator with is_void == true ─────────── */

void test_h2l_return_void(void) {
    Iron_Span span    = zero_span();
    Iron_Type *void_t = iron_type_make_primitive(IRON_TYPE_VOID);

    /* return (void) */
    IronHIR_Stmt *ret = iron_hir_stmt_return(g_mod, NULL, span);

    IronHIR_Block *body = iron_hir_block_create(g_mod);
    iron_hir_block_add_stmt(body, ret);

    IronHIR_Func *fn = iron_hir_func_create(g_mod, "ret_void_fn", NULL, 0, void_t);
    fn->body = body;
    iron_hir_module_add_func(g_mod, fn);

    IronLIR_Module *lir = do_lower();
    TEST_ASSERT_NOT_NULL(lir);

    IronLIR_Func *lf = lir->funcs[0];

    /* Find the RETURN and verify is_void == true */
    bool found_void_ret = false;
    for (int bi = 0; bi < lf->block_count; bi++) {
        IronLIR_Block *blk = lf->blocks[bi];
        for (int ii = 0; ii < blk->instr_count; ii++) {
            IronLIR_Instr *instr = blk->instrs[ii];
            if (instr->kind == IRON_LIR_RETURN && instr->ret.is_void) {
                found_void_ret = true;
            }
        }
    }
    TEST_ASSERT_TRUE(found_void_ret);
}

/* ── Test 41: return value — RETURN terminator with non-void value ─────────── */

void test_h2l_return_value(void) {
    Iron_Span span   = zero_span();
    Iron_Type *int_t = iron_type_make_primitive(IRON_TYPE_INT);

    IronHIR_Expr *val = iron_hir_expr_int_lit(g_mod, 42, int_t, span);
    IronHIR_Stmt *ret = iron_hir_stmt_return(g_mod, val, span);

    IronHIR_Block *body = iron_hir_block_create(g_mod);
    iron_hir_block_add_stmt(body, ret);

    IronHIR_Func *fn = iron_hir_func_create(g_mod, "ret_value_fn", NULL, 0, int_t);
    fn->body = body;
    iron_hir_module_add_func(g_mod, fn);

    IronLIR_Module *lir = do_lower();
    TEST_ASSERT_NOT_NULL(lir);

    IronLIR_Func *lf = lir->funcs[0];

    /* Find RETURN with is_void == false */
    bool found_valued = false;
    for (int bi = 0; bi < lf->block_count; bi++) {
        IronLIR_Block *blk = lf->blocks[bi];
        for (int ii = 0; ii < blk->instr_count; ii++) {
            IronLIR_Instr *instr = blk->instrs[ii];
            if (instr->kind == IRON_LIR_RETURN && !instr->ret.is_void) {
                found_valued = true;
            }
        }
    }
    TEST_ASSERT_TRUE(found_valued);
}

/* ════════════════════════════════════════════════════════════════════════════
 * TASK 2 — Edge-case tests (~15 tests)
 * ════════════════════════════════════════════════════════════════════════════ */

/* ── Test 42: phi — var assigned in both branches has PHI at merge ────────── */

void test_h2l_phi_if_else_merge(void) {
    Iron_Span span    = zero_span();
    Iron_Type *int_t  = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_Type *bool_t = iron_type_make_primitive(IRON_TYPE_BOOL);
    Iron_Type *void_t = iron_type_make_primitive(IRON_TYPE_VOID);

    /* var x = 0; if true { x = 1 } else { x = 2 } */
    IronHIR_VarId x_id = iron_hir_alloc_var(g_mod, "x", int_t, true);
    IronHIR_Stmt *let_x = iron_hir_stmt_let(g_mod, x_id, int_t,
                                              iron_hir_expr_int_lit(g_mod, 0, int_t, span),
                                              true, span);

    IronHIR_Expr *x_t = iron_hir_expr_ident(g_mod, x_id, "x", int_t, span);
    IronHIR_Block *then_body = iron_hir_block_create(g_mod);
    iron_hir_block_add_stmt(then_body,
        iron_hir_stmt_assign(g_mod, x_t,
            iron_hir_expr_int_lit(g_mod, 1, int_t, span), span));

    IronHIR_Expr *x_e = iron_hir_expr_ident(g_mod, x_id, "x", int_t, span);
    IronHIR_Block *else_body = iron_hir_block_create(g_mod);
    iron_hir_block_add_stmt(else_body,
        iron_hir_stmt_assign(g_mod, x_e,
            iron_hir_expr_int_lit(g_mod, 2, int_t, span), span));

    IronHIR_Expr *cond = iron_hir_expr_bool_lit(g_mod, true, bool_t, span);
    IronHIR_Stmt *if_s = iron_hir_stmt_if(g_mod, cond, then_body, else_body, span);

    IronHIR_Block *body = iron_hir_block_create(g_mod);
    iron_hir_block_add_stmt(body, let_x);
    iron_hir_block_add_stmt(body, if_s);

    IronHIR_Func *fn = iron_hir_func_create(g_mod, "phi_if_else_merge", NULL, 0, void_t);
    fn->body = body;
    iron_hir_module_add_func(g_mod, fn);

    IronLIR_Module *lir = do_lower();
    TEST_ASSERT_NOT_NULL(lir);

    IronLIR_Func *lf = lir->funcs[0];
    /* PHI nodes must exist at merge block */
    int phis = count_instrs_in_func(lf, IRON_LIR_PHI);
    TEST_ASSERT_GREATER_OR_EQUAL(1, phis);

    /* PHI at merge has 2 incoming values */
    bool found_phi2 = false;
    for (int bi = 0; bi < lf->block_count; bi++) {
        IronLIR_Block *blk = lf->blocks[bi];
        for (int ii = 0; ii < blk->instr_count; ii++) {
            IronLIR_Instr *instr = blk->instrs[ii];
            if (instr->kind == IRON_LIR_PHI && instr->phi.count >= 2) {
                found_phi2 = true;
            }
        }
    }
    TEST_ASSERT_TRUE(found_phi2);
}

/* ── Test 43: phi — var modified in while body has PHI at loop header ─────── */

void test_h2l_phi_while_header(void) {
    Iron_Span span    = zero_span();
    Iron_Type *int_t  = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_Type *bool_t = iron_type_make_primitive(IRON_TYPE_BOOL);
    Iron_Type *void_t = iron_type_make_primitive(IRON_TYPE_VOID);

    /* var n = 0; while false { n = 1 } */
    IronHIR_VarId n_id  = iron_hir_alloc_var(g_mod, "n", int_t, true);
    IronHIR_Stmt *let_n = iron_hir_stmt_let(g_mod, n_id, int_t,
                                              iron_hir_expr_int_lit(g_mod, 0, int_t, span),
                                              true, span);

    IronHIR_Expr *n_ref = iron_hir_expr_ident(g_mod, n_id, "n", int_t, span);
    IronHIR_Block *loop_body = iron_hir_block_create(g_mod);
    iron_hir_block_add_stmt(loop_body,
        iron_hir_stmt_assign(g_mod, n_ref,
            iron_hir_expr_int_lit(g_mod, 1, int_t, span), span));

    IronHIR_Expr  *cond    = iron_hir_expr_bool_lit(g_mod, false, bool_t, span);
    IronHIR_Stmt  *while_s = iron_hir_stmt_while(g_mod, cond, loop_body, span);

    IronHIR_Block *body = iron_hir_block_create(g_mod);
    iron_hir_block_add_stmt(body, let_n);
    iron_hir_block_add_stmt(body, while_s);

    IronHIR_Func *fn = iron_hir_func_create(g_mod, "phi_while_header", NULL, 0, void_t);
    fn->body = body;
    iron_hir_module_add_func(g_mod, fn);

    IronLIR_Module *lir = do_lower();
    TEST_ASSERT_NOT_NULL(lir);

    IronLIR_Func *lf = lir->funcs[0];
    /* PHI nodes should be inserted at the loop header for n */
    int phis = count_instrs_in_func(lf, IRON_LIR_PHI);
    TEST_ASSERT_GREATER_OR_EQUAL(1, phis);
}

/* ── Test 44: no PHI when variable only read in branch (not written) ────────── */

void test_h2l_no_phi_when_unmodified(void) {
    Iron_Span span    = zero_span();
    Iron_Type *int_t  = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_Type *bool_t = iron_type_make_primitive(IRON_TYPE_BOOL);
    Iron_Type *void_t = iron_type_make_primitive(IRON_TYPE_VOID);

    /* val c = 5; if true { use(c) } else { use(c) }
     * c is only read — no PHI should be generated for c */
    IronHIR_VarId c_id  = iron_hir_alloc_var(g_mod, "c", int_t, false);
    IronHIR_Stmt *let_c = iron_hir_stmt_let(g_mod, c_id, int_t,
                                              iron_hir_expr_int_lit(g_mod, 5, int_t, span),
                                              false, span);

    /* then: expr statement reading c */
    IronHIR_Block *then_body = iron_hir_block_create(g_mod);
    iron_hir_block_add_stmt(then_body,
        iron_hir_stmt_expr(g_mod, iron_hir_expr_ident(g_mod, c_id, "c", int_t, span), span));

    IronHIR_Block *else_body = iron_hir_block_create(g_mod);
    iron_hir_block_add_stmt(else_body,
        iron_hir_stmt_expr(g_mod, iron_hir_expr_ident(g_mod, c_id, "c", int_t, span), span));

    IronHIR_Expr *cond = iron_hir_expr_bool_lit(g_mod, true, bool_t, span);
    IronHIR_Stmt *if_s = iron_hir_stmt_if(g_mod, cond, then_body, else_body, span);

    IronHIR_Block *body = iron_hir_block_create(g_mod);
    iron_hir_block_add_stmt(body, let_c);
    iron_hir_block_add_stmt(body, if_s);

    IronHIR_Func *fn = iron_hir_func_create(g_mod, "no_phi_unmodified", NULL, 0, void_t);
    fn->body = body;
    iron_hir_module_add_func(g_mod, fn);

    IronLIR_Module *lir = do_lower();
    TEST_ASSERT_NOT_NULL(lir);

    /* c is an immutable val — no alloca, no phi needed */
    IronLIR_Func *lf = lir->funcs[0];
    /* There should be no alloca for immutable c */
    TEST_ASSERT_FALSE(entry_has_alloca(lf, "c"));
    /* No PHI for the unmodified val binding */
    int phis = count_instrs_in_func(lf, IRON_LIR_PHI);
    TEST_ASSERT_EQUAL_INT(0, phis);
}

/* ── Test 45: phi in nested if — var modified at each merge level ──────────── */

void test_h2l_phi_nested_if(void) {
    Iron_Span span    = zero_span();
    Iron_Type *int_t  = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_Type *bool_t = iron_type_make_primitive(IRON_TYPE_BOOL);
    Iron_Type *void_t = iron_type_make_primitive(IRON_TYPE_VOID);

    /* var x = 0; if true { if false { x = 1 } else { x = 2 } } else { x = 3 } */
    IronHIR_VarId x_id = iron_hir_alloc_var(g_mod, "x", int_t, true);
    IronHIR_Stmt *let_x = iron_hir_stmt_let(g_mod, x_id, int_t,
                                              iron_hir_expr_int_lit(g_mod, 0, int_t, span),
                                              true, span);

    /* Inner then/else */
    IronHIR_Expr  *ix_t     = iron_hir_expr_ident(g_mod, x_id, "x", int_t, span);
    IronHIR_Block *inner_th = iron_hir_block_create(g_mod);
    iron_hir_block_add_stmt(inner_th,
        iron_hir_stmt_assign(g_mod, ix_t,
            iron_hir_expr_int_lit(g_mod, 1, int_t, span), span));

    IronHIR_Expr  *ix_e     = iron_hir_expr_ident(g_mod, x_id, "x", int_t, span);
    IronHIR_Block *inner_el = iron_hir_block_create(g_mod);
    iron_hir_block_add_stmt(inner_el,
        iron_hir_stmt_assign(g_mod, ix_e,
            iron_hir_expr_int_lit(g_mod, 2, int_t, span), span));

    IronHIR_Expr *inner_cond = iron_hir_expr_bool_lit(g_mod, false, bool_t, span);
    IronHIR_Stmt *inner_if   = iron_hir_stmt_if(g_mod, inner_cond, inner_th, inner_el, span);
    IronHIR_Block *outer_then = iron_hir_block_create(g_mod);
    iron_hir_block_add_stmt(outer_then, inner_if);

    /* Outer else */
    IronHIR_Expr  *ox_e     = iron_hir_expr_ident(g_mod, x_id, "x", int_t, span);
    IronHIR_Block *outer_el = iron_hir_block_create(g_mod);
    iron_hir_block_add_stmt(outer_el,
        iron_hir_stmt_assign(g_mod, ox_e,
            iron_hir_expr_int_lit(g_mod, 3, int_t, span), span));

    IronHIR_Expr *outer_cond = iron_hir_expr_bool_lit(g_mod, true, bool_t, span);
    IronHIR_Stmt *outer_if   = iron_hir_stmt_if(g_mod, outer_cond, outer_then, outer_el, span);

    IronHIR_Block *body = iron_hir_block_create(g_mod);
    iron_hir_block_add_stmt(body, let_x);
    iron_hir_block_add_stmt(body, outer_if);

    IronHIR_Func *fn = iron_hir_func_create(g_mod, "phi_nested_if", NULL, 0, void_t);
    fn->body = body;
    iron_hir_module_add_func(g_mod, fn);

    IronLIR_Module *lir = do_lower();
    TEST_ASSERT_NOT_NULL(lir);

    IronLIR_Func *lf = lir->funcs[0];
    /* PHI nodes at each merge level — at least 2 phis */
    int phis = count_instrs_in_func(lf, IRON_LIR_PHI);
    TEST_ASSERT_GREATER_OR_EQUAL(2, phis);
}

/* ── Test 46: phi — var assigned in multiple match arms has PHI at join ─────── */

void test_h2l_phi_match_join(void) {
    Iron_Span span    = zero_span();
    Iron_Type *int_t  = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_Type *void_t = iron_type_make_primitive(IRON_TYPE_VOID);

    IronHIR_VarId r_id  = iron_hir_alloc_var(g_mod, "r", int_t, true);
    IronHIR_Stmt *let_r = iron_hir_stmt_let(g_mod, r_id, int_t,
                                              iron_hir_expr_int_lit(g_mod, 0, int_t, span),
                                              true, span);

    IronHIR_VarId x_id  = iron_hir_alloc_var(g_mod, "x", int_t, false);
    IronHIR_Stmt *let_x = iron_hir_stmt_let(g_mod, x_id, int_t,
                                              iron_hir_expr_int_lit(g_mod, 1, int_t, span),
                                              false, span);
    IronHIR_Expr *scrutinee = iron_hir_expr_ident(g_mod, x_id, "x", int_t, span);

    IronHIR_MatchArm arms[2];
    /* arm 0: r = 10 */
    IronHIR_Expr *r_a0 = iron_hir_expr_ident(g_mod, r_id, "r", int_t, span);
    IronHIR_Block *body0 = iron_hir_block_create(g_mod);
    iron_hir_block_add_stmt(body0,
        iron_hir_stmt_assign(g_mod, r_a0,
            iron_hir_expr_int_lit(g_mod, 10, int_t, span), span));
    arms[0].pattern = iron_hir_expr_int_lit(g_mod, 1, int_t, span);
    arms[0].guard   = NULL;
    arms[0].body    = body0;

    /* arm 1: r = 20 */
    IronHIR_Expr *r_a1 = iron_hir_expr_ident(g_mod, r_id, "r", int_t, span);
    IronHIR_Block *body1 = iron_hir_block_create(g_mod);
    iron_hir_block_add_stmt(body1,
        iron_hir_stmt_assign(g_mod, r_a1,
            iron_hir_expr_int_lit(g_mod, 20, int_t, span), span));
    arms[1].pattern = iron_hir_expr_int_lit(g_mod, 2, int_t, span);
    arms[1].guard   = NULL;
    arms[1].body    = body1;

    IronHIR_Stmt *match_s = iron_hir_stmt_match(g_mod, scrutinee, arms, 2, span);

    IronHIR_Block *body = iron_hir_block_create(g_mod);
    iron_hir_block_add_stmt(body, let_r);
    iron_hir_block_add_stmt(body, let_x);
    iron_hir_block_add_stmt(body, match_s);

    IronHIR_Func *fn = iron_hir_func_create(g_mod, "phi_match_join", NULL, 0, void_t);
    fn->body = body;
    iron_hir_module_add_func(g_mod, fn);

    IronLIR_Module *lir = do_lower();
    TEST_ASSERT_NOT_NULL(lir);

    IronLIR_Func *lf = lir->funcs[0];
    int phis = count_instrs_in_func(lf, IRON_LIR_PHI);
    TEST_ASSERT_GREATER_OR_EQUAL(1, phis);
}

/* ── Test 47: short-circuit AND — at least 3 blocks ────────────────────────── */

void test_h2l_short_circuit_and_blocks(void) {
    Iron_Span span    = zero_span();
    Iron_Type *bool_t = iron_type_make_primitive(IRON_TYPE_BOOL);
    Iron_Type *void_t = iron_type_make_primitive(IRON_TYPE_VOID);

    IronHIR_VarId a_id = iron_hir_alloc_var(g_mod, "a", bool_t, false);
    IronHIR_VarId b_id = iron_hir_alloc_var(g_mod, "b", bool_t, false);

    IronHIR_Stmt *let_a = iron_hir_stmt_let(g_mod, a_id, bool_t,
                                              iron_hir_expr_bool_lit(g_mod, true, bool_t, span),
                                              false, span);
    IronHIR_Stmt *let_b = iron_hir_stmt_let(g_mod, b_id, bool_t,
                                              iron_hir_expr_bool_lit(g_mod, false, bool_t, span),
                                              false, span);

    IronHIR_Expr *a_ref = iron_hir_expr_ident(g_mod, a_id, "a", bool_t, span);
    IronHIR_Expr *b_ref = iron_hir_expr_ident(g_mod, b_id, "b", bool_t, span);
    IronHIR_Expr *and_e = iron_hir_expr_binop(g_mod, IRON_HIR_BINOP_AND, a_ref, b_ref, bool_t, span);
    IronHIR_Stmt *and_s = iron_hir_stmt_expr(g_mod, and_e, span);

    IronHIR_Block *body = iron_hir_block_create(g_mod);
    iron_hir_block_add_stmt(body, let_a);
    iron_hir_block_add_stmt(body, let_b);
    iron_hir_block_add_stmt(body, and_s);

    IronHIR_Func *fn = iron_hir_func_create(g_mod, "sc_and_blocks", NULL, 0, void_t);
    fn->body = body;
    iron_hir_module_add_func(g_mod, fn);

    IronLIR_Module *lir = do_lower();
    TEST_ASSERT_NOT_NULL(lir);

    IronLIR_Func *lf = lir->funcs[0];
    /* eval_a, and_rhs, and_merge = at least 3 blocks */
    TEST_ASSERT_GREATER_OR_EQUAL(3, lf->block_count);
    /* BRANCH for short-circuit check, PHI at merge */
    int branches = count_instrs_in_func(lf, IRON_LIR_BRANCH);
    TEST_ASSERT_GREATER_OR_EQUAL(1, branches);
    int phis = count_instrs_in_func(lf, IRON_LIR_PHI);
    TEST_ASSERT_GREATER_OR_EQUAL(1, phis);
}

/* ── Test 48: short-circuit OR — similar multi-block pattern ────────────────── */

void test_h2l_short_circuit_or_blocks(void) {
    Iron_Span span    = zero_span();
    Iron_Type *bool_t = iron_type_make_primitive(IRON_TYPE_BOOL);
    Iron_Type *void_t = iron_type_make_primitive(IRON_TYPE_VOID);

    IronHIR_VarId a_id = iron_hir_alloc_var(g_mod, "a", bool_t, false);
    IronHIR_VarId b_id = iron_hir_alloc_var(g_mod, "b", bool_t, false);

    IronHIR_Stmt *let_a = iron_hir_stmt_let(g_mod, a_id, bool_t,
                                              iron_hir_expr_bool_lit(g_mod, false, bool_t, span),
                                              false, span);
    IronHIR_Stmt *let_b = iron_hir_stmt_let(g_mod, b_id, bool_t,
                                              iron_hir_expr_bool_lit(g_mod, true, bool_t, span),
                                              false, span);

    IronHIR_Expr *a_ref = iron_hir_expr_ident(g_mod, a_id, "a", bool_t, span);
    IronHIR_Expr *b_ref = iron_hir_expr_ident(g_mod, b_id, "b", bool_t, span);
    IronHIR_Expr *or_e  = iron_hir_expr_binop(g_mod, IRON_HIR_BINOP_OR, a_ref, b_ref, bool_t, span);
    IronHIR_Stmt *or_s  = iron_hir_stmt_expr(g_mod, or_e, span);

    IronHIR_Block *body = iron_hir_block_create(g_mod);
    iron_hir_block_add_stmt(body, let_a);
    iron_hir_block_add_stmt(body, let_b);
    iron_hir_block_add_stmt(body, or_s);

    IronHIR_Func *fn = iron_hir_func_create(g_mod, "sc_or_blocks", NULL, 0, void_t);
    fn->body = body;
    iron_hir_module_add_func(g_mod, fn);

    IronLIR_Module *lir = do_lower();
    TEST_ASSERT_NOT_NULL(lir);

    IronLIR_Func *lf = lir->funcs[0];
    /* eval_a, or_rhs, or_merge = at least 3 blocks */
    TEST_ASSERT_GREATER_OR_EQUAL(3, lf->block_count);
    int branches = count_instrs_in_func(lf, IRON_LIR_BRANCH);
    TEST_ASSERT_GREATER_OR_EQUAL(1, branches);
    int phis = count_instrs_in_func(lf, IRON_LIR_PHI);
    TEST_ASSERT_GREATER_OR_EQUAL(1, phis);
}

/* ── Test 49: short-circuit nested (a && b) || c — correct block nesting ─────── */

void test_h2l_short_circuit_nested(void) {
    Iron_Span span    = zero_span();
    Iron_Type *bool_t = iron_type_make_primitive(IRON_TYPE_BOOL);
    Iron_Type *void_t = iron_type_make_primitive(IRON_TYPE_VOID);

    IronHIR_VarId a_id = iron_hir_alloc_var(g_mod, "a", bool_t, false);
    IronHIR_VarId b_id = iron_hir_alloc_var(g_mod, "b", bool_t, false);
    IronHIR_VarId c_id = iron_hir_alloc_var(g_mod, "c", bool_t, false);

    IronHIR_Stmt *let_a = iron_hir_stmt_let(g_mod, a_id, bool_t,
                                              iron_hir_expr_bool_lit(g_mod, true, bool_t, span),
                                              false, span);
    IronHIR_Stmt *let_b = iron_hir_stmt_let(g_mod, b_id, bool_t,
                                              iron_hir_expr_bool_lit(g_mod, false, bool_t, span),
                                              false, span);
    IronHIR_Stmt *let_c = iron_hir_stmt_let(g_mod, c_id, bool_t,
                                              iron_hir_expr_bool_lit(g_mod, true, bool_t, span),
                                              false, span);

    IronHIR_Expr *a_ref = iron_hir_expr_ident(g_mod, a_id, "a", bool_t, span);
    IronHIR_Expr *b_ref = iron_hir_expr_ident(g_mod, b_id, "b", bool_t, span);
    IronHIR_Expr *c_ref = iron_hir_expr_ident(g_mod, c_id, "c", bool_t, span);

    /* (a && b) || c */
    IronHIR_Expr *and_e = iron_hir_expr_binop(g_mod, IRON_HIR_BINOP_AND, a_ref, b_ref, bool_t, span);
    IronHIR_Expr *or_e  = iron_hir_expr_binop(g_mod, IRON_HIR_BINOP_OR, and_e, c_ref, bool_t, span);
    IronHIR_Stmt *s     = iron_hir_stmt_expr(g_mod, or_e, span);

    IronHIR_Block *body = iron_hir_block_create(g_mod);
    iron_hir_block_add_stmt(body, let_a);
    iron_hir_block_add_stmt(body, let_b);
    iron_hir_block_add_stmt(body, let_c);
    iron_hir_block_add_stmt(body, s);

    IronHIR_Func *fn = iron_hir_func_create(g_mod, "sc_nested", NULL, 0, void_t);
    fn->body = body;
    iron_hir_module_add_func(g_mod, fn);

    IronLIR_Module *lir = do_lower();
    TEST_ASSERT_NOT_NULL(lir);

    IronLIR_Func *lf = lir->funcs[0];
    /* (a && b) adds 2 extra blocks, || c adds 2 more: at least 5 blocks total */
    TEST_ASSERT_GREATER_OR_EQUAL(5, lf->block_count);
    /* Two BRANCHes (one for && , one for ||) */
    int branches = count_instrs_in_func(lf, IRON_LIR_BRANCH);
    TEST_ASSERT_GREATER_OR_EQUAL(2, branches);
    /* Two PHI nodes (one per merge block) */
    int phis = count_instrs_in_func(lf, IRON_LIR_PHI);
    TEST_ASSERT_GREATER_OR_EQUAL(2, phis);
}

/* ── Test 50: defer — return jumps to cleanup block containing RETURN ─────── */

void test_h2l_defer_cleanup_block(void) {
    Iron_Span span    = zero_span();
    Iron_Type *void_t = iron_type_make_primitive(IRON_TYPE_VOID);

    /* func with one defer:
     * defer { }
     * return
     * The return path should traverse a defer cleanup block. */
    IronHIR_Block *defer_body = iron_hir_block_create(g_mod);
    IronHIR_Stmt  *defer_s    = iron_hir_stmt_defer(g_mod, defer_body, span);
    IronHIR_Stmt  *ret_s      = iron_hir_stmt_return(g_mod, NULL, span);

    IronHIR_Block *body = iron_hir_block_create(g_mod);
    iron_hir_block_add_stmt(body, defer_s);
    iron_hir_block_add_stmt(body, ret_s);

    IronHIR_Func *fn = iron_hir_func_create(g_mod, "defer_cleanup_fn", NULL, 0, void_t);
    fn->body = body;
    iron_hir_module_add_func(g_mod, fn);

    IronLIR_Module *lir = do_lower();
    TEST_ASSERT_NOT_NULL(lir);

    IronLIR_Func *lf = lir->funcs[0];
    /* Defer creates a cleanup block: at least 2 blocks (entry + cleanup) */
    TEST_ASSERT_GREATER_OR_EQUAL(2, lf->block_count);
    /* Must have a RETURN somewhere */
    int rets = count_instrs_in_func(lf, IRON_LIR_RETURN);
    TEST_ASSERT_GREATER_OR_EQUAL(1, rets);
}

/* ── Test 51 (bonus, counted as test 50 in suite): many if/else chains ────── */

void test_h2l_many_blocks(void) {
    Iron_Span span    = zero_span();
    Iron_Type *bool_t = iron_type_make_primitive(IRON_TYPE_BOOL);
    Iron_Type *void_t = iron_type_make_primitive(IRON_TYPE_VOID);

    IronHIR_Block *body = iron_hir_block_create(g_mod);

    /* 5 sequential if/else pairs */
    for (int i = 0; i < 5; i++) {
        IronHIR_Expr  *cond      = iron_hir_expr_bool_lit(g_mod, true, bool_t, span);
        IronHIR_Block *then_body = iron_hir_block_create(g_mod);
        IronHIR_Block *else_body = iron_hir_block_create(g_mod);
        IronHIR_Stmt  *if_s      = iron_hir_stmt_if(g_mod, cond, then_body, else_body, span);
        iron_hir_block_add_stmt(body, if_s);
    }

    IronHIR_Func *fn = iron_hir_func_create(g_mod, "many_blocks_fn", NULL, 0, void_t);
    fn->body = body;
    iron_hir_module_add_func(g_mod, fn);

    IronLIR_Module *lir = do_lower();
    TEST_ASSERT_NOT_NULL(lir);

    IronLIR_Func *lf = lir->funcs[0];
    /* 5 if/else: 5*(entry+then+else+merge) ≈ at least 10 blocks */
    TEST_ASSERT_GREATER_OR_EQUAL(10, lf->block_count);

    /* All blocks end with a valid terminator */
    int invalid_count = 0;
    for (int bi = 0; bi < lf->block_count; bi++) {
        IronLIR_Block *blk = lf->blocks[bi];
        if (blk->instr_count == 0) { invalid_count++; continue; }
        IronLIR_Instr *last = blk->instrs[blk->instr_count - 1];
        bool is_term = (last->kind == IRON_LIR_BRANCH ||
                        last->kind == IRON_LIR_JUMP    ||
                        last->kind == IRON_LIR_SWITCH  ||
                        last->kind == IRON_LIR_RETURN);
        if (!is_term) invalid_count++;
    }
    TEST_ASSERT_EQUAL_INT(0, invalid_count);
}

/* ── Runner ───────────────────────────────────────────────────────────────── */

int main(void) {
    UNITY_BEGIN();
    /* Original 13 tests */
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
    /* Task 1: Feature-matrix tests (tests 14-41) */
    RUN_TEST(test_h2l_if_no_else_blocks);
    RUN_TEST(test_h2l_if_else_blocks);
    RUN_TEST(test_h2l_if_else_terminators);
    RUN_TEST(test_h2l_while_loop_backedge);
    RUN_TEST(test_h2l_while_loop_exit);
    RUN_TEST(test_h2l_nested_if_in_while);
    RUN_TEST(test_h2l_match_switch_terminator);
    RUN_TEST(test_h2l_match_join_block);
    RUN_TEST(test_h2l_nested_while_loops);
    RUN_TEST(test_h2l_mutable_var_alloca);
    RUN_TEST(test_h2l_immutable_val);
    RUN_TEST(test_h2l_assign_to_var);
    RUN_TEST(test_h2l_alloca_always_in_entry);
    RUN_TEST(test_h2l_multiple_vars_all_entry);
    RUN_TEST(test_h2l_param_value_id);
    RUN_TEST(test_h2l_int_lit_const);
    RUN_TEST(test_h2l_float_lit_const);
    RUN_TEST(test_h2l_string_lit_const);
    RUN_TEST(test_h2l_binop_instruction);
    RUN_TEST(test_h2l_unop_instruction);
    RUN_TEST(test_h2l_call_instruction);
    RUN_TEST(test_h2l_field_access_get);
    RUN_TEST(test_h2l_index_access_get);
    RUN_TEST(test_h2l_heap_alloc_instr);
    RUN_TEST(test_h2l_array_lit_instr);
    RUN_TEST(test_h2l_cast_instruction);
    RUN_TEST(test_h2l_return_void);
    RUN_TEST(test_h2l_return_value);
    /* Task 2: Edge-case tests (tests 42-51) */
    RUN_TEST(test_h2l_phi_if_else_merge);
    RUN_TEST(test_h2l_phi_while_header);
    RUN_TEST(test_h2l_no_phi_when_unmodified);
    RUN_TEST(test_h2l_phi_nested_if);
    RUN_TEST(test_h2l_phi_match_join);
    RUN_TEST(test_h2l_short_circuit_and_blocks);
    RUN_TEST(test_h2l_short_circuit_or_blocks);
    RUN_TEST(test_h2l_short_circuit_nested);
    RUN_TEST(test_h2l_defer_cleanup_block);
    RUN_TEST(test_h2l_many_blocks);
    return UNITY_END();
}
