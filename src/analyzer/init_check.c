/* init_check.c -- Definite assignment analysis for Iron.
 *
 * Runs after type checking. For each function:
 *   1. Collect `var` declarations without initializers (uninit vars).
 *   2. Walk statements tracking a "definitely assigned" name set.
 *   3. On identifier use, check if name is in the uninit set AND not
 *      in the definitely-assigned set => emit E0314.
 *   4. On assignment to a tracked var, add to definitely-assigned set.
 */

#include "analyzer/init_check.h"
#include "vendor/stb_ds.h"
#include <string.h>
#include <stdio.h>

/* Stub -- to be implemented in GREEN phase. */
void iron_init_check(Iron_Program *program, Iron_Scope *global_scope,
                     Iron_Arena *arena, Iron_DiagList *diags) {
    (void)program;
    (void)global_scope;
    (void)arena;
    (void)diags;
}
