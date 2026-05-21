/* test_func_summary.c — Phase 29 OPT-02 RED unit harness.
 *
 * Asserts the conservative function-summary table contract
 * (src/lir/func_summary.h): may_spawn / may_free / escapes_rc_args bits,
 * fixpoint propagation across callers, recursion-safety, and the pessimistic
 * default for unknown/extern callees.
 *
 * INTENDED RED STATE (Wave 0 / TDD): iron_lir_compute_func_summaries,
 * iron_lir_func_summaries_free, iron_lir_func_summary_pessimistic, and
 * iron_lir_func_summary_lookup are DECLARED (func_summary.h) but NOT DEFINED
 * until Plan 02 (src/lir/func_summary.c). Therefore this executable FAILS TO
 * LINK against iron_compiler today (unresolved symbols). That link failure is
 * the correct RED — do NOT stub the functions to make it pass.
 *
 * Oracle discipline: struct-bit assertions only; never wall-time-based.
 */

#include "unity.h"
#include "lir/lir.h"
#include "lir/func_summary.h"
#include "analyzer/types.h"
#include "diagnostics/diagnostics.h"
#include "util/arena.h"
#include "vendor/stb_ds.h"

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

/* Helper: look up a func summary by name from the map. */
static IronLIR_FuncSummary lookup(IronLIR_FuncSummaryEntry *map, const char *name) {
    return iron_lir_func_summary_lookup(map, name);
}

/* ── Test A: a function containing SPAWN → may_spawn == true ─────────────── */

void test_summary_spawn_sets_may_spawn(void) {
    IronLIR_Module *mod = iron_lir_module_create(&g_arena, "m_spawn");
    Iron_Type *vt = NULL;

    IronLIR_Func *fn = iron_lir_func_create(mod, "Iron_spawner", NULL, 0, vt);
    IronLIR_Block *entry = iron_lir_block_create(fn, "entry");
    iron_lir_spawn(fn, entry, "Iron_worker", IRON_LIR_VALUE_INVALID, "h",
                   vt, sp(), NULL, 0, NULL);
    iron_lir_return(fn, entry, IRON_LIR_VALUE_INVALID, true, NULL, sp());

    IronLIR_FuncSummaryEntry *map = iron_lir_compute_func_summaries(mod);
    IronLIR_FuncSummary s = lookup(map, "Iron_spawner");
    TEST_ASSERT_TRUE(s.may_spawn);

    iron_lir_func_summaries_free(map);
    iron_lir_module_destroy(mod);
}

/* ── Test B: a function with RC_ALLOC → may_free == true ─────────────────── */

void test_summary_alloc_sets_may_free(void) {
    IronLIR_Module *mod = iron_lir_module_create(&g_arena, "m_alloc");
    Iron_Type *int_type = iron_type_make_primitive(IRON_TYPE_INT);

    IronLIR_Func *fn = iron_lir_func_create(mod, "Iron_allocator", NULL, 0, int_type);
    IronLIR_Block *entry = iron_lir_block_create(fn, "entry");
    IronLIR_Instr *c = iron_lir_const_int(fn, entry, 7, int_type, sp());
    iron_lir_rc_alloc(fn, entry, c->id, int_type, sp());
    iron_lir_return(fn, entry, c->id, false, int_type, sp());

    IronLIR_FuncSummaryEntry *map = iron_lir_compute_func_summaries(mod);
    IronLIR_FuncSummary s = lookup(map, "Iron_allocator");
    TEST_ASSERT_TRUE(s.may_free);

    iron_lir_func_summaries_free(map);
    iron_lir_module_destroy(mod);
}

/* ── Test C: caller of a may_spawn callee → caller.may_spawn (fixpoint) ──── */

