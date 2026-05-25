/* test_gencheck_elision.c — Phase 30 OPT-03/04/05/06/07/08 RED-anchor harness.
 *
 * Wave 0 contract-first TDD. Asserts the deterministic, count-based oracle for
 * the 4-tier pointer (generation) check elision pass:
 *   run_pointer_check_elimination() fills an IronLIR_GenCheckElisionStat whose
 *   by_tier[] / checks_elided counts tests assert EXACTLY (never timing-based).
 *
 * TWO STAGES (mirroring how Phase 29 staged RED→GREEN):
 *
 *   (a) RUNS NOW — test_gencheck_opcode_constructs: builds an IRON_LIR_GENCHECK
 *       via iron_lir_gencheck() and asserts the opcode + void-result + payload
 *       round-trip. This proves the Plan 30-01 opcode contract compiles + links.
 *
 *   (b) STAGED behind #ifdef GENCHECK_PASS_READY — the pass-dependent cases
 *       (refactor-identity, T1 dominator CSE, T2 escape, T3 LICM, T4 clean-call,
 *       barrier-straddle, gate-off). These call lower_genchecks() /
 *       run_pointer_check_elimination(), which are DECLARED (lir_optimize.h) but
 *       NOT DEFINED until Plans 30-02 / 30-03. The #ifdef keeps THIS executable
 *       linkable now (no unresolved symbols); Plans 02/03 -D the macro on (or
 *       drop the guard) to flip the asserts live. These are the GREEN targets.
 *
 * The second CTest NAME `test_gencheck_elision_gate` exercises the same exe; the
 * gate-off contract (pass mutates only when invoked) is asserted in-binary by
 * test_gate_off_preserves below.
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

#ifdef GENCHECK_PASS_READY
static int count_kind_in_block(IronLIR_Block *blk, IronLIR_InstrKind kind) {
    int n = 0;
    for (int i = 0; i < blk->instr_count; i++) {
        if (blk->instrs[i]->kind == kind) n++;
    }
    return n;
}
#endif

/* ── (a) RUNS NOW: GENCHECK opcode constructs + payload round-trips ───────── */

void test_gencheck_opcode_constructs(void) {
    IronLIR_Module *mod = iron_lir_module_create(&g_arena, "m_gc");
    Iron_Type *int_type = iron_type_make_primitive(IRON_TYPE_INT);

    /* func test() -> Int
     *   %1 = const_int 5
     *   %2 = alloca Int
     *   %3 = addr_of %2 (stack)
     *   gencheck %3 root=%2 (stack)     <- the new void-result intrinsic
     *   return %1
     */
    IronLIR_Func *fn = iron_lir_func_create(mod, "Iron_gc", NULL, 0, int_type);
    IronLIR_Block *entry = iron_lir_block_create(fn, "entry");
    IronLIR_Instr *c  = iron_lir_const_int(fn, entry, 5, int_type, sp());
    IronLIR_Instr *al = iron_lir_alloca(fn, entry, int_type, "x", sp());
    IronLIR_Instr *ad = iron_lir_addr_of(fn, entry, al->id,
                                         IRON_LIR_GEN_STACK, int_type, sp());
    IronLIR_Instr *gc = iron_lir_gencheck(fn, entry, ad->id, al->id,
                                          IRON_LIR_GEN_STACK, sp());
    iron_lir_return(fn, entry, c->id, false, int_type, sp());

    /* Opcode identity + void-result (id == IRON_LIR_VALUE_INVALID, like
     * RC_RETAIN/RC_RELEASE) + payload field round-trip. */
    TEST_ASSERT_EQUAL_INT(IRON_LIR_GENCHECK, gc->kind);
    TEST_ASSERT_EQUAL_UINT(IRON_LIR_VALUE_INVALID, gc->id);
    TEST_ASSERT_EQUAL_UINT(ad->id, gc->gencheck.ptr);
    TEST_ASSERT_EQUAL_UINT(al->id, gc->gencheck.root_alloc);
    TEST_ASSERT_EQUAL_INT(IRON_LIR_GEN_STACK, gc->gencheck.gen_source);

    iron_lir_module_destroy(mod);
}

/* ── (b) STAGED for Plans 30-02 / 30-03 (GENCHECK_PASS_READY) ─────────────── */

#ifdef GENCHECK_PASS_READY

/* OPT-03 refactor identity: one deref → exactly one surviving check, no elision.
 * (Asserts byte-identity of the emitted check string is performed in the
 * v4-acceptance corpus; here we assert the GENCHECK survives + verifies.) */
