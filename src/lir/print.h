#ifndef IRON_IR_PRINT_H
#define IRON_IR_PRINT_H

#include "ir/ir.h"

/* Print IR module to human-readable text. Returns heap-allocated string (caller frees). */
char *iron_ir_print(const IronIR_Module *module, bool show_annotations);

#endif /* IRON_IR_PRINT_H */
