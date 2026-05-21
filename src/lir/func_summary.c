/* func_summary.c — Phase 29 OPT-02: conservative function-summary table.
 *
 * Implements the contract declared in src/lir/func_summary.h: a reusable,
 * default-pessimistic { may_spawn, may_free, escapes_rc_args } table keyed by
 * function name. The Plan 03 rc-pair-elision pass (run_rc_pair_elimination)
 * and the Phase 30 pointer-check elimination pass both consume it as the
 * "is this call a barrier?" oracle, so it lives in its own TU rather than
 * being pinned pass-private to lir_optimize.c (CONTEXT GA3 / STATE OQ-12).
 *
 * Structure mirrors compute_func_purity (lir_optimize.c:1775): a two-phase
 * bottom-up seed + module fixpoint. Every bit is an over-approximation — a
 * `true` means the property MAY hold; `false` means it provably does NOT hold
 * for that function. Any callee that is extern, indirect (func_decl == NULL
 * with an unresolvable func_ptr), or simply absent from the map is folded in
 * as the conservative pessimistic summary { true, true, true }.
 *
 * Soundness invariants:
 *   - Monotone OR over a finite 3-bit lattice → the fixpoint converges. The
 *     loop is additionally bounded by func_count + 1 iterations as a
 *     recursion-safety belt so a self- or mutually-recursive call graph can
 *     never spin forever (Test D).
 *   - The seeding switch is EXHAUSTIVE over IronLIR_InstrKind so the parent
 *     build's -Werror=switch-enum stays clean and any future opcode forces a
 *     deliberate barrier-classification decision here.
 */

#include "lir/func_summary.h"
#include "vendor/stb_ds.h"

#include <string.h>

/* ── Pessimistic default ──────────────────────────────────────────────────── */

IronLIR_FuncSummary iron_lir_func_summary_pessimistic(void) {
    IronLIR_FuncSummary p = { true, true, true };
    return p;
}

/* ── Lookup (string keyed; pessimistic when absent) ───────────────────────── */

IronLIR_FuncSummary iron_lir_func_summary_lookup(IronLIR_FuncSummaryEntry *map,
                                                 const char *name) {
    if (!map || !name) return iron_lir_func_summary_pessimistic();
    IronLIR_FuncSummaryEntry *e = shgetp_null(map, name);
    if (!e) return iron_lir_func_summary_pessimistic();
    return e->value;
}

/* ── Free ─────────────────────────────────────────────────────────────────── */

void iron_lir_func_summaries_free(IronLIR_FuncSummaryEntry *map) {
    if (!map) return;
    shfree(map);
}

/* ── Helpers ──────────────────────────────────────────────────────────────── */

/* Resolve the callee name of a CALL instruction within `fn`.
 * Returns NULL for an indirect call whose func_ptr does not resolve to a
 * FUNC_REF — the caller must treat NULL as the pessimistic default. */
static const char *resolve_callee_name(IronLIR_Func *fn, IronLIR_Instr *call) {
    if (call->call.func_decl) {
        /* Direct call. An extern target has no module body to summarize. */
        if (call->call.func_decl->is_extern) return NULL;
        return call->call.func_decl->name;
    }
    IronLIR_ValueId fptr = call->call.func_ptr;
    if (fptr != IRON_LIR_VALUE_INVALID &&
        fptr < (IronLIR_ValueId)arrlen(fn->value_table) &&
        fn->value_table[fptr] != NULL &&
        fn->value_table[fptr]->kind == IRON_LIR_FUNC_REF) {
        return fn->value_table[fptr]->func_ref.func_name;
    }
    return NULL;
}

/* OR-fold `src` into `*dst`, returning true if any bit was newly set. */
static bool summary_or_into(IronLIR_FuncSummary *dst, IronLIR_FuncSummary src) {
    bool changed = false;
    if (src.may_spawn && !dst->may_spawn)             { dst->may_spawn = true; changed = true; }
    if (src.may_free && !dst->may_free)               { dst->may_free = true; changed = true; }
    if (src.escapes_rc_args && !dst->escapes_rc_args) { dst->escapes_rc_args = true; changed = true; }
    return changed;
}

/* Seed a function's own (non-call) instruction bits. Exhaustive switch over
 * IronLIR_InstrKind so -Werror=switch-enum stays clean. CALL is intentionally
 * a no-op here — inter-procedural propagation happens in the fixpoint phase. */