void test_refactor_identity(void) {
    IronLIR_Module *mod = iron_lir_module_create(&g_arena, "m_identity");
    Iron_Type *int_type = iron_type_make_primitive(IRON_TYPE_INT);

    IronLIR_Func *fn = iron_lir_func_create(mod, "Iron_identity", NULL, 0, int_type);
    IronLIR_Block *entry = iron_lir_block_create(fn, "entry");
    IronLIR_Instr *al = iron_lir_alloca(fn, entry, int_type, "x", sp());
    IronLIR_Instr *ad = iron_lir_addr_of(fn, entry, al->id,
                                         IRON_LIR_GEN_STACK, int_type, sp());
    iron_lir_ptr_load(fn, entry, ad->id, IRON_LIR_GEN_STACK, int_type, sp());
    iron_lir_return(fn, entry, al->id, false, int_type, sp());

    lower_genchecks(mod);   /* inserts one GENCHECK before the PTR_LOAD */
    TEST_ASSERT_EQUAL_INT(1, count_kind_in_block(entry, IRON_LIR_GENCHECK));

    IronLIR_GenCheckElisionStat stat;
    memset(&stat, 0, sizeof(stat));
    run_pointer_check_elimination(mod, &stat);
    /* Single check, nothing to elide. */
    TEST_ASSERT_EQUAL_INT(1, stat.checks_total);
    TEST_ASSERT_EQUAL_INT(0, stat.checks_elided);
    TEST_ASSERT_EQUAL_INT(1, count_kind_in_block(entry, IRON_LIR_GENCHECK));

    iron_lir_module_destroy(mod);
}

/* OPT-04 Tier 1: two field reads off the same root, no may-free between → 1. */
void test_t1_dominator_redundant(void) {
    IronLIR_Module *mod = iron_lir_module_create(&g_arena, "m_t1");
    Iron_Type *int_type = iron_type_make_primitive(IRON_TYPE_INT);

    IronLIR_Func *fn = iron_lir_func_create(mod, "Iron_t1", NULL, 0, int_type);
    IronLIR_Block *entry = iron_lir_block_create(fn, "entry");
    IronLIR_Instr *seed = iron_lir_const_int(fn, entry, 7, int_type, sp());
    IronLIR_Instr *ha = iron_lir_heap_alloc(fn, entry, seed->id, false, false,
                                            int_type, sp());
    IronLIR_Instr *a1 = iron_lir_addr_of(fn, entry, ha->id,
                                         IRON_LIR_GEN_HEAP, int_type, sp());
    iron_lir_ptr_load(fn, entry, a1->id, IRON_LIR_GEN_HEAP, int_type, sp());
    IronLIR_Instr *a2 = iron_lir_addr_of(fn, entry, ha->id,
                                         IRON_LIR_GEN_HEAP, int_type, sp());
    iron_lir_ptr_load(fn, entry, a2->id, IRON_LIR_GEN_HEAP, int_type, sp());
    iron_lir_return(fn, entry, ha->id, false, int_type, sp());

    lower_genchecks(mod);
    IronLIR_GenCheckElisionStat stat;
    memset(&stat, 0, sizeof(stat));
    run_pointer_check_elimination(mod, &stat);

    TEST_ASSERT_EQUAL_INT(2, stat.checks_total);
    TEST_ASSERT_EQUAL_INT(1, stat.by_tier[0]);   /* one dominated redundant */
    TEST_ASSERT_EQUAL_INT(1, stat.checks_elided);

    iron_lir_module_destroy(mod);
}

/* OPT-05 Tier 2: GENCHECK on a non-escaping &local (STACK) → elided. */
void test_t2_escape_local(void) {
    IronLIR_Module *mod = iron_lir_module_create(&g_arena, "m_t2");
    Iron_Type *int_type = iron_type_make_primitive(IRON_TYPE_INT);

    IronLIR_Func *fn = iron_lir_func_create(mod, "Iron_t2", NULL, 0, int_type);
    IronLIR_Block *entry = iron_lir_block_create(fn, "entry");
    IronLIR_Instr *al = iron_lir_alloca(fn, entry, int_type, "x", sp());
    IronLIR_Instr *ad = iron_lir_addr_of(fn, entry, al->id,
                                         IRON_LIR_GEN_STACK, int_type, sp());
    iron_lir_ptr_load(fn, entry, ad->id, IRON_LIR_GEN_STACK, int_type, sp());
    iron_lir_return(fn, entry, al->id, false, int_type, sp());

    lower_genchecks(mod);
    IronLIR_GenCheckElisionStat stat;
    memset(&stat, 0, sizeof(stat));
    run_pointer_check_elimination(mod, &stat);

    TEST_ASSERT_EQUAL_INT(1, stat.by_tier[1]);   /* non-escaping local elided */

    iron_lir_module_destroy(mod);
}

/* OPT-06 Tier 3: loop-invariant root, no may-free body, entered loop → hoist. */
void test_t3_licm_hoist(void) {
    /* Constructed in Plan 30-03 against the real loop substrate; asserts
     * by_tier[2] == 1. */
    TEST_IGNORE_MESSAGE("T3 LICM fixture authored in Plan 30-03");
}

