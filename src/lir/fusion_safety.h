/* fusion_safety.h -- which functions may run inside a fused list loop.
 *
 * Loop fusion (emit_c.c, emit_fusion.c) turns `xs.map(f).filter(g).sum()`
 * from three loops, each finishing before the next starts, into one loop
 * that runs f and g element by element. The two orders are
 * indistinguishable only when every callback is free of observable effects
 * and cannot stop the program. This module is the proof: a whitelist over a
 * function's LIR and everything it calls.
 *
 * It also checks the `//iron:fusible` hint: a function that carries the
 * hint and fails the proof gets warning W0618 naming the reason.
 */
#ifndef IRON_LIR_FUSION_SAFETY_H
#define IRON_LIR_FUSION_SAFETY_H

#include "lir/lir.h"
#include "diagnostics/diagnostics.h"

typedef struct IronLIR_FusionSafety IronLIR_FusionSafety;

IronLIR_FusionSafety *iron_lir_fusion_safety_new(IronLIR_Module *module);
void iron_lir_fusion_safety_free(IronLIR_FusionSafety *s);

/* The function of the module with this name, or NULL. */
IronLIR_Func *iron_lir_fusion_find_func(IronLIR_FusionSafety *s, const char *name);

/* NULL when fn and every function it can reach are proven free of observable
 * effects and of operations that can panic. Otherwise a short reason, valid
 * until the safety object is freed ("calls 'println', which ..."). */
const char *iron_lir_fusion_unsafe_reason(IronLIR_FusionSafety *s, IronLIR_Func *fn);

/* True for an instruction that only computes a value from its operands: no
 * memory access, no call, no panic. Such an instruction may sit between two
 * stages of a chain without pinning their order. */
bool iron_lir_fusion_instr_is_value_only(IronLIR_Func *fn, IronLIR_Instr *in);

/* True for a value whose copies need no bookkeeping: numbers, Bool, and
 * objects and tuples made only of those, with no drop or copy hook and not
 * nocopy. Only such values may flow through a stage of a fused loop. */
bool iron_lir_fusion_type_is_trivial(const Iron_Type *t);

/* W0618 for every function of the program that carries `//iron:fusible` (or
 * the older `@fusible`) and cannot run inside a fused loop. Run on the LIR
 * before optimization, so the result does not depend on --no-optimize. */
void iron_lir_check_fusible_hints(IronLIR_Module *module, Iron_DiagList *diags,
                                  Iron_Arena *arena);

#endif /* IRON_LIR_FUSION_SAFETY_H */
