#ifndef IRON_IR_LOWER_H
#define IRON_IR_LOWER_H

#include "ir/ir.h"
#include "parser/ast.h"
#include "analyzer/scope.h"

/* Lower analyzed AST program to IR module. Returns NULL on error. */
IronIR_Module *iron_ir_lower(Iron_Program *program, Iron_Scope *global_scope,
                              Iron_Arena *ir_arena, Iron_DiagList *diags);

#endif /* IRON_IR_LOWER_H */
