#ifndef IRON_GLOBAL_ORDER_H
#define IRON_GLOBAL_ORDER_H

/* Initialization order of global bindings (manual 5.7).
 *
 * A global is initialized after every global its initializer reads,
 * directly or through the functions, methods and initializers it calls
 * (calls known statically; a lambda's body counts where it is written).
 * Globals with no such dependency keep declaration order, which in a
 * package is the order iron concatenates its files. A global that depends
 * on itself is an initialization cycle (E0345).
 *
 * Returns the number of cycles found, each reported to diags when diags
 * and arena are not NULL. When out_order is not NULL it receives the
 * global declarations (Iron_ValDecl / Iron_VarDecl) in initialization
 * order, an stb_ds array the caller frees with arrfree. */

#include "parser/ast.h"
#include "diagnostics/diagnostics.h"
#include "util/arena.h"

int iron_global_init_order(Iron_Program *program, Iron_Arena *arena, Iron_DiagList *diags,
                           Iron_Node ***out_order);

#endif
