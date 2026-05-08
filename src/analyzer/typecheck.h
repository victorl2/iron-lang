#ifndef IRON_TYPECHECK_H
#define IRON_TYPECHECK_H

#include "parser/ast.h"
#include "analyzer/scope.h"
#include "analyzer/types.h"
#include "diagnostics/diagnostics.h"
#include "util/arena.h"

#include <stdatomic.h>
#include <stdbool.h>

/* Run type checking on a name-resolved program.
 * Annotates resolved_type on all expression nodes and declared_type on val/var decls.
 * Validates: type assignments, return types, val immutability, nullable access,
 * interface completeness, ConstructExpr/CallExpr disambiguation.
 * Errors accumulated in diags.
 *
 * HARD-05: cancel_flag (NULL = never cancel) is polled at entry and inside
 * recursive walkers. */
void iron_typecheck(Iron_Program *program, Iron_Scope *global_scope,
                    Iron_Arena *arena, Iron_DiagList *diags,
                    const _Atomic bool *cancel_flag);

/* Phase 20 PTR-10 (Plan 20-02a): walk a chain of FIELD_ACCESS / INDEX
 * back to the rooted IRON_NODE_IDENT and return its resolved Iron_Symbol.
 * Returns NULL when the chain doesn't terminate at an Iron_Ident or the
 * ident has no resolved_sym. Exposed publicly so the analyzer.c
 * mark_takes_local_addr_pass walker (Plan 20-02a Task 3) can reuse it
 * without pulling typecheck.c internals through a private header. */
Iron_Symbol *iron_walk_to_root_binding(Iron_Node *expr);

#endif /* IRON_TYPECHECK_H */
