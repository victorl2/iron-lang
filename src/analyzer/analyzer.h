#ifndef IRON_ANALYZER_H
#define IRON_ANALYZER_H

#include "parser/ast.h"
#include "analyzer/scope.h"
#include "analyzer/types.h"
#include "diagnostics/diagnostics.h"
#include "util/arena.h"

/* Result of running the full analysis pipeline. */
typedef struct {
    Iron_Scope *global_scope;   /* root of scope tree */
    bool        has_errors;     /* true if any semantic errors occurred */
} Iron_AnalyzeResult;

/* Run the complete semantic analysis pipeline on the given program:
 *   1. Initialize type system
 *   2. Name resolution (two-pass)
 *   3. Type checking
 *   4. Escape analysis
 *   5. Concurrency checks
 *
 * Returns result with global scope and error flag.
 * If has_errors is true, the program should NOT be passed to codegen.
 * All errors are accumulated in diags.
 */
Iron_AnalyzeResult iron_analyze(Iron_Program *program, Iron_Arena *arena, Iron_DiagList *diags);

#endif /* IRON_ANALYZER_H */
