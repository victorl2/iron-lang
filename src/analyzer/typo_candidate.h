#ifndef IRON_TYPO_CANDIDATE_H
#define IRON_TYPO_CANDIDATE_H

/* typo_candidate.h — Phase 4 Plan 04-01 (EDIT-07).
 *
 * Compiler-side helper used by `src/analyzer/resolve.c` to seed the
 * `Iron_Diagnostic.suggestion` field with a near-miss identifier name for
 * IRON_ERR_UNDEFINED_VAR emits. The seeding is a *correctness* prerequisite
 * for the code-action dispatch layer landing in Plan 04-04 (LSP reads the
 * .suggestion string to build a replace-text edit).
 *
 * The API lives in the `analyzer` layer so it can see Iron_Scope internals
 * and stays header-decoupled from the LSP facade (zero new lsp/ call sites).
 *
 * Design:
 *   - `iron_levenshtein` is a classic 2-row dynamic-programming Levenshtein
 *     implementation. It accepts a `max_dist` threshold and early-exits
 *     when the current row's minimum cell exceeds the threshold — this
 *     keeps the DP O(n*m) worst-case bounded and avoids pathological
 *     blow-up on long identifier names (T-4-2 mitigation in 04-PLAN
 *     threat register).
 *   - `iron_best_typo_candidate` walks the parent-linked Iron_Scope chain
 *     and returns the best-matching symbol name within `max_dist=2` (the
 *     same threshold rustc uses). Tie-breaks alphabetically by strcmp.
 *
 * Both functions are side-effect-free apart from arena allocation of the
 * result string; thread-safe across independent compilation units.
 */

#include "diagnostics/diagnostics.h"
#include "util/arena.h"
#include <stddef.h>

/* Iron_Scope is forward-declared to avoid pulling analyzer/scope.h into
 * every consumer; the implementation TU (typo_candidate.c) does the full
 * include. Iron_Arena is a typedef (not a struct tag), so we include
 * util/arena.h directly for its full shape. */
struct Iron_Scope;

/* Classic 2-row DP Levenshtein.
 *
 * Returns the edit distance between `a` and `b` (ignoring case, NULLs treated
 * as empty). Early-exits at `max_dist+1` — when the minimum cell of the
 * current row exceeds `max_dist`, the function returns `max_dist+1`
 * immediately. This keeps worst-case bounded to roughly O(min(|a|,|b|) *
 * max_dist) under the early-exit.
 *
 * `max_dist` must be >= 0. Pass `max_dist=INT_MAX` to compute the full
 * distance without early-exit.
 *
 * Implementation uses heap-allocated scratch buffers; caller does not
 * need to supply an arena. No persistent allocations are made.
 */
int iron_levenshtein(const char *a, const char *b, int max_dist);

/* Walk the parent-linked Iron_Scope chain starting at `scope`, collect every
 * visible symbol name, and return the best-matching name to `name` via
 * Levenshtein distance <= 2. On tie, prefers alphabetically earlier name
 * (strcmp order) for deterministic output.
 *
 * Returns:
 *   - arena-strdup'd copy of the candidate name on success (owned by `arena`)
 *   - NULL if no candidate is within max distance 2 or on arena OOM
 *
 * Skips the exact-match case (`strcmp(name, candidate) == 0`) because the
 * identifier would have resolved successfully if it matched exactly —
 * returning the same name as the input is not a useful quickfix.
 *
 * Stdlib + dep candidate enrichment is deferred: the LSP-layer completion
 * provider (Plan 04-02+) already has access to the stdlib/dep surface and
 * can enrich the quickfix at dispatch time. Compiler-side seeding only
 * needs the local scope chain to cover the common typo case.
 */
const char *iron_best_typo_candidate(struct Iron_Scope *scope,
                                      Iron_Arena        *arena,
                                      const char        *name);

#endif /* IRON_TYPO_CANDIDATE_H */
