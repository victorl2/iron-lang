#include "analyzer/analyzer.h"
#include "analyzer/resolve.h"
#include "analyzer/typecheck.h"
#include "analyzer/capture.h"
#include "analyzer/init_check.h"
#include "analyzer/escape.h"
#include "analyzer/concurrency.h"
#include "analyzer/web_await_check.h"
#include "comptime/comptime.h"
#include "vendor/stb_ds.h"
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>

Iron_AnalyzeResult iron_analyze(Iron_Program *program, Iron_Arena *arena,
                                 Iron_DiagList *diags,
                                 const char *source_file_dir,
                                 const char *source_text, size_t source_len,
                                 bool force_comptime,
                                 IronBuildTarget target) {
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

    /* Step 3b: Capture analysis — annotate Iron_LambdaExpr.captures[] */
    iron_capture_analyze(program, result.global_scope, arena, diags);

    /* Step 3.5: Definite assignment analysis */
    iron_init_check(program, result.global_scope, arena, diags);

    /* Step 4: Escape analysis */
    iron_escape_analyze(program, result.global_scope, arena, diags);

    /* Step 5: Concurrency checks */
    iron_concurrency_check(program, result.global_scope, arena, diags);

    /* Step 5.5: Web target `await` reachability check (WEB-RUNTIME-04).
     * Runs only when target == IRON_TARGET_WEB. No-op on native. */
    iron_web_await_check(program, arena, diags, target);
    if (diags->error_count > 0) {
        result.has_errors = true;
        return result;
    }

    /* Step 5b: Interface implementor collection — build IfaceRegistry */
    result.iface_registry = iron_iface_collect(program, arena);

    /* Step 5c: Dead implementor elimination */
    iron_iface_eliminate_dead(&result.iface_registry, program);


    /* Step 6: Comptime evaluation — replace IRON_NODE_COMPTIME nodes with literals */
    if (diags->error_count == 0) {
        iron_comptime_apply(program, result.global_scope, arena, diags,
                            source_file_dir, force_comptime,
                            source_text, source_len);
    }

    result.has_errors = (diags->error_count > 0);
    return result;
}
