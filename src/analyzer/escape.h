#ifndef IRON_ESCAPE_H
#define IRON_ESCAPE_H

#include "parser/ast.h"
#include "analyzer/scope.h"
#include "analyzer/types.h"
#include "diagnostics/diagnostics.h"
#include "util/arena.h"

/* Run escape analysis on a type-checked program.
 * Sets auto_free and escapes flags on Iron_HeapExpr nodes.
 * Emits E0207 for escaping heap values without free/leak.
 * Emits E0212/E0213/E0214 for invalid free/leak usage.
 */
void iron_escape_analyze(Iron_Program *program, Iron_Scope *global_scope,
                         Iron_Arena *arena, Iron_DiagList *diags);

#endif /* IRON_ESCAPE_H */
