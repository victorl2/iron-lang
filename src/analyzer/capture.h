#ifndef IRON_CAPTURE_H
#define IRON_CAPTURE_H

#include "parser/ast.h"
#include "analyzer/scope.h"
#include "analyzer/types.h"
#include "diagnostics/diagnostics.h"
#include "util/arena.h"

/* Run free variable (capture) analysis on a type-checked program.
 * Annotates each Iron_LambdaExpr with its capture set.
 * Sets lambda->captures and lambda->capture_count for every lambda in the
 * program. Non-capturing lambdas get capture_count == 0 and captures == NULL.
 *
 * Must be called after iron_typecheck() and before escape analysis.
 */
void iron_capture_analyze(Iron_Program *program, Iron_Scope *global_scope,
                          Iron_Arena *arena, Iron_DiagList *diags);

/* Print a human-readable summary of all lambda/spawn/pfor captures in
 * the program to stderr.  Called by the build pipeline when --verbose is set,
 * after iron_capture_analyze() has annotated the AST.
 */
void iron_capture_verbose_report(Iron_Program *program, Iron_Arena *arena);

#endif /* IRON_CAPTURE_H */
