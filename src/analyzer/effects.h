#ifndef IRON_EFFECTS_H
#define IRON_EFFECTS_H

/* Transitive effects for the method purity tiers.
 *
 * `pure` methods may not perform I/O or touch global state and `readonly`
 * methods may not perform I/O (spec §6, mutation tiers). The type checker
 * enforces that for what a method body does directly; this pass carries it
 * through calls. It infers, for every user free function and method,
 * whether running it can perform I/O (console builtins, the I/O stdlib
 * modules, `extern` functions) or write a module global, following calls to
 * a fixed point. Then it reports a `pure` method that calls a free function
 * with either effect (E0242) and a `readonly` method that calls one that
 * performs I/O (E0278). Before this pass a pure method could print or bump a
 * global simply by calling a helper that did.
 *
 * Runs after type checking (method calls need receiver types). */

#include "parser/ast.h"
#include "analyzer/scope.h"
#include "diagnostics/diagnostics.h"
#include "util/arena.h"

#include <stdatomic.h>
#include <stdbool.h>

void iron_effects_check(Iron_Program *program,
                        Iron_Scope *global_scope,
                        Iron_Arena *arena,
                        Iron_DiagList *diags,
                        const _Atomic bool *cancel_flag);

#endif /* IRON_EFFECTS_H */
