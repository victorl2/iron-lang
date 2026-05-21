/* test_rc_pair_elim.c — Phase 29 OPT-01 RED unit harness.
 *
 * Asserts the deterministic, count-based oracle for the matched redundant
 * retain/release pair elimination pass: run_rc_pair_elimination() fills an
 * IronLIR_ElisionStat whose pairs_eliminated count tests assert EXACTLY.
 *
 *   Test A — clean local pair (retain v … release v, no barrier)  → 1
 *   Test B — release in a join block not dominated on all paths   → 0
 *   Test C — barrier (SPAWN) between retain and release           → 0
 *   Test D — post-elimination iron_lir_verify reports no 300-307
 *   Gate   — the pass is a pure function: it mutates only when called, so
 *            "not invoked" (the -O0/debug gate, enforced in Plan 03/build.c)
 *            leaves rc ops intact / count 0. Modelled here as: a NULL stat
 *            is never written when the pass is not run.
 *
 * INTENDED RED STATE (Wave 0 / TDD): run_rc_pair_elimination is DECLARED
 * (lir_optimize.h) but NOT DEFINED until Plan 03 (src/lir/lir_optimize.c).
 * This executable therefore FAILS TO LINK against iron_compiler today
 * (unresolved symbol). That is the correct RED — do NOT stub the function.
 *
 * Oracle discipline: TEST_ASSERT_EQUAL_INT on counts only; never any
 * wall-time-based assertion.
 */

#include "unity.h"
#include "lir/lir.h"
#include "lir/lir_optimize.h"
#include "lir/verify.h"
#include "analyzer/types.h"
#include "diagnostics/diagnostics.h"
#include "util/arena.h"

#include <string.h>
#include <stdbool.h>

static Iron_Arena g_arena;

void setUp(void) {
    g_arena = iron_arena_create(131072);
    iron_types_init(&g_arena);
}

void tearDown(void) {
    iron_arena_free(&g_arena);
}

static Iron_Span sp(void) { return iron_span_make("test.iron", 1, 1, 1, 1); }

static int count_kind_in_block(IronLIR_Block *blk, IronLIR_InstrKind kind) {
    int n = 0;
    for (int i = 0; i < blk->instr_count; i++) {
        if (blk->instrs[i]->kind == kind) n++;
    }
    return n;
}

/* Build an rc-typed value source we can retain/release. We use an RC_ALLOC of
 * an int so the target is a genuine rc value the pass can match on. */

/* ── Test A: clean local pair eliminated → pairs_eliminated == 1 ─────────── */

void test_clean_local_pair_eliminated(void) {
    IronLIR_Module *mod = iron_lir_module_create(&g_arena, "m_clean");
    Iron_Type *int_type = iron_type_make_primitive(IRON_TYPE_INT);

    /* func test() -> Int
     *   %1 = const_int 5
     *   %2 = rc_alloc %1
     *   rc_retain %2
     *   rc_release %2     <- cancels with the retain, no barrier between
     *   return %2
     */
    IronLIR_Func *fn = iron_lir_func_create(mod, "Iron_clean", NULL, 0, int_type);
    IronLIR_Block *entry = iron_lir_block_create(fn, "entry");
    IronLIR_Instr *c   = iron_lir_const_int(fn, entry, 5, int_type, sp());
    IronLIR_Instr *rc  = iron_lir_rc_alloc(fn, entry, c->id, int_type, sp());
    iron_lir_rc_retain(fn, entry, rc->id, sp());
    iron_lir_rc_release(fn, entry, rc->id, sp());
    iron_lir_return(fn, entry, rc->id, false, int_type, sp());

    IronLIR_ElisionStat stat;
    memset(&stat, 0, sizeof(stat));
    run_rc_pair_elimination(mod, &stat);

    TEST_ASSERT_EQUAL_INT(1, stat.pairs_eliminated);
    /* Both retain and release should be gone from the block. */
    TEST_ASSERT_EQUAL_INT(0, count_kind_in_block(entry, IRON_LIR_RC_RETAIN));
    TEST_ASSERT_EQUAL_INT(0, count_kind_in_block(entry, IRON_LIR_RC_RELEASE));

    iron_lir_module_destroy(mod);
}

