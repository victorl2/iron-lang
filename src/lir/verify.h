#ifndef IRON_LIR_VERIFY_H
#define IRON_LIR_VERIFY_H

#include "lir/lir.h"

/* Verify structural invariants of IR module. Returns true if valid.
 * Errors reported to diags. arena used for diagnostic message allocation. */
bool iron_lir_verify(const IronLIR_Module *module, Iron_DiagList *diags, Iron_Arena *arena);

#endif /* IRON_LIR_VERIFY_H */
