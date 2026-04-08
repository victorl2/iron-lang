#ifndef IRON_LIR_EMIT_C_H
#define IRON_LIR_EMIT_C_H

#include "lir/lir.h"
#include "lir/lir_optimize.h"
#include "analyzer/iface_collect.h"

/* Emit C source from IR module. Returns arena-allocated C string. NULL on error.
 * opt_info must be pre-populated by iron_lir_optimize().
 * iface_reg: interface implementor registry for tagged union generation. May be NULL. */
const char *iron_lir_emit_c(IronLIR_Module *module, Iron_Arena *arena,
                            Iron_DiagList *diags, IronLIR_OptimizeInfo *opt_info,
                            Iron_IfaceRegistry *iface_reg,
                            bool warn_fusion_break,
                            bool report_compression);

#endif /* IRON_LIR_EMIT_C_H */
