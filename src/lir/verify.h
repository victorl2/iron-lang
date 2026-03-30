#ifndef IRON_IR_VERIFY_H
#define IRON_IR_VERIFY_H

#include "ir/ir.h"

/* Verify structural invariants of IR module. Returns true if valid.
 * Errors reported to diags. arena used for diagnostic message allocation. */
bool iron_ir_verify(const IronIR_Module *module, Iron_DiagList *diags, Iron_Arena *arena);

#endif /* IRON_IR_VERIFY_H */