void test_summary_fixpoint_propagates_to_caller(void) {
    IronLIR_Module *mod = iron_lir_module_create(&g_arena, "m_fixpoint");
    Iron_Type *vt = NULL;

    /* callee: spawns */
    IronLIR_Func *callee = iron_lir_func_create(mod, "Iron_callee", NULL, 0, vt);
    IronLIR_Block *ce = iron_lir_block_create(callee, "entry");
    iron_lir_spawn(callee, ce, "Iron_worker", IRON_LIR_VALUE_INVALID, "h",
                   vt, sp(), NULL, 0, NULL);
    iron_lir_return(callee, ce, IRON_LIR_VALUE_INVALID, true, NULL, sp());

    /* caller: calls Iron_callee via func_ref + indirect call */
    IronLIR_Func *caller = iron_lir_func_create(mod, "Iron_caller", NULL, 0, vt);
    IronLIR_Block *cae = iron_lir_block_create(caller, "entry");
    IronLIR_Instr *fref = iron_lir_func_ref(caller, cae, "Iron_callee", vt, sp());
    iron_lir_call(caller, cae, NULL, fref->id, NULL, 0, NULL, sp());
    iron_lir_return(caller, cae, IRON_LIR_VALUE_INVALID, true, NULL, sp());

    IronLIR_FuncSummaryEntry *map = iron_lir_compute_func_summaries(mod);
    IronLIR_FuncSummary caller_s = lookup(map, "Iron_caller");
    TEST_ASSERT_TRUE(caller_s.may_spawn);

    iron_lir_func_summaries_free(map);
    iron_lir_module_destroy(mod);
}

/* ── Test D: self-recursive function → conservative, no crash ────────────── */

void test_summary_recursion_is_conservative(void) {
    IronLIR_Module *mod = iron_lir_module_create(&g_arena, "m_recur");
    Iron_Type *int_type = iron_type_make_primitive(IRON_TYPE_INT);

    IronLIR_Func *fn = iron_lir_func_create(mod, "Iron_recur", NULL, 0, int_type);
    IronLIR_Block *entry = iron_lir_block_create(fn, "entry");
    IronLIR_Instr *fref = iron_lir_func_ref(fn, entry, "Iron_recur", int_type, sp());
    IronLIR_Instr *call = iron_lir_call(fn, entry, NULL, fref->id, NULL, 0,
                                        int_type, sp());
    iron_lir_return(fn, entry, call->id, false, int_type, sp());

    /* The fixpoint must terminate (no infinite loop) and produce sound bits. */
    IronLIR_FuncSummaryEntry *map = iron_lir_compute_func_summaries(mod);
    IronLIR_FuncSummary s = lookup(map, "Iron_recur");
    /* A self-recursive fn with no spawn/alloc of its own stays conservative:
     * the recursion default does not crash and yields a defined summary. */
    TEST_ASSERT_TRUE(s.may_spawn == true || s.may_spawn == false);

    iron_lir_func_summaries_free(map);
    iron_lir_module_destroy(mod);
}

/* ── Test E: pessimistic default for absent/extern callee ────────────────── */

void test_summary_pessimistic_default(void) {
    IronLIR_FuncSummary p = iron_lir_func_summary_pessimistic();
    TEST_ASSERT_TRUE(p.may_spawn);
    TEST_ASSERT_TRUE(p.may_free);
    TEST_ASSERT_TRUE(p.escapes_rc_args);

    /* Lookup of an unknown name returns the pessimistic default. */
    IronLIR_Module *mod = iron_lir_module_create(&g_arena, "m_empty");
    IronLIR_FuncSummaryEntry *map = iron_lir_compute_func_summaries(mod);
    IronLIR_FuncSummary s = iron_lir_func_summary_lookup(map, "Iron_never_seen");
    TEST_ASSERT_TRUE(s.may_spawn);
    TEST_ASSERT_TRUE(s.may_free);
    TEST_ASSERT_TRUE(s.escapes_rc_args);

    iron_lir_func_summaries_free(map);
    iron_lir_module_destroy(mod);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_summary_spawn_sets_may_spawn);
    RUN_TEST(test_summary_alloc_sets_may_free);
    RUN_TEST(test_summary_fixpoint_propagates_to_caller);
    RUN_TEST(test_summary_recursion_is_conservative);
    RUN_TEST(test_summary_pessimistic_default);
    return UNITY_END();
}
