#ifndef IRON_LSP_FACADE_NAV_FUZZY_H
#define IRON_LSP_FACADE_NAV_FUZZY_H

/* Phase 3 Plan 03 Task 01 (D-11, NAV-08) -- fzy-algorithm fuzzy scorer.
 *
 * Pure C17 subsequence scorer based on John Hawthorn's fzy. The public
 * surface is two functions:
 *
 *   ilsp_fuzzy_has_match(needle, haystack) -- O(|haystack|) subsequence
 *     test; case-insensitive. True iff every character of `needle`
 *     appears in `haystack` in order.
 *
 *   ilsp_fuzzy_match(needle, haystack, arena, out_positions) -- full
 *     Wagner-Fischer-like DP score. Returns -INFINITY when there is no
 *     match, 0.0 for an empty needle, otherwise a positive-bounded
 *     double. When `out_positions` is non-NULL, populated with the
 *     haystack indices (arena-allocated, length = strlen(needle)) of
 *     the best match.
 *
 * Pinned scoring constants (from fzy.js/index.js HEAD; see PLAN
 * 03-03-PLAN.md §threat_model and RESEARCH.md §Fuzzy Scorer):
 *
 *   SCORE_GAP_LEADING       = -0.005
 *   SCORE_GAP_TRAILING      = -0.005
 *   SCORE_GAP_INNER         = -0.01
 *   SCORE_MATCH_CONSECUTIVE =  1.0
 *   SCORE_MATCH_SLASH       =  0.9
 *   SCORE_MATCH_WORD        =  0.8   (after '_', '-', ' ')
 *   SCORE_MATCH_CAPITAL     =  0.7   (lowercase -> uppercase)
 *   SCORE_MATCH_DOT         =  0.6
 *
 * DoS defense (T-03-07): haystack length > ILSP_FUZZY_MATCH_MAX_LEN
 * (1024) falls back to a linear subsequence test returning 0.0 on
 * match and -INFINITY otherwise -- no DP matrix allocation. Callers
 * (workspace/symbol) also reject queries > 256 characters before ever
 * entering this layer.
 *
 * Kind-ranking tiebreak for workspace/symbol: macro
 * ILSP_FUZZY_KIND_RANK(kind) returns 0-9 for the canonical Iron
 * symbol-kind ordering (Function best, Import worst) per D-11.
 */

#include "analyzer/scope.h"   /* Iron_SymbolKind */
#include "util/arena.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Haystack length cap for the DP path. Longer haystacks downgrade to a
 * linear subsequence match and score 0.0 on match. */
#define ILSP_FUZZY_MATCH_MAX_LEN   1024
/* Query length cap. workspace/symbol rejects queries above this limit. */
#define ILSP_FUZZY_NEEDLE_MAX_LEN   256

/* Return value for "no match". Mirrors fzy.js SCORE_MIN. */
#define ILSP_FUZZY_SCORE_NO_MATCH   (-1.0 / 0.0)   /* -INFINITY */

typedef double ilsp_fuzzy_score_t;

/* Case-insensitive subsequence test: true iff every char of `needle`
 * appears in `haystack` in order. Both NULL-tolerant: empty needle
 * matches anything; NULL haystack matches only when needle is NULL/empty. */
bool ilsp_fuzzy_has_match(const char *needle, const char *haystack);

/* Compute a fuzzy-match score. Returns ILSP_FUZZY_SCORE_NO_MATCH if
 * `needle` is not a subsequence of `haystack`. Empty needle scores 0.0.
 *
 * When `out_positions` is non-NULL, populated with the matched
 * haystack indices (arena-allocated, length = strlen(needle)). If
 * `arena` is NULL and `out_positions` was requested, positions are
 * silently skipped.
 *
 * Haystacks longer than ILSP_FUZZY_MATCH_MAX_LEN fall back to a linear
 * subsequence test (score 0.0 on match) -- see T-03-07 DoS defense. */
ilsp_fuzzy_score_t ilsp_fuzzy_match(const char *needle,
                                     const char *haystack,
                                     Iron_Arena *arena,
                                     size_t **out_positions);

/* Kind ranking for workspace/symbol tiebreak per D-11: lower rank is
 * surfaced higher in results.
 *
 *   Function       = 0
 *   Method         = 1
 *   Object (Type)  = 2
 *   Interface      = 3
 *   Enum           = 4
 *   Field          = 5
 *   Val (handled via Variable downstream)
 *   Variable       = 7
 *   Enum Variant   (treated as Field rank)
 *   Param          (treated as Variable rank)
 *   Module/Import  = 9
 *
 * Unknown kinds fall back to 9 (lowest priority). */
int ilsp_fuzzy_kind_rank(Iron_SymbolKind kind);

#define ILSP_FUZZY_KIND_RANK(k) ilsp_fuzzy_kind_rank(k)

#ifdef __cplusplus
}
#endif

#endif /* IRON_LSP_FACADE_NAV_FUZZY_H */
