#ifndef IRON_LIR_EMIT_C_H
#define IRON_LIR_EMIT_C_H

#include "lir/lir.h"
#include "lir/lir_optimize.h"

/* Emit C source from IR module. Returns arena-allocated C string. NULL on error.
 * opt_info must be pre-populated by iron_lir_optimize(). */
const char *iron_lir_emit_c(IronLIR_Module *module, Iron_Arena *arena,
                            Iron_DiagList *diags, IronLIR_OptimizeInfo *opt_info);

#endif /* IRON_LIR_EMIT_C_H */