/* ── Test B: release in non-dominated join block → preserved (count 0) ───── */

void test_non_dominated_release_preserved(void) {
    IronLIR_Module *mod = iron_lir_module_create(&g_arena, "m_dom");
    Iron_Type *int_type  = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_Type *bool_type = iron_type_make_primitive(IRON_TYPE_BOOL);

    /* entry:
     *   %1 = const_int 5
     *   %2 = rc_alloc %1
     *   %3 = const_bool true
     *   branch %3, then_b, join_b
     * then_b:
     *   rc_retain %2          <- retain only on one arm
     *   jump join_b
     * join_b:
     *   rc_release %2         <- release on the merge: NOT dominated by retain
     *   return %2
     */
    IronLIR_Func *fn = iron_lir_func_create(mod, "Iron_dom", NULL, 0, int_type);
    IronLIR_Block *entry  = iron_lir_block_create(fn, "entry");
    IronLIR_Block *then_b = iron_lir_block_create(fn, "then_b");
    IronLIR_Block *join_b = iron_lir_block_create(fn, "join_b");

    IronLIR_Instr *c   = iron_lir_const_int(fn, entry, 5, int_type, sp());
    IronLIR_Instr *rc  = iron_lir_rc_alloc(fn, entry, c->id, int_type, sp());
    IronLIR_Instr *cnd = iron_lir_const_bool(fn, entry, true, bool_type, sp());
    iron_lir_branch(fn, entry, cnd->id, then_b->id, join_b->id, sp());

    iron_lir_rc_retain(fn, then_b, rc->id, sp());
    iron_lir_jump(fn, then_b, join_b->id, sp());

    iron_lir_rc_release(fn, join_b, rc->id, sp());
    iron_lir_return(fn, join_b, rc->id, false, int_type, sp());

    IronLIR_ElisionStat stat;
    memset(&stat, 0, sizeof(stat));
    run_rc_pair_elimination(mod, &stat);

    /* retain does not dominate release on all paths → preserved. */
    TEST_ASSERT_EQUAL_INT(0, stat.pairs_eliminated);
    TEST_ASSERT_EQUAL_INT(1, count_kind_in_block(then_b, IRON_LIR_RC_RETAIN));
    TEST_ASSERT_EQUAL_INT(1, count_kind_in_block(join_b, IRON_LIR_RC_RELEASE));

    iron_lir_module_destroy(mod);
}

/* ── Test C: SPAWN barrier between pair → preserved (count 0) ─────────────── */

void test_spawn_barrier_preserves_pair(void) {
    IronLIR_Module *mod = iron_lir_module_create(&g_arena, "m_barrier");
    Iron_Type *int_type = iron_type_make_primitive(IRON_TYPE_INT);

    /* func test() -> Int
     *   %1 = const_int 5
     *   %2 = rc_alloc %1
     *   rc_retain %2
     *   spawn worker()        <- hard barrier
     *   rc_release %2
     *   return %2
     */
    IronLIR_Func *fn = iron_lir_func_create(mod, "Iron_barrier", NULL, 0, int_type);
    IronLIR_Block *entry = iron_lir_block_create(fn, "entry");
    IronLIR_Instr *c   = iron_lir_const_int(fn, entry, 5, int_type, sp());
    IronLIR_Instr *rc  = iron_lir_rc_alloc(fn, entry, c->id, int_type, sp());
    iron_lir_rc_retain(fn, entry, rc->id, sp());
    iron_lir_spawn(fn, entry, "Iron_worker", IRON_LIR_VALUE_INVALID, "h",
                   NULL, sp(), NULL, 0, NULL);
    iron_lir_rc_release(fn, entry, rc->id, sp());
    iron_lir_return(fn, entry, rc->id, false, int_type, sp());

    IronLIR_ElisionStat stat;
    memset(&stat, 0, sizeof(stat));
    run_rc_pair_elimination(mod, &stat);

    /* barrier between retain and release → preserved. */
    TEST_ASSERT_EQUAL_INT(0, stat.pairs_eliminated);
    TEST_ASSERT_EQUAL_INT(1, count_kind_in_block(entry, IRON_LIR_RC_RETAIN));
    TEST_ASSERT_EQUAL_INT(1, count_kind_in_block(entry, IRON_LIR_RC_RELEASE));

    iron_lir_module_destroy(mod);
}

