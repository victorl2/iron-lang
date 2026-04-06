#ifndef IRON_INIT_CHECK_H
#define IRON_INIT_CHECK_H

#include "parser/ast.h"
#include "analyzer/scope.h"
#include "analyzer/types.h"
#include "diagnostics/diagnostics.h"
#include "util/arena.h"

/* Run definite assignment analysis on a type-checked program.
 * For each function body, tracks which variables are definitely assigned
 * at each program point. Emits IRON_ERR_POSSIBLY_UNINITIALIZED (E0314)
 * when a variable may be read before being assigned on all paths.
 *
 * Only checks `var` declarations without initializers.
 * `val` declarations always have initializers; function parameters are
 * always initialized. Uses bounded name-set tracking, O(n*v).
 */
void iron_init_check(Iron_Program *program, Iron_Scope *global_scope,
                     Iron_Arena *arena, Iron_DiagList *diags);

#endif /* IRON_INIT_CHECK_H */