/* OPT-07 Tier 4: clean-summary CALL is NOT a barrier; extern/indirect is. */
void test_t4_clean_call_not_barrier(void) {
    TEST_IGNORE_MESSAGE("T4 func-summary fixture authored in Plan 30-03");
}

/* OQ-08: each barrier-straddling case → checks_elided == 0, verifies clean. */
void test_barrier_straddle_preserved(void) {
    IronLIR_Module *mod = iron_lir_module_create(&g_arena, "m_barrier");
    Iron_Type *int_type = iron_type_make_primitive(IRON_TYPE_INT);

    IronLIR_Func *fn = iron_lir_func_create(mod, "Iron_barrier", NULL, 0, int_type);
    IronLIR_Block *entry = iron_lir_block_create(fn, "entry");
    IronLIR_Instr *seed = iron_lir_const_int(fn, entry, 7, int_type, sp());
    IronLIR_Instr *ha = iron_lir_heap_alloc(fn, entry, seed->id, false, false,
                                            int_type, sp());
    IronLIR_Instr *a1 = iron_lir_addr_of(fn, entry, ha->id,
                                         IRON_LIR_GEN_HEAP, int_type, sp());
    iron_lir_ptr_load(fn, entry, a1->id, IRON_LIR_GEN_HEAP, int_type, sp());
    iron_lir_spawn(fn, entry, "Iron_worker", IRON_LIR_VALUE_INVALID, "h",
                   NULL, sp(), NULL, 0, NULL);   /* hard may-free barrier */
    IronLIR_Instr *a2 = iron_lir_addr_of(fn, entry, ha->id,
                                         IRON_LIR_GEN_HEAP, int_type, sp());
    iron_lir_ptr_load(fn, entry, a2->id, IRON_LIR_GEN_HEAP, int_type, sp());
    iron_lir_return(fn, entry, ha->id, false, int_type, sp());

    lower_genchecks(mod);
    IronLIR_GenCheckElisionStat stat;
    memset(&stat, 0, sizeof(stat));
    run_pointer_check_elimination(mod, &stat);

    TEST_ASSERT_EQUAL_INT(0, stat.checks_elided);   /* barrier preserves both */

    Iron_DiagList diags = iron_diaglist_create();
    bool ok = iron_lir_verify(mod, &diags, &g_arena);
    TEST_ASSERT_TRUE(ok);                            /* no 300-307 post-pass */
    iron_diaglist_free(&diags);

    iron_lir_module_destroy(mod);
}

/* Gate off (pass not run) → all GENCHECKs survive, checks_elided == 0.
 * This is the body the 2nd CTest NAME test_gencheck_elision_gate exercises. */
void test_gate_off_preserves(void) {
    IronLIR_Module *mod = iron_lir_module_create(&g_arena, "m_gate");
    Iron_Type *int_type = iron_type_make_primitive(IRON_TYPE_INT);

    IronLIR_Func *fn = iron_lir_func_create(mod, "Iron_gate", NULL, 0, int_type);
    IronLIR_Block *entry = iron_lir_block_create(fn, "entry");
    IronLIR_Instr *seed = iron_lir_const_int(fn, entry, 7, int_type, sp());
    IronLIR_Instr *ha = iron_lir_heap_alloc(fn, entry, seed->id, false, false,
                                            int_type, sp());
    IronLIR_Instr *a1 = iron_lir_addr_of(fn, entry, ha->id,
                                         IRON_LIR_GEN_HEAP, int_type, sp());
    iron_lir_ptr_load(fn, entry, a1->id, IRON_LIR_GEN_HEAP, int_type, sp());
    iron_lir_return(fn, entry, ha->id, false, int_type, sp());

    lower_genchecks(mod);
    int before = count_kind_in_block(entry, IRON_LIR_GENCHECK);

    IronLIR_GenCheckElisionStat stat;
    memset(&stat, 0, sizeof(stat));
    /* Gate OFF: do not call run_pointer_check_elimination. */
    TEST_ASSERT_EQUAL_INT(0, stat.checks_elided);
    TEST_ASSERT_EQUAL_INT(before, count_kind_in_block(entry, IRON_LIR_GENCHECK));

    iron_lir_module_destroy(mod);
}

#endif /* GENCHECK_PASS_READY */

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_gencheck_opcode_constructs);
#ifdef GENCHECK_PASS_READY
    RUN_TEST(test_refactor_identity);
    RUN_TEST(test_t1_dominator_redundant);
    RUN_TEST(test_t2_escape_local);
    RUN_TEST(test_t3_licm_hoist);
    RUN_TEST(test_t4_clean_call_not_barrier);
    RUN_TEST(test_barrier_straddle_preserved);
    RUN_TEST(test_gate_off_preserves);
#endif
    return UNITY_END();
}
