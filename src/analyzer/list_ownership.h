#ifndef IRON_LIST_OWNERSHIP_H
#define IRON_LIST_OWNERSHIP_H

#include "parser/ast.h"
#include "diagnostics/diagnostics.h"
#include "util/arena.h"

#include <stdatomic.h>
#include <stdbool.h>

/* A dynamic list is never duplicated implicitly (#174). Where a list gets a
 * new owner (a binding initializer, an assignment, a constructor argument
 * that becomes a field, a list element, a push / insert / set argument, or
 * a return of anything but an owned local) the value must be a fresh list:
 * a literal, a call result, `a.copy()` or `a.take()`. A list that already
 * lives in a variable, parameter, field or element is rejected there with
 * E0328. Passing a list to a function or method borrows it and is not
 * affected.
 *
 * Runs after type checking; reads resolved types only. */
void iron_list_ownership_check(Iron_Program *program, Iron_Arena *arena,
                               Iron_DiagList *diags,
                               const _Atomic bool *cancel_flag);

#endif /* IRON_LIST_OWNERSHIP_H */
