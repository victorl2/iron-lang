#ifndef IRON_LIR_LOWER_H
#define IRON_LIR_LOWER_H

#include "lir/lir.h"
#include "parser/ast.h"
#include "analyzer/scope.h"

/* Lower analyzed AST program to IR module. Returns NULL on error. */
IronLIR_Module *iron_lir_lower(Iron_Program *program, Iron_Scope *global_scope,
                              Iron_Arena *ir_arena, Iron_DiagList *diags);

#endif /* IRON_LIR_LOWER_H */
