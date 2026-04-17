#ifndef IRON_CONCURRENCY_H
#define IRON_CONCURRENCY_H

#include "parser/ast.h"
#include "analyzer/scope.h"
#include "analyzer/types.h"
#include "diagnostics/diagnostics.h"
#include "util/arena.h"

#include <stdatomic.h>
#include <stdbool.h>

/* Run concurrency checks on a type-checked program.
 * Validates parallel-for bodies do not mutate outer non-mutex variables.
 * Validates spawn block captures (future: more checks).
 *
 * HARD-05: cancel_flag (NULL = never cancel) polled at entry and walkers.
 */
void iron_concurrency_check(Iron_Program *program, Iron_Scope *global_scope,
                            Iron_Arena *arena, Iron_DiagList *diags,
                            const _Atomic bool *cancel_flag);

#endif /* IRON_CONCURRENCY_H */
