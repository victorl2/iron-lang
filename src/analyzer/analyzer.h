#ifndef IRON_ANALYZER_H
#define IRON_ANALYZER_H

#include "parser/ast.h"
#include "analyzer/scope.h"
#include "analyzer/types.h"
#include "analyzer/iface_collect.h"
#include "diagnostics/diagnostics.h"
#include "util/arena.h"
#include "cli/build.h"
#include <stddef.h>
#include <stdbool.h>
#include <stdatomic.h>

/* ── Analysis mode (HARD-02) ──────────────────────────────────────────────── */
/* Controls compile-time side effects and cascade-suppression behaviour.
 * CLI preserves legacy semantics; LSP disables comptime FS I/O and disables
 * cascade-suppression (in_error_recovery effect on diagnostic emission).
 * Plan 01: enum declared; behavioural gating lands in Plan 05 (FS) and
 * Plan 02 (cascade-suppression). */
typedef enum {
    IRON_ANALYSIS_MODE_CLI = 0,
    IRON_ANALYSIS_MODE_LSP = 1
} IronAnalysisMode;

/* Result of running the full analysis pipeline. */
typedef struct {
    Iron_Scope         *global_scope;   /* root of scope tree */
    Iron_IfaceRegistry  iface_registry; /* interface implementor map */
    bool                has_errors;     /* true if any semantic errors occurred */
    /* Phase 3 NAV-15: AST root. Always set to the parsed Iron_Program
     * (even when has_errors is true, so NAV consumers can still walk
     * partial trees). For iron_analyze(), which takes program as an
     * input, this mirrors the input pointer. For iron_analyze_buffer,
     * which owns the parse, it is the freshly-parsed root. */
    Iron_Program *program;
} Iron_AnalyzeResult;

/* Run the complete semantic analysis pipeline on the given program:
 *   1. Initialize type system
 *   2. Name resolution (two-pass)
 *   3. Type checking
 *   4. Escape analysis
 *   5. Concurrency checks
 *   5.5. Web target `await` reachability check (WEB-RUNTIME-04)
 *   6. Comptime evaluation (if no errors)
 *
 * source_file_dir: directory of the source .iron file, for read_file() path
 *   resolution in comptime expressions.  Pass NULL if unknown.
 * source_text/len: full source text used as comptime cache key.  Pass NULL/0
 *   to skip caching.
 * force_comptime: bypass cache and re-evaluate all comptime expressions.
 * target: build target enum from IronBuildOpts.target. Used to gate
 *   web-specific analyzer passes (WEB-RUNTIME-04).
 *
 * Returns result with global scope and error flag.
 * If has_errors is true, the program should NOT be passed to codegen.
 * All errors are accumulated in diags.
 */
Iron_AnalyzeResult iron_analyze(Iron_Program *program, Iron_Arena *arena,
                                 Iron_DiagList *diags,
                                 const char *source_file_dir,
                                 const char *source_text, size_t source_len,
                                 bool force_comptime,
                                 IronBuildTarget target);

/* HARD-02 / HARD-03 / HARD-05: mode-aware, cancel-aware analyzer dispatcher.
 * Identical to iron_analyze() but carries an IronAnalysisMode (so downstream
 * passes and the comptime stage can gate LSP-specific behaviour) and a
 * cancel flag (NULL means never cancel; threaded into every pass walker).
 * iron_analyze() is a thin delegator that passes IRON_ANALYSIS_MODE_CLI and
 * a NULL cancel_flag. */
Iron_AnalyzeResult iron_analyze_with_mode(Iron_Program *program,
                                           IronAnalysisMode mode,
                                           Iron_Arena *arena,
                                           Iron_DiagList *diags,
                                           const char *source_file_dir,
                                           const char *source_text, size_t source_len,
                                           bool force_comptime,
                                           IronBuildTarget target,
                                           const _Atomic bool *cancel_flag);

/* ── Unified analysis entry point (HARD-01) ───────────────────────────────── */
/* Unified analysis entry used by both `iron check` and the future LSP facade.
 *
 * Preconditions:
 *   - `source` points to `len` bytes of Iron source; `filename` is either
 *     arena-interned by the caller or the impl copies it into `arena`.
 *   - `arena` is caller-owned. iron_analyze_buffer MUST NOT create its own
 *     arena on the analysis path (HARD-06).
 *   - `diags` is caller-owned; iron_analyze_buffer accumulates into it.
 *   - `cancel_flag` may be NULL (meaning "never cancel") or point to a
 *     caller-owned `_Atomic bool`.
 *
 * Cancellation (HARD-05, wired in Plan 03):
 *   When *cancel_flag becomes true, returns at the next safepoint with
 *   partial diagnostics plus a NOTE-level IRON_ERR_CANCELLED meta-diagnostic.
 *
 * Thread safety (HARD-07, wired in Plan 04):
 *   Safe to call concurrently from multiple threads provided each call
 *   uses its own arena, diags, and cancel_flag. The primitive type
 *   singletons (iron_types_init) are pthread_once-guarded.
 *
 * Returns Iron_AnalyzeResult with the same shape as iron_analyze().
 */
Iron_AnalyzeResult iron_analyze_buffer(const char         *source,
                                        size_t              len,
                                        const char         *filename,
                                        IronAnalysisMode    mode,
                                        Iron_Arena         *arena,
                                        Iron_DiagList      *diags,
                                        const _Atomic bool *cancel_flag);

#endif /* IRON_ANALYZER_H */
