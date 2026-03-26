#include "analyzer/analyzer.h"
#include "analyzer/resolve.h"
#include "analyzer/typecheck.h"
#include "analyzer/escape.h"
#include "analyzer/concurrency.h"
#include "comptime/comptime.h"

Iron_AnalyzeResult iron_analyze(Iron_Program *program, Iron_Arena *arena, Iron_DiagList *diags) {
    Iron_AnalyzeResult result = { .global_scope = NULL, .has_errors = false };

    /* Step 1: Initialize type system (interned primitives) */
    iron_types_init(arena);

    /* Step 2: Name resolution */
    result.global_scope = iron_resolve(program, arena, diags);
    if (diags->error_count > 0) {
        result.has_errors = true;
        return result;  /* Cannot type-check with unresolved names */
    }

    /* Step 3: Type checking */
    iron_typecheck(program, result.global_scope, arena, diags);
    if (diags->error_count > 0) {
        result.has_errors = true;
        return result;  /* Cannot continue analysis with type errors */
    }

    /* Step 4: Escape analysis */
    iron_escape_analyze(program, result.global_scope, arena, diags);

    /* Step 5: Concurrency checks */
    iron_concurrency_check(program, result.global_scope, arena, diags);

    /* Step 6: Comptime evaluation — replace IRON_NODE_COMPTIME nodes with literals */
    if (diags->error_count == 0) {
        iron_comptime_apply(program, result.global_scope, arena, diags,
                            NULL /* source_file_dir — set by caller if needed */,
                            false /* force_comptime */);
    }

    result.has_errors = (diags->error_count > 0);
    return result;
}
