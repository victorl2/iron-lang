#ifndef IRON_HIR_TO_LIR_H
#define IRON_HIR_TO_LIR_H

#include "hir/hir.h"
#include "lir/lir.h"
#include "parser/ast.h"
#include "analyzer/scope.h"

/* Convert HIR module to LIR module with SSA construction.
 * program is needed for type declarations (objects, interfaces, enums, externs).
 * global_scope is used for scope resolution during type lowering.
 * lir_arena: arena that will own the resulting LIR module.
 * diags: diagnostic list for error reporting.
 *
 * Returns a valid IronLIR_Module* on success, or NULL on fatal error.
 * The returned module has passed iron_lir_verify() internally. */
IronLIR_Module *iron_hir_to_lir(IronHIR_Module *hir, Iron_Program *program,
                                Iron_Scope *global_scope,
                                Iron_Arena *lir_arena, Iron_DiagList *diags);

#endif /* IRON_HIR_TO_LIR_H */
