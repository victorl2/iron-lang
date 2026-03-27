#ifndef IRON_IR_EMIT_C_H
#define IRON_IR_EMIT_C_H

#include "ir/ir.h"

/* Emit C source from IR module. Returns arena-allocated C string. NULL on error. */
const char *iron_ir_emit_c(IronIR_Module *module, Iron_Arena *arena, Iron_DiagList *diags);

#endif /* IRON_IR_EMIT_C_H */