/* ── Test D: post-elimination IR verifies clean (no 300-307) ─────────────── */

void test_post_elimination_verify_clean(void) {
    IronLIR_Module *mod = iron_lir_module_create(&g_arena, "m_verify");
    Iron_Type *int_type = iron_type_make_primitive(IRON_TYPE_INT);

    IronLIR_Func *fn = iron_lir_func_create(mod, "Iron_verify", NULL, 0, int_type);
    IronLIR_Block *entry = iron_lir_block_create(fn, "entry");
    IronLIR_Instr *c   = iron_lir_const_int(fn, entry, 5, int_type, sp());
    IronLIR_Instr *rc  = iron_lir_rc_alloc(fn, entry, c->id, int_type, sp());
    iron_lir_rc_retain(fn, entry, rc->id, sp());
    iron_lir_rc_release(fn, entry, rc->id, sp());
    iron_lir_return(fn, entry, rc->id, false, int_type, sp());

    IronLIR_ElisionStat stat;
    memset(&stat, 0, sizeof(stat));
    run_rc_pair_elimination(mod, &stat);
    TEST_ASSERT_EQUAL_INT(1, stat.pairs_eliminated);

    /* The mutated stream must still pass the LIR verifier. */
    Iron_DiagList diags = iron_diaglist_create();
    bool ok = iron_lir_verify(mod, &diags, &g_arena);
    TEST_ASSERT_TRUE(ok);

    iron_diaglist_free(&diags);
    iron_lir_module_destroy(mod);
}

/* ── Gate: pass is a pure function — count stays 0 when not invoked ──────── */

void test_gate_off_leaves_count_zero(void) {
    /* Models the -O0/debug gate (enforced in Plan 03/build.c): when the pass
     * is NOT invoked, no rc op is eliminated and the stat stays at its
     * caller-initialized 0. The pass never mutates module state implicitly. */
    IronLIR_Module *mod = iron_lir_module_create(&g_arena, "m_gate");
    Iron_Type *int_type = iron_type_make_primitive(IRON_TYPE_INT);

    IronLIR_Func *fn = iron_lir_func_create(mod, "Iron_gate", NULL, 0, int_type);
    IronLIR_Block *entry = iron_lir_block_create(fn, "entry");
    IronLIR_Instr *c   = iron_lir_const_int(fn, entry, 5, int_type, sp());
    IronLIR_Instr *rc  = iron_lir_rc_alloc(fn, entry, c->id, int_type, sp());
    iron_lir_rc_retain(fn, entry, rc->id, sp());
    iron_lir_rc_release(fn, entry, rc->id, sp());
    iron_lir_return(fn, entry, rc->id, false, int_type, sp());

    IronLIR_ElisionStat stat;
    memset(&stat, 0, sizeof(stat));
    /* Gate OFF: do not call run_rc_pair_elimination. */
    TEST_ASSERT_EQUAL_INT(0, stat.pairs_eliminated);
    /* rc ops remain intact because the pass was never run. */
    TEST_ASSERT_EQUAL_INT(1, count_kind_in_block(entry, IRON_LIR_RC_RETAIN));
    TEST_ASSERT_EQUAL_INT(1, count_kind_in_block(entry, IRON_LIR_RC_RELEASE));

    iron_lir_module_destroy(mod);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_clean_local_pair_eliminated);
    RUN_TEST(test_non_dominated_release_preserved);
    RUN_TEST(test_spawn_barrier_preserves_pair);
    RUN_TEST(test_post_elimination_verify_clean);
    RUN_TEST(test_gate_off_leaves_count_zero);
    return UNITY_END();
}
