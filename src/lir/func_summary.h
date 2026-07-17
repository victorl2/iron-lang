#ifndef IRON_LIR_FUNC_SUMMARY_H
#define IRON_LIR_FUNC_SUMMARY_H

/* Phase 29 OPT-02: conservative function-summary table foundation.
 *
 * A reusable, pass-private-free module (NOT pinned to lir_optimize.c) so the
 * Phase 30 pointer-check elision optimizer can share it (CONTEXT GA3 / STATE
 * OQ-12). The contract here is a CONTRACT ONLY — the implementation lands in
 * Plan 02 (src/lir/func_summary.c). Until then, linking any consumer against
 * iron_compiler fails to resolve iron_lir_compute_func_summaries et al.; that
 * unresolved-symbol link failure is the intended Wave 0 TDD RED state.
 *
 * Shape mirrors compute_func_purity's output map (lir_optimize.c:1775): a
 * bottom-up + module-fixpoint computation with a conservative pessimistic
 * default for any callee not in the map.
 */

#include "lir/lir.h"   /* IronLIR_Module */
#include <stdbool.h>

/* Per-function conservative summary. A bit being `true` means the property
 * MAY hold (over-approximation); `false` means it provably does NOT hold for
 * this function. The elision pass treats any `true` bit between a candidate
 * retain/release as a barrier. */
typedef struct {
    bool may_spawn;       /* transitively reaches SPAWN / PARALLEL_FOR */
    bool may_free;        /* transitively reaches FREE / *_ALLOC / user drop */
    bool escapes_rc_args; /* stores/returns/thread-shares a pointer arg */
} IronLIR_FuncSummary;

/* stb_ds string map: func_name -> summary. */
typedef struct { char *key; IronLIR_FuncSummary value; } IronLIR_FuncSummaryEntry;

/* Compute the per-module summary table. Any key NOT in the returned map (and
 * any extern / indirect / recursion-in-flight callee) must be treated by the
 * caller as the conservative pessimistic summary { true, true, true }.
 * Returned map is owned by the caller; release with
 * iron_lir_func_summaries_free(). Implementation: Plan 02. */
IronLIR_FuncSummaryEntry *iron_lir_compute_func_summaries(IronLIR_Module *module);

/* Free a summary map produced by iron_lir_compute_func_summaries(). */
void iron_lir_func_summaries_free(IronLIR_FuncSummaryEntry *map);

/* The conservative-default pessimistic summary for an unknown callee:
 * { may_spawn=true, may_free=true, escapes_rc_args=true }. */
IronLIR_FuncSummary iron_lir_func_summary_pessimistic(void);

/* Look up a callee's summary by name; returns the pessimistic default if the
 * name is absent from the map (or `map` is NULL / `name` is NULL). */
IronLIR_FuncSummary iron_lir_func_summary_lookup(IronLIR_FuncSummaryEntry *map,
                                                 const char *name);

#endif /* IRON_LIR_FUNC_SUMMARY_H */
