/* Phase 3 Plan 03 Task 01 (D-11, NAV-08) -- fzy-algorithm fuzzy scorer.
 *
 * Pure C17 port of John Hawthorn's fzy algorithm (see fzy.js/index.js
 * HEAD and fzy/ALGORITHM.md). Wagner-Fischer-like DP with two matrices:
 *
 *   M[i][j] -- best score ending at haystack[j] matching needle[0..i].
 *   D[i][j] -- best score ending at haystack[j] with needle[i] PINNED to
 *              position j (i.e. a match at (i, j)).
 *
 * Recurrence:
 *   D[i][j] = match_bonus(i, j)
 *             + (if i == 0:  0 + leading_gap(j))
 *               (else:        max(M[i-1][j-1], D[i-1][j-1] + CONSECUTIVE))
 *   M[i][j] = max(D[i][j],
 *                 (i == 0  ? M[i][j-1] + GAP_LEADING
 *                          : M[i][j-1] + GAP_INNER))
 *
 * When haystack length exceeds ILSP_FUZZY_MATCH_MAX_LEN the DP path is
 * skipped: a linear subsequence test determines match/no-match; the
 * score is then 0.0 / -INFINITY. This mirrors fzy's own DoS-defense
 * fallback and is required by T-03-07.
 *
 * Positions backtrack: when out_positions is non-NULL, walk the D
 * matrix backwards from the last-needle/last-haystack cell, choosing
 * whichever predecessor produced each D[i][j] to recover the haystack
 * indices. Emitted in needle-order (positions[0] = index of match for
 * needle[0], etc.).
 *
 * Kind ranking per D-11 is exposed alongside the scorer because every
 * caller that scores also sorts by kind as a tiebreaker.
 */

#include "lsp/facade/nav/fuzzy.h"

#include "analyzer/scope.h"
#include "util/arena.h"

#include <ctype.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/* ── Pinned fzy.js constants ──────────────────────────────────────── */

static const double SCORE_GAP_LEADING       = -0.005;
static const double SCORE_GAP_TRAILING      = -0.005;
static const double SCORE_GAP_INNER         = -0.01;
static const double SCORE_MATCH_CONSECUTIVE =  1.0;
static const double SCORE_MATCH_SLASH       =  0.9;
static const double SCORE_MATCH_WORD        =  0.8;
static const double SCORE_MATCH_CAPITAL     =  0.7;
static const double SCORE_MATCH_DOT         =  0.6;

/* ── Case helpers ─────────────────────────────────────────────────── */

static inline char to_lower_c(char c) {
    if (c >= 'A' && c <= 'Z') return (char)(c - 'A' + 'a');
    return c;
}

static inline bool chars_eq_ci(char a, char b) {
    return to_lower_c(a) == to_lower_c(b);
}

static inline bool is_upper(char c) { return c >= 'A' && c <= 'Z'; }
static inline bool is_lower(char c) { return c >= 'a' && c <= 'z'; }

/* ── has_match ────────────────────────────────────────────────────── */

bool ilsp_fuzzy_has_match(const char *needle, const char *haystack) {
    if (!needle || *needle == '\0') return true;
    if (!haystack) return false;
    const char *n = needle;
    const char *h = haystack;
    while (*n != '\0' && *h != '\0') {
        if (chars_eq_ci(*n, *h)) n++;
        h++;
    }
    return *n == '\0';
}

/* ── Match bonus lookup ───────────────────────────────────────────── */

/* Given haystack[j] and haystack[j-1] (the character immediately
 * before), return the structural bonus contribution at position j.
 * Matches fzy.js compute_bonus logic. */
static double match_bonus_at(char curr, char prev, bool at_start) {
    /* First position: no "prev" — treat as slash/word boundary with
     * no penalty but no bonus either (fzy.js uses 0 for this case
     * implicitly because the gap math handles the leading position). */
    if (at_start) return 0.0;
    if (prev == '/')                                  return SCORE_MATCH_SLASH;
    if (prev == '-' || prev == '_' || prev == ' ')    return SCORE_MATCH_WORD;
    if (prev == '.')                                  return SCORE_MATCH_DOT;
    if (is_lower(prev) && is_upper(curr))             return SCORE_MATCH_CAPITAL;
    return 0.0;
}

