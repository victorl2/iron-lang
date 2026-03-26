#ifndef IRON_TYPECHECK_H
#define IRON_TYPECHECK_H

#include "parser/ast.h"
#include "analyzer/scope.h"
#include "analyzer/types.h"
#include "diagnostics/diagnostics.h"
#include "util/arena.h"

/* Run type checking on a name-resolved program.
 * Annotates resolved_type on all expression nodes and declared_type on val/var decls.
 * Validates: type assignments, return types, val immutability, nullable access,
 * interface completeness, ConstructExpr/CallExpr disambiguation.
 * Errors accumulated in diags.
 */
void iron_typecheck(Iron_Program *program, Iron_Scope *global_scope,
                    Iron_Arena *arena, Iron_DiagList *diags);

#endif /* IRON_TYPECHECK_H */
