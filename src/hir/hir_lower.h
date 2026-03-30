#ifndef IRON_HIR_LOWER_H
#define IRON_HIR_LOWER_H

#include "hir/hir.h"
#include "parser/ast.h"
#include "analyzer/scope.h"

/* ── Public API ──────────────────────────────────────────────────────────── */

/* Lower a fully-analyzed Iron program to a HIR module.
 *
 * Parameters:
 *   program      — annotated AST program (output of the analyzer)
 *   global_scope — the top-level scope built by the analyzer
 *   hir_arena    — arena for the HIR module and all its nodes (may be NULL;
 *                  if NULL, the module allocates its own arena)
 *   diags        — diagnostic list; any errors are appended here
 *
 * Returns the new IronHIR_Module on success, or NULL if diagnostics
 * contain one or more errors after lowering.
 *
 * The returned module owns all allocated HIR nodes through its arena.
 * Call iron_hir_module_destroy() to release it.
 */
IronHIR_Module *iron_hir_lower(Iron_Program *program, Iron_Scope *global_scope,
                               Iron_Arena *hir_arena, Iron_DiagList *diags);

#endif /* IRON_HIR_LOWER_H */