static void seed_func(IronLIR_Func *fn, IronLIR_FuncSummary *out) {
    for (int bi = 0; bi < fn->block_count; bi++) {
        IronLIR_Block *blk = fn->blocks[bi];
        for (int ii = 0; ii < blk->instr_count; ii++) {
            IronLIR_Instr *in = blk->instrs[ii];
            switch (in->kind) {
                /* Concurrency → may_spawn */
                case IRON_LIR_SPAWN:
                case IRON_LIR_PARALLEL_FOR:
                    out->may_spawn = true;
                    /* A spawn/parallel_for captures bindings → potential escape. */
                    out->escapes_rc_args = true;
                    break;

                /* Allocation / free / drop → may_free */
                case IRON_LIR_HEAP_ALLOC:
                case IRON_LIR_RC_ALLOC:
                case IRON_LIR_ARENA_ALLOC:
                case IRON_LIR_FREE:
                case IRON_LIR_RC_RELEASE:
                case IRON_LIR_WEAK_RC_RELEASE:
                    out->may_free = true;
                    break;

                /* Stores / returns / closures → coarse escape (v1). */
                case IRON_LIR_STORE:
                case IRON_LIR_SET_FIELD:
                case IRON_LIR_SET_INDEX:
                case IRON_LIR_MAKE_CLOSURE:
                    out->escapes_rc_args = true;
                    break;
                case IRON_LIR_RETURN:
                    if (!in->ret.is_void) out->escapes_rc_args = true;
                    break;

                /* CALL: handled in the fixpoint phase, not the seed. */
                case IRON_LIR_CALL:
                    break;

                /* Everything else has no first-order effect on these bits. */
                case IRON_LIR_CONST_INT:
                case IRON_LIR_CONST_FLOAT:
                case IRON_LIR_CONST_BOOL:
                case IRON_LIR_CONST_STRING:
                case IRON_LIR_CONST_NULL:
                case IRON_LIR_ADD:
                case IRON_LIR_SUB:
                case IRON_LIR_MUL:
                case IRON_LIR_DIV:
                case IRON_LIR_MOD:
                case IRON_LIR_EQ:
                case IRON_LIR_NEQ:
                case IRON_LIR_LT:
                case IRON_LIR_LTE:
                case IRON_LIR_GT:
                case IRON_LIR_GTE:
                case IRON_LIR_AND:
                case IRON_LIR_OR:
                case IRON_LIR_SHL:
                case IRON_LIR_SHR:
                case IRON_LIR_BAND:
                case IRON_LIR_BOR:
                case IRON_LIR_BXOR:
                case IRON_LIR_NEG:
                case IRON_LIR_NOT:
                case IRON_LIR_BNOT:
                case IRON_LIR_ALLOCA:
                case IRON_LIR_LOAD:
                case IRON_LIR_GET_FIELD:
                case IRON_LIR_GET_INDEX:
                case IRON_LIR_JUMP:
                case IRON_LIR_BRANCH:
                case IRON_LIR_SWITCH:
                case IRON_LIR_CAST:
                case IRON_LIR_CONSTRUCT:
                case IRON_LIR_ARRAY_LIT:
                case IRON_LIR_SLICE:
                case IRON_LIR_IS_NULL:
                case IRON_LIR_IS_NOT_NULL:
                case IRON_LIR_INTERP_STRING:
                case IRON_LIR_ARENA_PUSH:
                case IRON_LIR_ARENA_POP:
                case IRON_LIR_RC_RETAIN:
                case IRON_LIR_WEAK_RC_RETAIN:
                case IRON_LIR_WEAK_RC_DOWNGRADE:
                case IRON_LIR_WEAK_RC_UPGRADE:
                case IRON_LIR_FUNC_REF:
                case IRON_LIR_AWAIT:
                case IRON_LIR_PHI:
                case IRON_LIR_POISON:
                case IRON_LIR_ADDR_OF:
                case IRON_LIR_PTR_LOAD:
                case IRON_LIR_PTR_STORE:
                case IRON_LIR_PTR_OFFSET:
                case IRON_LIR_PTR_DIFF:
                case IRON_LIR_INSTR_COUNT:
                    break;
            }
        }
    }
}

/* ── Driver: two-phase seed + module fixpoint ─────────────────────────────── */

IronLIR_FuncSummaryEntry *iron_lir_compute_func_summaries(IronLIR_Module *module) {
    IronLIR_FuncSummaryEntry *map = NULL;
    if (!module || module->func_count == 0) return map;

    /* Phase 1: seed each non-extern function from its own instructions, starting
     * optimistic-false (so recursion/fixpoint can only OR bits up monotonically). */
    for (int fi = 0; fi < module->func_count; fi++) {
        IronLIR_Func *fn = module->funcs[fi];
        if (fn->is_extern || fn->block_count == 0) continue;
        IronLIR_FuncSummary s = { false, false, false };
        seed_func(fn, &s);
        shput(map, (char *)fn->name, s);
    }

    /* Phase 2: fixpoint propagation of callee bits into callers.
     * Monotone OR over a finite lattice converges; bound by func_count + 1 as a
     * recursion-safety belt (self- and mutually-recursive graphs terminate). */
    int max_iters = module->func_count + 1;
    bool changed = true;
    for (int iter = 0; iter < max_iters && changed; iter++) {
        changed = false;
        for (int fi = 0; fi < module->func_count; fi++) {
            IronLIR_Func *fn = module->funcs[fi];
            if (fn->is_extern || fn->block_count == 0) continue;

            IronLIR_FuncSummaryEntry *self = shgetp_null(map, fn->name);
            if (!self) continue;
            IronLIR_FuncSummary cur = self->value;

            for (int bi = 0; bi < fn->block_count; bi++) {
                IronLIR_Block *blk = fn->blocks[bi];
                for (int ii = 0; ii < blk->instr_count; ii++) {
                    IronLIR_Instr *in = blk->instrs[ii];
                    if (in->kind != IRON_LIR_CALL) continue;

                    const char *callee = resolve_callee_name(fn, in);
                    if (!callee) {
                        /* extern / indirect-unresolved → fully pessimistic. */
                        summary_or_into(&cur, iron_lir_func_summary_pessimistic());
                        continue;
                    }
                    /* Known module callee → OR its current summary; absent name
                     * (e.g. a builtin not in the table) → pessimistic. */
                    summary_or_into(&cur, iron_lir_func_summary_lookup(map, callee));
                }
            }

            /* Re-fetch the slot (shput may rehash) and commit if anything grew. */
            IronLIR_FuncSummaryEntry *slot = shgetp_null(map, fn->name);
            if (slot && summary_or_into(&slot->value, cur)) {
                changed = true;
            }
        }
    }

    return map;
}
