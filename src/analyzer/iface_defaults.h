#ifndef IRON_IFACE_DEFAULTS_H
#define IRON_IFACE_DEFAULTS_H

#include "parser/ast.h"
#include "util/arena.h"
#include "diagnostics/diagnostics.h"

/* Interface default bodies (docs/plans/2026-09-22-interface-mutation-design.md §5).
 *
 * An interface method signature that carries a body is a default. Every
 * `impl` object that does not define that method inherits it. Rather than
 * teaching the resolver, typecheck, HIR lowering and the dispatcher emitter
 * about a second kind of method, the default is MONOMORPHISED: for each
 * (implementor, defaulted method) pair with no user-written method, the
 * signature+body is pretty-printed, wrapped in `patch object <T> { ... }`,
 * re-parsed, and the resulting method decls are appended to the program.
 * Downstream passes then see an ordinary patch method on the concrete type:
 * `self` resolves to the implementor, the interface's tier (readonly / pure /
 * mutating) is preserved verbatim, and the generated dispatcher finds
 * `Iron_<t>_<m>` like any other case.
 *
 * Must run before name resolution. Idempotent: a second run finds every
 * default already present. */
void iron_iface_synthesize_defaults(Iron_Program *program, Iron_Arena *arena,
                                    Iron_DiagList *diags);

#endif /* IRON_IFACE_DEFAULTS_H */
