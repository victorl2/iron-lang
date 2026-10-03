#ifndef IRON_ANALYZER_GENERICS_H
#define IRON_ANALYZER_GENERICS_H

/* User generics by monomorphisation.
 *
 * A generic function `func f[T](...)` or object `object C[T] { ... }` in
 * user code is a template. The type checker requests an instance for each
 * set of concrete type arguments it meets (explicit `f[Int](x)`,
 * `C[Int](x)`, a `C[Int]` annotation, or arguments inferred from a call).
 * After a type-check round, iron_generics_materialize clones each new
 * instance: the template is printed, renamed to a mangled name, its type
 * parameters replaced by the concrete types' source text, and the result
 * parsed and appended to the program. The analyzer then re-runs name
 * resolution and type checking so the instances are checked like
 * hand-written code, and finally drops the templates so no later pass
 * sees them.
 */

#include "parser/ast.h"
#include "analyzer/types.h"
#include "diagnostics/diagnostics.h"
#include "util/arena.h"

#include <stdbool.h>

/* Forget every instance (start of an analysis). */
void iron_generics_reset(void);

/* Is `decl` a user generic template (function, object, or a method of a
 * generic object)? Stdlib generics are compiler-handled and are not. */
bool iron_generics_is_template(Iron_Program *program, Iron_Node *decl);

/* The mangled name of the instance of template `decl` (a FuncDecl or
 * ObjectDecl) for `args`, recording it if new. NULL when an argument has
 * no source spelling (error / unresolved types). */
const char *iron_generics_request(Iron_Node *decl, Iron_Type **args, int argc,
                                  Iron_Arena *arena);

/* The template an instance named `mangled` was made from, with its type
 * arguments (so `swap(p)` with `p: Pair[Int]` can bind T). NULL when the
 * name is not an instance. */
Iron_Node *iron_generics_instance_of(const char *mangled, Iron_Type ***out_args, int *out_argc);

/* Clone every requested, not yet materialised instance into the program.
 * Returns the number of instances added. */
int iron_generics_materialize(Iron_Program *program, Iron_Arena *arena,
                              Iron_DiagList *diags);

/* Remove templates from the program (after the last type-check round). */
void iron_generics_drop_templates(Iron_Program *program);

#endif /* IRON_ANALYZER_GENERICS_H */
