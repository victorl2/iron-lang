#include "analyzer/analyzer.h"
#include "analyzer/resolve.h"
#include "analyzer/typecheck.h"
#include "analyzer/capture.h"
#include "analyzer/init_check.h"
#include "analyzer/escape.h"
#include "analyzer/concurrency.h"
#include "analyzer/web_await_check.h"
#include "analyzer/web_top_level_loader_check.h"
#include "comptime/comptime.h"
#include "lexer/lexer.h"
#include "parser/parser.h"
#include "vendor/stb_ds.h"
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdatomic.h>

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

    /* Step 5.6: Web target top-level loader guard (WEB-ASSET-03).
     * Emits E0502 if LoadTexture/LoadSound/LoadFont/LoadModel is called at
     * module level (outside any function body) for --target=web.
     * No-op on native. */
    iron_web_top_level_loader_check(program, arena, diags, target);
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

/* ── iron_analyze_buffer stub (HARD-01 — Plan 01) ─────────────────────────── */
/* Plan 01: delegates to the existing pipeline without semantic change.
 * Plan 02 removes analyzer short-circuits; Plan 03 wires cancellation;
 * Plan 04 adds pthread_once init + parser recursion guard; Plan 05 gates
 * comptime FS I/O on `mode`. */
Iron_AnalyzeResult iron_analyze_buffer(const char         *source,
                                        size_t              len,
                                        const char         *filename,
                                        IronAnalysisMode    mode,
                                        Iron_Arena         *arena,
                                        Iron_DiagList      *diags,
                                        const _Atomic bool *cancel_flag) {
    Iron_AnalyzeResult result = { .global_scope = NULL, .has_errors = false };

    /* Pre-cancel check (Plan 03 populates poll sites; Plan 01 honors the flag
     * at the pipeline entry only). */
    if (cancel_flag &&
        atomic_load_explicit(cancel_flag, memory_order_relaxed)) {
        return result;
    }

    Iron_Lexer lexer = iron_lexer_create(source, filename, arena, diags);
    Iron_Token *tokens = iron_lex_all(&lexer);

    int token_count = (int)arrlen(tokens);
    Iron_Parser parser = iron_parser_create(tokens, token_count, source,
                                            filename, arena, diags);
    Iron_Node *ast = iron_parse(&parser);
    arrfree(tokens);

    /* Plan 01 PRESERVES the existing short-circuit behaviour via delegating
     * to iron_analyze unchanged. Plan 02 removes the short-circuits inside
     * iron_analyze's interior directly. */
    result = iron_analyze((Iron_Program *)ast, arena, diags,
                          NULL, NULL, 0, false, IRON_TARGET_NATIVE);
    (void)mode;
    (void)len;
    return result;
}
