/* lower_exprs.c — Expression lowering from AST to IR.
 * Implements lower_expr() — the main expression lowering dispatch function.
 * Mirrors gen_exprs.c but produces IronIR instructions instead of C text.
 *
 * Populated in Task 3 of Plan 08-01.
 */

#include "ir/lower_internal.h"
#include "lexer/lexer.h"
#include <string.h>
#include <stdlib.h>

/* Forward declaration for recursive calls */
IronIR_ValueId lower_expr(IronIR_LowerCtx *ctx, Iron_Node *node);

IronIR_ValueId lower_expr(IronIR_LowerCtx *ctx, Iron_Node *node) {
    if (!node) return IRON_IR_VALUE_INVALID;

    /* Stub — returns INVALID for all nodes until Task 3 populates this */
    (void)ctx;
    return IRON_IR_VALUE_INVALID;
}
