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

/* HARD-02 / HARD-03: mode-aware analyzer dispatcher.
 * Every pass runs unconditionally on the AST (no short-circuits on
 * diags->error_count). Passes are responsible for tolerating ErrorNode and
 * partial annotations (HARD-04). The one remaining `error_count == 0` guard
 * is the comptime stage (Step 6) — running comptime on a broken AST is
 * unsafe, so we preserve that semantic gate. */
Iron_AnalyzeResult iron_analyze_with_mode(Iron_Program *program,
                                           IronAnalysisMode mode,
                                           Iron_Arena *arena,
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

    /* Step 3: Type checking — runs regardless of resolve errors (HARD-03) */
    iron_typecheck(program, result.global_scope, arena, diags);

    /* Step 3b: Capture analysis — annotate Iron_LambdaExpr.captures[] */
    iron_capture_analyze(program, result.global_scope, arena, diags);

    /* Step 3.5: Definite assignment analysis */
    iron_init_check(program, result.global_scope, arena, diags);

    /* Step 4: Escape analysis */
    iron_escape_analyze(program, result.global_scope, arena, diags);

    /* Step 5: Concurrency checks */
    iron_concurrency_check(program, result.global_scope, arena, diags);

    /* Step 5.5: Web target `await` reachability check (WEB-RUNTIME-04).
     * Runs only when target == IRON_TARGET_WEB. No-op on native.
     * HARD-03: no short-circuit — pass must tolerate ErrorNode. */
    iron_web_await_check(program, arena, diags, target);

    /* Step 5.6: Web target top-level loader guard (WEB-ASSET-03).
     * Emits E0502 if LoadTexture/LoadSound/LoadFont/LoadModel is called at
     * module level (outside any function body) for --target=web.
     * No-op on native. HARD-03: no short-circuit. */
    iron_web_top_level_loader_check(program, arena, diags, target);

    /* Step 5b: Interface implementor collection — build IfaceRegistry */
    result.iface_registry = iron_iface_collect(program, arena);

    /* Step 5c: Dead implementor elimination */
    iron_iface_eliminate_dead(&result.iface_registry, program);


    /* Step 6: Comptime evaluation — replace IRON_NODE_COMPTIME nodes with literals.
     * PRESERVED: comptime on a broken AST is unsafe (const-eval on unresolved
     * symbols is undefined). This guard is semantic, NOT a HARD-03 short-circuit. */
    if (diags->error_count == 0) {
        iron_comptime_apply(program, result.global_scope, arena, diags,
                            source_file_dir, force_comptime,
                            source_text, source_len);
    }

    (void)mode; /* Plan 05 consumes mode for comptime FS gating */

    result.has_errors = (diags->error_count > 0);
    return result;
}

/* Backwards-compatible dispatcher — CLI mode preserves legacy behaviour. */
Iron_AnalyzeResult iron_analyze(Iron_Program *program, Iron_Arena *arena,
                                 Iron_DiagList *diags,
                                 const char *source_file_dir,
                                 const char *source_text, size_t source_len,
                                 bool force_comptime,
                                 IronBuildTarget target) {
    return iron_analyze_with_mode(program, IRON_ANALYSIS_MODE_CLI, arena, diags,
                                   source_file_dir, source_text, source_len,
                                   force_comptime, target);
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
    iron_parser_set_mode(&parser, mode); /* HARD-02: LSP mode disables cascade suppression */
    Iron_Node *ast = iron_parse(&parser);
    arrfree(tokens);

    /* HARD-02/HARD-03: mode is threaded into iron_analyze_with_mode so each
     * pass runs unconditionally and LSP-specific gating can be honoured. */
    result = iron_analyze_with_mode((Iron_Program *)ast, mode, arena, diags,
                                    NULL, NULL, 0, false, IRON_TARGET_NATIVE);
    (void)len;
    return result;
}
