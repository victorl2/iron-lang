#ifndef IRON_RESOLVE_H
#define IRON_RESOLVE_H

#include "parser/ast.h"
#include "analyzer/scope.h"
#include "analyzer/types.h"
#include "diagnostics/diagnostics.h"
#include "util/arena.h"

/* Run name resolution on the given program.
 * Builds scope tree, populates symbol tables, and sets resolved_sym on all
 * Iron_Ident nodes.
 * Returns the global scope (root of the scope tree).
 * Errors are accumulated in diags.
 */
Iron_Scope *iron_resolve(Iron_Program *program, Iron_Arena *arena,
                          Iron_DiagList *diags);

#endif /* IRON_RESOLVE_H */
