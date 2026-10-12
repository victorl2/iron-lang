#ifndef IRON_VIEW_CHECK_H
#define IRON_VIEW_CHECK_H

#include "parser/ast.h"
#include "diagnostics/diagnostics.h"
#include "util/arena.h"

#include <stdatomic.h>
#include <stdbool.h>

/* The borrow rules of views (manual 2.15): a view never escapes its block
 * (E0369) and its source is not resized, reassigned or taken while the
 * view is in use (E0370). Runs after type checking and capture analysis;
 * reads resolved types only. */
void iron_view_check(Iron_Program *program, Iron_Arena *arena, Iron_DiagList *diags,
                     const _Atomic bool *cancel_flag);

/* W0631: `//iron:inline` on a function that calls itself. */
void iron_hint_check(Iron_Program *program, Iron_Arena *arena, Iron_DiagList *diags);

/* E0368: `//iron:align` below the natural alignment, or an over-aligned
 * value in storage that cannot honor it yet. */
void iron_align_check(Iron_Program *program, Iron_Arena *arena, Iron_DiagList *diags);

#endif /* IRON_VIEW_CHECK_H */
