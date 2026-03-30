#ifndef IRON_LIR_PRINT_H
#define IRON_LIR_PRINT_H

#include "lir/lir.h"

/* Print IR module to human-readable text. Returns heap-allocated string (caller frees). */
char *iron_lir_print(const IronLIR_Module *module, bool show_annotations);

#endif /* IRON_LIR_PRINT_H */
