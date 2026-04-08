/* emit_fusion.h -- Fused loop emission for chained collection operations.
 *
 * This module handles fused loop emission for chained collection method
 * calls (map/filter/reduce/forEach/sum). Instead of allocating intermediate
 * collections, operations are fused into a single loop per concrete type.
 *
 * Extracted from emit_c.c (Phase 52, Plan 03).
 */

#ifndef IRON_EMIT_FUSION_H
#define IRON_EMIT_FUSION_H

#include "lir/emit_helpers.h"

/* Expression inlining -- defined in emit_c.c, used by emit_fusion.c.
 * Recursively builds a C expression for a given value. If the value is
 * inline-eligible, reconstructs it as a sub-expression; otherwise emits _vN. */
void emit_expr_to_buf(Iron_StrBuf *sb, IronLIR_ValueId vid,
                       IronLIR_Func *fn, EmitCtx *ctx,
                       IronLIR_BlockId use_block_id, int depth);

/* Emit a fused loop replacing chained collection method calls
 * (map/filter/reduce/forEach/sum) with a single loop per concrete type.
 * Called at the terminal node of a detected fusion chain.
 *
 * @param ctx    Emitter context
 * @param sb     Output buffer (implementations or lifted_funcs)
 * @param fn     Current LIR function
 * @param chain  Detected fusion chain with source, nodes, and metadata
 * @param terminal_instr  The terminal CALL instruction triggering emission
 * @param indent Current indentation level
 */
void emit_fused_chain(EmitCtx *ctx, Iron_StrBuf *sb, IronLIR_Func *fn,
                       FusionChain *chain, IronLIR_Instr *terminal_instr,
                       int indent);

#endif /* IRON_EMIT_FUSION_H */
