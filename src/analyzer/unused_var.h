#ifndef IRON_UNUSED_VAR_H
#define IRON_UNUSED_VAR_H

/* Phase 17 VAL-05/VAL-06: unused-var warning pass.
 *
 * Per-function walker that collects `var` parameters + `var` local bindings,
 * scans the function body for IDENT-LHS assigns and compound-assigns, and
 * emits IRON_WARN_UNUSED_VAR (locals) or IRON_WARN_UNUSED_VAR_PARAM
 * (parameters) for entries with no IDENT-write reaching them.
 *
 * Semantics (per CONTEXT.md + RESEARCH Pitfall 2):
 * - "Mutation = binding reassignment only" — direct `x = ...` and compound
 *   `x += ...`, `x -= ...`, etc. count. Field writes (`x.f = ...`),
 *   `&x` address-of, pass-to-`var`-slot do NOT count.
 * - "Any-write-anywhere" semantics: a write anywhere in the function body
 *   (including inside if/match/while/for branches) counts. No flow-sensitive
 *   analysis is required; if a future phase needs path-sensitivity it can
 *   reuse init_check.c's forward-flow infrastructure.
 * - Param shadowing (RESEARCH Pitfall 3): identity is by Iron_Symbol*
 *   (resolved_sym pointer) when available, name-fallback otherwise. A var
 *   param shadowed by an inner val with the same name still warns on the
 *   param.
 *
 * Cancellation: poll cancel_flag at per-function loop boundary (HARD-05). */

#include "parser/ast.h"
#include "analyzer/scope.h"
#include "diagnostics/diagnostics.h"
#include "util/arena.h"

#include <stdatomic.h>
#include <stdbool.h>

void iron_unused_var_check(Iron_Program *program,
                            Iron_Scope *global_scope,
                            Iron_Arena *arena,
                            Iron_DiagList *diags,
                            const _Atomic bool *cancel_flag);

#endif /* IRON_UNUSED_VAR_H */