/* ── Linear fallback (no DP) ──────────────────────────────────────── */

/* Return the position-array for a linear subsequence match. Positions
 * array length == strlen(needle). Only valid when has_match returns
 * true for (needle, haystack). */
static void linear_positions(const char *needle, const char *haystack,
                              size_t *out_pos, size_t needle_len) {
    const char *n = needle;
    size_t i = 0;
    for (size_t j = 0; haystack[j] != '\0' && i < needle_len; j++) {
        if (chars_eq_ci(*n, haystack[j])) {
            out_pos[i++] = j;
            n++;
        }
    }
}

/* ── DP core ──────────────────────────────────────────────────────── */

ilsp_fuzzy_score_t ilsp_fuzzy_match(const char *needle,
                                     const char *haystack,
                                     Iron_Arena *arena,
                                     size_t **out_positions) {
    if (out_positions) *out_positions = NULL;

    /* Empty needle matches anything with score 0. */
    if (!needle || *needle == '\0') return 0.0;
    if (!haystack) return ILSP_FUZZY_SCORE_NO_MATCH;

    /* Reject non-matches cheaply. */
    if (!ilsp_fuzzy_has_match(needle, haystack)) {
        return ILSP_FUZZY_SCORE_NO_MATCH;
    }

    size_t n = strlen(needle);
    size_t m = strlen(haystack);

    /* T-03-07: skip DP on oversized haystacks or oversized needles.
     * Return 0.0 on match (has_match was already true), allowing the
     * caller to still surface the entry deterministically by kind /
     * path / line tiebreak. */
    if (m > ILSP_FUZZY_MATCH_MAX_LEN || n > ILSP_FUZZY_NEEDLE_MAX_LEN) {
        if (out_positions && arena && n > 0) {
            size_t *pos = (size_t *)iron_arena_alloc(
                arena, n * sizeof(size_t), _Alignof(size_t));
            if (pos) {
                linear_positions(needle, haystack, pos, n);
                *out_positions = pos;
            }
        }
        return 0.0;
    }

    /* Exact whole-string match short-circuit (case-insensitive). */
    if (n == m) {
        bool exact = true;
        for (size_t j = 0; j < m; j++) {
            if (!chars_eq_ci(needle[j], haystack[j])) { exact = false; break; }
        }
        if (exact) {
            if (out_positions && arena) {
                size_t *pos = (size_t *)iron_arena_alloc(
                    arena, n * sizeof(size_t), _Alignof(size_t));
                if (pos) {
                    for (size_t j = 0; j < n; j++) pos[j] = j;
                    *out_positions = pos;
                }
            }
            /* fzy.js returns SCORE_MAX = infinity for exact match; we
             * use a finite high value so downstream sort stays stable.
             * n * SCORE_MATCH_CONSECUTIVE is the max possible DP score. */
            return (double)n * SCORE_MATCH_CONSECUTIVE;
        }
    }

    /* Allocate D and M as flat n*m arrays. Use malloc (not arena) so
     * the caller's per-request arena isn't burned up on every scoring
     * pass (workspace/symbol scores every entry in the index). */
    double *D = (double *)malloc(sizeof(double) * n * m);
    double *M = (double *)malloc(sizeof(double) * n * m);
    if (!D || !M) {
        free(D); free(M);
        return ILSP_FUZZY_SCORE_NO_MATCH;
    }

    /* Fill row by row. */
    for (size_t i = 0; i < n; i++) {
        double prev_score = ILSP_FUZZY_SCORE_NO_MATCH;
        double gap_score = (i == n - 1) ? SCORE_GAP_TRAILING : SCORE_GAP_INNER;
        /* Leading gap penalty for row 0 accumulates per-column. */
        double lead_gap = (i == 0) ? SCORE_GAP_LEADING : SCORE_GAP_INNER;

        for (size_t j = 0; j < m; j++) {
            double score = ILSP_FUZZY_SCORE_NO_MATCH;

            if (chars_eq_ci(needle[i], haystack[j])) {
                if (i == 0) {
                    /* Leading gap cost: j leading gaps before this
                     * match. */
                    score = (double)j * SCORE_GAP_LEADING +
                            match_bonus_at(haystack[j],
                                           j > 0 ? haystack[j - 1] : 0,
                                           j == 0);
                } else if (j > 0) {
                    /* Best of: (prev needle matched at j-1) + CONSECUTIVE,
                     *          (prev needle matched earlier)  + bonus. */
                    double a = M[(i - 1) * m + (j - 1)] +
                               match_bonus_at(haystack[j],
                                              j > 0 ? haystack[j - 1] : 0,
                                              j == 0);
                    double b = D[(i - 1) * m + (j - 1)] +
                               SCORE_MATCH_CONSECUTIVE;
                    score = (a > b) ? a : b;
                }
            }
            D[i * m + j] = score;

            /* M[i][j] = best of (match at j) or (extend gap from j-1). */
            double extended;
            if (j == 0) {
                extended = (i == 0) ? 0.0 : ILSP_FUZZY_SCORE_NO_MATCH;
            } else {
                double gap = (i == 0) ? lead_gap : gap_score;
                extended = prev_score + gap;
            }
            double chosen = (score > extended) ? score : extended;
            M[i * m + j] = chosen;
            prev_score = chosen;
        }
    }

    /* Final score = M[n-1][m-1]. */
    double final_score = M[(n - 1) * m + (m - 1)];

    /* Backtrack for positions. Walk from (n-1, m-1) upwards: at each
     * row i, find the rightmost j <= current-j where D[i][j] is the
     * dominant contributor. */
    if (isfinite(final_score) && out_positions && arena) {
        size_t *pos = (size_t *)iron_arena_alloc(
            arena, n * sizeof(size_t), _Alignof(size_t));
        if (pos) {
            /* Classic fzy backtrack: for each needle-char i (last to
             * first), find the j where D[i][j] == M[i][j] (i.e. a
             * match, not an extension). For row i, j must be to the
             * right of row i-1's chosen j + 1 constraint handled
             * implicitly by the DP. Simpler robust scan: walk the DP
             * table from (n-1, m-1) backwards by choosing, at each
             * row i, the latest j for which D[i][j] == M[i][j]. */
            long i = (long)n - 1;
            long j = (long)m - 1;
            bool match_required = false;
            while (i >= 0) {
                /* Look for the j where D matches M going backwards. */
                while (j >= 0) {
                    double dij = D[i * m + j];
                    double mij = M[i * m + j];
                    if (isfinite(dij) && dij == mij) {
                        pos[i] = (size_t)j;
                        j--;
                        match_required = true;
                        break;
                    }
                    j--;
                }
                if (!match_required) break;
                match_required = false;
                i--;
            }
            *out_positions = pos;
        }
    }

    free(D);
    free(M);
    return final_score;
}

/* ── Kind rank ────────────────────────────────────────────────────── */

int ilsp_fuzzy_kind_rank(Iron_SymbolKind kind) {
    /* Lower rank = surfaced higher in workspace/symbol results.
     * Ordering per D-11. Iron_SymbolKind enum (see scope.h):
     *   IRON_SYM_VARIABLE, IRON_SYM_FUNCTION, IRON_SYM_METHOD,
     *   IRON_SYM_TYPE, IRON_SYM_ENUM, IRON_SYM_ENUM_VARIANT,
     *   IRON_SYM_INTERFACE, IRON_SYM_PARAM, IRON_SYM_FIELD. */
    switch (kind) {
        case IRON_SYM_FUNCTION:     return 0;
        case IRON_SYM_METHOD:       return 1;
        case IRON_SYM_TYPE:         return 2;
        case IRON_SYM_INTERFACE:    return 3;
        case IRON_SYM_ENUM:         return 4;
        case IRON_SYM_ENUM_VARIANT: return 5;
        case IRON_SYM_FIELD:        return 6;
        case IRON_SYM_VARIABLE:     return 7;
        case IRON_SYM_PARAM:        return 8;
    }
    return 9;
}
