#ifndef IRON_IR_EMIT_C_H
#define IRON_IR_EMIT_C_H

#include "ir/ir.h"
#include "ir/ir_optimize.h"

/* Emit C source from IR module. Returns arena-allocated C string. NULL on error.
 * opt_info must be pre-populated by iron_ir_optimize(). */
const char *iron_ir_emit_c(IronIR_Module *module, Iron_Arena *arena,
                            Iron_DiagList *diags, IronIR_OptimizeInfo *opt_info);

#endif /* IRON_IR_EMIT_C_H */
